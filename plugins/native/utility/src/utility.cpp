// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/utility.h"

#include <cmath>

#include "sonora/dsp/math.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace utility;

const char* const kOffOn[] = {"off", "on"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"gain", "Gain", Unit::Decibel, Taper::Linear, -72.0f, 24.0f, 0.0f, 20.0f, true, false, false, "%.2f dB", nullptr, 0},
    {"polarity_l", "Invert left", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 10.0f, true, false, false, "%.0f", kOffOn, 2},
    {"polarity_r", "Invert right", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 10.0f, true, false, false, "%.0f", kOffOn, 2},
    {"swap", "Swap left and right", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 10.0f, true, false, false, "%.0f", kOffOn, 2},
    {"balance", "Balance", Unit::Percent, Taper::Linear, -100.0f, 100.0f, 0.0f, 20.0f, true, false, false, "%+.0f %%", nullptr, 0},
    {"width", "Width", Unit::Percent, Taper::Linear, 0.0f, 200.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"mono", "Mono sum", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 10.0f, true, false, false, "%.0f", kOffOn, 2},
    {"dc_block", "DC blocker", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 10.0f, true, false, false, "%.0f", kOffOn, 2},
};

const Descriptor kDesc = {"org.sonora.utility", "Sonora Utility", "Utility", 1, kParams, kParamCount, 1, 2, false};

// Below the audible band, above the drift of a mic-pre's coupling: the
// -3 dB corner of the first-order blocker.
constexpr double kDcHz = 5.0;

class Utility final : public ProcessorBase {
public:
    Utility() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        a_ = 1.0 - std::exp(-dsp::kTwoPi * kDcHz / sample_rate());
        return true;
    }
    void on_reset() noexcept override { lp_[0] = lp_[1] = 0.0; }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        for (std::uint32_t i = 0; i < frames; ++i) {
            const double g = dsp::db_to_gain(smoother(kGain).next());
            const double pl = 1.0 - 2.0 * smoother(kPolarityL).next();
            const double pr = 1.0 - 2.0 * smoother(kPolarityR).next();
            const double sw = smoother(kSwap).next();
            const double bal = smoother(kBalance).next() * 0.01;
            const double mono = smoother(kMono).next();
            const double width = smoother(kWidth).next() * 0.01 * (1.0 - mono);
            const double dc = smoother(kDcBlock).next();
            // Balance attenuates the far side only, so the centre keeps 0 dB.
            const double gl = g * (bal > 0.0 ? 1.0 - bal : 1.0);
            const double gr = g * (bal < 0.0 ? 1.0 + bal : 1.0);

            double x[2] = {in[0][i], chs > 1 ? in[1][i] : 0.0};
            for (std::uint32_t c = 0; c < chs; ++c) {
                lp_[c] += a_ * (x[c] - lp_[c]);
                x[c] -= dc * lp_[c];
            }
            if (chs == 1) {
                out[0][i] = static_cast<float>(x[0] * pl * g);
                continue;
            }
            const double l = x[0] * pl, r = x[1] * pr;
            const double l2 = l + sw * (r - l);
            const double r2 = r + sw * (l - r);
            const double m = 0.5 * (l2 + r2);
            const double s = 0.5 * (l2 - r2) * width;
            out[0][i] = static_cast<float>((m + s) * gl);
            out[1][i] = static_cast<float>((m - s) * gr);
        }
    }

private:
    double a_ = 0.0;
    double lp_[2] = {0.0, 0.0};
};

}  // namespace

std::unique_ptr<Processor> make_utility() { return std::make_unique<Utility>(); }

}  // namespace sonora::native
