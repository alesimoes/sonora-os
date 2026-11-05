// SPDX-License-Identifier: GPL-3.0-or-later
//
// The utility against its bars: unity at the defaults, gain, polarity,
// swap, balance, width and mono sum measured on noise whose channels differ,
// the DC blocker's corner, a one-channel layout, and the common checks.
#include <cmath>
#include <string>

#include "sonora/measure/measure.h"
#include "sonora/native/utility.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

constexpr double kFs = 48000.0;

// The last half of a render, after every 10-20 ms ramp has settled.
std::size_t tail_from(const Chans& y) { return y[0].size() / 2; }

Chans run(Suite& s, const Config& c, const Chans& x, double fs = kFs) {
    auto p = s.make();
    measure::apply(*p, c);
    p->prepare(fs, 128, static_cast<std::uint32_t>(x.size()));
    return measure::render(*p, x);
}

double max_diff(const std::vector<float>& a, const std::vector<float>& b, std::size_t from) {
    double d = 0.0;
    for (std::size_t i = from; i < a.size(); ++i) d = std::fmax(d, std::fabs(static_cast<double>(a[i]) - b[i]));
    return d;
}

void unity(Suite& s) {
    const Chans x = measure::noise(kFs, -6.0, 1.0, 11);
    const Chans y = run(s, {}, x);
    s.check_le("defaults: max |out - in|", std::fmax(max_diff(y[0], x[0], 0), max_diff(y[1], x[1], 0)),
               s.bar("unity_abs"), "abs");
}

void gain(Suite& s) {
    double worst = 0.0;
    for (float g : {-40.0f, -6.0f, 6.0f, 12.0f}) {
        const Chans x = measure::sine(kFs, 997.0, -30.0, 1.0);
        const Chans y = run(s, {{"gain", g}}, x);
        const std::size_t f = tail_from(y);
        for (int c = 0; c < 2; ++c) {
            worst = std::fmax(worst, std::fabs(measure::to_db(measure::rms(y[c], f) / measure::rms(x[c], f)) - g));
        }
    }
    s.check_le("gain: |measured - set|, -40 to +12 dB", worst, s.bar("gain_db"), "dB");
}

void routing(Suite& s) {
    const Chans x = measure::noise(kFs, -6.0, 1.0, 21);
    const std::size_t f = x[0].size() / 2;
    {
        const Chans y = run(s, {{"polarity_l", 1.0f}}, x);
        double e = 0.0;
        for (std::size_t i = f; i < y[0].size(); ++i) e = std::fmax(e, std::fabs(y[0][i] + x[0][i]));
        s.check_le("polarity left: max |out + in| (inverted exactly)", e, s.bar("exact_abs"), "abs");
        s.check_le("polarity left: right untouched, max |out - in|", max_diff(y[1], x[1], f), s.bar("exact_abs"), "abs");
    }
    {
        const Chans y = run(s, {{"swap", 1.0f}}, x);
        s.check_le("swap: left out is right in, max |difference|", max_diff(y[0], x[1], f), s.bar("exact_abs"), "abs");
        s.check_le("swap: right out is left in, max |difference|", max_diff(y[1], x[0], f), s.bar("exact_abs"), "abs");
    }
    {
        const Chans y = run(s, {{"mono", 1.0f}}, x);
        double e = 0.0, lr = 0.0;
        for (std::size_t i = f; i < y[0].size(); ++i) {
            e = std::fmax(e, std::fabs(y[0][i] - 0.5 * (x[0][i] + x[1][i])));
            lr = std::fmax(lr, std::fabs(y[0][i] - y[1][i]));
        }
        s.check_le("mono sum: max |out - (L + R) / 2|", e, s.bar("exact_abs"), "abs");
        s.check_le("mono sum: max |L out - R out|", lr, s.bar("exact_abs"), "abs");
    }
    {
        // Width on a signal with known mid and side: side-only is silent at 0 %.
        Chans side = x;
        for (std::size_t i = 0; i < side[0].size(); ++i) side[1][i] = -side[0][i];
        const Chans y0 = run(s, {{"width", 0.0f}}, side);
        s.check_le("width 0 %: side-only input, peak out", measure::peak(y0[0], f), s.bar("exact_abs"), "abs");
        const Chans y2 = run(s, {{"width", 200.0f}}, side);
        s.check_le("width 200 %: side doubled, max |out - 2 in|", max_diff(y2[0], [&] {
                       std::vector<float> d(side[0].size());
                       for (std::size_t i = 0; i < d.size(); ++i) d[i] = 2.0f * side[0][i];
                       return d;
                   }(), f), s.bar("exact_abs"), "abs");
        // A centred signal is untouched by any width.
        Chans mid = x;
        mid[1] = mid[0];
        const Chans ym = run(s, {{"width", 200.0f}}, mid);
        s.check_le("width 200 %: mid-only input unchanged, max |difference|", max_diff(ym[0], mid[0], f),
                   s.bar("exact_abs"), "abs");
    }
    {
        const Chans yb = run(s, {{"balance", 50.0f}}, x);
        const double l = measure::to_db(measure::rms(yb[0], f) / measure::rms(x[0], f));
        const double r = measure::to_db(measure::rms(yb[1], f) / measure::rms(x[1], f));
        s.check_le("balance +50 %: left at -6.02 dB, |error|", std::fabs(l + 6.0206), s.bar("gain_db"), "dB");
        s.check_le("balance +50 %: right unchanged, |gain|", std::fabs(r), s.bar("gain_db"), "dB");
        const Chans yf = run(s, {{"balance", -100.0f}}, x);
        s.check_le("balance -100 %: right silent, peak", measure::peak(yf[1], f), s.bar("exact_abs"), "abs");
    }
}

