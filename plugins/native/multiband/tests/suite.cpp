// SPDX-License-Identifier: GPL-3.0-or-later
//
// The multiband compressor against its bars: the crossovers (null sum, flat
// magnitude, levels at the crossover, stopband), each band's static curve and
// timing, look-ahead, the side-chain high-pass, auto make-up, stereo link,
// band bypass and solo, the gain-reduction meters, latency, and the common
// checks.
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "sonora/dsp/dynamics.h"
#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/multiband.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

const char* const kThr[4] = {"b1_threshold", "b2_threshold", "b3_threshold", "b4_threshold"};
const char* const kRatio[4] = {"b1_ratio", "b2_ratio", "b3_ratio", "b4_ratio"};
const char* const kAtk[4] = {"b1_attack", "b2_attack", "b3_attack", "b4_attack"};
const char* const kRel[4] = {"b1_release", "b2_release", "b3_release", "b4_release"};
const char* const kMake[4] = {"b1_makeup", "b2_makeup", "b3_makeup", "b4_makeup"};
const char* const kSolo[4] = {"b1_solo", "b2_solo", "b3_solo", "b4_solo"};
const char* const kByp[4] = {"b1_bypass", "b2_bypass", "b3_bypass", "b4_bypass"};

Config cat(Config a, const Config& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// Nothing compresses: ratio 1 and the threshold at 0 dB, in every band.
Config flat() {
    Config c;
    for (int b = 0; b < 4; ++b) {
        c.push_back({kThr[b], 0.0f});
        c.push_back({kRatio[b], 1.0f});
    }
    return c;
}

const Config kMin = {{"phase", 0.0f}};
const Config kLin = {{"phase", 1.0f}};

std::string at(double fs) { return " at " + std::to_string(static_cast<int>(fs)) + " Hz"; }

std::unique_ptr<Processor> make_prepared(Suite& s, double fs, const Config& cfg, std::uint32_t chs = 2) {
    auto p = s.make();
    measure::apply(*p, cfg);
    p->prepare(fs, 128, chs);
    return p;
}

double db(double lin) { return dsp::gain_to_db(lin, -300.0); }

double tail_rms_db(const std::vector<float>& y, double fs, double secs = 0.2) {
    const auto n = static_cast<std::size_t>(secs * fs);
    return db(measure::rms(y, y.size() - n, y.size()));
}
double tail_peak_db(const std::vector<float>& y, double fs, double secs = 0.2) {
    const auto n = static_cast<std::size_t>(secs * fs);
    return db(measure::peak(y, y.size() - n, y.size()));
}

Chans dc(double fs, const std::vector<std::pair<double, double>>& segs) {
    std::vector<Chans> parts;
    for (const auto& [secs, level] : segs) {
        Chans p = measure::silence(fs, secs);
        for (auto& c : p) {
            for (float& v : c) v = static_cast<float>(dsp::db_to_gain(level));
        }
        parts.push_back(std::move(p));
    }
    return measure::concat(parts);
}

// ---- the crossovers ---------------------------------------------------------

// The bands sum to the input. Linear phase: to the input delayed by the
// reported latency, sample for sample. Minimum phase: flat in magnitude (the
// sum is an all-pass), from 20 Hz to 20 kHz.
void null_and_flat(Suite& s) {
    const double null_bar = s.bar("multiband.null_db");
    const double flat_bar = s.bar("multiband.flat_db");
    const Config xo = {{"x1", 180.0f}, {"x2", 1300.0f}, {"x3", 7000.0f}};
    for (double fs : measure::kRates) {
        for (const float bands : {0.0f, 1.0f}) {
            const std::string nb = bands > 0.5f ? "4 bands" : "3 bands";
            Chans x = measure::silence(fs, 0.25);
            x[0][0] = 1.0f;
            x[1][0] = 1.0f;
            {
                auto p = make_prepared(s, fs, cat(cat(cat(flat(), kLin), xo), {{"bands", bands}}));
                const Chans y = measure::render(*p, x);
                const auto lat = static_cast<std::size_t>(p->latency());
                double worst = 0.0;
                for (std::size_t c = 0; c < 2; ++c) {
                    for (std::size_t i = 0; i < y[c].size(); ++i) {
                        const float want = i >= lat ? x[c][i - lat] : 0.0f;
                        worst = std::fmax(worst, std::fabs(y[c][i] - want));
                    }
                }
                s.check_le("linear phase " + nb + ": sum of bands vs input delayed by " + std::to_string(lat) +
                               " samples" + at(fs),
                           db(worst), null_bar, "dB");
            }
            {
                auto p = make_prepared(s, fs, cat(cat(cat(flat(), kMin), xo), {{"bands", bands}}));
                const Chans y = measure::render(*p, x);
                double worst = 0.0;
                for (double f = 20.0; f <= std::fmin(20000.0, 0.45 * fs); f *= 1.0905) {
                    worst = std::fmax(worst, std::fabs(measure::response_db_deg(y[0], f, fs).first));
                }
                s.check_le("minimum phase " + nb + ": magnitude of the sum, |dB| 20 Hz to 20 kHz" + at(fs), worst,
                           flat_bar, "dB");
            }
        }
    }
}

// Level of a tone through one band soloed, in dB re the tone.
double solo_gain_db(Suite& s, double fs, const Config& cfg, int band, double f) {
    auto p = make_prepared(s, fs, cat(cat(flat(), cfg), {{kSolo[band], 1.0f}}));
    const Chans y = measure::render(*p, measure::sine(fs, f, -12.0, 0.7));
    return tail_rms_db(y[0], fs) - (-12.0 - 3.0103);
}

void crossover_levels(Suite& s) {
    const double fs = 48000.0;
    const Config xo = {{"x1", 500.0f}, {"x2", 2000.0f}, {"x3", 6000.0f}};
    const double f[3] = {500.0, 2000.0, 6000.0};
    for (const auto& [mode, label, bar_name, want] :
         {std::tuple<const Config*, const char*, const char*, double>{&kMin, "minimum phase", "multiband.crossover_db", -6.0206},
          {&kLin, "linear phase", "multiband.crossover_lin_db", -6.0206}}) {
        const double bar = s.bar(bar_name);
        for (int j = 0; j < 3; ++j) {
            for (int side = 0; side < 2; ++side) {
                const double g = solo_gain_db(s, fs, cat(*mode, xo), j + side, f[j]);
                s.check_le(std::string(label) + ": band " + std::to_string(j + side + 1) + " at crossover " +
                               std::to_string(j + 1) + " (" + std::to_string(static_cast<int>(f[j])) +
                               " Hz) is -6.02 dB, |error|",
                           std::fabs(g - want), bar, "dB", "measured " + std::to_string(g) + " dB");
            }
        }
        // Two octaves outside a band: Linkwitz-Riley is 24 dB per octave, the
        // windowed sinc's skirt is steeper still once past its transition.
        const double stop = s.bar("multiband.stopband_db");
        s.check_le(std::string(label) + ": band 1 at 4x its crossover (2 kHz), dB",
                   solo_gain_db(s, fs, cat(*mode, xo), 0, 2000.0), stop, "dB");
        s.check_le(std::string(label) + ": band 4 at a quarter of its crossover (1.5 kHz), dB",
                   solo_gain_db(s, fs, cat(*mode, xo), 3, 1500.0), stop, "dB");
    }
}

// ---- the compressors ----------------------------------------------------------

// Each band, soloed at a tone inside it: the output level against the
// Giannoulis et al. gain computer applied to the level the detector sees.
// The detector reads sample peaks, so that level is taken from the same band
// with no compression (ratio 1), which has the same sampling of the same tone.
void static_curves(Suite& s) {
    const double bar = s.bar("multiband.static_db");
    const double fs = 48000.0;
    const double freq[4] = {120.0, 1000.0, 3500.0, 7000.0};
    const double ref_level = -20.0;
    double worst = 0.0;
    json::Value pts = json::Value::array();
    for (int b = 0; b < 4; ++b) {
        const Config base = {{"phase", 0.0f}, {"x1", 500.0f}, {"x2", 2000.0f}, {"x3", 6000.0f},
                             {kSolo[b], 1.0f}, {kAtk[b], 0.05f}, {kRel[b], 500.0f}};
        auto rp = make_prepared(s, fs, cat(cat(flat(), base), {{"knee", 0.0f}}));
        const Chans yr = measure::render(*rp, measure::sine(fs, freq[b], ref_level, 1.2));
        const double rms_ref = tail_rms_db(yr[0], fs);
        const double pk_ref = tail_peak_db(yr[0], fs);
        for (const Config& set : {Config{{"knee", 0.0f}, {kThr[b], -30.0f}, {kRatio[b], 4.0f}},
                                  Config{{"knee", 8.0f}, {kThr[b], -24.0f}, {kRatio[b], 10.0f}}}) {
            for (double level = -50.0; level <= 0.0; level += 5.0) {
                auto p = make_prepared(s, fs, cat(base, set));
                const Chans y = measure::render(*p, measure::sine(fs, freq[b], level, 1.2));
                const double got = tail_rms_db(y[0], fs);
                const double detected = pk_ref + (level - ref_level);
                const double want = rms_ref + (level - ref_level) +
                                    (dsp::compressor_curve_db(detected, p->param(measure::param_index(*p, kThr[b])),
                                                              p->param(measure::param_index(*p, kRatio[b])),
                                                              p->param(measure::param_index(*p, "knee"))) -
                                     detected);
                worst = std::fmax(worst, std::fabs(got - want));
                json::Value pt = json::Value::array();
                pt.push(json::Value::number_float(static_cast<float>(level)));
                pt.push(json::Value::number_float(static_cast<float>(got)));
                pt.push(json::Value::number_float(static_cast<float>(want)));
                pts.push(std::move(pt));
            }
        }
    }
    s.data().set("static_curve_level_got_want", std::move(pts));
    s.check_le("static curve vs gain computer, 4 bands x 2 settings x 11 levels", worst, bar, "dB");
}

// The gain applied, in dB over time: the output over the same render with no
// compression, which has the same band signal under the same delay. Where
// the reference is near a zero crossing the last good value holds.
std::vector<double> gain_curve(const std::vector<float>& y, const std::vector<float>& ref) {
    // Low enough to read the -40 dB legs of the step tests.
    const double floor_level = 0.005 * measure::peak(ref);
    std::vector<double> g(y.size(), 0.0);
    double last = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        if (std::fabs(ref[i]) > floor_level) last = db(std::fabs(y[i] / ref[i]));
        g[i] = last;
    }
    return g;
}

