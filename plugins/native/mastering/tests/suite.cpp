// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mastering console against its bars (quality.json):
// - the neutral console passes the programme unchanged;
// - each stage is the Sonora stage it is built from: the console with only
//   that stage in use equals the standalone processor with the same
//   settings (the wiring, the fixed Q, shelf corners and knee);
// - width against mid/side, and mono at 0 %;
// - the ceiling is never exceeded, on the 16x reference meter;
// - saturation adds harmonics, and none at Mix 0;
// - auto-level reaches the target within 0.5 LU, and stops at its +-12 dB;
// - the common checks (latency, non-finite input, reset, denormals,
//   zipper, golden, CPU against class D).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/dsp/loudness.h"
#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/compressor.h"
#include "sonora/native/eq.h"
#include "sonora/native/limiter.h"
#include "sonora/native/mastering.h"
#include "sonora/native/saturator.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

Chans render_aligned(native::Processor& p, Chans x) {
    const std::size_t lat = p.latency();
    for (auto& c : x) c.resize(c.size() + lat, 0.0f);
    Chans y = measure::render(p, x);
    for (auto& c : y) c.erase(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(lat));
    return y;
}

double max_diff(const Chans& a, const Chans& b) {
    double w = 0.0;
    for (std::size_t c = 0; c < a.size(); ++c) {
        const std::size_t n = std::min(a[c].size(), b[c].size());
        for (std::size_t i = 0; i < n; ++i) w = std::fmax(w, std::fabs(static_cast<double>(a[c][i]) - b[c][i]));
    }
    return w;
}

// Integrated loudness (BS.1770, gated) of x from `from`.
double lufs_of(const Chans& x, double fs, std::size_t from) {
    dsp::LoudnessMeter m;
    m.prepare(fs, static_cast<std::uint32_t>(x.size()), nullptr, false);
    const std::size_t block = 4800;
    for (std::size_t i = from; i < x[0].size(); i += block) {
        const std::size_t n = std::min(block, x[0].size() - i);
        const float* p[2] = {x[0].data() + i, x.size() > 1 ? x[1].data() + i : nullptr};
        m.process(p, static_cast<std::uint32_t>(n));
    }
    return m.integrated_lufs();
}

std::string fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
    char buf[200];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

void neutral(Suite& s) {
    const double fs = 48000.0;
    auto p = s.make();
    p->prepare(fs, 128, 2);
    const Chans x = measure::noise(fs, -12.0, 0.5, 11u);
    const Chans y = render_aligned(*p, x);
    const double w = max_diff(y, x);
    s.check_le("default console (every stage neutral) against the input, noise at -12 dBFS", w,
               s.bar("neutral.max_abs"), "abs", fmt("%.2e", w));
}

// The console with one stage in use against that stage standalone.
void wiring(Suite& s) {
    const double fs = 48000.0;
    const double bar = s.bar("wiring.max_abs");
    const Chans x = measure::noise(fs, -18.0, 1.0, 21u);
    const Chans hot = measure::noise(fs, -2.0, 1.0, 22u);

    auto compare = [&](const char* what, std::unique_ptr<Processor> ref, const Config& ref_cfg, const Config& con_cfg,
                       const Chans& in) {
        auto con = s.make();
        measure::apply(*con, con_cfg);
        con->prepare(fs, 128, 2);
        measure::apply(*ref, ref_cfg);
        ref->prepare(fs, 128, 2);
        Chans yc = render_aligned(*con, in);
        Chans yr = render_aligned(*ref, in);
        const double w = max_diff(yc, yr);
        s.check_le(std::string(what) + ": console against the standalone stage", w, bar, "abs", fmt("%.2e", w));
    };
    compare("EQ (low shelf +5, bell 250 Hz -4, bell 3 kHz +3, high shelf -3)", make_eq(),
            {{"mode", 0.0f}, {"low_shelf_gain", 5.0f}, {"bell1_freq", 250.0f}, {"bell1_gain", -4.0f},
             {"bell1_q", 0.7f}, {"bell2_freq", 3000.0f}, {"bell2_gain", 3.0f}, {"bell2_q", 0.7f},
             {"high_shelf_gain", -3.0f}},
            {{"low_gain", 5.0f}, {"lowmid_freq", 250.0f}, {"lowmid_gain", -4.0f}, {"highmid_freq", 3000.0f},
             {"highmid_gain", 3.0f}, {"high_gain", -3.0f}},
            x);
    compare("compressor (-30 dB, 3:1, 20 ms, 150 ms, 70 %)", make_compressor(),
            {{"threshold", -30.0f}, {"ratio", 3.0f}, {"knee", 12.0f}, {"attack", 20.0f}, {"release", 150.0f},
             {"mix", 70.0f}},
            {{"comp_threshold", -30.0f}, {"comp_ratio", 3.0f}, {"comp_attack", 20.0f}, {"comp_release", 150.0f},
             {"comp_mix", 70.0f}},
            x);
    compare("saturation (12 dB drive, 50 %)", make_saturator(),
            {{"drive", 12.0f}, {"mix", 50.0f}, {"shape", 0.0f}, {"mode", 0.0f}, {"quality", 0.0f}},
            {{"sat_drive", 12.0f}, {"sat_mix", 50.0f}}, x);
    compare("limiter (ceiling -3 dBTP, 100 ms) on a hot input", make_limiter(),
            {{"ceiling", -3.0f}, {"release", 100.0f}}, {{"ceiling", -3.0f}, {"limit_release", 100.0f}}, hot);
}

