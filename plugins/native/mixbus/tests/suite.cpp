// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mix bus against its bars (quality.json): the static curve at every
// stepped ratio, manual attack and release from step tests at three rates,
// auto release on a transient and after sustained reduction, the
// side-chain high-pass, THD+N below threshold, the parallel mix and the
// width stage to the bit, width's own zipper on a stereo signal, and the
// common checks.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/dsp/dynamics.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/transformer.h"
#include "sonora/measure/measure.h"
#include "sonora/native/mixbus.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

// The render with the processor's latency taken out: the input padded with
// `latency` samples of silence, and the first `latency` output samples
// dropped, so sample n of the result answers sample n of the input. The
// compressor and width bars below are about the signal, not the delay.
Chans render_aligned(native::Processor& p, Chans x) {
    const std::size_t lat = p.latency();
    for (auto& c : x) c.resize(c.size() + lat, 0.0f);
    Chans y = measure::render(p, x);
    for (auto& c : y) c.erase(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(lat));
    return y;
}

Chans dc(double fs, const std::vector<std::pair<double, double>>& segs) {
    std::vector<Chans> parts;
    for (const auto& [secs, db] : segs) {
        Chans p = measure::silence(fs, secs);
        for (auto& c : p) {
            for (float& v : c) v = static_cast<float>(dsp::db_to_gain(db));
        }
        parts.push_back(std::move(p));
    }
    return measure::concat(parts);
}

std::string rate(double fs) { return std::to_string(static_cast<int>(fs)) + " Hz"; }

// DC levels (side-chain high-pass off, so the detector sees the level):
// the settled output level against the Giannoulis et al. gain computer,
// once per stepped ratio.
void static_curve(Suite& s) {
    const double bar = s.bar("mixbus.static_db");
    const double fs = 48000.0;
    double worst = 0.0;
    json::Value pts = json::Value::array();
    const struct {
        float thr, ratio_index, knee;
    } settings[] = {{-30.0f, 0.0f, 6.0f}, {-24.0f, 1.0f, 12.0f}, {-20.0f, 2.0f, 0.0f}, {-10.0f, 3.0f, 3.0f}};
    for (const auto& st : settings) {
        const double ratio = mixbus::kRatios[static_cast<int>(st.ratio_index)];
        for (double level = -40.0; level <= 0.0; level += 2.0) {
            auto p = s.make();
            measure::apply(*p, {{"threshold", st.thr}, {"ratio", st.ratio_index}, {"knee", st.knee},
                                {"attack", 1.0f}, {"release", 50.0f}});
            p->prepare(fs, 128, 2);
            const Chans y = render_aligned(*p, dc(fs, {{0.5, level}}));
            const double out_db = dsp::gain_to_db(y[0].back());
            const double want = dsp::compressor_curve_db(level, st.thr, ratio, st.knee);
            worst = std::fmax(worst, std::fabs(out_db - want));
            json::Value pt = json::Value::array();
            pt.push(json::Value::number_float(static_cast<float>(ratio)));
            pt.push(json::Value::number_float(static_cast<float>(level)));
            pt.push(json::Value::number_float(static_cast<float>(out_db)));
            pt.push(json::Value::number_float(static_cast<float>(want)));
            pts.push(std::move(pt));
        }
    }
    s.data().set("static_curve_ratio_in_out_formula", std::move(pts));
    s.check_le("static curve vs gain computer, 4 ratios x 21 levels", worst, bar, "dB");
}

// Time from the step to 63 % of the gain change, in dB, as the detector
// defines its attack and release (Giannoulis et al. §3).
double time_to_63(const std::vector<float>& y, const std::vector<float>& x, std::size_t from,
                  std::size_t to, double fs) {
    auto gdb = [&](std::size_t i) { return dsp::gain_to_db(std::fabs(y[i] / x[i])); };
    const double g0 = gdb(from - 1), g1 = gdb(to - 1);
    const double mark = g0 + (1.0 - std::exp(-1.0)) * (g1 - g0);
    for (std::size_t i = from; i < to; ++i) {
        if ((g1 < g0 && gdb(i) <= mark) || (g1 > g0 && gdb(i) >= mark)) return (i - from + 1) / fs;
    }
    return -1.0;
}

