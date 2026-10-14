// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/opto.h"

#include <cmath>
#include <vector>

#include "sonora/dsp/delay_line.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/opto.h"
#include "sonora/dsp/oversampling.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace opto;

const char* const kOffOn[] = {"off", "on"};
const char* const kModes[] = {"compress", "limit"};
const char* const kQualities[] = {"eco (no latency)", "standard (4x)", "high (8x)"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    // 70: a few dB of levelling on material peaking near -12 dBFS.
    {"peak_reduction", "Peak reduction", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 70.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"gain", "Gain", Unit::Decibel, Taper::Linear, -12.0f, 40.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
    // Smoothed rather than declicked: R7 moves in over 20 ms, so the
    // attenuator's gain changes continuously.
    {"mode", "Mode", Unit::Choice, Taper::Stepped, 0.0f, 1.0f, 0.0f, 20.0f, true, false, false, "%.0f", kModes, 2},
    {"emphasis", "Side-chain emphasis", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 0.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"link", "Stereo link", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 1.0f, 20.0f, true, false, false, "%.0f", kOffOn, 2},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"external_sc", "External key", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kOffOn, 2},
    {"quality", "Quality", Unit::Choice, Taper::Stepped, 0.0f, 2.0f, 1.0f, 0.0f, false, true, false, "%.0f", kQualities, 3},
};

const Descriptor kDesc = {"org.sonora.opto", "Sonora Opto", "Dynamics", 1, kParams, kParamCount, 1, 2, true};

// The peak reduction law and the pre-emphasis coefficients are worked out
// at this rate (base-rate samples); the cell has milliseconds of inertia,
// so 16 samples of staircase in its drive never reach the audio as a step.
constexpr std::uint32_t kControl = 16;
constexpr std::uint32_t kMaxCh = 2;