// Time from the step to a fraction of the change in the curve: 63 % for the
// gain in dB (Giannoulis et al.), 50 % for the band signal's own step.
double time_to_frac(const std::vector<double>& g, std::size_t from, std::size_t to, double fs, double frac) {
    const double g0 = g[from - 1], g1 = g[to - 1];
    const double mark = g0 + frac * (g1 - g0);
    for (std::size_t i = from; i < to; ++i) {
        if ((g1 < g0 && g[i] <= mark) || (g1 > g0 && g[i] >= mark)) return static_cast<double>(i - from + 1) / fs;
    }
    return -1.0;
}

void timing_one(Suite& s, double fs, int band, double a, double r, bool tone, double tone_hz) {
    const Config base = {{"phase", 0.0f}, {"x1", 1000.0f}, {"x2", 2000.0f}, {"x3", 6000.0f}, {kSolo[band], 1.0f}};
    const double len = 6.0 * r * 1e-3 + 0.2;
    Chans x;
    if (tone) {
        x = measure::concat({measure::sine(fs, tone_hz, -40.0, 0.2), measure::sine(fs, tone_hz, -10.0, len),
                             measure::sine(fs, tone_hz, -40.0, len)});
    } else {
        x = dc(fs, {{0.2, -40.0}, {len, -10.0}, {len, -40.0}});
    }
    auto p = make_prepared(s, fs, cat(cat(base, {{"knee", 0.0f}}),
                                      {{kThr[band], -20.0f}, {kRatio[band], 4.0f}, {kAtk[band], static_cast<float>(a)},
                                       {kRel[band], static_cast<float>(r)}}));
    auto q = make_prepared(s, fs, cat(base, flat()));
    const Chans y = measure::render(*p, x);
    const Chans ref = measure::render(*q, x);
    const std::vector<double> g = gain_curve(y[0], ref[0]);
    const auto n0 = static_cast<std::size_t>(0.2 * fs);
    const auto n1 = n0 + static_cast<std::size_t>(len * fs);
    const double ta = time_to_frac(g, n0, n1, fs, 1.0 - std::exp(-1.0));
    const double tr = time_to_frac(g, n1, g.size(), fs, 1.0 - std::exp(-1.0));
    const std::string what = " band " + std::to_string(band + 1) + at(fs);
    if (tone) {
        // A slow attack on a tone is not one exponential: the branching
        // detector rises only on the samples above its state, so the rise
        // is stretched by the duty cycle. The DC steps hold the bar.
        s.info("attack " + std::to_string(a) + " ms, release " + std::to_string(r) + " ms," + what + ", 63 % times on a tone",
               ta * 1e3, "ms", "attack, then release " + std::to_string(tr * 1e3) + " ms; stretched by the detector's duty cycle");
        return;
    }
    // The detector sees the step after the band's own low-pass, whose step
    // reaches half height after its group delay: that is not detector time.
    std::vector<double> env(ref[0].size());
    for (std::size_t i = 0; i < env.size(); ++i) env[i] = std::fabs(ref[0][i]);
    const double d_up = time_to_frac(env, n0, n1, fs, 0.5);
    const double d_down = time_to_frac(env, n1, env.size(), fs, 0.5);
    const double tol = s.bar("multiband.time_tolerance");
    s.check_le("attack " + std::to_string(a) + " ms," + what + ", |measured/set - 1|",
               std::fabs((ta - d_up) / (a * 1e-3) - 1.0), tol, "ratio",
               "measured " + std::to_string((ta - d_up) * 1e3) + " ms after the band's own " + std::to_string(d_up * 1e3) + " ms");
    s.check_le("release " + std::to_string(r) + " ms," + what + ", |measured/set - 1|",
               std::fabs((tr - d_down) / (r * 1e-3) - 1.0), tol, "ratio",
               "measured " + std::to_string((tr - d_down) * 1e3) + " ms after the band's own " + std::to_string(d_down * 1e3) + " ms");
}