const Config kStepCurve = {{"threshold", -20.0f}, {"ratio", 2.0f}, {"knee", 0.0f}};

void attack_release(Suite& s) {
    const double tol = s.bar("mixbus.time_tolerance");
    for (double fs : measure::kRates) {
        for (const auto& [a, r] : {std::pair<float, float>{0.3f, 50.0f}, {10.0f, 300.0f}, {30.0f, 1200.0f}}) {
            auto p = s.make();
            measure::apply(*p, kStepCurve);
            measure::apply(*p, {{"attack", a}, {"release", r}});
            p->prepare(fs, 128, 2);
            const double len = 6.0 * r * 1e-3 + 0.2;
            const Chans x = dc(fs, {{0.2, -40.0}, {len, -10.0}, {len, -40.0}});
            const Chans y = render_aligned(*p, x);
            const auto n0 = static_cast<std::size_t>(0.2 * fs);
            const auto n1 = n0 + static_cast<std::size_t>(len * fs);
            const double ta = time_to_63(y[0], x[0], n0, n1, fs);
            const double tr = time_to_63(y[0], x[0], n1, y[0].size(), fs);
            s.check_le("attack " + std::to_string(a) + " ms, |measured/set - 1| at " + rate(fs),
                       std::fabs(ta / (a * 1e-3) - 1.0), tol, "ratio", "measured " + std::to_string(ta * 1e3) + " ms");
            s.check_le("release " + std::to_string(r) + " ms, |measured/set - 1| at " + rate(fs),
                       std::fabs(tr / (r * 1e-3) - 1.0), tol, "ratio", "measured " + std::to_string(tr * 1e3) + " ms");
        }
    }
}

// Auto release: after a 10 ms overload the fast path recovers
// (kAutoFastRelease); after 4 s of reduction the slow path, charged, holds
// the recovery to kAutoSlowRelease. The release knob is set far from both
// so a path that ignored the switch would fail.
void auto_release(Suite& s) {
    const double tol = s.bar("mixbus.time_tolerance");
    const double fs = 48000.0;
    const struct {
        const char* what;
        double over_s, want_s;
    } cases[] = {{"after a 10 ms overload", 0.010, mixbus::kAutoFastRelease},
                 {"after 4 s of reduction", 4.0, mixbus::kAutoSlowRelease}};
    for (const auto& c : cases) {
        auto p = s.make();
        measure::apply(*p, kStepCurve);
        measure::apply(*p, {{"attack", 0.3f}, {"release", 600.0f}, {"auto_release", 1.0f}});
        p->prepare(fs, 128, 2);
        const double tail = 6.0 * mixbus::kAutoSlowRelease;
        const Chans x = dc(fs, {{0.2, -40.0}, {c.over_s, -10.0}, {tail, -40.0}});
        const Chans y = render_aligned(*p, x);
        const auto n1 = static_cast<std::size_t>(0.2 * fs) + static_cast<std::size_t>(c.over_s * fs);
        const double tr = time_to_63(y[0], x[0], n1, y[0].size(), fs);
        s.check_le(std::string("auto release ") + c.what + ", |measured/expected - 1|",
                   std::fabs(tr / c.want_s - 1.0), tol, "ratio", "measured " + std::to_string(tr * 1e3) + " ms");
    }
}

// Reduction in dB once settled: input over output level, on a tone, over
// the last 0.5 s of a 1.5 s render.
double settled_reduction_db(Suite& s, const Config& c, double f) {
    const double fs = 48000.0;
    auto p = s.make();
    measure::apply(*p, c);
    p->prepare(fs, 128, 2);
    const Chans x = measure::sine(fs, f, -6.0, 1.5);
    const Chans y = render_aligned(*p, x);
    const auto from = static_cast<std::size_t>(1.0 * fs);
    return measure::to_db(measure::rms(x[0], from) / measure::rms(y[0], from));
}

