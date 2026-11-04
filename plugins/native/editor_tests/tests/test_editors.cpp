// SPDX-License-Identifier: GPL-3.0-or-later
//
// native.editors, processor side (ADR-0057): for the compressor, the FET, the
// opto, the limiter and the EQ,
//   - the UiHints each declares agree with its parameter table (a hint for a
//     parameter that is not there, a switch on a continuous parameter, a
//     curve_source naming no curve would mislead the editor built from it);
//   - process() publishes the meters it declares: input and output level,
//     and the gain reduction, which is zero on silence and not on a signal
//     over the threshold;
//   - the EQ's response and the compressor's transfer curve are the curves
//     the settings make;
//   - none of that allocates once the processor is prepared.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "sonora/native/compressor.h"
#include "sonora/native/eq.h"
#include "sonora/native/fet.h"
#include "sonora/native/limiter.h"
#include "sonora/native/opto.h"
#include "sonora/native/processor_base.h"

namespace {
int failures = 0;
std::atomic<long> g_allocations{0};
}  // namespace

#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size > 0 ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace sonora;
using namespace sonora::native;

namespace {

constexpr std::uint32_t kBlock = 256;
constexpr double kRate = 48000.0;

struct Rig {
    std::unique_ptr<Processor> p;
    ProcessorBase* base = nullptr;
    double phase = 0.0;

    explicit Rig(std::unique_ptr<Processor> processor) : p(std::move(processor)) {
        base = dynamic_cast<ProcessorBase*>(p.get());
        CHECK(base != nullptr);
        CHECK(p->prepare(kRate, kBlock, 2));
    }

    // `blocks` of a 1 kHz sine at `db` dBFS on both channels (silence below -150).
    void run(int blocks, double db) {
        float l[kBlock], r[kBlock], ol[kBlock], orr[kBlock];   // on the stack: no_allocation() counts
        const float* in[2] = {l, r};
        float* out[2] = {ol, orr};
        const double amp = db < -150.0 ? 0.0 : std::pow(10.0, db / 20.0);
        for (int b = 0; b < blocks; ++b) {
            for (std::uint32_t i = 0; i < kBlock; ++i) {
                l[i] = r[i] = static_cast<float>(amp * std::sin(phase));
                phase += 2.0 * 3.14159265358979323846 * 1000.0 / kRate;
            }
            AudioBuffers io{in, out, nullptr, 2, 0, kBlock};
            p->process(io, nullptr, 0, Transport{});
        }
    }

