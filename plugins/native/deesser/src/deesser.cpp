// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/deesser.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/dynamics.h"
#include "sonora/dsp/math.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace deesser;

const char* const kOffOn[] = {"off", "on"};
const char* const kModes[] = {"band", "wide"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"mode", "Mode", Unit::Choice, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kModes, 2},
    {"frequency", "Frequency", Unit::Hertz, Taper::Log, 2000.0f, 16000.0f, 6500.0f, 20.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"q", "Width (Q)", Unit::Ratio, Taper::Log, 0.5f, 4.0f, 1.4f, 20.0f, true, false, false, "%.2f", nullptr, 0},
    {"threshold", "Threshold", Unit::Decibel, Taper::Linear, -60.0f, 0.0f, -30.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"ratio", "Ratio", Unit::Ratio, Taper::Log, 1.0f, 20.0f, 4.0f, 20.0f, true, false, false, "%.1f:1", nullptr, 0},
    {"knee", "Knee", Unit::Decibel, Taper::Linear, 0.0f, 24.0f, 6.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"range", "Range", Unit::Decibel, Taper::Linear, 0.0f, 40.0f, 12.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"attack", "Attack", Unit::Milliseconds, Taper::Log, 0.05f, 50.0f, 0.5f, 20.0f, true, false, false, "%.2f ms", nullptr, 0},
    {"release", "Release", Unit::Milliseconds, Taper::Log, 5.0f, 1000.0f, 60.0f, 20.0f, true, false, false, "%.0f ms", nullptr, 0},
    {"link", "Stereo link", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"output", "Output", Unit::Decibel, Taper::Linear, -12.0f, 12.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
    {"listen", "Listen", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kOffOn, 2},
};

const Descriptor kDesc = {"org.sonora.deesser", "Sonora De-esser", "Dynamics", 1, kParams, kParamCount, 1, 2, false};

constexpr std::uint32_t kControl = 16;
constexpr std::uint32_t kMaxCh = 2;
// The detector's RMS window is a few periods of the band's own centre, so
// the ripple at twice that frequency stays near 0.2 dB at any setting,
// while the lag it adds stays small against a 5 ms attack.
constexpr double kWindowPeriods = 3.0;