// The side-chain high-pass keeps bass from driving the bus compressor and
// leaves the mid-band's reduction alone.
void sidechain_hpf(Suite& s) {
    const Config base = {{"threshold", -30.0f}, {"ratio", 2.0f}, {"knee", 0.0f}, {"attack", 1.0f},
                         {"release", 300.0f}, {"sc_hpf_freq", 150.0f}};
    Config on = base;
    on.push_back({"sc_hpf_on", 1.0f});
    const double lf_off = settled_reduction_db(s, base, 40.0);
    const double lf_on = settled_reduction_db(s, on, 40.0);
    const double mid_off = settled_reduction_db(s, base, 1000.0);
    const double mid_on = settled_reduction_db(s, on, 1000.0);
    s.check("side-chain high-pass at 150 Hz: 40 Hz reduction drops by", lf_off - lf_on,
            s.bar("sc_hpf.min_drop_db"), lf_off - lf_on >= s.bar("sc_hpf.min_drop_db"), "dB",
            "off " + std::to_string(lf_off) + " dB, on " + std::to_string(lf_on) + " dB");
    s.check_le("side-chain high-pass at 150 Hz: 1 kHz reduction moves by", std::fabs(mid_off - mid_on),
               s.bar("sc_hpf.passband_db"), "dB");
}

