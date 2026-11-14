// SPDX-License-Identifier: GPL-3.0-or-later
//
// The amp against its bars. Here: latency measured equals latency reported
// at every session rate and block size, with the resampler pair and
// without; the resampled path is transparent in its passband (an identity
// capture); a 48 kHz capture runs in a 44.1 kHz session at the level it has
// at its own rate; the loudness normalisation; a change of capture with no
// click; the dry path; and the common checks. suite_load.cpp has the
// load-time side: the class-E refusal, the file slot, a deferred capture
// and loads under a running audio thread.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "suite_load.h"
#include "suite_models.h"
#include "sonora/measure/measure.h"
#include "sonora/native/amp.h"
#include "sonora/dsp/resample.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;
namespace at = amp_test;

std::string rate_name(double fs) { return std::to_string(static_cast<int>(fs)) + " Hz"; }

std::uint32_t planned_latency(double session, double capture) {
    dsp::RatePlan plan;
    std::string err;
    return dsp::plan_rates(session, capture, &plan, &err) ? plan.latency : 0;
}

// max |y[n] - x[n - lag]| over the whole render, ignoring the first `skip`.
double delayed_error(const std::vector<float>& x, const std::vector<float>& y, std::size_t lag, std::size_t skip = 0) {
    double e = 0.0;
    for (std::size_t n = std::max(lag, skip); n < y.size(); ++n)
        e = std::max(e, static_cast<double>(std::fabs(y[n] - x[n - lag])));
    return e;
}

// An identity capture at 48 kHz, both channels the same signal (the amp
// sums them to one): what is left is the resampler pair, whose
// delay is the latency and whose passband (0.418 of the lower rate) holds
// the test noise, band-limited to 16 kHz.
void latency_identity(Suite& s) {
    const std::uint32_t blocks[] = {32u, 128u, 480u};
    for (double fs : measure::kRates) {
        for (std::uint32_t block : blocks) {
            auto p = make_amp();
            std::string msg;
            p->load_capture(at::identity(48000.0), msg);
            p->prepare(fs, block, 2);
            Chans x = measure::noise(fs, -6.0, 0.5, 21u);
            measure::band_limit_and_fade(x, fs, 16000.0);
            x[1] = x[0];  // the amp is mono: equal channels sum to exactly this
            measure::RenderOptions opt;
            opt.block = block;
            const Chans y = measure::render(*p, x, {}, opt);
            const long lag = measure::xcorr_lag(x[0], y[0], 400);
            const std::string n = rate_name(fs) + ", block " + std::to_string(block);
            s.check("latency, 48 kHz capture, " + n + ": measured - reported",
                    static_cast<double>(lag - static_cast<long>(p->latency())), 0.0,
                    lag == static_cast<long>(p->latency()), "samples", "reported " + std::to_string(p->latency()));
            const double err = delayed_error(x[0], y[0], p->latency()) / measure::peak(x[0]);
            s.check_le("identity capture: output vs input delayed by the latency / peak, " + n, err,
                       fs == 48000.0 ? s.bar("amp.exact_error") : s.bar("amp.resampled_error"), "linear");
        }
    }
}

// No capture: the input, delayed by what a 48 kHz capture would need, so
// loading one does not move PDC. Exact.
void passthrough(Suite& s) {
    for (double fs : measure::kRates) {
        auto p = make_amp();
        p->prepare(fs, 128, 2);
        const Chans x = measure::noise(fs, -6.0, 0.25, 5u);
        const Chans y = measure::render(*p, x);
        const std::uint32_t want = planned_latency(fs, 48000.0);
        s.check("no capture: reported latency is a 48 kHz capture's, " + rate_name(fs), p->latency(), want,
                p->latency() == want, "samples");
        double e = 0.0;
        for (std::size_t c = 0; c < 2; ++c) e = std::max(e, delayed_error(x[c], y[c], p->latency()));
        s.check("no capture: output is the input delayed by the latency, " + rate_name(fs), e, 0.0, e == 0.0, "linear");
    }
}