class Opto final : public ProcessorBase {
public:
    Opto() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        long q = std::lround(value(kQuality));
        q = q < 0 ? 0 : (q > 2 ? 2 : q);
        factor_ = kFactors[q];
        const std::uint32_t chs = channels();
        latency_ = 0;
        if (factor_ > 1) {
            // The input's channels, then the key's: both run at the high
            // rate, so a keyed side chain sees what an unkeyed one would.
            // The protected band is 20 kHz at every rate, as in the tube.
            const double passband = std::fmin(0.4535, 20000.0 / sample_rate());
            if (!os_.prepare(factor_, 2 * chs, max_frames(), passband)) return false;
            latency_ = os_.latency();
        }
        set_latency(latency_);
        for (auto& ch : ch_) ch.prepare(sample_rate() * factor_);
        for (std::uint32_t c = 0; c < chs; ++c) {
            dry_[c].prepare(latency_ + 1);
            // The base-rate copy eco processes in place; the oversampler
            // holds the high-rate one.
            buf_[c].assign(factor_ == 1 ? max_frames() : 0, 0.0);
        }
        wet_.assign(max_frames(), 0.0f);
        gain_.assign(max_frames(), 0.0);
        limit_.assign(max_frames(), 0.0);
        link_.assign(max_frames(), 0.0);
        mix_.assign(max_frames(), 0.0);
        last_pr_ = -1.0;
        update_controls(true);
        return true;
    }

    void on_reset() noexcept override {
        os_.reset();
        for (auto& ch : ch_) ch.reset();
        for (auto& d : dry_) d.reset();
    }

    void render(const float* const* in, float* const* out, const float* const* sc, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        const bool keyed = value(kExternalSc) >= 0.5f;
        const double fsv = ch_[0].full_scale_v();
        const std::uint32_t f = factor_;
        // The input, and the key when it is used, at the cell's rate.
        double* x[kMaxCh] = {nullptr, nullptr};
        const double* key[kMaxCh] = {nullptr, nullptr};
        for (std::uint32_t c = 0; c < chs; ++c) {
            if (f > 1) {
                x[c] = os_.up(c, in[c], frames);
                // Upsampled whether or not it is used, so switching the
                // key on never starts the side chain from a stale history.
                key[c] = os_.up(chs + c, sc[c], frames);
            } else {
                double* b = buf_[c].data();
                for (std::uint32_t k = 0; k < frames; ++k) b[k] = in[c][k];
                x[c] = b;
            }
        }
        // The smoothers advance once per base-rate sample.
        for (std::uint32_t k = 0; k < frames; ++k) {
            gain_[k] = dsp::db_to_gain(smoother(kGain).next());
            limit_[k] = smoother(kMode).next();
            link_[k] = smoother(kLink).next();
            mix_[k] = smoother(kMix).next() * 0.01;
        }
        // The cell, at the high rate; the attenuated signal in place.
        double* const* y = x;
        for (std::uint32_t pos = 0; pos < frames; pos += kControl) {
            const std::uint32_t m = frames - pos < kControl ? frames - pos : kControl;
            smoother(kPeakReduction).advance(m);
            smoother(kEmphasis).advance(m);
            update_controls(false);
            for (std::uint32_t k = pos; k < pos + m; ++k) {
                const double limit = limit_[k], link = link_[k];
                for (std::uint32_t c = 0; c < chs; ++c) ch_[c].set_limit(limit);
                for (std::uint32_t j = k * f; j < (k + 1) * f; ++j) {
                    double g[kMaxCh], grid[kMaxCh];
                    for (std::uint32_t c = 0; c < chs; ++c) {
                        double tap;
                        ch_[c].attenuate(g[c], tap);
                        // The key through the same attenuator share as
                        // the tap (opto.h, External key).
                        const double v = (keyed ? (f > 1 ? key[c][j] : sc[c][k]) : y[c][j]) * tap;
                        grid[c] = ch_[c].sidechain(v * fsv);
                    }
                    if (chs == 2) {
                        // Linked, the two grids are joined: each sees the mean.
                        const double mean = 0.5 * (grid[0] + grid[1]);
                        grid[0] += link * (mean - grid[0]);
                        grid[1] += link * (mean - grid[1]);
                    }
                    for (std::uint32_t c = 0; c < chs; ++c) {
                        ch_[c].drive(grid[c]);
                        y[c][j] *= g[c];
                    }
                }
            }
        }
        for (std::uint32_t c = 0; c < chs; ++c) {
            if (f > 1) os_.down(c, wet_.data(), frames);
            for (std::uint32_t k = 0; k < frames; ++k) {
                // The dry signal delayed by the oversampler's latency, so
                // the mix never combs.
                dry_[c].push(in[c][k]);
                const double dry = dry_[c].read(latency_);
                const double w = (f > 1 ? static_cast<double>(wet_[k]) : y[c][k]) * gain_[k];
                out[c][k] = static_cast<float>(dry + mix_[k] * (w - dry));
            }
        }
    }

private:
    void update_controls(bool jump) noexcept {
        const double pr = jump ? smoother(kPeakReduction).target() : smoother(kPeakReduction).current();
        const double em = jump ? smoother(kEmphasis).target() : smoother(kEmphasis).current();
        if (pr != last_pr_) {
            last_pr_ = pr;
            const double share = dsp::peak_reduction_share(pr * 0.01);
            for (auto& ch : ch_) ch.set_peak_share(share);
        }
        for (auto& ch : ch_) ch.set_r37(em * 0.01 * ch.circuit().r37_max);
    }

    dsp::Oversampler os_;
    dsp::OptoChannel ch_[kMaxCh];
    dsp::DelayLine dry_[kMaxCh];
    std::vector<double> buf_[kMaxCh];
    std::vector<float> wet_;
    std::vector<double> gain_, limit_, link_, mix_;
    std::uint32_t factor_ = 4;
    std::uint32_t latency_ = 0;
    double last_pr_ = -1.0;
};

}  // namespace

std::unique_ptr<Processor> make_opto() { return std::make_unique<Opto>(); }

}  // namespace sonora::native