    bool read(plugin::TelemetrySnapshot& s) const { return base->telemetry().read(s); }
    void set(std::uint32_t index, float v) { CHECK(p->set_param(index, v)); }
};

bool has_unit(const ParamInfo& p, Unit u) { return p.unit == u; }

// The hints agree with the table.
void hints_are_coherent(Processor& proc, const char* who) {
    const UiHints* ui = proc.ui_hints();
    if (ui == nullptr) {
        std::fprintf(stderr, "%s: no ui_hints\n", who);
        ++failures;
        return;
    }
    const Descriptor& d = proc.descriptor();
    std::set<std::string> seen, curves, meters;
    CHECK(ui->meter_count <= plugin::kMaxTelemetryMeters && ui->curve_count <= plugin::kMaxTelemetryCurves);
    for (std::uint32_t i = 0; i < ui->curve_count; ++i) {
        const CurveDecl& c = ui->curves[i];
        CHECK(curves.insert(c.id).second);
        CHECK(c.points >= 2 && c.points <= plugin::kMaxCurvePoints);
        CHECK(c.x_min < c.x_max && c.y_min < c.y_max);
        CHECK(!c.x_log || c.x_min > 0.0f);
    }
    for (std::uint32_t i = 0; i < ui->meter_count; ++i) {
        CHECK(meters.insert(ui->meters[i].id).second);
        CHECK(ui->meters[i].min < ui->meters[i].max);
    }
    for (std::uint32_t k = 0; k < ui->param_count; ++k) {
        const ParamHint& h = ui->params[k];
        CHECK(seen.insert(h.id).second);
        const ParamInfo* info = nullptr;
        for (std::uint32_t i = 0; i < d.param_count; ++i)
            if (std::strcmp(d.params[i].id, h.id) == 0) info = &d.params[i];
        if (info == nullptr) {
            std::fprintf(stderr, "%s: hint for unknown parameter %s\n", who, h.id);
            ++failures;
            continue;
        }
        CHECK(h.section == nullptr || std::strlen(h.section) <= 64);
        if (h.widget == Widget::Switch) CHECK(has_unit(*info, Unit::Toggle) || info->choice_count == 2);
        if (h.widget == Widget::Choice) CHECK(info->choice_count >= 2);
        if (h.curve_source != nullptr) CHECK(curves.count(h.curve_source) == 1);
    }
}

// Silence reads as no reduction and a floor level; a loud signal moves the
// input meter to where it is and the reduction off zero.
void publishes_meters(Rig& rig, const char* who, std::uint32_t gr, std::uint32_t in, std::uint32_t out,
                      double loud_db) {
    plugin::TelemetrySnapshot s;
    rig.run(8, -200.0);
    CHECK(rig.read(s));
    CHECK(s.sequence > 0);
    CHECK((s.meter_mask & ((1u << gr) | (1u << in) | (1u << out))) == ((1u << gr) | (1u << in) | (1u << out)));
    CHECK(s.meters[in] <= -100.0f);
    CHECK(s.meters[gr] < 0.5f);
    rig.run(200, loud_db);
    CHECK(rig.read(s));
    if (std::fabs(s.meters[in] - loud_db) > 1.0f || !(s.meters[gr] > 0.5f) || !std::isfinite(s.meters[out])) {
        std::fprintf(stderr, "%s: in %.2f (want %.1f) gr %.2f out %.2f\n", who, double(s.meters[in]), loud_db,
                     double(s.meters[gr]), double(s.meters[out]));
        ++failures;
    }
}

void dynamics() {
    {
        Rig r(make_compressor());
        hints_are_coherent(*r.p, "compressor");
        r.set(compressor::kThreshold, -30.0f);
        r.set(compressor::kRatio, 8.0f);
        publishes_meters(r, "compressor", 0, 1, 2, -6.0);
        // The transfer curve is the gain computer at these settings.
        plugin::TelemetrySnapshot s;
        CHECK(r.read(s) && s.curve_points[0] == plugin::kMaxCurvePoints);
        CHECK(std::fabs(s.curves[0][0] - (-60.0f)) < 0.01f);            // below the knee: unity
        CHECK(std::fabs(s.curves[0][127] - (-30.0f + 30.0f / 8.0f)) < 0.2f);  // 0 dB in
        r.set(compressor::kRatio, 2.0f);
        r.run(2, -20.0);
        CHECK(r.read(s) && std::fabs(s.curves[0][127] - (-30.0f + 30.0f / 2.0f)) < 0.2f);
    }
    {
        Rig r(make_fet());
        hints_are_coherent(*r.p, "fet");
        r.set(fet::kInput, 12.0f);
        publishes_meters(r, "fet", 0, 1, 2, -6.0);
    }
    {
        Rig r(make_opto());
        hints_are_coherent(*r.p, "opto");
        r.set(opto::kPeakReduction, 100.0f);
        publishes_meters(r, "opto", 0, 1, 2, -6.0);
    }
    {
        Rig r(make_limiter());
        hints_are_coherent(*r.p, "limiter");
        r.set(limiter::kInput, 12.0f);
        publishes_meters(r, "limiter", 0, 1, 2, -6.0);
    }
}

void eq_curve() {
    Rig r(make_eq());
    hints_are_coherent(*r.p, "eq");
    plugin::TelemetrySnapshot s;
    r.run(4, -20.0);
    CHECK(r.read(s));
    CHECK((s.meter_mask & 3u) == 3u);                       // in and out
    CHECK(s.curve_points[0] == plugin::kMaxCurvePoints);
    // Defaults: every band at 0 dB, the filters off: a flat response.
    for (std::uint32_t i = 0; i < plugin::kMaxCurvePoints; ++i) CHECK(std::fabs(s.curves[0][i]) < 0.1f);
    // A +12 dB bell at 1 kHz: the curve peaks there and is flat far away.
    r.set(eq::bell(1, 2), 12.0f);
    r.run(2, -20.0);
    CHECK(r.read(s));
    std::uint32_t peak = 0;
    for (std::uint32_t i = 0; i < plugin::kMaxCurvePoints; ++i)
        if (s.curves[0][i] > s.curves[0][peak]) peak = i;
    const double want = 127.0 * std::log(1000.0 / 20.0) / std::log(1000.0);  // index of 1 kHz
    CHECK(std::fabs(double(peak) - want) <= 2.0);
    CHECK(std::fabs(s.curves[0][peak] - 12.0f) < 0.5f);
    CHECK(std::fabs(s.curves[0][0]) < 0.5f);
    // A high-pass at 200 Hz takes the bottom of the range off.
    r.set(eq::kHpfOn, 1.0f);
    r.set(eq::kHpfFreq, 200.0f);
    r.run(2, -20.0);
    CHECK(r.read(s));
    CHECK(s.curves[0][0] < -15.0f);
    // The output gain moves the whole curve.
    r.set(eq::kHpfOn, 0.0f);
    r.set(eq::bell(1, 2), 0.0f);
    r.set(eq::kOutput, -6.0f);
    r.run(2, -20.0);
    CHECK(r.read(s));
    CHECK(std::fabs(s.curves[0][64] - (-6.0f)) < 0.1f);
}

// The publish path allocates nothing once prepared.
void no_allocation() {
    Rig comp(make_compressor());
    Rig eq(make_eq());
    comp.run(2, -20.0);
    eq.run(2, -20.0);
    const long before = g_allocations.load();
    for (int i = 0; i < 50; ++i) {
        comp.run(1, -10.0);
        eq.run(1, -10.0);
        eq.set(eq::bell(1, 2), static_cast<float>(i % 12));   // a new curve each time
    }
    CHECK(g_allocations.load() == before);
}

}  // namespace

int main() {
    dynamics();
    eq_curve();
    no_allocation();
    if (failures != 0) return EXIT_FAILURE;
    std::printf("native editors: ok\n");
    return EXIT_SUCCESS;
}