// The acceptance case: a 48 kHz LSTM in a 44.1 kHz session runs, reports
// the resampler pair's latency, and plays at the level it has at 48 kHz.
void resampled_capture(Suite& s) {
    auto at441 = make_amp();
    std::string msg;
    const auto r = at441->load_nam(at::lstm(1, 8), msg);
    at441->prepare(44100.0, 128, 2);
    s.check("48 kHz capture in a 44.1 kHz session: loaded", r == Amp::LoadResult::Ok && at441->status().loaded ? 0 : 1,
            0, r == Amp::LoadResult::Ok && at441->status().loaded, "", msg);
    const std::uint32_t want = planned_latency(44100.0, 48000.0);
    s.check("48 kHz capture in a 44.1 kHz session: reported latency = the resampler pair's", at441->latency(), want,
            at441->latency() == want && want > 0, "samples", std::to_string(want) + " samples");
    s.info("48 kHz capture in a 44.1 kHz session: latency", 1000.0 * at441->latency() / 44100.0, "ms");

    auto at48 = make_amp();
    at48->load_nam(at::lstm(1, 8), msg);
    at48->prepare(48000.0, 128, 2);
    const Chans y441 = measure::render(*at441, measure::sine(44100.0, 220.0, -12.0, 1.0));
    const Chans y48 = measure::render(*at48, measure::sine(48000.0, 220.0, -12.0, 1.0));
    bool finite = true;
    for (float v : y441[0]) finite = finite && std::isfinite(v);
    s.check("44.1 kHz session: output finite", finite ? 0 : 1, 0, finite, "");
    const double l441 = measure::to_db(measure::rms(y441[0], 22050)), l48 = measure::to_db(measure::rms(y48[0], 24000));
    s.check_le("44.1 kHz session vs the capture's own 48 kHz: |level difference|", std::fabs(l441 - l48),
               s.bar("amp.resampled_level_db"), "dB", std::to_string(l48) + " dBFS at 48 kHz");
}

double sine_level_db(Amp& p, double fs) {
    const Chans y = measure::render(p, measure::sine(fs, 1000.0, -6.0, 0.5));
    return measure::to_db(measure::rms(y[0], static_cast<std::size_t>(0.2 * fs)));
}

void normalisation(Suite& s) {
    const double fs = 48000.0;
    struct Case {
        const char* what;
        bool has;
        double loudness, want;
    };
    const Case cases[] = {
        {"loudness -10 dB: trimmed to -18 dB", true, -10.0, -8.0},
        {"loudness -30 dB: trimmed to -18 dB", true, -30.0, 12.0},
        {"loudness -80 dB: trim held to +24 dB", true, -80.0, 24.0},
        {"no loudness: unity", false, 0.0, 0.0},
    };
    for (const Case& c : cases) {
        auto on = make_amp(), off = make_amp();
        std::string msg;
        on->load_capture(at::identity(fs, c.has, c.loudness), msg);
        off->load_capture(at::identity(fs, c.has, c.loudness), msg);
        measure::set(*off, "normalise", 0.0f);
        on->prepare(fs, 128, 2);
        off->prepare(fs, 128, 2);
        const double d = sine_level_db(*on, fs) - sine_level_db(*off, fs);
        s.check_le(std::string("normalise, ") + c.what + ": |measured - expected|", std::fabs(d - c.want), 0.01, "dB",
                   std::to_string(d) + " dB");
        const amp::Status st = on->status();
        s.check(std::string("normalise, ") + c.what + ": status shows it", st.trim_db, c.want,
                st.has_loudness == c.has && std::fabs(st.trim_db - c.want) < 1e-9, "dB");
    }
}

template <class Hook>
std::vector<float> run_blocks(Processor& p, const std::vector<float>& x, Hook hook) {
    std::vector<float> y(x.size());
    for (std::size_t pos = 0; pos < x.size(); pos += 128) {
        const auto m = static_cast<std::uint32_t>(std::min<std::size_t>(128, x.size() - pos));
        hook(pos / 128);
        const float* in[1] = {x.data() + pos};
        float* out[1] = {y.data() + pos};
        AudioBuffers io{in, out, nullptr, 1, 0, m};
        p.process(io, nullptr, 0, Transport{});
    }
    return y;
}

