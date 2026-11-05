// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mastering suggestion pass (ADR-0046): the analysis against synthetic
// mixes of known shape, the rules against hand-made analyses (exact
// numbers), the plan against a real History (preview, accept, one undo),
// and the offline run's cancellation.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sonora/analyze/mastering_plan.h"
#include "sonora/analyze/mastering_rules.h"
#include "sonora/analyze/mix_analysis.h"
#include "sonora/dsp/fft.h"
#include "sonora/dsp/math.h"
#include "sonora/model/history.h"
#include "sonora/model/plan.h"

using namespace sonora;
using namespace sonora::analyze;
namespace mastering = sonora::native::mastering;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)
#define CHECK_NEAR(a, b, tol)                                                                              \
    do {                                                                                                   \
        const double va = (a), vb = (b);                                                                   \
        if (!(std::fabs(va - vb) <= (tol))) {                                                              \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %.4f, %s = %.4f\n", __FILE__, __LINE__, #a, va, #b, vb); \
            ++g_failures;                                                                                  \
        }                                                                                                  \
    } while (0)

using Planar = std::vector<std::vector<float>>;

// --- synthetic mixes ----------------------------------------------------------

std::uint32_t xorshift(std::uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// Periodic noise whose octave-band powers are `level_db[b]` (the bands of
// mix_analysis.h, summed over the bins in them), by random-phase synthesis
// in the frequency domain: 131072 samples, tiled to `seconds`. RMS 1 before
// the caller's scaling.
std::vector<float> shaped_noise(const double (&level_db)[kBands], double rate, double seconds, std::uint32_t seed) {
    constexpr std::uint32_t n = 131072;
    dsp::RealFft fft;
    fft.prepare(n);
    std::vector<std::complex<double>> X(n / 2 + 1);
    std::vector<double> x(n);
    std::uint32_t s = seed * 2654435761u + 1u;
    for (int b = 0; b < kBands; ++b) {
        std::vector<std::uint32_t> bins;
        for (std::uint32_t k = 1; k < n / 2; ++k) {
            const double f = k * rate / n;
            if (f >= band_lower_hz(b) && f < band_upper_hz(b)) bins.push_back(k);
        }
        if (bins.empty()) continue;
        const double amp = std::sqrt(std::pow(10.0, level_db[b] / 10.0) / static_cast<double>(bins.size()));
        for (const std::uint32_t k : bins) {
            const double ph = dsp::kTwoPi * (xorshift(s) / 4294967296.0);
            X[k] = std::polar(amp, ph);
        }
    }
    fft.inverse(X.data(), x.data());
    double ms = 0.0;
    for (double v : x) ms += v * v;
    const double scale = 1.0 / std::sqrt(ms / n);
    std::vector<float> out(static_cast<std::size_t>(seconds * rate));
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<float>(x[i % n] * scale);
    return out;
}

struct MixSpec {
    double dev_db[kBands] = {};  // added to the target curve
    double rms_dbfs = -20.0;
    double independent_db = -15.0;  // the independent part of each channel, against the common part
    double clip_at_rms = 0.0;       // >0: clip at this many times the RMS (an over-compressed mix)
};

// Stereo: L = common + a * left noise, R = common + a * right noise.
Planar make_mix(const MixSpec& m, double rate = 48000.0, double seconds = 12.0) {
    double lvl[kBands];
    for (int b = 0; b < kBands; ++b) lvl[b] = kTargetDb[b] + m.dev_db[b];
    const auto common = shaped_noise(lvl, rate, seconds, 1);
    const auto nl = shaped_noise(lvl, rate, seconds, 2);
    const auto nr = shaped_noise(lvl, rate, seconds, 3);
    const double a = std::pow(10.0, m.independent_db / 20.0);
    Planar x(2, std::vector<float>(common.size()));
    for (std::size_t i = 0; i < common.size(); ++i) {
        x[0][i] = static_cast<float>(common[i] + a * nl[i]);
        x[1][i] = static_cast<float>(common[i] + a * nr[i]);
    }
    double ms = 0.0;
    for (const auto& c : x) for (float v : c) ms += static_cast<double>(v) * v;
    ms /= 2.0 * static_cast<double>(x[0].size());
    const double g = std::pow(10.0, m.rms_dbfs / 20.0) / std::sqrt(ms);
    const double clip = m.clip_at_rms > 0.0 ? m.clip_at_rms * std::pow(10.0, m.rms_dbfs / 20.0) : 1e9;
    for (auto& c : x) {
        for (float& v : c) v = static_cast<float>(std::fmax(-clip, std::fmin(clip, v * g)));
    }
    return x;
}

MixAnalysis analyse(const Planar& x, double rate = 48000.0) {
    MemorySource src(x, rate);
    MixAnalysis a;
    std::string err;
    const bool ok = analyse_mix(src, RunOptions{}, a, err);
    if (!ok) std::fprintf(stderr, "analyse_mix: %s\n", err.c_str());
    CHECK(ok);
    return a;
}

MasteringSuggestion suggest(const MixAnalysis& a, const MasteringConfig& c = {}) {
    MasteringSuggestion s;
    std::string err;
    const bool ok = suggest_mastering(a, c, s, err);
    if (!ok) std::fprintf(stderr, "suggest_mastering: %s\n", err.c_str());
    CHECK(ok);
    return s;
}

const Finding* find_rule(const MasteringSuggestion& s, const std::string& rule) {
    for (const Finding& f : s.findings) if (f.rule == rule) return &f;
    return nullptr;
}

// An analysis whose bands sit on the target curve, healthy stereo, PLR 12 dB
// at the streaming target: the rules propose nothing for it.
MixAnalysis neutral_analysis() {
    MixAnalysis a;
    a.sample_rate = 48000.0;
    a.frames = 48000 * 30;
    a.channels = 2;
    a.duration_s = 30.0;
    a.integrated_lufs = -14.0;
    a.max_short_term_lufs = -12.0;
    a.loudness_range_lu = 6.0;
    a.true_peak_dbtp = -2.0;
    a.sample_peak_dbfs = -2.0;
    a.rms_dbfs = -17.0;
    a.crest_db = 15.0;
    a.plr_db = 12.0;
    a.correlation = 0.9;
    a.side_mid_db = -12.0;
    for (int b = 0; b < kBands; ++b) {
        a.band_db[b] = -40.0 + kTargetDb[b];
        a.band_valid[b] = true;
    }
    a.spectrum_frames = 100;
    return a;
}

float setting(const MasteringSuggestion& s, std::uint32_t index) { return s.values[index]; }

// --- the analysis recovers what the mix was made of --------------------------------

void analysis_recovers_shape() {
    MixSpec m;
    m.dev_db[1] = 4.0;
    m.dev_db[8] = -6.0;
    m.rms_dbfs = -20.0;
    const Planar x = make_mix(m);
    const MixAnalysis a = analyse(x);
    double anchor = 0.0, anchor_t = 0.0;
    for (int b = 4; b <= 6; ++b) {
        anchor += a.band_db[b] / 3.0;
        anchor_t += kTargetDb[b] / 3.0;
    }
    for (int b = 0; b < kBands; ++b) {
        const double dev = (a.band_db[b] - anchor) - (kTargetDb[b] - anchor_t);
        CHECK_NEAR(dev, m.dev_db[b], 1.5);
    }
    CHECK_NEAR(a.rms_dbfs, -20.0, 0.1);
    CHECK(a.correlation > 0.9 && a.correlation < 1.0);
    CHECK(a.side_mid_db < -10.0 && a.side_mid_db > -20.0);
    CHECK(a.crest_db > 8.0);
    CHECK(a.channels == 2 && a.spectrum_frames > 20);
    // Loudness is that of a 20 dB-under noise, within the K-weighting's tilt.
    CHECK(a.integrated_lufs < -14.0 && a.integrated_lufs > -26.0);
}

// --- the four synthetic mixes of the brief ------------------------------------------

void mix_too_dull() {
    MixSpec m;
    m.dev_db[8] = -10.0;
    m.dev_db[9] = -10.0;
    const MasteringSuggestion s = suggest(analyse(make_mix(m)));
    const Finding* f = find_rule(s, "balance.high");
    CHECK(f != nullptr);
    CHECK(setting(s, mastering::kHighGain) > 1.5f);
    CHECK(setting(s, mastering::kLowGain) == 0.0f);
    CHECK(setting(s, mastering::kLowMidGain) == 0.0f);
    if (f != nullptr) {
        CHECK(f->title.find("High shelf +") == 0);
        CHECK(f->reason.find("8 and 16 kHz") != std::string::npos);
        CHECK(f->reason.find("below the target curve") != std::string::npos);
    }
}

void mix_too_boomy() {
    MixSpec m;
    m.dev_db[1] = 10.0;
    m.dev_db[2] = 10.0;
    m.dev_db[3] = 10.0;
    const MasteringSuggestion s = suggest(analyse(make_mix(m)));
    CHECK(find_rule(s, "balance.low") != nullptr);
    CHECK(find_rule(s, "balance.lowmid") != nullptr);
    CHECK(setting(s, mastering::kLowGain) < -2.0f);
    CHECK(setting(s, mastering::kLowMidGain) < -2.0f);
    CHECK(setting(s, mastering::kLowMidFreq) == 250.0f);
    CHECK(setting(s, mastering::kHighGain) == 0.0f);
}

void mix_over_compressed() {
    MixSpec m;
    m.clip_at_rms = 1.4;
    m.rms_dbfs = -12.0;
    const MixAnalysis a = analyse(make_mix(m));
    CHECK(a.plr_db < 8.0);
    const MasteringSuggestion s = suggest(a);
    CHECK(find_rule(s, "dynamics.dense") != nullptr);
    CHECK(find_rule(s, "dynamics.glue") == nullptr);
    CHECK(setting(s, mastering::kCompRatio) == 1.0f);
    CHECK(setting(s, mastering::kSatMix) == 0.0f);
}

void mix_too_quiet() {
    MixSpec m;
    m.rms_dbfs = -36.0;
    const MixAnalysis a = analyse(make_mix(m));
    const MasteringSuggestion s = suggest(a);
    const Finding* f = find_rule(s, "loudness.gain");
    CHECK(f != nullptr);
    const double need = -14.0 - a.integrated_lufs;
    CHECK(need > 12.0);  // beyond the console's range: the gain stops at +12
    CHECK_NEAR(setting(s, mastering::kGain), 12.0, 1e-6);
    if (f != nullptr) CHECK(f->reason.find("Capped") != std::string::npos || f->title.find("+12.0") != std::string::npos);
    CHECK(s.fader_trim_db == 0.0);
}

// --- the rules, on exact numbers ---------------------------------------------------

void rules_neutral_proposes_nothing() {
    const MasteringSuggestion s = suggest(neutral_analysis());
    // Only the loudness is within half a LU: no finding at all.
    CHECK(s.findings.empty());
    for (std::uint32_t i = mastering::kLowGain; i <= mastering::kHighGain; i += 1) {
        if (i == mastering::kLowMidFreq || i == mastering::kHighMidFreq) continue;
        CHECK(setting(s, i) == 0.0f);
    }
    CHECK(setting(s, mastering::kCeiling) == -1.0f);
    CHECK(setting(s, mastering::kTargetLufs) == -14.0f);
    CHECK(setting(s, mastering::kAutoLevel) == 0.0f);
    CHECK(!s.observations.empty());
}

void rules_balance_numbers() {
    MixAnalysis a = neutral_analysis();
    a.band_db[8] -= 8.0;
    a.band_db[9] -= 8.0;  // 8 dB under: (8 - 3) * 0.5 = +2.5 dB
    a.band_db[1] += 9.0;
    a.band_db[2] += 9.0;  // 9 dB over: -(9 - 3) * 0.5 = -3.0 dB
    a.band_db[7] += 20.0;  // far over: -(20 - 3) * 0.5 = -8.5, held to 6 dB
    const MasteringSuggestion s = suggest(a);
    CHECK_NEAR(setting(s, mastering::kHighGain), 2.5, 1e-6);
    CHECK_NEAR(setting(s, mastering::kLowGain), -3.0, 1e-6);
    CHECK_NEAR(setting(s, mastering::kHighMidGain), -6.0, 1e-6);
    CHECK(setting(s, mastering::kHighMidFreq) == 4000.0f);
    CHECK(find_rule(s, "balance.lowmid") == nullptr);
}

void rules_deterministic() {
    MixAnalysis a = neutral_analysis();
    a.band_db[8] -= 7.0;
    a.plr_db = 16.0;
    a.true_peak_dbtp = a.integrated_lufs + 16.0;
    const MasteringSuggestion s1 = suggest(a), s2 = suggest(a);
    CHECK(s1.values == s2.values);
    CHECK(s1.findings.size() == s2.findings.size());
    for (std::size_t i = 0; i < s1.findings.size(); ++i) {
        CHECK(s1.findings[i].title == s2.findings[i].title && s1.findings[i].reason == s2.findings[i].reason);
    }
    // Every finding is explained: a title, a reason with a number in it.
    for (const Finding& f : s1.findings) {
        CHECK(!f.rule.empty() && !f.title.empty());
        CHECK(f.reason.find_first_of("0123456789") != std::string::npos);
    }
}

void rules_dynamics() {
    MixAnalysis dense = neutral_analysis();
    dense.plr_db = 6.0;
    dense.true_peak_dbtp = dense.integrated_lufs + 6.0;  // -8 dBTP
    const MasteringSuggestion sd = suggest(dense);
    CHECK(find_rule(sd, "dynamics.dense") != nullptr);
    CHECK(setting(sd, mastering::kCompRatio) == 1.0f);

    MixAnalysis dyn = neutral_analysis();
    dyn.plr_db = 15.0;
    dyn.true_peak_dbtp = dyn.integrated_lufs + 15.0;
    const MasteringSuggestion s15 = suggest(dyn);
    CHECK(find_rule(s15, "dynamics.glue") != nullptr);
    CHECK_NEAR(setting(s15, mastering::kCompRatio), 1.5, 1e-6);
    CHECK_NEAR(setting(s15, mastering::kCompThreshold), -10.0, 1e-6);  // -14 + 4
    CHECK(find_rule(s15, "dynamics.saturation") == nullptr);

    dyn.plr_db = 19.0;
    dyn.true_peak_dbtp = dyn.integrated_lufs + 19.0;
    const MasteringSuggestion s19 = suggest(dyn);
    CHECK_NEAR(setting(s19, mastering::kCompRatio), 2.0, 1e-6);
    CHECK_NEAR(setting(s19, mastering::kSatMix), 15.0, 1e-6);
    CHECK_NEAR(setting(s19, mastering::kSatDrive), 6.0, 1e-6);
}

void rules_stereo() {
    MixAnalysis a = neutral_analysis();
    a.correlation = -0.2;
    CHECK_NEAR(setting(suggest(a), mastering::kWidth), 80.0, 1e-6);
    a.correlation = 0.5;
    a.side_mid_db = -1.0;
    CHECK_NEAR(setting(suggest(a), mastering::kWidth), 90.0, 1e-6);
    a.side_mid_db = -40.0;
    const MasteringSuggestion s = suggest(a);
    CHECK_NEAR(setting(s, mastering::kWidth), 100.0, 1e-6);
    bool said = false;
    for (const auto& o : s.observations) said = said || o.find("near-mono") != std::string::npos;
    CHECK(said);
    a.channels = 1;
    CHECK_NEAR(setting(suggest(a), mastering::kWidth), 100.0, 1e-6);
}

void rules_loudness() {
    MasteringConfig c;
    // Quiet, lots of headroom: the full gap, within +-12.
    MixAnalysis q = neutral_analysis();
    q.integrated_lufs = -20.0;
    q.true_peak_dbtp = -8.0;
    q.plr_db = 12.0;
    CHECK_NEAR(setting(suggest(q, c), mastering::kGain), 6.0, 1e-6);

    // Quiet and peaky: held to headroom + 5 dB of limiting.
    MixAnalysis p = neutral_analysis();
    p.integrated_lufs = -26.0;
    p.true_peak_dbtp = -3.0;
    p.plr_db = 23.0;
    const MasteringSuggestion sp = suggest(p, c);
    CHECK_NEAR(setting(sp, mastering::kGain), 7.0, 1e-6);  // 2 dB headroom + 5 dB
    const Finding* f = find_rule(sp, "loudness.gain");
    CHECK(f != nullptr && f->reason.find("Capped") != std::string::npos);
    CHECK_NEAR(sp.aimed_lufs, -19.0, 1e-6);

    // Dense: the limiter's share is 2 dB, not 5.
    MixAnalysis d = neutral_analysis();
    d.integrated_lufs = -20.0;
    d.true_peak_dbtp = -14.0;
    d.plr_db = 6.0;
    CHECK_NEAR(setting(suggest(d, c), mastering::kGain), 6.0, 1e-6);  // headroom 13 dB: the whole gap fits
    d.true_peak_dbtp = -1.0;
    d.integrated_lufs = -7.0;
    d.plr_db = 6.0;
    // Needs -7 dB (louder than target): turned down.
    CHECK_NEAR(setting(suggest(d, c), mastering::kGain), -7.0, 1e-6);

    // Too loud for the console's -12 dB: the fader takes the rest.
    MixAnalysis l = neutral_analysis();
    l.integrated_lufs = -0.5;
    l.true_peak_dbtp = 5.0;
    l.plr_db = 5.5;
    const MasteringSuggestion sl = suggest(l, c);
    CHECK_NEAR(setting(sl, mastering::kGain), -12.0, 1e-6);
    CHECK_NEAR(sl.fader_trim_db, -1.5, 1e-6);
    CHECK(!sl.fader_reason.empty());

    // Within half a LU: no gain at all.
    MixAnalysis n = neutral_analysis();
    n.integrated_lufs = -14.3;
    CHECK(find_rule(suggest(n, c), "loudness.gain") == nullptr);

    // Other targets and ceilings carry through.
    MasteringConfig r128;
    r128.target_lufs = -23.0;
    r128.ceiling_dbtp = -2.0;
    const MasteringSuggestion sr = suggest(neutral_analysis(), r128);
    CHECK_NEAR(setting(sr, mastering::kGain), -9.0, 1e-6);
    CHECK(setting(sr, mastering::kCeiling) == -2.0f);
    CHECK(setting(sr, mastering::kTargetLufs) == -23.0f);

    // A silent or invalid analysis is refused, not guessed at.
    MixAnalysis silent = neutral_analysis();
    silent.integrated_lufs = -200.0;
    MasteringSuggestion out;
    std::string err;
    CHECK(!suggest_mastering(silent, c, out, err) && !err.empty());
    MasteringConfig bad;
    bad.target_lufs = 3.0;
    CHECK(!suggest_mastering(neutral_analysis(), bad, out, err));
}

// --- the offline run ----------------------------------------------------------------

void run_errors_and_cancel() {
    MixAnalysis a;
    std::string err;
    {
        Planar empty;
        MemorySource src(empty, 48000.0);
        CHECK(!analyse_mix(src, RunOptions{}, a, err));
    }
    {
        Planar tiny(2, std::vector<float>(1000, 0.1f));
        MemorySource src(tiny, 48000.0);
        CHECK(!analyse_mix(src, RunOptions{}, a, err) && err.find("too short") != std::string::npos);
    }
    {
        Planar silent(2, std::vector<float>(48000 * 2, 0.0f));
        MemorySource src(silent, 48000.0);
        CHECK(!analyse_mix(src, RunOptions{}, a, err) && err.find("silent") != std::string::npos);
    }
    const Planar x = make_mix(MixSpec{}, 48000.0, 10.0);
    {
        // Cancelled before it starts, and in the middle.
        std::atomic<bool> cancel{true};
        MemorySource src(x, 48000.0);
        RunOptions o;
        o.cancel = &cancel;
        CHECK(!analyse_mix(src, o, a, err) && err == "cancelled");
    }
    {
        std::atomic<bool> cancel{false};
        MemorySource src(x, 48000.0);
        RunOptions o;
        o.cancel = &cancel;
        o.block_frames = 4096;
        double last = 0.0;
        bool monotone = true;
        o.progress = [&](double f) {
            monotone = monotone && f >= last;
            last = f;
            if (f > 0.5) cancel.store(true);
        };
        CHECK(!analyse_mix(src, o, a, err) && err == "cancelled");
        CHECK(monotone && last > 0.5 && last < 1.0);
    }
    {
        // Progress reaches 1 on a complete run; the block size does not change the answer.
        MemorySource s1(x, 48000.0), s2(x, 48000.0);
        RunOptions o1, o2;
        o1.block_frames = 4096;
        o2.block_frames = 30000;
        double last = 0.0;
        o1.progress = [&](double f) { last = f; };
        MixAnalysis a1, a2;
        CHECK(analyse_mix(s1, o1, a1, err) && analyse_mix(s2, o2, a2, err));
        CHECK(last == 1.0);
        CHECK_NEAR(a1.integrated_lufs, a2.integrated_lufs, 1e-9);
        for (int b = 0; b < kBands; ++b) CHECK_NEAR(a1.band_db[b], a2.band_db[b], 1e-9);
    }
}

// --- the plan on a real History -----------------------------------------------------

model::Snapshot session_with_strip(model::History& h, model::Id& strip, std::uint16_t channels = 2) {
    using namespace model;
    AddTrack t;
    t.track = h.current().next_id;
    t.playlist = t.track + 1;
    t.name = "Mix";
    t.playlist_name = "Mix.01";
    t.channels = channels;
    for (std::uint16_t c = 0; c < channels; ++c) t.mix.input.push_back(c);
    std::string err;
    CHECK(h.apply(Operation{t}, err));
    strip = t.track;
    return h.current();
}

void plan_flow() {
    using namespace model;
    SessionSettings settings;
    settings.name = "Plan";
    History h{new_session(settings)};
    Id strip = 0;
    session_with_strip(h, strip);

    MixSpec spec;
    spec.dev_db[8] = -10.0;
    spec.dev_db[9] = -10.0;
    const MixAnalysis a = analyse(make_mix(spec));
    MasteringPlanConfig cfg;
    cfg.track = strip;
    MasteringPlanner planner(a, cfg);
    Plan plan;
    std::string err;
    CHECK(planner.propose(h.current(), plan, err));
    CHECK(plan_problem(plan).empty());
    CHECK(plan.label == "Mastering Plan");
    CHECK(plan.base_revision == h.current().revision);
    CHECK(!plan.steps.empty());
    const auto* add = std::get_if<AddInsert>(&plan.steps[0].op);
    CHECK(add != nullptr);
    if (add != nullptr) {
        CHECK(add->track == strip && add->slot == 0);
        CHECK(add->plugin.id == "urn:sonora:native:mastering");
        CHECK(add->insert >= h.current().next_id);
        bool high = false;
        for (const ParamValue& p : add->params) high = high || (p.key == "high_gain" && p.value > 1.5f);
        CHECK(high);
    }
    CHECK(plan.steps[0].title.find("Mix") != std::string::npos);
    CHECK(plan.steps[0].reason.find("High shelf") != std::string::npos);
    CHECK(!plan.notes.empty());  // what was measured and left alone

    PlanPreview prev;
    CHECK(preview(h.current(), plan, Selection::all(plan), prev, err));
    CHECK(!prev.lines.empty());
    bool added = false;
    for (const DiffLine& l : prev.lines) added = added || l.kind == DiffKind::Added;
    CHECK(added);

    // Rejecting the step leaves the session alone.
    PlanReceipt none;
    CHECK(accept_plan(h, plan, Selection::none(), none, err));
    CHECK(h.current().edit->tracks[0]->inserts.empty());

    PlanReceipt receipt;
    CHECK(accept_plan(h, plan, Selection::all(plan), receipt, err));
    CHECK(h.current().edit->tracks[0]->inserts.size() == 1);
    CHECK(undo_plan(h, receipt, err));
    CHECK(h.current().edit->tracks[0]->inserts.empty());
}

void plan_inserts_and_fader() {
    using namespace model;
    SessionSettings settings;
    settings.name = "Plan2";
    History h{new_session(settings)};
    Id strip = 0;
    session_with_strip(h, strip);
    std::string err;
    AddInsert other;
    other.track = strip;
    other.insert = h.current().next_id;
    other.slot = 0;
    other.plugin = PluginRef{PluginFormat::Lv2, "http://example.org/some-eq", "Some EQ", "1"};
    CHECK(h.apply(Operation{other}, err));

    MixAnalysis loud = neutral_analysis();
    loud.integrated_lufs = -0.5;
    loud.true_peak_dbtp = 5.0;
    loud.plr_db = 5.5;
    MasteringPlanConfig cfg;
    cfg.track = strip;
    MasteringPlanner planner(loud, cfg);
    Plan plan;
    CHECK(planner.propose(h.current(), plan, err));
    const auto* add = std::get_if<AddInsert>(&plan.steps[0].op);
    CHECK(add != nullptr && add->slot == 1);  // after the other insert
    bool noted = false;
    for (const PlanNote& n : plan.notes) noted = noted || (n.text.find("third-party") != std::string::npos && n.subject.find("Some EQ") != std::string::npos);
    CHECK(noted);
    CHECK(plan.steps.size() == 2);
    const auto* mix = std::get_if<SetMix>(&plan.steps[1].op);
    CHECK(mix != nullptr);
    if (mix != nullptr) CHECK_NEAR(mix->mix.gain_db, -1.5, 1e-5);
    CHECK(plan.steps[1].title.find("fader") != std::string::npos);

    // Accept it all, then one undo takes the insert and the fader back.
    PlanReceipt r;
    CHECK(accept_plan(h, plan, Selection::all(plan), r, err));
    CHECK(h.current().edit->tracks[0]->inserts.size() == 2);
    CHECK_NEAR(h.current().mix.find(strip)->gain_db, -1.5, 1e-5);
    CHECK(undo_plan(h, r, err));
    CHECK(h.current().edit->tracks[0]->inserts.size() == 1);
    CHECK_NEAR(h.current().mix.find(strip)->gain_db, 0.0, 1e-6);

    // Once the console is there, the next plan replaces it in place.
    CHECK(planner.propose(h.current(), plan, err));
    CHECK(accept_plan(h, plan, Selection::all(plan), r, err));
    MasteringPlanner again(neutral_analysis(), cfg);
    Plan p2;
    CHECK(again.propose(h.current(), p2, err));
    CHECK(std::get_if<ReplaceInsert>(&p2.steps[0].op) != nullptr);
    CHECK(p2.steps[0].title.find("Update") == 0);

    // A stale plan is refused.
    PlanReceipt stale;
    CHECK(!accept_plan(h, plan, Selection::all(plan), stale, err));
}

void plan_refusals() {
    using namespace model;
    SessionSettings settings;
    settings.name = "Plan3";
    History h{new_session(settings)};
    Id strip = 0;
    session_with_strip(h, strip);
    std::string err;
    Plan plan;
    MasteringPlanConfig cfg;
    cfg.track = strip + 100;
    MasteringPlanner missing(neutral_analysis(), cfg);
    CHECK(!missing.propose(h.current(), plan, err) && !err.empty());
    cfg.track = strip;
    MixAnalysis silent = neutral_analysis();
    silent.integrated_lufs = -200.0;
    MasteringPlanner s(silent, cfg);
    CHECK(!s.propose(h.current(), plan, err) && !err.empty());

}

}  // namespace

int main() {
    analysis_recovers_shape();
    mix_too_dull();
    mix_too_boomy();
    mix_over_compressed();
    mix_too_quiet();
    rules_neutral_proposes_nothing();
    rules_balance_numbers();
    rules_deterministic();
    rules_dynamics();
    rules_stereo();
    rules_loudness();
    run_errors_and_cancel();
    plan_flow();
    plan_inserts_and_fader();
    plan_refusals();
    if (g_failures == 0) std::puts("test_analyze_mastering: ok");
    return g_failures == 0 ? 0 : 1;
}