// 10 log10(x) for x > 0 by the atanh series on the mantissa folded to
// [sqrt(1/2), sqrt(2)): ln m = 2 (z + z^3/3 + z^5/5 + z^7/7) with
// z = (m - 1) / (m + 1), |z| < 0.172, so the series' error is under 3e-8 of
// ln m. It runs once per sample and channel on the detector's level; the
// libm log10 was a fifth of the block's cost (measured).
inline double ten_log10(double x) noexcept {
    std::uint64_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    int e = static_cast<int>((bits >> 52) & 0x7ff) - 1023;
    bits = (bits & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
    double m;
    std::memcpy(&m, &bits, sizeof m);
    if (m > 1.4142135623730951) {
        m *= 0.5;
        ++e;
    }
    const double z = (m - 1.0) / (m + 1.0);
    const double z2 = z * z;
    const double ln_m = 2.0 * z * (1.0 + z2 * (1.0 / 3.0 + z2 * (0.2 + z2 * (1.0 / 7.0))));
    constexpr double kLn2 = 0.6931471805599453;
    constexpr double kTenOverLn10 = 4.342944819032518;
    return kTenOverLn10 * (ln_m + e * kLn2);
}

class Deesser final : public ProcessorBase {
public:
    Deesser() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        update_detector(0, true);
        update_times();
        for (std::uint32_t c = 0; c < kMaxCh; ++c) design_bell(c, 0, true);
        out_gain_ = dsp::db_to_gain(smoother(kOutput).current());
        return true;
    }

    void on_reset() noexcept override {
        for (std::uint32_t c = 0; c < kMaxCh; ++c) {
            bp_[c].reset();
            bell_[c].reset();
            ms_[c].reset();
            det_[c].reset(0.0);
            red_[c] = 0.0;
            design_bell(c, 0, true);
        }
    }

    void on_param(std::uint32_t index, bool jump) noexcept override {
        if (!jump) return;
        if (index == kFrequency || index == kQ) {
            update_detector(0, true);
            for (std::uint32_t c = 0; c < kMaxCh; ++c) design_bell(c, 0, true);
        }
        if (index == kAttack || index == kRelease) update_times();
        if (index == kOutput) out_gain_ = dsp::db_to_gain(smoother(kOutput).current());
    }

    void render(const float* const* in, float* const* out, const float* const*,
                std::uint32_t, std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        const bool wide = value(kMode) >= 0.5f;
        const bool listen = value(kListen) >= 0.5f;
        for (std::uint32_t pos = 0; pos < frames; pos += kControl) {
            const std::uint32_t m = frames - pos < kControl ? frames - pos : kControl;
            const bool fq_moving = smoother(kFrequency).moving() || smoother(kQ).moving();
            if (fq_moving) {
                update_detector(m, false);
            } else {
                smoother(kFrequency).advance(m);
                smoother(kQ).advance(m);
            }
            const bool tmov = smoother(kAttack).moving() || smoother(kRelease).moving();
            smoother(kAttack).advance(m);
            smoother(kRelease).advance(m);
            if (tmov) update_times();
            // The output trim moves linearly in gain between block ends, so
            // the exp runs once per block and only while it moves.
            double out_end = out_gain_;
            if (smoother(kOutput).moving()) out_end = dsp::db_to_gain(smoother(kOutput).advance(m));
            else smoother(kOutput).advance(m);
            const double out_step = (out_end - out_gain_) / static_cast<double>(m);
            // The bell takes the reduction reached at the end of the last
            // block: a lag of at most kControl samples (0.33 ms at 48 kHz),
            // which wide mode, where the gain is per sample, does not have.
            if (!wide && !listen) {
                for (std::uint32_t c = 0; c < chs; ++c) design_bell(c, m, false, fq_moving);
            }
            for (std::uint32_t i = 0; i < m; ++i) {
                const std::uint32_t k = pos + i;
                out_gain_ += out_step;
                const double thr = smoother(kThreshold).next();
                const double ratio = smoother(kRatio).next();
                const double knee = smoother(kKnee).next();
                const double range = smoother(kRange).next();
                const double link = smoother(kLink).next() * 0.01;
                const dsp::SvfState::Derived& db = bp_ramp_.next();

                double band[kMaxCh], lvl[kMaxCh];
                double lmax = -400.0;
                for (std::uint32_t c = 0; c < chs; ++c) {
                    band[c] = bp_[c].tick(db, in[c][k]);
                    // Times two: a sine reads its peak level, as the
                    // threshold of a peak detector would.
                    lvl[c] = ten_log10(2.0 * ms_[c].process(band[c]) + 1e-30);
                    if (lvl[c] > lmax) lmax = lvl[c];
                }
                for (std::uint32_t c = 0; c < chs; ++c) {
                    const double x = link * lmax + (1.0 - link) * lvl[c];
                    double cdb = x - dsp::compressor_curve_db(x, thr, ratio, knee);
                    if (cdb > range) cdb = range;
                    red_[c] = det_[c].process(cdb);
                    const double dry = in[c][k];
                    double y;
                    if (listen) y = band[c];
                    else if (wide) y = dry * dsp::db_to_gain(-red_[c]);
                    else y = bell_[c].tick(bell_ramp_[c].next(), dry);
                    out[c][k] = static_cast<float>(y * out_gain_);
                }
            }
            out_gain_ = out_end;
        }
    }

private:
    void update_detector(std::uint32_t m, bool jump) noexcept {
        const double f = jump ? smoother(kFrequency).current() : smoother(kFrequency).advance(m);
        const double q = jump ? smoother(kQ).current() : smoother(kQ).advance(m);
        const auto c = dsp::to_svf(dsp::design_matched({dsp::Shape::BandPass, f, q, 0.0}, sample_rate()));
        if (jump) bp_ramp_.set(c);
        else bp_ramp_.ramp_to(c, m);
        const double tau = kWindowPeriods / f;
        for (auto& w : ms_) w.set_time(tau, sample_rate());
    }
    // The bell at the detector's frequency and width, cut by the reduction
    // so far. Zero reduction is a 0 dB bell, which is the identity.
    void design_bell(std::uint32_t c, std::uint32_t m, bool jump, bool force = true) noexcept {
        // A redesign costs more than the block's filtering; below 0.002 dB
        // of change (and with the frequency and width still) the last
        // design stands.
        if (!jump && !force && std::fabs(red_[c] - bell_red_[c]) < 0.002) return;
        bell_red_[c] = red_[c];
        const double f = smoother(kFrequency).current();
        const double q = smoother(kQ).current();
        const auto b = dsp::to_svf(dsp::design_matched({dsp::Shape::Peak, f, q, -red_[c]}, sample_rate()));
        if (jump) bell_ramp_[c].set(b);
        else bell_ramp_[c].ramp_to(b, m);
    }
    void update_times() noexcept {
        const double a = smoother(kAttack).current() * 1e-3;
        const double r = smoother(kRelease).current() * 1e-3;
        for (auto& d : det_) d.set_times(a, r, sample_rate());
    }

    dsp::SvfRamp bp_ramp_, bell_ramp_[kMaxCh];
    dsp::SvfState bp_[kMaxCh], bell_[kMaxCh];
    dsp::MeanSquare ms_[kMaxCh];
    dsp::BranchingSmoother det_[kMaxCh];
    double red_[kMaxCh] = {0.0, 0.0};
    double bell_red_[kMaxCh] = {0.0, 0.0};
    double out_gain_ = 1.0;
};

}  // namespace

std::unique_ptr<Processor> make_deesser() { return std::make_unique<Deesser>(); }

}  // namespace sonora::native
