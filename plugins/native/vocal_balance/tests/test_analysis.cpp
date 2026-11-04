// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vocal Balance's analysis and plan against synthetic vocals whose answer is
// known: a harmonic voice (gliding pitch, vibrato, noise floor, gaps) with
// stationary resonances added at stated frequencies, and fricative bursts.
// Then the processor, set from the analysis, on the same audio: the bands
// found must cut the resonances and leave the voice alone.
#include <cmath>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

#include "model_test.h"
#include "sonora/dsp/biquad.h"
#include "sonora/measure/measure.h"
#include "sonora/model/plan.h"
#include "sonora/native/vocal_balance.h"
#include "sonora/native/vocal_balance_analysis.h"

using namespace model_test;
using sonora::native::VocalAnalysis;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;

struct Lcg {
    std::uint32_t s;
    std::uint32_t next() { return s = s * 1664525u + 1013904223u; }
    double uniform() { return (next() >> 8) * (1.0 / 8388608.0) - 1.0; }  // [-1, 1)
};

struct Res {
    double freq, dbfs;
};

// 12 s: notes of 0.4-0.7 s at a different pitch each, 0.15 s apart.
std::vector<float> make_voice(const std::vector<Res>& resonances, bool sibilants, std::uint32_t seed = 1) {
    const std::size_t n = static_cast<std::size_t>(12.0 * kFs);
    std::vector<double> y(n, 0.0), env(n, 0.0);
    Lcg rng{seed};
    // Notes.
    std::size_t pos = static_cast<std::size_t>(0.1 * kFs);
    std::vector<std::pair<std::size_t, std::size_t>> gaps;
    while (pos + static_cast<std::size_t>(0.8 * kFs) < n) {
        const std::size_t len = static_cast<std::size_t>((0.4 + 0.3 * (0.5 + 0.5 * rng.uniform())) * kFs);
        const double f0 = 115.0 + 140.0 * (0.5 + 0.5 * rng.uniform());
        const double vib_ph = 2.0 * kPi * (0.5 + 0.5 * rng.uniform());
        std::vector<double> phase(160, 0.0);
        for (std::size_t i = 0; i < len; ++i) {
            const double t = static_cast<double>(i) / kFs;
            const double f = f0 * (1.0 + 0.03 * std::sin(2.0 * kPi * 5.5 * t + vib_ph) + 0.05 * (t / 0.5 - 1.0));
            double a = 1.0;
            if (i < 960) a = static_cast<double>(i) / 960.0;
            if (len - i < 1920) a = static_cast<double>(len - i) / 1920.0;
            env[pos + i] = a;
            double s = 0.0;
            for (int h = 1; h * f < 14000.0 && h < 160; ++h) {
                phase[h] += 2.0 * kPi * h * f / kFs;
                const double hf = h * f / 7000.0;  // a smooth roll-off, as a real voice has
                s += std::sin(phase[h]) * 0.12 / std::pow(static_cast<double>(h), 1.2) / (1.0 + hf * hf * hf * hf);
            }
            y[pos + i] = s * a;
        }
        pos += len;
        gaps.push_back({pos, pos + static_cast<std::size_t>(0.15 * kFs)});
        pos += static_cast<std::size_t>(0.15 * kFs);
    }
    // Resonances ride on the voice's envelope, as a room or a throat would.
    for (const Res& r : resonances) {
        const double amp = std::pow(10.0, r.dbfs / 20.0);
        for (std::size_t i = 0; i < n; ++i) y[i] += amp * env[i] * std::sin(2.0 * kPi * r.freq * static_cast<double>(i) / kFs);
    }
    // Fricatives in every second gap: 0.12 s of noise through a band-pass at 7 kHz.
    if (sibilants) {
        const auto bp = sonora::dsp::design_matched({sonora::dsp::Shape::BandPass, 7000.0, 1.2, 0.0}, kFs);
        sonora::dsp::BiquadState st;
        for (std::size_t g = 0; g < gaps.size(); g += 2) {
            const std::size_t from = gaps[g].first + static_cast<std::size_t>(0.015 * kFs);
            for (std::size_t i = 0; i < static_cast<std::size_t>(0.12 * kFs) && from + i < n; ++i) {
                y[from + i] += 0.35 * st.tick(bp, rng.uniform());
            }
        }
    }
    // A floor of noise, -70 dBFS peak.
    for (std::size_t i = 0; i < n; ++i) y[i] += 3e-4 * rng.uniform();
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(y[i]);
    return out;
}