void attack_release(Suite& s) {
    for (double fs : measure::kRates) {
        for (const auto& [a, r] : {std::pair<double, double>{5.0, 60.0}, {30.0, 300.0}}) {
            timing_one(s, fs, 0, a, r, false, 0.0);
        }
    }
    // A band that carries no DC: a 4 kHz tone in band 3.
    timing_one(s, 48000.0, 2, 10.0, 100.0, true, 4000.0);
}

// Look-ahead puts the gain reduction in place before the audio it is for.
void lookahead(Suite& s) {
    const double fs = 48000.0;
    const Config base = {{"phase", 0.0f}, {"x1", 1000.0f}, {"b1_solo", 1.0f}, {"knee", 0.0f}, {"b1_threshold", -30.0f},
                         {"b1_ratio", 20.0f}, {"b1_attack", 2.0f}, {"b1_release", 100.0f}};
    const Chans x = dc(fs, {{0.3, -50.0}, {0.3, -6.0}});
    double peak[2];
    std::uint32_t lat[2];
    int k = 0;
    for (const float ms : {0.0f, 6.0f}) {
        auto p = make_prepared(s, fs, cat(base, {{"lookahead", ms}}));
        const Chans y = measure::render(*p, x);
        const auto from = static_cast<std::size_t>(0.3 * fs);
        peak[k] = db(measure::peak(y[0], from, from + static_cast<std::size_t>(0.03 * fs)));
        lat[k++] = p->latency();
    }
    s.check_le("look-ahead 6 ms lowers the first 30 ms of a -50 to -6 dBFS step (2 ms attack)", -(peak[0] - peak[1]),
               -s.bar("multiband.lookahead_gain_db"), "dB", "peak " + std::to_string(peak[0]) + " dBFS without, " +
                                                                 std::to_string(peak[1]) + " with");
    s.check("look-ahead 6 ms reports 288 samples at 48 kHz", static_cast<double>(lat[1]), 288.0, lat[1] == 288 && lat[0] == 0,
            "samples");
}

