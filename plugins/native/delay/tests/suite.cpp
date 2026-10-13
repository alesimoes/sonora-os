// SPDX-License-Identifier: GPL-3.0-or-later
//
// The delay against its bars: echoes exact to the sample for milliseconds,
// note values and host tempo; feedback gain; ping-pong; stability at the
// top of the feedback range; THD+N; and the common checks.
#include <cmath>
#include <cstdlib>
#include <string>

#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/delay.h"
#include "sonora/native/processor_base.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

// The first sample after `from` that is not exactly zero. With mix at 100 %
// the output is the wet signal alone, and the loop filters are causal with
// b0 != 0, so the first non-zero sample is the echo's arrival.
long first_nonzero(const std::vector<float>& y, std::size_t from) {
    for (std::size_t i = from; i < y.size(); ++i) {
        if (y[i] != 0.0f) return static_cast<long>(i);
    }
    return -1;
}

void exact_times(Suite& s) {
    struct Case {
        std::string name;
        Config cfg;
        double seconds;
        double host_bpm;  // 0: no host tempo
    };
    const std::vector<Case> cases = {
        {"250 ms", {{"sync", 0.0f}, {"time_l", 250.0f}}, 0.25, 0.0},
        {"1/4 at 120 BPM", {{"note_l", 11.0f}}, 0.5, 0.0},
        {"1/8 dotted at 120 BPM", {{"note_l", 10.0f}}, 0.375, 0.0},
        {"1/8 triplet at 120 BPM", {{"note_l", 6.0f}}, 0.5 / 3.0, 0.0},
        {"1/16 at 96 BPM", {{"note_l", 5.0f}, {"tempo", 96.0f}}, 0.25 * 60.0 / 96.0, 0.0},
        {"1/4 at host tempo 100 BPM (parameter says 120)", {{"note_l", 11.0f}}, 0.6, 100.0},
        // The longest note at the slowest tempo the parameter allows: the
        // line is sized for it, not for the 4 s of the time parameter.
        {"1/1 at 20 BPM (the longest)", {{"note_l", 17.0f}, {"tempo", 20.0f}}, 12.0, 0.0},
        {"1/2 dotted at host tempo 20 BPM", {{"note_l", 16.0f}}, 9.0, 20.0},
    };
    for (const Case& c : cases) {
        for (double fs : measure::kRates) {
            auto p = s.make();
            measure::apply(*p, {{"mix", 100.0f}, {"feedback", 0.0f}, {"link", 1.0f}});
            measure::apply(*p, c.cfg);
            p->prepare(fs, 128, 2);
            Chans x = measure::silence(fs, c.seconds + 0.2);
            x[0][100] = x[1][100] = 1.0f;
            measure::RenderOptions opt;
            if (c.host_bpm > 0.0) {
                opt.transport.tempo_bpm = c.host_bpm;
                opt.transport.tempo_valid = true;
            }
            const Chans y = measure::render(*p, x, {}, opt);
            const long want = std::lround(c.seconds * fs);
            const long got = first_nonzero(y[0], 0) - 100;
            s.check("echo at the set time, " + c.name + ", " + std::to_string(static_cast<int>(fs)) + " Hz: error",
                    static_cast<double>(got - want), 0.0, got == want, "samples", "expected " + std::to_string(want));
        }
    }
}

// A host tempo below the range the line is sized for (20 BPM) cannot be
// followed. It is bounded at 20 BPM, never silently: every block that does
// so is counted where the host and the editor can read it.
void host_tempo_out_of_range(Suite& s) {
    const double fs = 48000.0;
    auto p = s.make();
    measure::apply(*p, {{"mix", 100.0f}, {"feedback", 0.0f}, {"link", 1.0f}, {"note_l", 17.0f}});
    p->prepare(fs, 128, 2);
    Chans x = measure::silence(fs, 12.2);
    x[0][100] = x[1][100] = 1.0f;
    measure::RenderOptions opt;
    opt.transport.tempo_bpm = 10.0;
    opt.transport.tempo_valid = true;
    const Chans y = measure::render(*p, x, {}, opt);
    const long want = std::lround(12.0 * fs);
    const long got = first_nonzero(y[0], 0) - 100;
    s.check("host tempo 10 BPM, 1/1: echo bounded at 20 BPM (12 s), error", static_cast<double>(got - want), 0.0,
            got == want, "samples", "expected " + std::to_string(want));
    const auto* base = dynamic_cast<const ProcessorBase*>(p.get());
    const double counted = base != nullptr ? static_cast<double>(base->bounded_blocks()) : 0.0;
    const double blocks = static_cast<double>((x[0].size() + 127) / 128);
    s.check("host tempo 10 BPM: every block reports the bound", counted, blocks, counted == blocks, "blocks");
    // In range, nothing is reported.
    auto q = s.make();
    measure::apply(*q, {{"note_l", 17.0f}});
    q->prepare(fs, 128, 2);
    opt.transport.tempo_bpm = 20.0;
    measure::render(*q, measure::silence(fs, 0.5), {}, opt);
    const auto* qb = dynamic_cast<const ProcessorBase*>(q.get());
    const double none = qb != nullptr ? static_cast<double>(qb->bounded_blocks()) : -1.0;
    s.check("host tempo 20 BPM: nothing reported", none, 0.0, none == 0.0, "blocks");
}