void width_stage(Suite& s) {
    const double fs = 48000.0;
    const double bar = s.bar("width.max_error");
    const Chans x = measure::noise(fs, -24.0, 0.5, 31u);
    double worst = 0.0, mono = 0.0;
    for (const float wp : {0.0f, 50.0f, 150.0f, 200.0f}) {
        auto p = s.make();
        measure::apply(*p, {{"width", wp}});
        p->prepare(fs, 128, 2);
        const Chans y = render_aligned(*p, x);
        const double w = wp * 0.01;
        for (std::size_t i = 0; i < x[0].size(); ++i) {
            const double mid = 0.5 * (static_cast<double>(x[0][i]) + x[1][i]);
            const double side = 0.5 * (static_cast<double>(x[0][i]) - x[1][i]);
            worst = std::fmax(worst, std::fabs(y[0][i] - (mid + w * side)));
            worst = std::fmax(worst, std::fabs(y[1][i] - (mid - w * side)));
            if (wp == 0.0f) mono = std::fmax(mono, std::fabs(y[0][i] - y[1][i]));
        }
    }
    s.check_le("width 0, 50, 150, 200 % against mid + w side, mid - w side", worst, bar, "abs");
    s.check_le("width 0 %: left against right", mono, s.bar("width.mono_abs"), "abs");
}

void ceiling(Suite& s) {
    const double fs = 48000.0;
    const double bar = s.bar("ceiling.max_dbtp");
    double worst = -400.0;
    for (const Chans& x : {measure::noise(fs, -3.0, 2.0, 41u), measure::sine(fs, 997.0, -1.0, 2.0)}) {
        for (const float ceil : {-1.0f, -3.0f}) {
            auto p = s.make();
            measure::apply(*p, {{"gain", 12.0f}, {"ceiling", ceil}, {"sat_mix", 40.0f}, {"sat_drive", 12.0f}});
            p->prepare(fs, 128, 2);
            const Chans y = render_aligned(*p, x);
            for (const auto& ch : y) {
                const double tp = measure::to_db(measure::reference_true_peak(ch, 0.3));
                worst = std::fmax(worst, tp - ceil);
            }
        }
    }
    s.check_le("true peak over the ceiling, +12 dB gain into the limiter, 16x reference (dB re ceiling)", worst, bar,
               "dB");
}

void saturation(Suite& s) {
    const double fs = 48000.0;
    double thd[2];
    for (int i = 0; i < 2; ++i) {
        auto p = s.make();
        measure::apply(*p, {{"sat_drive", 18.0f}, {"sat_mix", i ? 100.0f : 0.0f}});
        p->prepare(fs, 128, 2);
        const Chans y = render_aligned(*p, measure::sine(fs, 1000.0, -18.0, 1.5));
        thd[i] = measure::thdn_db(y[0], 19200, fs, 1000.0);
    }
    s.info("THD+N, 1 kHz at -18 dBFS, drive 18 dB, saturation 0 %", thd[0], "dB");
    s.info("THD+N, 1 kHz at -18 dBFS, drive 18 dB, saturation 100 %", thd[1], "dB");
    s.check("saturation 100 % raises THD+N over 0 %", thd[1] - thd[0], s.bar("saturation.min_rise_db"),
            thd[1] - thd[0] >= s.bar("saturation.min_rise_db"), "dB");
}