// The key's high-pass keeps a low tone from compressing its band.
void sidechain_hpf(Suite& s) {
    const double fs = 48000.0;
    const Config base = {{"phase", 0.0f}, {"x1", 500.0f}, {"b1_solo", 1.0f}, {"knee", 0.0f}, {"b1_threshold", -30.0f},
                         {"b1_ratio", 8.0f}, {"b1_attack", 5.0f}, {"b1_release", 500.0f}};
    const Chans x = measure::sine(fs, 60.0, -10.0, 1.5);
    double level[2];
    int k = 0;
    for (const float on : {0.0f, 1.0f}) {
        auto p = make_prepared(s, fs, cat(base, {{"sc_hpf_on", on}, {"sc_hpf_freq", 400.0f}}));
        level[k++] = tail_rms_db(measure::render(*p, x)[0], fs, 0.3);
    }
    s.check("side-chain high-pass at 400 Hz: a 60 Hz tone is reduced less by at least the bar", level[1] - level[0],
            s.bar("multiband.hpf_gain_db"), level[1] - level[0] >= s.bar("multiband.hpf_gain_db"), "dB",
            "output " + std::to_string(level[0]) + " dB off, " + std::to_string(level[1]) + " dB on");
}

void auto_makeup(Suite& s) {
    const double fs = 48000.0;
    {
        auto p = s.make();
        s.check("auto make-up defaults to off", p->param(measure::param_index(*p, "auto_makeup")), 0.0,
                p->param(measure::param_index(*p, "auto_makeup")) == 0.0f, "");
    }
    const Config base = {{"phase", 0.0f}, {"x1", 500.0f}, {"b1_solo", 1.0f}, {"knee", 0.0f}, {"b1_threshold", -20.0f},
                         {"b1_ratio", 4.0f}};
    const Chans x = measure::sine(fs, 100.0, -40.0, 1.0);
    double level[2];
    int k = 0;
    for (const float on : {0.0f, 1.0f}) {
        auto p = make_prepared(s, fs, cat(base, {{"auto_makeup", on}}));
        level[k++] = tail_rms_db(measure::render(*p, x)[0], fs);
    }
    // 20 dB threshold, 4:1: the curve at 0 dBFS is -15 dB, halved is 7.5 dB.
    s.check_le("auto make-up, 20 dB threshold at 4:1: |gain - 7.5 dB|", std::fabs(level[1] - level[0] - 7.5),
               s.bar("multiband.auto_makeup_db"), "dB", "measured " + std::to_string(level[1] - level[0]) + " dB");
}