void unity_mix_width(Suite& s) {
    const double fs = 48000.0;
    const Config transparent = {{"threshold", 0.0f}, {"knee", 0.0f}};
    {
        auto p = s.make();
        measure::apply(*p, transparent);
        p->prepare(fs, 128, 2);
        const Chans y = render_aligned(*p, measure::sine(fs, 1000.0, -1.0, 1.2));
        s.check_le("THD+N at -1 dBFS, 1 kHz, below threshold", measure::thdn_db(y[0], 19200, fs, 1000.0),
                   s.bar("mixbus.thdn_db"), "dB");
    }
    const Chans x = measure::noise(fs, -6.0, 0.5, 3u);
    auto worst_diff = [](const Chans& a, const Chans& b) {
        double w = 0.0;
        for (std::size_t c = 0; c < a.size(); ++c) {
            for (std::size_t i = 0; i < a[c].size(); ++i) w = std::fmax(w, std::fabs(a[c][i] - b[c][i]));
        }
        return w;
    };
    {
        // Parallel mix at 0 % is the dry signal, exactly.
        auto p = s.make();
        measure::apply(*p, {{"mix", 0.0f}, {"threshold", -40.0f}, {"ratio", 3.0f}});
        p->prepare(fs, 128, 2);
        const double w = worst_diff(render_aligned(*p, x), x);
        s.check("mix 0 % equals the dry input", w, 0.0, w == 0.0, "abs");
    }
    {
        // Width 100 % with nothing over the threshold: the input, to the bit.
        auto p = s.make();
        measure::apply(*p, transparent);
        p->prepare(fs, 128, 2);
        const double w = worst_diff(render_aligned(*p, x), x);
        s.check("width 100 %, below threshold, equals the input", w, 0.0, w == 0.0, "abs");
    }
    {
        // Width w on decorrelated noise: L' = M + w S, R' = M - w S.
        const double bar = s.bar("width.max_error");
        double worst = 0.0;
        for (const float wp : {0.0f, 50.0f, 150.0f, 200.0f}) {
            auto p = s.make();
            measure::apply(*p, transparent);
            measure::apply(*p, {{"width", wp}});
            p->prepare(fs, 128, 2);
            const Chans y = render_aligned(*p, x);
            const double w = wp * 0.01;
            for (std::size_t i = 0; i < x[0].size(); ++i) {
                const double mid = 0.5 * (static_cast<double>(x[0][i]) + x[1][i]);
                const double side = 0.5 * (static_cast<double>(x[0][i]) - x[1][i]);
                worst = std::fmax(worst, std::fabs(y[0][i] - (mid + w * side)));
                worst = std::fmax(worst, std::fabs(y[1][i] - (mid - w * side)));
            }
            if (wp == 0.0f) {
                double diff = 0.0;
                for (std::size_t i = 0; i < y[0].size(); ++i) diff = std::fmax(diff, std::fabs(y[0][i] - y[1][i]));
                s.check("width 0 %: left equals right (mono)", diff, 0.0, diff == 0.0, "abs");
            }
        }
        s.check_le("width 0, 50, 150, 200 % against mid + w side, mid - w side", worst, bar, "abs");
    }
    {
        // The common zipper sweep feeds the same tone to both channels, where
        // width does nothing. Here the tone is on the left only, so the side
        // is as loud as the mid and every move of width is heard.
        const double tone_bar = s.bar("zipper_dbfs");
        auto p = s.make();
        const std::uint32_t idx = measure::param_index(*p, "width");
        const ParamInfo& info = p->descriptor().params[idx];
        p->prepare(fs, 128, 2);
        Chans in = measure::zipper_tone(fs, 3.25);
        for (float& v : in[1]) v = 0.0f;
        std::vector<measure::Event> ev;
        const double n0 = to_normalized(info, info.def);
        const auto start = static_cast<std::uint64_t>(0.25 * fs);
        for (std::uint64_t f = start; f < in[0].size(); f += 128) {
            const double t = static_cast<double>(f - start) / fs;
            double n = t < n0 ? n0 - t : t < n0 + 1.0 ? t - n0 : t < 2.0 ? 1.0 - (t - n0 - 1.0) : n0;
            n = std::fmin(1.0, std::fmax(0.0, n));
            ev.push_back({f, idx, static_cast<float>(from_normalized(info, n))});
        }
        const Chans y = measure::render(*p, in, ev);
        double worst = 0.0;
        for (const auto& ch : y) worst = std::fmax(worst, measure::hf_residue_peak(ch, fs, start));
        s.check_le("zipper: full-range sweep of 'width', tone on the left only, HF residue peak",
                   measure::to_db(worst), tone_bar, "dBFS");
    }
    {
        // Aliasing of the fastest, deepest reduction on a 5 kHz tone: the
        // gain modulation's sidebands, reported (no oversampling here).
        auto p = s.make();
        measure::apply(*p, {{"threshold", -40.0f}, {"ratio", 3.0f}, {"knee", 0.0f}, {"attack", 0.1f}, {"release", 50.0f}});
        p->prepare(fs, 128, 2);
        const Chans y = render_aligned(*p, measure::sine(fs, 5000.0, -6.0, 1.2));
        s.info("non-harmonic energy, 5 kHz at -6 dBFS, 10:1, 0.1/50 ms", measure::alias_dbc(y[0], 19200, fs, 5000.0),
               "dBc", "gain modulation only; no nonlinearity in the audio path");
    }
}