void feedback_and_pingpong(Suite& s) {
    const double fs = 48000.0;
    {
        // A 1 kHz burst; each repeat is the previous one times the feedback
        // (the loop filters at 20 Hz and 20 kHz are flat at 1 kHz).
        auto p = s.make();
        measure::apply(*p, {{"sync", 0.0f}, {"time_l", 100.0f}, {"link", 1.0f}, {"mix", 100.0f}, {"feedback", 50.0f}});
        p->prepare(fs, 128, 2);
        Chans x = measure::sine(fs, 1000.0, -6.0, 0.02);
        measure::band_limit_and_fade(x, fs);
        x = measure::concat({x, measure::silence(fs, 0.5)});
        const Chans y = measure::render(*p, x);
        const auto n = static_cast<std::size_t>(0.1 * fs);
        const double e1 = measure::peak(y[0], n, 2 * n), e2 = measure::peak(y[0], 2 * n, 3 * n);
        s.check_le("feedback 50 %: second repeat / first, |error|", std::fabs(measure::to_db(e2 / e1) - measure::to_db(0.5)),
                   0.05, "dB");
    }
    {
        // Ping-pong: an impulse on the left only comes back left, right,
        // left, at L, L + R, 2 L + R.
        auto p = s.make();
        measure::apply(*p, {{"sync", 0.0f}, {"time_l", 100.0f}, {"time_r", 150.0f}, {"pingpong", 1.0f},
                            {"mix", 100.0f}, {"feedback", 50.0f}});
        p->prepare(fs, 128, 2);
        Chans x = measure::silence(fs, 0.6);
        x[0][0] = 1.0f;
        const Chans y = measure::render(*p, x);
        const long l = std::lround(0.1 * fs), r = std::lround(0.15 * fs);
        // The loop filters ring, so a later echo is found as the largest
        // sample near where it is due, not as the first non-zero one.
        auto argmax = [&](const std::vector<float>& v, long around) {
            long best = around - 200;
            for (long i = around - 200; i <= around + 200; ++i) {
                if (std::fabs(v[i]) > std::fabs(v[best])) best = i;
            }
            return best;
        };
        const long l1 = first_nonzero(y[0], 0);
        const long r1 = argmax(y[1], l + r), l2 = argmax(y[0], 2 * l + r);
        const bool ok = l1 == l && std::labs(r1 - (l + r)) <= 2 && std::labs(l2 - (2 * l + r)) <= 2 &&
                        first_nonzero(y[1], 0) == l + r;
        s.check("ping-pong: left, right, left at L, L + R, 2L + R", ok ? 0.0 : 1.0, 0.0, ok, "",
                std::to_string(l1) + ", " + std::to_string(r1) + ", " + std::to_string(l2));
    }
    {
        auto p = s.make();
        measure::apply(*p, {{"sync", 0.0f}, {"time_l", 30.0f}, {"time_r", 47.0f}, {"feedback", 95.0f}, {"mix", 100.0f}});
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, measure::noise(fs, -6.0, 10.0, 4u));
        s.check_le("feedback 95 % on 10 s of noise: peak output", measure::peak(y[0]), 20.0, "linear",
                   "bounded: the loop gain is under 1");
    }
    {
        auto p = s.make();
        measure::apply(*p, {{"sync", 0.0f}, {"time_l", 10.0f}, {"link", 1.0f}, {"mix", 100.0f}, {"feedback", 0.0f}});
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, measure::sine(fs, 1000.0, -1.0, 1.2));
        s.check_le("THD+N at -1 dBFS, 1 kHz, wet only", measure::thdn_db(y[0], 19200, fs, 1000.0),
                   s.bar("delay.thdn_db"), "dB");
    }
}

}  // namespace

// The golden configuration, shared with the host tests (sonora-dsp-measure
// --host-fixture): the bundles are rendered through Sonora's hosts with it.
measure::Config delay_golden_config() {
    return {{"note_l", 8.0f}, {"note_r", 7.0f}, {"tempo", 140.0f}, {"feedback", 40.0f},
            {"hpf", 150.0f}, {"lpf", 6000.0f}, {"mix", 40.0f}};
}

int delay(const measure::Args& args) {
    Suite s("delay", [] { return make_delay(); }, args);
    exact_times(s);
    host_tempo_out_of_range(s);
    feedback_and_pingpong(s);
    // Every automatable parameter is swept twice (quality.json "zipper"):
    // with sync off below, where the times are heard, and here with sync
    // on, where the note values and the tempo are. Each change of time is
    // a crossfade, never a click.
    measure::check_zipper(s, {{"mix", 50.0f}, {"feedback", 50.0f}}, ", synced");
    const Config golden = delay_golden_config();
    const Config zipper = {{"sync", 0.0f}, {"mix", 50.0f}, {"feedback", 50.0f}};
    measure::check_common(s, golden,
                          {{"dry path, mix 0", {{"mix", 0.0f}}}, {"dry path, ping-pong, mix 0", {{"mix", 0.0f}, {"pingpong", 1.0f}}}},
                          &zipper);
    return s.finish();
}

}  // namespace sonora::native::suites