// A loud left and a quiet right: linked, both get the left's reduction;
// unlinked, the right is untouched.
void stereo_link(Suite& s) {
    const double fs = 48000.0;
    const Config base = {{"phase", 0.0f}, {"x1", 400.0f}, {"x2", 2500.0f}, {"b2_solo", 1.0f}, {"knee", 0.0f},
                         {"b2_threshold", -30.0f}, {"b2_ratio", 8.0f}, {"b2_attack", 1.0f}, {"b2_release", 500.0f}};
    Chans x = measure::sine(fs, 1000.0, -10.0, 1.5, 1);
    const Chans quiet = measure::sine(fs, 1000.0, -40.0, 1.5, 1);
    x.push_back(quiet[0]);
    auto ref = make_prepared(s, fs, cat(base, flat()));
    const Chans yr = measure::render(*ref, x);
    for (const float link : {100.0f, 0.0f}) {
        auto p = make_prepared(s, fs, cat(base, {{"link", link}}));
        const Chans y = measure::render(*p, x);
        const double gl = tail_rms_db(y[0], fs) - tail_rms_db(yr[0], fs);
        const double gr = tail_rms_db(y[1], fs) - tail_rms_db(yr[1], fs);
        const double bar = s.bar("multiband.link_db");
        if (link > 50.0f) {
            s.check_le("link 100 %: the quiet channel gets the loud one's reduction (" + std::to_string(gl) + " dB), |difference|",
                       std::fabs(gr - gl), bar, "dB");
        } else {
            s.check_le("link 0 %: the quiet channel (below threshold) is untouched, |gain|", std::fabs(gr), bar, "dB");
        }
    }
}