std::string fmt(const char* f, double a, double b = 0.0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

// Harmonics 1..9 of a tone over whole periods from `from`.
std::vector<std::complex<double>> harmonics_of(const std::vector<float>& y, std::size_t from, std::size_t per) {
    std::vector<std::complex<double>> h(10);
    const std::size_t n = (y.size() - from) / per * per;
    for (int k = 1; k <= 9; ++k) {
        std::complex<double> a = 0.0;
        for (std::size_t i = 0; i < n; ++i)
            a += static_cast<double>(y[from + i]) *
                 std::exp(std::complex<double>(0.0, -dsp::kTwoPi * k * static_cast<double>(i % per) / static_cast<double>(per)));
        h[k] = a;
    }
    return h;
}

const Config kColourOnly = {{"threshold", 0.0f}, {"knee", 0.0f}, {"colour", 100.0f}, {"sag", 0.0f}};

// 0 % bypasses the stage exactly: whatever Sag and Quality say, the output
// is the compressor's, delayed by the reported latency, to the bit.
void colour_off(Suite& s) {
    const double fs = 48000.0;
    const Chans x = measure::noise(fs, -6.0, 0.4, 5u);
    bool same = true;
    for (int q = 0; q < 3; ++q) {
        auto p = s.make();
        measure::apply(*p, {{"threshold", -30.0f}, {"ratio", 2.0f}, {"quality", static_cast<float>(q)}, {"sag", 100.0f}});
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, x);
        auto ref = s.make();
        measure::apply(*ref, {{"threshold", -30.0f}, {"ratio", 2.0f}, {"quality", 1.0f}});
        ref->prepare(fs, 128, 2);
        const Chans r = measure::render(*ref, x);
        const std::size_t lat = p->latency();
        const std::size_t lat_ref = ref->latency();
        // Both are the same compressor; the one at the other quality is the
        // other latency later. Compare on the common span.
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t m = 0; m + std::max(lat, lat_ref) < x[c].size(); ++m) same &= y[c][m + lat] == r[c][m + lat_ref];
    }
    s.check("colour 0 %: the compressor's output delayed by the latency, bit-exact at every quality", same ? 1.0 : 0.0, 1.0,
            same, "bool");
}

// The harmonics the colour stage makes, against the same transformer model
// (common/dsp, held to its own physics by test_dsp_transformer) driven at the
// oversampled rate by an ideal sine: the processor's path adds only the
// oversampler's passband ripple.
void colour_model(Suite& s) {
    const double fs = 48000.0;
    const std::uint32_t factor = mixbus::kFactors[1];
    const double floor_db = s.bar("colour.floor_dbc");
    const double bar = s.bar("colour.harmonic_db");
    double worst = 0.0, lf_h = -400.0, mid_h = -400.0;
    for (double f : {1000.0, 40.0}) {
        const double amp = dsp::db_to_gain(-12.0);
        const double os_fs = fs * factor;
        dsp::OutputStage model;
        model.prepare(os_fs, 1, {}, {}, factor / 2);
        model.set_sag(0.0);
        const std::size_t n = static_cast<std::size_t>(1.5 * os_fs);
        std::vector<double> in(n), out(n);
        for (std::size_t i = 0; i < n; ++i) in[i] = amp * std::sin(dsp::kTwoPi * f * static_cast<double>(i) / os_fs);
        const double* ip[1] = {in.data()};
        double* op[1] = {out.data()};
        model.process_block(ip, op, static_cast<std::uint32_t>(n / (factor / 2)), nullptr, 1);
        std::vector<float> ref(out.begin(), out.end());
        const auto hr = harmonics_of(ref, static_cast<std::size_t>(0.5 * os_fs), static_cast<std::size_t>(os_fs / f));

        auto p = s.make();
        measure::apply(*p, kColourOnly);
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, measure::sine(fs, f, -12.0, 1.5));
        const auto hp = harmonics_of(y[0], static_cast<std::size_t>(0.5 * fs), static_cast<std::size_t>(fs / f));
        double w = 0.0;
        for (int k = 2; k <= 9; ++k) {
            const double a = measure::to_db(std::abs(hp[k]) / std::abs(hp[1]));
            const double b = measure::to_db(std::abs(hr[k]) / std::abs(hr[1]));
            if (b < floor_db) continue;
            w = std::fmax(w, std::fabs(a - b));
            (f == 40.0 ? lf_h : mid_h) = std::fmax(f == 40.0 ? lf_h : mid_h, b);
        }
        worst = std::fmax(worst, w);
        double thd = 0.0;
        for (int k = 2; k <= 9; ++k) thd += std::norm(hp[k]);
        s.info(fmt("harmonics 2-9 at %.0f Hz, -12 dBFS, colour 100 %%, sag 0, standard: total re fundamental",
                   f), measure::to_db(std::sqrt(thd) / std::abs(hp[1])), "dB");
    }
    s.check_le("colour 100 %: every harmonic 2-9 above the floor at 1 kHz and 40 Hz against the model", worst, bar, "dB",
               fmt("strongest reference harmonic %.1f dBc (40 Hz), %.1f dBc (1 kHz)", lf_h, mid_h));
    s.check("colour 100 %: LF first, strongest harmonic at 40 Hz over that at 1 kHz", lf_h - mid_h,
            s.bar("colour.lf_over_mid_db"), lf_h - mid_h >= s.bar("colour.lf_over_mid_db"), "dB");
}