// Auto-level on noise of known loudness: the output's loudness over the
// last seconds against the target.
void auto_level(Suite& s) {
    const double fs = 48000.0;
    const double secs = 34.0;
    const Chans x = measure::noise(fs, -30.0, secs, 51u);
    const double in_lufs = lufs_of(x, fs, 0);
    const struct {
        const char* what;
        double want_gain;
        double bar_lu;
    } cases[] = {{"reachable target (+8 LU)", 8.0, s.bar("autolevel.target_lu")},
                 {"unreachable target (+20 LU): stops at the +12 dB trim", 12.0, s.bar("autolevel.clamp_lu")}};
    for (const auto& c : cases) {
        const double target = in_lufs + (c.want_gain < 12.0 ? 8.0 : 20.0);
        auto p = s.make();
        measure::apply(*p, {{"auto_level", 1.0f}, {"target_lufs", static_cast<float>(target)}, {"ceiling", -0.5f}});
        p->prepare(fs, 128, 2);
        const Chans y = render_aligned(*p, x);
        const double out = lufs_of(y, fs, static_cast<std::size_t>((secs - 6.0) * fs));
        const double want = in_lufs + c.want_gain;
        s.check_le(std::string("auto-level, ") + c.what + ": |output - expected| over the last 6 s", std::fabs(out - want),
                   c.bar_lu, "LU", fmt("input %.2f LUFS, target %.2f, output %.2f", in_lufs, target, out));
    }
    // Off: no trim, whatever the target.
    Chans head = x;
    for (auto& c : head) c.resize(static_cast<std::size_t>(10.0 * fs));
    auto p = s.make();
    measure::apply(*p, {{"auto_level", 0.0f}, {"target_lufs", -10.0f}});
    p->prepare(fs, 128, 2);
    const Chans y = render_aligned(*p, head);
    const double d = std::fabs(lufs_of(y, fs, 0) - lufs_of(head, fs, 0));
    s.check_le("auto-level off: output loudness against input", d, s.bar("autolevel.off_lu"), "LU");
}

Config worst_case_config() {
    return {{"low_gain", 3.0f}, {"lowmid_gain", -3.0f}, {"highmid_gain", 3.0f}, {"high_gain", 3.0f},
            {"comp_ratio", 4.0f}, {"comp_threshold", -24.0f}, {"sat_drive", 18.0f}, {"sat_mix", 100.0f},
            {"width", 130.0f}, {"gain", 6.0f}, {"auto_level", 1.0f}};
}

}  // namespace

// The golden configuration, shared with the host tests.
measure::Config mastering_golden_config() {
    return {{"low_gain", 2.0f}, {"lowmid_freq", 250.0f}, {"lowmid_gain", -2.0f}, {"highmid_gain", 1.5f},
            {"high_gain", 1.5f}, {"comp_threshold", -20.0f}, {"comp_ratio", 2.0f}, {"sat_drive", 9.0f},
            {"sat_mix", 25.0f}, {"width", 110.0f}, {"gain", 2.0f}, {"ceiling", -1.0f}};
}

int mastering(const measure::Args& args) {
    Suite s("mastering", [] { return make_mastering(); }, args);
    neutral(s);
    wiring(s);
    width_stage(s);
    ceiling(s);
    saturation(s);
    auto_level(s);
    const Config golden = mastering_golden_config();
    measure::check_latency(s, {{"default", {}},
                               {"auto-level", {{"auto_level", 1.0f}}},
                               {"saturation, compressor", {{"sat_mix", 50.0f}, {"comp_ratio", 3.0f}}},
                               {"golden", golden}});
    measure::check_nonfinite(s, golden);
    measure::check_reset(s, golden);
    measure::check_denormals(s, golden);
    measure::check_zipper(s, {{"comp_ratio", 3.0f}, {"sat_mix", 30.0f}}, "");
    measure::check_golden(s, golden);
    if (args.cpu) {
        const double fs = 48000.0;
        measure::check_cpu(s, golden, "golden settings");
        measure::check_cpu_on(s, worst_case_config(), measure::cpu_programme(fs, args.cpu_seconds, 12.0),
                              "every stage in use, auto-level on, programme +12 dB");
    }
    return s.finish();
}

}  // namespace sonora::native::suites