double tone_amp(const std::vector<float>& y, double f, std::size_t from, std::size_t n) {
    double s = 0.0, c = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * f * static_cast<double>(from + i) / kFs;
        s += y[from + i] * std::sin(ph);
        c += y[from + i] * std::cos(ph);
    }
    return 2.0 * std::hypot(s, c) / static_cast<double>(n);
}

double rms_db(const std::vector<float>& y) {
    double s = 0.0;
    for (float v : y) s += static_cast<double>(v) * v;
    return 10.0 * std::log10(s / static_cast<double>(y.size()) + 1e-20);
}

// The processor, set the way the plan sets it, on mono audio (both channels).
std::vector<float> process(const std::vector<float>& x, const VocalAnalysis& a) {
    auto p = sonora::native::make_vocal_balance();
    sonora::measure::Config cfg;
    const auto params = sonora::native::vocal_balance_params(a);
    for (const auto& [k, v] : params) cfg.push_back({k.c_str(), v});
    sonora::measure::apply(*p, cfg);
    p->prepare(kFs, 128, 2);
    const sonora::measure::Chans in = {x, x};
    return sonora::measure::render(*p, in)[0];
}

const sonora::model::AddInsert* find_insert(const sonora::model::Plan& plan, const char* name) {
    for (const auto& st : plan.steps) {
        if (const auto* ai = std::get_if<sonora::model::AddInsert>(&st.op)) {
            if (ai->plugin.name == name) return ai;
        }
    }
    return nullptr;
}

float param_of(const sonora::model::AddInsert& ai, const char* key) {
    for (const auto& p : ai.params) {
        if (p.key == key) return p.value;
    }
    return -1.0f;
}

double band_rms(const std::vector<float>& x, double centre_hz) {
    const auto b = sonora::measure::octave_band(x, kFs, centre_hz);
    double s = 0.0;
    for (std::size_t i = static_cast<std::size_t>(kFs); i < b.size(); ++i) s += b[i] * b[i];
    return std::sqrt(s / static_cast<double>(b.size()));
}

bool has_resonance_near(const VocalAnalysis& a, double f, double tol, double min_excess) {
    for (const auto& r : a.resonances) {
        if (std::fabs(r.freq_hz - f) <= tol && r.excess_db >= min_excess) return true;
    }
    return false;
}

int test_resonances_found() {
    const std::vector<float> x = make_voice({{3712.0, -30.0}, {6130.0, -34.0}}, false);
    VocalAnalysis a;
    std::string err;
    CHECK(sonora::native::analyse_vocal(x.data(), x.size(), kFs, a, err));
    for (const auto& r : a.resonances) std::printf("  resonance %.1f Hz excess %.1f dB Q %.1f cut %.1f\n", r.freq_hz, r.excess_db, r.q, r.cut_db);
    CHECK(a.resonances.size() >= 2 && a.resonances.size() <= 3);
    CHECK(has_resonance_near(a, 3712.0, 40.0, 8.0));
    CHECK(has_resonance_near(a, 6130.0, 60.0, 8.0));
    for (const auto& r : a.resonances) CHECK(r.freq_hz >= 250.0);
    CHECK(a.active_fraction > 0.5 && a.active_fraction <= 1.0);
    CHECK(a.level_db > -40.0 && a.level_db < -10.0);
    return 0;
}

int test_clean_voice_has_none() {
    const std::vector<float> x = make_voice({}, false, 5);
    VocalAnalysis a;
    std::string err;
    CHECK(sonora::native::analyse_vocal(x.data(), x.size(), kFs, a, err));
    for (const auto& r : a.resonances) std::printf("  spurious %.1f Hz excess %.1f dB\n", r.freq_hz, r.excess_db);
    CHECK(a.resonances.empty());
    CHECK(!a.sibilance.found);
    return 0;
}