// From one capture to another mid-signal: the second at -6 dB, so the
// change is audible; identities, so before and after are exact.
void change_of_capture(Suite& s) {
    const double fs = 48000.0;
    const std::vector<float> x = measure::sine(fs, 1000.0, -6.0, 1.0, 1)[0];
    auto steady = [&](bool second) {
        auto p = make_amp();
        std::string msg;
        p->load_capture(at::identity(fs, second, -12.0), msg);
        p->prepare(fs, 128, 1);
        return run_blocks(*p, x, [](std::size_t) {});
    };
    const auto ya = steady(false), yb = steady(true);
    auto maxstep = [](const std::vector<float>& v, std::size_t from, std::size_t to) {
        double m = 0.0;
        for (std::size_t i = from + 1; i < to; ++i) m = std::max(m, static_cast<double>(std::fabs(v[i] - v[i - 1])));
        return m;
    };
    const double ref_step = std::max(maxstep(ya, 20000, 40000), maxstep(yb, 20000, 40000));
    auto p = make_amp();
    std::string msg;
    p->load_capture(at::identity(fs), msg);
    p->prepare(fs, 128, 1);
    Amp::LoadResult first = Amp::LoadResult::Refused, second = Amp::LoadResult::Ok;
    const std::size_t at = 100;
    const auto y = run_blocks(*p, x, [&](std::size_t blk) {
        if (blk != at) return;
        first = p->load_capture(at::identity(fs, true, -12.0), msg);
        second = p->load_capture(at::identity(fs, true, -12.0), msg);
    });
    s.check("a change of capture is accepted", first == Amp::LoadResult::Ok ? 0 : 1, 0, first == Amp::LoadResult::Ok, "");
    s.check("a second load before the first is taken over is Busy", second == Amp::LoadResult::Busy ? 0 : 1, 0,
            second == Amp::LoadResult::Busy, "");
    const std::size_t from = at * 128;
    double before = 0.0, after = 0.0;
    for (std::size_t i = 0; i < from; ++i) before = std::max(before, static_cast<double>(std::fabs(y[i] - ya[i])));
    for (std::size_t i = (at + 20) * 128; i < y.size(); ++i)
        after = std::max(after, static_cast<double>(std::fabs(y[i] - yb[i])));
    s.check_le("before the change: identical to the old capture alone", before, 0.0, "linear");
    s.check_le("after the change: identical to the new capture alone", after, 0.0, "linear");
    s.check_le("largest sample step across the change / steady sine's", maxstep(y, from, y.size()) / ref_step,
               s.bar("amp.change_step_ratio"), "ratio", "no click");
}

void dry_path(Suite& s) {
    const double fs = 44100.0;
    auto p = make_amp();
    std::string msg;
    p->load_nam(at::lstm(1, 8), msg);
    measure::apply(*p, {{"mix", 0.0f}});
    p->prepare(fs, 128, 2);
    const Chans x = measure::noise(fs, -6.0, 0.5, 5u);
    const Chans y = measure::render(*p, x);
    const double d = std::max(delayed_error(x[0], y[0], p->latency()), delayed_error(x[1], y[1], p->latency()));
    s.check("mix 0, 44.1 kHz session, 48 kHz capture: the input delayed by the latency", d, 0.0, d == 0.0, "linear",
            "the dry path is aligned with the wet");
}

}  // namespace

measure::Config amp_golden_config() { return {{"input", 6.0f}, {"output", -3.0f}, {"mix", 90.0f}}; }

int amp(const measure::Args& args) {
    Suite s("amp",
            [] {
                auto p = make_amp();
                std::string msg;
                p->load_nam(amp_test::lstm(1, 8), msg);
                return std::unique_ptr<Processor>(std::move(p));
            },
            args);
    latency_identity(s);
    passthrough(s);
    resampled_capture(s);
    normalisation(s);
    change_of_capture(s);
    dry_path(s);
    amp_load::refusal(s);
    amp_load::file_slot(s);
    amp_load::deferred(s);
    amp_load::concurrent_loads(s);
    const Config golden = amp_golden_config();
    measure::check_nonfinite(s, golden);
    measure::check_reset(s, golden);
    measure::check_denormals(s, golden);
    measure::check_zipper(s, {}, "");
    measure::check_golden(s, golden);
    // Reported, not held: class E is a capture and its cabinet together
    // (ADR-0011 §3.5, Amendment 1), and load time already holds the amp to it.
    measure::check_cpu(s, golden, "", false);
    return s.finish();
}

}  // namespace sonora::native::suites
