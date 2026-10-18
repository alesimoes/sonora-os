// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FET gain-reduction stage and its side chain: invariants that need
// nothing but common/dsp. The behaviour against the published figures and
// against ngspice is the FET processor's suite (plugins/native/fet/tests/
// suite.cpp), which reads the committed SPICE reference.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>

#include "check.h"
#include "sonora/dsp/fet.h"
#include "sonora/dsp/math.h"

namespace {

using sonora::dsp::FetChannel;
using sonora::dsp::FetCircuit;
using sonora::dsp::JfetParams;
using sonora::dsp::kFetAllButtons;
using sonora::dsp::kTwoPi;

// [SH] written out directly, as SPICE's JFET level 1: normal mode for
// vds >= 0, the terminals swapped for vds < 0. The model's channel_current
// takes the LN gate (vg0 + vds / 2) and a symmetric form; both must agree.
double spice_jfet(const JfetParams& j, double vgs, double vgd, double vds) {
    if (vds >= 0.0) {
        const double vgst = vgs - j.vto;
        if (vgst <= 0.0) return 0.0;
        const double betap = j.beta * (1.0 + j.lambda * vds);
        if (vgst <= vds) return betap * vgst * vgst;
        return betap * vds * (2.0 * vgst - vds);
    }
    const double vgdt = vgd - j.vto;
    if (vgdt <= 0.0) return 0.0;
    const double betap = j.beta * (1.0 - j.lambda * vds);
    if (vgdt <= -vds) return -betap * vgdt * vgdt;
    return betap * vds * (2.0 * vgdt + vds);
}

int test_channel_law() {
    const JfetParams j;
    double worst = 0.0, worst_slope = 0.0;
    for (double vg0 : {-1.5, -1.37, -1.3, -1.0, -0.5, 0.0, 0.5}) {
        for (double vds = -2.0; vds <= 2.0; vds += 0.0137) {
            const double vg = vg0 + 0.5 * vds;
            const double want = spice_jfet(j, vg, vg - vds, vds);
            double slope;
            const double got = FetChannel::channel_current(j, vg0, vds, &slope);
            worst = std::fmax(worst, std::fabs(got - want));
            // The slope against a central difference of the same law.
            const double h = 1e-7;
            const double num = (FetChannel::channel_current(j, vg0, vds + h) -
                                FetChannel::channel_current(j, vg0, vds - h)) / (2.0 * h);
            worst_slope = std::fmax(worst_slope, std::fabs(slope - num) / (std::fabs(num) + 1e-9));
            CHECK(FetChannel::channel_current(j, vg0, -vds) == -got);
        }
    }
    std::printf("  channel law against SPICE's level 1 with the LN gate: worst %.3g A, slope %.3g\n", worst, worst_slope);
    CHECK_LE(worst, 1e-15, "channel current error, A");
    CHECK_LE(worst_slope, 1e-4, "channel slope, relative");
    return 0;
}

// The LN circuit's point: with the gate at vg0 + vds / 2 the ohmic channel
// is a resistor (but for lambda), so the divider's small-signal gain does
// not depend on the level or the sign.
int test_ln_linear() {
    FetChannel ch;
    ch.prepare(192000.0);
    const double g = ch.drain(1e-4) / 1e-4;
    CHECK(std::fabs(g - ch.rest_gain()) <= 1e-6 * ch.rest_gain());
    double worst = 0.0;
    for (double v : {1e-4, 3e-4, 1e-3, -1e-3, 2e-3}) worst = std::fmax(worst, std::fabs(ch.drain(v) / v / g - 1.0));
    std::printf("  divider at rest, 0.1-2 mV either sign: gain %.6f (rest loss %.4f dB), worst deviation %.3g\n", g,
                -20.0 * std::log10(g), worst);
    CHECK_LE(worst, 1e-5, "LN divider linearity at rest");
    return 0;
}

// [M]'s "Q" bias: the divider loses rest_loss_db at rest, at every ratio,
// attack and release (the rest point is solved with the rectifiers'
// leakage), and silence leaves the states where they are.
int test_rest() {
    double worst = 0.0, drift = 0.0;
    for (double fs : {44100.0, 192000.0}) {
        for (double pos : {0.0, 1.0, 2.0, 3.0, 4.0}) {
            for (double r56 : {0.0, 2.5e6, 5e6}) {
                for (double r55 : {0.0, 25e3}) {
                    FetChannel ch;
                    ch.prepare(fs);
                    ch.set_ratio(pos);
                    ch.set_attack_ohms(r55);
                    ch.set_release_ohms(r56);
                    ch.reset();
                    const double vc0 = ch.control_v();
                    for (int i = 0; i < static_cast<int>(fs * 2.0); ++i) CHECK(ch.tick(0.0) == 0.0);
                    drift = std::fmax(drift, std::fabs(ch.control_v() - vc0));
                    worst = std::fmax(worst, std::fabs(ch.gain_reduction_db()));
                }
            }
        }
    }
    std::printf("  rest: |GR| after 2 s of silence %.3g dB, control voltage drift %.3g V\n", worst, drift);
    CHECK_LE(worst, 1e-6, "rest gain reduction, dB");
    CHECK_LE(drift, 1e-9, "rest drift, V");
    return 0;
}

// The drain solve: the root of (vin - vd) / R5 = I(vd) in [0, vin] for
// any gate and level, saturated region included.
int test_drain_solve() {
    const FetCircuit c;
    const JfetParams j;
    double worst = 0.0;
    FetChannel ch;
    ch.prepare(48000.0);
    for (int i = 0; i < 48000; ++i) ch.tick(0.02 * std::sin(kTwoPi * 1000.0 * i / 48000.0));
    for (double vin = -3.0; vin <= 3.0; vin += 0.00731) {
        const double vd = ch.drain(vin);
        CHECK(std::fabs(vd) <= std::fabs(vin));
        const double vg0 = std::fmin(0.5 * (ch.control_v() + ch.c9_v()), c.gate_max_v);
        const double r = (vin - vd) / c.r5 - FetChannel::channel_current(j, vg0, vd);
        // Relative to R5's current: the solve's tolerance is relative.
        if (vin != 0.0) worst = std::fmax(worst, std::fabs(r) * c.r5 / std::fabs(vin));
    }
    std::printf("  drain solve residual over +-3 V, relative to R5's current: %.3g\n", worst);
    CHECK_LE(worst, 1e-9, "drain residual, relative");
    // The joint solve's drain is the divider's at the state it leaves: the
    // drain and the gate belong to the same instant (no loop delay).
    double joint = 0.0;
    int most = 0;
    FetChannel d;
    d.prepare(192000.0);
    d.set_ratio(3.0);
    for (int i = 0; i < 19200; ++i) {
        const double vin = (i < 9600 ? 0.01 : 0.5) * std::sin(kTwoPi * 997.0 * i / 192000.0);
        d.step(vin);
        if (vin != 0.0) joint = std::fmax(joint, std::fabs(d.drain(vin) - d.loop_drain()) / std::fabs(vin));
        most = d.last_iterations() > most ? d.last_iterations() : most;
    }
    std::printf("  joint solve: |drain(state after) - solved drain| / vin %.3g; at most %d iterations\n", joint, most);
    // The solve stops on a 1e-5 correction (fet.cpp): what remains is the
    // square of that, scaled by the channel's curvature.
    CHECK_LE(joint, 3e-8, "joint solve consistency, relative");
    CHECK(most < 60);
    return 0;
}

// [M] Fig. 5 as read: the side-chain tap rises and the bias offset rises
// with the ratio ("higher ratios also set the threshold higher"); all four
// in takes the top of R22 and R76's tap.
int test_ratio_network() {
    const FetCircuit c;
    double prev_tap = 0.0, prev_off = 0.0;
    for (int p = 0; p < kFetAllButtons; ++p) {
        double tap, off;
        FetChannel::ratio_network(c, p, tap, off);
        CHECK(tap > prev_tap && off > prev_off);
        prev_tap = tap;
        prev_off = off;
    }
    double tap, off;
    FetChannel::ratio_network(c, 0.0, tap, off);
    CHECK(std::fabs(tap - 47.0 / 228.0) <= 1e-12);
    FetChannel::ratio_network(c, 3.0, tap, off);
    CHECK(std::fabs(tap - 227.0 / 228.0) <= 1e-12);
    FetChannel::ratio_network(c, kFetAllButtons, tap, off);
    CHECK(std::fabs(tap - 47.0 / 48.0) <= 1e-12);
    CHECK(std::fabs(off - 10.0 / (10e3 + 150.0 + 1.5e3) * 150.0) <= 1e-12);
    return 0;
}

// Any input, however hot, keeps every state finite and the reduction
// bounded by the gate's forward limit; a NaN is not let into a state by
// the processor (it sanitises), but the channel's bounds hold for +-1e6 V.
int test_bounded() {
    for (double pos : {0.0, 3.0, 4.0}) {
        FetChannel ch;
        ch.prepare(192000.0);
        ch.set_ratio(pos);
        for (double a : {1.0, 100.0, 1e6}) {
            for (int i = 0; i < 19200; ++i) {
                const double v = ch.tick(i % 40 < 20 ? a : -a);
                CHECK(std::isfinite(v) && std::fabs(v) <= a);
                // Every step converges inside the Newton cap.
                CHECK(ch.last_iterations() < 60);
            }
            CHECK(std::isfinite(ch.control_v()) && std::isfinite(ch.c9_v()));
        }
        CHECK(ch.cap_hits() == 0);
        const FetCircuit& c = ch.circuit();
        const double g = 2.0 * ch.jfet().beta * (c.gate_max_v - ch.jfet().vto);
        const double deepest = 20.0 * std::log10(ch.rest_gain() * (1.0 + c.r5 * g));
        CHECK(ch.gain_reduction_db() <= deepest + 1e-9);
        std::printf("  hot squares, ratio position %.0f: reduction %.2f dB (gate limit %.2f dB)\n", pos,
                    ch.gain_reduction_db(), deepest);
    }
    // The same for a linked pair (the four-unknown solve), in phase, in
    // anti-phase and with one channel silent, every rate a quality runs at.
    int most = 0;
    for (double fs : {44100.0, 192000.0, 768000.0}) {
        for (int mode = 0; mode < 3; ++mode) {
            FetChannel l, r;
            l.prepare(fs);
            r.prepare(fs);
            l.set_ratio(4.0);
            r.set_ratio(4.0);
            for (double a : {1.0, 100.0, 1e6}) {
                for (int i = 0; i < 4410; ++i) {
                    const double v = i % 40 < 20 ? a : -a;
                    FetChannel::step_pair(l, r, v, mode == 0 ? v : (mode == 1 ? -v : 0.0), 1.0);
                    CHECK(std::isfinite(l.loop_drain()) && std::isfinite(r.loop_drain()));
                    CHECK(l.last_iterations() < 60 && r.last_iterations() < 60);
                    most = l.last_iterations() > most ? l.last_iterations() : most;
                }
            }
            CHECK(l.cap_hits() == 0 && r.cap_hits() == 0);
        }
    }
    std::printf("  hot squares, linked pair: at most %d iterations, no cap reached\n", most);

    // The input is bounded at R5 (input_limit_v): a drive past it solves as
    // the limit does. With the limit lifted, drives that overflowed the
    // solve before the saturated start (from about 3e16 V, RT audit of
    // #74, N1) stay finite; and a step that does go non-finite (forced
    // here with a NaN device parameter, which reset() does not use) is
    // never committed: the channels reset to rest and the step is counted.
    {
        FetChannel at, past;
        at.prepare(192000.0);
        past.prepare(192000.0);
        const double lim = at.circuit().input_limit_v;
        for (int i = 0; i < 1920; ++i) {
            const double sgn = i % 40 < 20 ? 1.0 : -1.0;
            CHECK(at.tick(sgn * lim) == past.tick(sgn * 1e9));
        }
        FetCircuit open;
        open.input_limit_v = 1e300;
        FetChannel l, r;
        l.prepare(192000.0, open);
        r.prepare(192000.0, open);
        for (double v0 : {1e17, 1e300}) {
            for (int i = 0; i < 200; ++i) {
                const double v = i % 40 < 20 ? v0 : -v0;
                FetChannel::step_pair(l, r, v, -v, 1.0);
                CHECK(std::isfinite(l.control_v()) && std::isfinite(r.control_v()));
                // Finite, though the solve may run to the cap here: the
                // bound at R5 is what keeps a processor out of this range.
                CHECK(std::isfinite(l.loop_drain()) && std::isfinite(r.loop_drain()));
            }
        }
        JfetParams bad;
        bad.lambda = std::nan("");
        FetChannel n1, n2;
        n1.prepare(192000.0, FetCircuit{}, bad);
        n2.prepare(192000.0, FetCircuit{}, bad);
        for (int i = 0; i < 100; ++i) {
            FetChannel::step_pair(n1, n2, 0.5, -0.5, 1.0);
            CHECK(std::isfinite(n1.control_v()) && std::isfinite(n2.control_v()) && std::isfinite(n1.c9_v()));
            CHECK(n1.loop_drain() == 0.0 && n2.loop_drain() == 0.0);
        }
        std::printf("  past the input limit: same as at it; 1e17 and 1e300 V with no limit finite; a forced "
                    "non-finite step reset %llu times, state finite\n",
                    static_cast<unsigned long long>(n1.cap_hits()));
        CHECK(n1.cap_hits() == 100 && n2.cap_hits() == 100);
    }
    return 0;
}

// The stereo adapter joins the control nodes: linked, a channel with no
// signal gets the same control voltage as the one with; identical inputs
// give identical results linked or not.
int test_link() {
    FetChannel a, b, c, d;
    for (FetChannel* ch : {&a, &b, &c, &d}) ch->prepare(192000.0);
    double worst_joined = 0.0, worst_same = 0.0;
    for (int i = 0; i < 192000; ++i) {
        const double x = 0.05 * std::sin(kTwoPi * 1000.0 * i / 192000.0);
        FetChannel::step_pair(a, b, x, 0.0, 1.0);
        FetChannel::step_pair(c, d, x, x, 1.0);
        if (i > 96000) worst_joined = std::fmax(worst_joined, std::fabs(a.control_v() - b.control_v()));
        worst_same = std::fmax(worst_same, std::fabs(c.control_v() - d.control_v()));
    }
    std::printf("  link: one channel driven, |Vc_a - Vc_b| %.3g V (reduction %.2f dB); identical inputs %.3g V\n",
                worst_joined, a.gain_reduction_db(), worst_same);
    CHECK(a.gain_reduction_db() > 3.0);
    CHECK_LE(worst_joined, 1e-3, "linked control voltages, V");
    CHECK_LE(worst_same, 1e-12, "identical linked channels, V");

    // [M]: linked, "the fastest attack time is doubled": one channel's
    // rectifier then charges both C22 nodes. At the slowest attack (R55
    // 25k, where the network and not C27 sets the rate) a DC step charges
    // the joined node half as fast as one channel's own. The step is large
    // enough that the drive stays at the followers' swing for the 30 us
    // compared: with 0.05 V the loop cut the single channel's drive within
    // 100 us and read 0.63.
    FetChannel alone, l1, l2;
    for (FetChannel* ch : {&alone, &l1, &l2}) {
        ch->prepare(768000.0);
        ch->set_controls(3.0, 25e3, 866e3);
        ch->reset();
    }
    const double v0 = alone.control_v();
    for (int i = 0; i < 23; ++i) {  // 30 us
        alone.step(1.0);
        FetChannel::step_pair(l1, l2, 1.0, 0.0, 1.0);
    }
    const double rate_ratio = (l1.control_v() - v0) / (alone.control_v() - v0);
    std::printf("  link: C22's rise in the first 30 us of a DC step, linked over alone: %.3f\n", rate_ratio);
    CHECK(rate_ratio >= 0.45 && rate_ratio <= 0.55);
    return 0;
}

// Where Newton stops, against the root. The solve stops on the corrections'
// size (solve() says why); here it is held against the same circuit
// iterated to round-off (set_reference_stop), so a change made for its cost
// cannot quietly trade away its accuracy (PR #74 follow-up (e) tried
// several, and measured each here). On programme: uniform noise with a log
// sweep under it, peaks about -12 dBFS (the harness's CPU signal), at the
// FET processor's golden settings (input +12 dB, 8:1, attack 0.3 ms,
// release 150 ms, linked) and its CPU worst case (+12 dB more, 20:1,
// fastest attack and release), at eco's rate and at standard's at 48 kHz,
// unlinked, and all buttons in. The drain is the audio: its departure from
// the root is held under 1e-8 of the peak drive (-160 dB), the gain
// reduction within 5e-7 dB: the stop rule as it is measures at most 5.9e-9
// and 2.5e-7 dB (one 3x looser read 4.8e-8 and 1.8e-6 dB).
int test_stop_rule() {
    struct Case {
        const char* name;
        double fs, input_db, position, attack_ms, release_ms, link;
    };
    const Case cases[] = {
        {"golden, eco rate (48 kHz), linked", 48000.0, 12.0, 1.0, 0.3, 150.0, 1.0},
        {"golden, standard rate (192 kHz), linked", 192000.0, 12.0, 1.0, 0.3, 150.0, 1.0},
        {"golden, eco rate, unlinked", 48000.0, 12.0, 1.0, 0.3, 150.0, 0.0},
        {"+12 dB, 20:1, fastest, eco rate, linked", 48000.0, 24.0, 3.0, 0.1034, 59.4, 1.0},
        {"+12 dB, all buttons, fastest, eco rate, linked", 48000.0, 24.0, 4.0, 0.1034, 59.4, 1.0},
    };
    const double seconds = dsp_quick() ? 2.0 : 8.0;
    // Every case is run and printed before any is held, so a failing run
    // still reports the figures of all of them.
    struct Result {
        double drain, gr, mean;
        std::uint64_t hits;
    };
    Result res[std::size(cases)];
    for (std::size_t ci = 0; ci < std::size(cases); ++ci) {
        const Case& k = cases[ci];
        const FetCircuit c;
        const double gin = std::pow(10.0, k.input_db / 20.0) * c.full_scale_v;
        FetChannel p[2], r[2];
        for (int i = 0; i < 2; ++i) {
            for (FetChannel* ch : {&p[i], &r[i]}) {
                ch->prepare(k.fs);
                ch->set_controls(k.position, k.attack_ms * 1e-3 / c.c22 - c.r54, k.release_ms * 1e-3 / c.c22 - c.r57);
                ch->reset();
            }
            r[i].set_reference_stop(true);
        }
        const auto n = static_cast<long>(seconds * k.fs);
        std::uint32_t seed[2] = {31u, 32u};
        double worst_drain = 0.0, worst_gr = 0.0, peak = 0.0, gr_max = 0.0;
        long iterations = 0, steps = 0;
        for (long i = 0; i < n; ++i) {
            // The sweep 20 Hz to 20 kHz over the run (Farina), the noise
            // uniform, each at -12 dBFS peak, halved together.
            const double t = static_cast<double>(i) / k.fs, len = seconds;
            const double l = std::log(20000.0 / 20.0);
            const double sweep = 0.2512 * std::sin(kTwoPi * 20.0 * len / l * (std::exp(t / len * l) - 1.0));
            double x[2];
            for (int ch = 0; ch < 2; ++ch) {
                seed[ch] ^= seed[ch] << 13;
                seed[ch] ^= seed[ch] >> 17;
                seed[ch] ^= seed[ch] << 5;
                const double noise = 0.2512 * (2.0 * (seed[ch] / 4294967295.0) - 1.0);
                x[ch] = 0.5 * (noise + sweep) * gin;
                peak = std::fmax(peak, std::fabs(x[ch]));
            }
            FetChannel::step_pair(p[0], p[1], x[0], x[1], k.link);
            FetChannel::step_pair(r[0], r[1], x[0], x[1], k.link);
            for (int ch = 0; ch < 2; ++ch) {
                worst_drain = std::fmax(worst_drain, std::fabs(p[ch].loop_drain() - r[ch].loop_drain()));
                worst_gr = std::fmax(worst_gr, std::fabs(p[ch].gain_reduction_db() - r[ch].gain_reduction_db()));
                gr_max = std::fmax(gr_max, r[ch].gain_reduction_db());
                iterations += p[ch].last_iterations();
                ++steps;
            }
        }
        const double mean = static_cast<double>(iterations) / static_cast<double>(steps);
        std::printf("  stop rule, %s: drain within %.3g of the peak drive, reduction within %.3g dB of the root "
                    "(up to %.1f dB); %.3f iterations a step\n",
                    k.name, worst_drain / peak, worst_gr, gr_max, mean);
        res[ci] = {worst_drain / peak, worst_gr, mean, p[0].cap_hits() + p[1].cap_hits() + r[0].cap_hits() + r[1].cap_hits()};
    }
    for (std::size_t ci = 0; ci < std::size(cases); ++ci) {
        CHECK(res[ci].hits == 0);
        CHECK_LE(res[ci].drain, 1e-8, "drain against the root, of the peak drive");
        CHECK_LE(res[ci].gr, 5e-7, "gain reduction against the root, dB");
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_channel_law();
    failed += test_ln_linear();
    failed += test_rest();
    failed += test_drain_solve();
    failed += test_ratio_network();
    failed += test_bounded();
    failed += test_link();
    failed += test_stop_rule();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp fet: all passed\n");
    return 0;
}