int test_sibilance() {
    const std::vector<float> x = make_voice({}, true, 9);
    VocalAnalysis a;
    std::string err;
    CHECK(sonora::native::analyse_vocal(x.data(), x.size(), kFs, a, err));
    std::printf("  sibilance: found %d freq %.0f Hz excess %.1f dB fraction %.3f\n", a.sibilance.found, a.sibilance.freq_hz,
                a.sibilance.excess_db, a.sibilance.fraction);
    CHECK(a.sibilance.found);
    CHECK(a.sibilance.freq_hz > 5500.0 && a.sibilance.freq_hz < 9000.0);
    CHECK(a.sibilance.excess_db >= 6.0);
    CHECK(a.sibilance.fraction > 0.01 && a.sibilance.fraction < 0.3);
    return 0;
}

int test_processor_cuts_what_was_found() {
    const std::vector<Res> res = {{3712.0, -30.0}, {6130.0, -34.0}};
    const std::vector<float> with = make_voice(res, false);
    const std::vector<float> clean = make_voice({}, false);
    VocalAnalysis a;
    std::string err;
    CHECK(sonora::native::analyse_vocal(with.data(), with.size(), kFs, a, err));
    const std::vector<float> y = process(with, a);
    const std::size_t from = static_cast<std::size_t>(1.0 * kFs), n = static_cast<std::size_t>(10.0 * kFs);
    const double r1 = -20.0 * std::log10(tone_amp(y, 3712.0, from, n) / tone_amp(with, 3712.0, from, n));
    const double r2 = -20.0 * std::log10(tone_amp(y, 6130.0, from, n) / tone_amp(with, 6130.0, from, n));
    std::printf("  reduction: 3712 Hz %.1f dB, 6130 Hz %.1f dB\n", r1, r2);
    CHECK(r1 >= 8.0);
    CHECK(r2 >= 3.0);
    // The voice around them is left alone: the clean voice, through the same
    // bands, changes by under 1 dB in the octave around 4 kHz (the 3712 Hz band
    // and its neighbours), and under 0.5 dB overall.
    const std::vector<float> yc = process(clean, a);
    const double dc = rms_db(yc) - rms_db(clean);
    const double d4k = 20.0 * std::log10(band_rms(yc, 4000.0) / band_rms(clean, 4000.0));
    std::printf("  clean voice through the same bands: %+.2f dB overall, %+.2f dB in the 4 kHz octave\n", dc, d4k);
    CHECK(std::fabs(dc) < 0.5);
    CHECK(std::fabs(d4k) < 1.0);
    // And a clean voice's own analysis gives no band that cuts it.
    VocalAnalysis ac;
    CHECK(sonora::native::analyse_vocal(clean.data(), clean.size(), kFs, ac, err));
    const std::vector<float> yd = process(clean, ac);
    CHECK(std::fabs(rms_db(yd) - rms_db(clean)) < 0.1);
    return 0;
}

int test_analysis_refusals() {
    VocalAnalysis a;
    std::string err;
    const std::vector<float> shorty(1000, 0.1f);
    CHECK(!sonora::native::analyse_vocal(shorty.data(), shorty.size(), kFs, a, err) && !err.empty());
    const std::vector<float> quiet(static_cast<std::size_t>(3 * kFs), 1e-6f);
    CHECK(!sonora::native::analyse_vocal(quiet.data(), quiet.size(), kFs, a, err) && err.find("quiet") != std::string::npos);
    std::vector<float> bad(static_cast<std::size_t>(3 * kFs), 0.1f);
    bad[5000] = NAN;
    CHECK(!sonora::native::analyse_vocal(bad.data(), bad.size(), kFs, a, err) && err.find("non-finite") != std::string::npos);
    return 0;
}