// Bypass leaves a band as it is; solo mutes the others; the meter reads the
// reduction; one channel gives the stereo result.
void band_controls(Suite& s) {
    const double fs = 48000.0;
    const Config heavy = {{"phase", 0.0f}, {"x1", 400.0f}, {"x2", 2500.0f}, {"knee", 0.0f}, {"b2_threshold", -40.0f},
                          {"b2_ratio", 20.0f}};
    const Chans x = measure::noise(fs, -6.0, 0.5, 11u);
    auto ref = make_prepared(s, fs, cat(heavy, cat(flat(), {{"b2_solo", 1.0f}})));
    auto byp = make_prepared(s, fs, cat(cat(flat(), heavy), {{"b2_solo", 1.0f}, {"b2_bypass", 1.0f}}));
    // The same band with the heavy settings and bypass on must equal the
    // untouched band.
    const Chans a = measure::render(*ref, x);
    const Chans b = measure::render(*byp, x);
    double worst = 0.0;
    for (std::size_t i = 0; i < a[0].size(); ++i) worst = std::fmax(worst, std::fabs(a[0][i] - b[0][i]));
    s.check_le("band bypass: a band at 20:1 with bypass on is the untouched band (max |diff|)", worst, 1e-6, "abs");

    {
        // Soloing one band mutes the rest: band 2 alone against everything.
        auto all = make_prepared(s, fs, cat(heavy, flat()));
        auto one = make_prepared(s, fs, cat(cat(heavy, flat()), {{"b1_solo", 1.0f}}));
        const double lo = measure::rms(measure::render(*one, x)[0]);
        const double hi = measure::rms(measure::render(*all, x)[0]);
        // Below 400 Hz is under 2 % of white noise's power (-17.8 dB).
        s.check_le("solo band 1 of white noise: far quieter than the whole", db(lo / hi), -12.0, "dB");
    }
    {
        // The meter: DC at -10 dBFS in band 1, -30 dB threshold, 4:1: 15 dB.
        auto p = make_prepared(s, fs, {{"phase", 0.0f}, {"x1", 1000.0f}, {"knee", 0.0f}, {"b1_threshold", -30.0f},
                                       {"b1_ratio", 4.0f}, {"b1_attack", 5.0f}, {"b1_release", 300.0f}});
        measure::render(*p, dc(fs, {{1.0, -10.0}}));
        auto* meters = dynamic_cast<BandMeters*>(p.get());
        const double gr = meters ? meters->band_gr_db(0) : -1.0;
        s.check_le("band 1 gain-reduction meter: |reading - 15 dB|", std::fabs(gr - 15.0), 0.15, "dB",
                   "reading " + std::to_string(gr) + " dB");
    }
    {
        // One channel is the stereo result with the same signal on both.
        const Config cfg = {{"phase", 0.0f}, {"b1_threshold", -30.0f}, {"b2_threshold", -30.0f}, {"b3_threshold", -30.0f}};
        auto m = make_prepared(s, fs, cfg, 1);
        auto st = make_prepared(s, fs, cfg, 2);
        Chans mono = {x[0]};
        Chans both = {x[0], x[0]};
        const Chans ym = measure::render(*m, mono);
        const Chans ys = measure::render(*st, both);
        double d = 0.0;
        for (std::size_t i = 0; i < ym[0].size(); ++i) d = std::fmax(d, std::fabs(ym[0][i] - ys[0][i]));
        s.check("one channel renders as the stereo left", d, 0.0, d == 0.0, "abs");
    }
}

void aliasing_info(Suite& s) {
    const double fs = 48000.0;
    auto p = make_prepared(s, fs, {{"phase", 0.0f}, {"b3_threshold", -30.0f}, {"b3_ratio", 20.0f}, {"b3_attack", 0.05f},
                                   {"b3_release", 5.0f}, {"knee", 0.0f}});
    const Chans y = measure::render(*p, measure::sine(fs, 3500.0, -6.0, 1.2));
    s.info("non-harmonic energy, 3.5 kHz at -6 dBFS, 20:1, 0.05/5 ms", measure::alias_dbc(y[0], 19200, fs, 3500.0), "dBc",
           "fast gain modulation; the only nonlinearity is the gain computer");
}

// ---- latency ------------------------------------------------------------------