void dc_blocker(Suite& s) {
    for (double fs : measure::kRates) {
        const Chans x = measure::sine(fs, 5.0, -20.0, 4.0);
        const Chans y = run(s, {{"dc_block", 1.0f}}, x, fs);
        const std::size_t f = y[0].size() / 2;
        const double g = measure::to_db(measure::rms(y[0], f) / measure::rms(x[0], f));
        // First order high-pass at 5 Hz: |H| = 1/sqrt(2) at the corner.
        s.check_le("dc blocker: gain at 5 Hz is -3.01 dB, " + std::to_string(static_cast<int>(fs)) + " Hz, |error|",
                   std::fabs(g + 3.0103), s.bar("dc_corner_db"), "dB");
        const Chans hi = measure::sine(fs, 1000.0, -20.0, 1.0);
        const Chans yh = run(s, {{"dc_block", 1.0f}}, hi, fs);
        const std::size_t fh = yh[0].size() / 2;
        s.check_le("dc blocker: 1 kHz passes, " + std::to_string(static_cast<int>(fs)) + " Hz, |gain|",
                   std::fabs(measure::to_db(measure::rms(yh[0], fh) / measure::rms(hi[0], fh))),
                   s.bar("dc_corner_db"), "dB");
    }
    Chans dc = measure::silence(kFs, 4.0);
    for (auto& c : dc) for (auto& v : c) v = 0.5f;
    const Chans y = run(s, {{"dc_block", 1.0f}}, dc);
    s.check_le("dc blocker: 0.5 DC after 4 s, residue", measure::peak(y[0], y[0].size() - 100), s.bar("dc_residue"), "abs");
}

void one_channel(Suite& s) {
    const Chans x = measure::sine(kFs, 997.0, -20.0, 1.0, 1);
    const Chans y = run(s, {{"gain", -6.0f}, {"polarity_l", 1.0f}}, x);
    const std::size_t f = tail_from(y);
    double e = 0.0;
    for (std::size_t i = f; i < y[0].size(); ++i) e = std::fmax(e, std::fabs(y[0][i] + x[0][i] * std::pow(10.0, -6.0 / 20.0)));
    s.check_le("one channel: gain and polarity apply, max error", e, s.bar("gain_abs_mono"), "abs");
}

}  // namespace

measure::Config utility_golden_config() {
    return {{"gain", -3.0f}, {"polarity_r", 1.0f}, {"balance", 20.0f}, {"width", 140.0f}, {"dc_block", 1.0f}};
}

int utility(const measure::Args& args) {
    Suite s("utility", [] { return make_utility(); }, args);
    unity(s);
    gain(s);
    routing(s);
    dc_blocker(s);
    one_channel(s);
    // The zipper sweeps need a signal with a side: the right channel
    // inverted makes the tone's energy all side, so width and balance are
    // audible.
    const Config zipper = {{"polarity_r", 1.0f}};
    measure::check_common(s, utility_golden_config(), {{"mono sum", {{"mono", 1.0f}}}}, &zipper);
    return s.finish();
}

}  // namespace sonora::native::suites