int test_plan() {
    using namespace sonora::model;
    const std::vector<float> x = make_voice({{3712.0, -30.0}, {6130.0, -34.0}}, true, 3);
    VocalAnalysis a;
    std::string err;
    CHECK(sonora::native::analyse_vocal(x.data(), x.size(), kFs, a, err));

    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    const Snapshot base = h.current();

    // A target 6 dB under the voice, so the trim step is there and is -6 dB.
    sonora::native::VocalBalancePlanSource src(vox.track, a, a.level_db - 6.0);
    Plan plan;
    CHECK(src.propose(base, plan, err));
    CHECK(plan_problem(plan).empty());
    for (const auto& st : plan.steps) std::printf("  step: %s | %s\n", st.title.c_str(), st.reason.c_str());
    const AddInsert* vb = find_insert(plan, "Sonora Vocal Balance");
    const AddInsert* de = find_insert(plan, "Sonora De-esser");
    CHECK(vb != nullptr && de != nullptr);
    // The bands are the resonances found, in the parameters the processor reads.
    bool f3712 = false, f6130 = false;
    for (int i = 1; i <= 5; ++i) {
        const float f = param_of(*vb, ("freq" + std::to_string(i)).c_str());
        const float c = param_of(*vb, ("cut" + std::to_string(i)).c_str());
        if (std::fabs(f - 3712.0f) < 40.0f && c >= 4.0f) f3712 = true;
        if (std::fabs(f - 6130.0f) < 60.0f && c >= 4.0f) f6130 = true;
    }
    CHECK(f3712 && f6130);
    CHECK(param_of(*vb, "bands") >= 0.0f);
    CHECK(param_of(*de, "frequency") > 5500.0f && param_of(*de, "frequency") < 9000.0f);
    CHECK(param_of(*de, "threshold") < 0.0f);
    for (const auto& st : plan.steps) CHECK(!st.title.empty() && !st.reason.empty());

    // Partial acceptance: only the resonance step.
    Selection only_vb;
    only_vb.accepted.assign(plan.steps.size(), false);
    for (std::size_t i = 0; i < plan.steps.size(); ++i) {
        if (const auto* ai = std::get_if<AddInsert>(&plan.steps[i].op)) only_vb.accepted[i] = ai->plugin.name == "Sonora Vocal Balance";
    }
    PlanPreview pv;
    CHECK(preview(base, plan, only_vb, pv, err));
    CHECK(pv.result.edit->tracks[0]->inserts.size() == 1);
    CHECK(pv.result.edit->tracks[0]->inserts[0].plugin.name == "Sonora Vocal Balance");
    CHECK(pv.result.mix.find(vox.track)->gain_db == 0.0f);

    // Everything, as one compound; one undo takes it all back.
    PlanReceipt rc;
    CHECK(accept_plan(h, plan, Selection::all(plan), rc, err));
    CHECK(h.current().edit->tracks[0]->inserts.size() >= 2);
    CHECK(std::fabs(h.current().mix.find(vox.track)->gain_db + 6.0f) <= 0.5f);
    CHECK(undo_plan(h, rc, err));
    CHECK(h.current().edit->tracks[0]->inserts.empty());
    CHECK(h.current().mix.find(vox.track)->gain_db == 0.0f);

    // A stale plan, an unknown track and an analysis with nothing to say.
    // The plan was made on a revision the history has left: refused, nothing applied.
    PlanReceipt late;
    CHECK(!accept_plan(h, plan, Selection::all(plan), late, err) && !err.empty());
    CHECK(h.current().edit->tracks[0]->inserts.empty());
    Plan stale;
    sonora::native::VocalBalancePlanSource nobody(999999, a);
    CHECK(!nobody.propose(base, stale, err) && !err.empty());
    VocalAnalysis nothing;
    nothing.level_db = -20.0;
    sonora::native::VocalBalancePlanSource idle(vox.track, nothing, -20.0);
    CHECK(!idle.propose(base, stale, err) && err.find("nothing to adjust") != std::string::npos);
    return 0;
}

}  // namespace

int main() {
    if (test_resonances_found()) return 1;
    if (test_clean_voice_has_none()) return 1;
    if (test_sibilance()) return 1;
    if (test_processor_cuts_what_was_found()) return 1;
    if (test_analysis_refusals()) return 1;
    if (test_plan()) return 1;
    std::printf("vocal_balance analysis: all passed\n");
    return 0;
}