// A held loud bass tone ends lower than it began with sag, not without.
void colour_sag(Suite& s) {
    const double fs = 48000.0;
    const Chans in = measure::sine(fs, 100.0, -1.0, 0.6);
    double ratio[2];
    for (int sag = 0; sag < 2; ++sag) {
        auto p = s.make();
        measure::apply(*p, kColourOnly);
        measure::set(*p, "sag", sag ? 100.0f : 0.0f);
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, in);
        auto peak = [&](double t0, double t1) {
            double m = 0.0;
            for (auto i = static_cast<std::size_t>(t0 * fs); i < static_cast<std::size_t>(t1 * fs); ++i)
                m = std::fmax(m, std::fabs(y[0][i]));
            return m;
        };
        ratio[sag] = measure::to_db(peak(0.5, 0.6) / peak(0.1, 0.2));
    }
    s.info("sag 0 %: 100 Hz at -1 dBFS, peak at 0.5-0.6 s over 0.1-0.2 s", ratio[0], "dB");
    s.info("sag 100 %: 100 Hz at -1 dBFS, peak at 0.5-0.6 s over 0.1-0.2 s", ratio[1], "dB");
    s.check("colour: sag compresses a held loud bass tone (100 % against 0 %)", ratio[1] - ratio[0], s.bar("colour.sag_db"),
            ratio[1] - ratio[0] <= -s.bar("colour.sag_db"), "dB");
}

// Non-harmonic energy with the stage in, per quality, 20 Hz-20 kHz, on the
// tubes' tones (ADR-0011 §3.2): the bar at the nominal level (-18 dBFS) and
// the hot level reported.
void colour_aliasing(Suite& s) {
    const double tones[] = {997.0, 2503.0, 5011.0, 7993.0, 10007.0, 14009.0, 19001.0};
    const char* bars[] = {"aliasing.eco_dbc", "aliasing.standard_dbc", "aliasing.high_dbc"};
    const char* names[] = {"eco (2x)", "standard (4x)", "high (8x)"};
    const std::vector<double> rates = measure::quick() ? std::vector<double>{48000.0}
                                                       : std::vector<double>{44100.0, 48000.0, 96000.0};
    json::Value rows = json::Value::array();
    for (int q = 0; q < 3; ++q) {
        for (double fs : rates) {
            for (int lv = 0; lv < 2; ++lv) {
                double worst = -400.0, worst_f = 0.0;
                for (double f0 : tones) {
                    if (f0 > 0.45 * fs) continue;
                    auto p = s.make();
                    measure::apply(*p, {{"threshold", 0.0f}, {"knee", 0.0f}, {"quality", static_cast<float>(q)}, {"colour", 100.0f}});
                    p->prepare(fs, 128, 1);
                    const Chans y = measure::render(*p, measure::sine(fs, f0, lv ? -6.0 : -18.0, 1.0, 1));
                    const double a = measure::alias_dbc(y[0], static_cast<std::size_t>(0.2 * fs), fs, f0);
                    if (a > worst) {
                        worst = a;
                        worst_f = f0;
                    }
                }
                json::Value row = json::Value::object();
                row.set("quality", json::Value::integer(q));
                row.set("rate", json::Value::number_float(static_cast<float>(fs)));
                row.set("dbfs", json::Value::number_float(lv ? -6.0f : -18.0f));
                row.set("alias_dbc", json::Value::number_float(static_cast<float>(worst)));
                rows.push(std::move(row));
                const std::string what = std::string("aliasing, colour 100 %, ") + names[q] + ", " + rate(fs) + ", " +
                                         (lv ? "-6 dBFS" : "nominal (-18 dBFS)") + ": worst non-harmonic";
                if (lv == 0) s.check_le(what, worst, s.bar(bars[q]), "dBc", fmt("at %.0f Hz", worst_f));
                else s.info(what, worst, "dBc", fmt("at %.0f Hz", worst_f));
            }
        }
    }
    s.data().set("colour_aliasing", rows);
}