// Minimum phase, by the impulse's first non-zero sample: an all-pass bank's
// cross-correlation lag is its phase delay, which is not buffering and which
// plugin delay compensation must not add.
void latency_minimum(Suite& s) {
    const std::vector<std::pair<std::string, Config>> configs = {
        {"minimum phase", {}},
        {"minimum phase, 3 bands, look-ahead 3 ms", {{"bands", 0.0f}, {"lookahead", 3.0f}}},
        {"minimum phase, look-ahead 10 ms", {{"lookahead", 10.0f}}},
    };
    for (const auto& [label, cfg] : configs) {
        for (double fs : measure::kRates) {
            auto p = make_prepared(s, fs, cat(cat(flat(), kMin), cfg));
            Chans x = measure::silence(fs, 0.3);
            x[0][0] = 1.0f;
            const Chans y = measure::render(*p, x);
            const auto reported = static_cast<long>(p->latency());
            long first = -1;
            for (std::size_t i = 0; i < y[0].size(); ++i) {
                if (std::fabs(y[0][i]) > 1e-9f) {
                    first = static_cast<long>(i);
                    break;
                }
            }
            s.check("latency, " + label + at(fs) + ": first non-zero sample - reported",
                    static_cast<double>(first - reported), 0.0, first == reported, "samples",
                    "reported " + std::to_string(reported));
            // Bypass is the input delayed by exactly the reported latency.
            auto b = make_prepared(s, fs, cat(cat(flat(), kMin), cfg));
            b->set_param(0, 1.0f);
            Chans in = measure::noise(fs, -30.0, 0.2, 77u);
            const Chans by = measure::render(*b, in);
            double worst = 0.0;
            for (std::size_t c = 0; c < 2; ++c) {
                for (std::size_t i = 0; i < by[c].size(); ++i) {
                    const float want = static_cast<long>(i) >= reported ? in[c][i - reported] : 0.0f;
                    worst = std::fmax(worst, std::fabs(by[c][i] - want));
                }
            }
            s.check("bypass = input delayed by latency, " + label + at(fs), worst, 0.0, worst == 0.0, "abs");
        }
    }
}

}  // namespace

// The golden configuration, shared with the host tests (sonora-dsp-measure
// --host-fixture): the bundles are rendered through Sonora's hosts with it.
measure::Config multiband_golden_config() {
    return {{"phase", 0.0f},         {"lookahead", 2.0f},       {"knee", 6.0f},         {"sc_hpf_on", 1.0f},
            {"b1_threshold", -26.0f}, {"b1_ratio", 3.0f},        {"b1_makeup", 2.0f},    {"b2_threshold", -24.0f},
            {"b2_ratio", 2.5f},       {"b3_threshold", -22.0f},  {"b3_ratio", 2.0f},     {"b4_threshold", -20.0f},
            {"b4_ratio", 3.0f},       {"b4_attack", 3.0f},       {"b4_release", 80.0f}};
}

int multiband(const measure::Args& args) {
    Suite s("multiband", [] { return make_multiband(); }, args);
    null_and_flat(s);
    crossover_levels(s);
    static_curves(s);
    attack_release(s);
    lookahead(s);
    sidechain_hpf(s);
    auto_makeup(s);
    stereo_link(s);
    band_controls(s);
    aliasing_info(s);
    latency_minimum(s);
    const Config golden = multiband_golden_config();
    // 100 Hz sits in band 1: its threshold, ratio, timing, make-up and solo
    // all change the gain the zipper tone gets.
    const Config zipper = {{"b1_threshold", -40.0f}, {"b1_ratio", 4.0f}, {"b2_threshold", -40.0f}};
    const Config lin_cfg = {{"phase", 1.0f}};
    measure::check_common(s, golden,
                          {{"linear phase, 4 bands", lin_cfg},
                           {"linear phase, 3 bands, look-ahead 2 ms", {{"phase", 1.0f}, {"bands", 0.0f}, {"lookahead", 2.0f}}}},
                          &zipper);
    if (args.cpu) {
        measure::check_cpu(s, cat(golden, {{"phase", 1.0f}}), "linear phase", false);
    }
    return s.finish();
}

}  // namespace sonora::native::suites