// ADR-0011 §3.5: a processor over its class ships only with an eco mode
// that meets it. Eco is held to class B on every load below, colour at 0 and
// at 100 %; standard and high are reported.
void cpu_qualities(Suite& s, const Config& golden) {
    if (!s.args().cpu) return;
    const double fs = 48000.0;
    const double secs = s.args().cpu_seconds;
    const Chans programme = measure::cpu_programme(fs, secs, 0.0);
    const Chans hot = measure::cpu_programme(fs, secs, 12.0);
    const char* names[] = {"eco (2x)", "standard (4x)", "high (8x)"};
    for (int q = 0; q < 3; ++q) {
        for (float colour : {0.0f, 100.0f}) {
            Config c = golden;
            c.push_back({"quality", static_cast<float>(q)});
            c.push_back({"colour", colour});
            const std::string tag = std::string(", ") + names[q] + (colour > 0.0f ? ", colour 100 %" : ", colour 0 %");
            measure::check_cpu_on(s, c, programme, "programme" + tag, q == 0);
            measure::check_cpu_on(s, c, hot, "programme +12 dB" + tag, q == 0);
        }
    }
}

}  // namespace

// The golden configuration, shared with the host tests (sonora-dsp-measure
// --host-fixture): the bundles are rendered through Sonora's hosts with it.
measure::Config mixbus_golden_config() {
    return {{"threshold", -20.0f}, {"ratio", 1.0f}, {"knee", 3.0f}, {"attack", 3.0f}, {"release", 200.0f},
            {"auto_release", 1.0f}, {"sc_hpf_on", 1.0f}, {"makeup", 3.0f}, {"mix", 80.0f}, {"width", 120.0f},
            {"colour", 60.0f}};
}

int mixbus(const measure::Args& args) {
    Suite s("mixbus", [] { return make_mixbus(); }, args);
    static_curve(s);
    attack_release(s);
    auto_release(s);
    sidechain_hpf(s);
    unity_mix_width(s);
    const Config golden = mixbus_golden_config();
    colour_off(s);
    colour_model(s);
    colour_sag(s);
    colour_aliasing(s);
    // check_common() without its CPU check, which holds one quality to the
    // class bar: cpu_qualities() instead.
    measure::check_latency(s, {{"eco (2x)", {{"quality", 0.0f}}},
                               {"standard (4x)", {{"quality", 1.0f}}},
                               {"high (8x)", {{"quality", 2.0f}}},
                               {"eco (2x), colour", {{"quality", 0.0f}, {"colour", 100.0f}}},
                               {"high (8x), auto release, high-pass", {{"quality", 2.0f}, {"auto_release", 1.0f}, {"sc_hpf_on", 1.0f}}}});
    measure::check_nonfinite(s, golden);
    measure::check_reset(s, golden);
    measure::check_denormals(s, golden);
    Config full = golden;
    full.push_back({"colour", 100.0f});
    measure::check_nonfinite(s, full);
    measure::check_denormals(s, full);
    measure::check_zipper(s, {{"threshold", -18.0f}, {"ratio", 2.0f}, {"colour", 100.0f}}, "");
    measure::check_golden(s, golden);
    cpu_qualities(s, golden);
    return s.finish();
}

}  // namespace sonora::native::suites
