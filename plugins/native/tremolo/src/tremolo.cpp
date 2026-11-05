// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/tremolo.h"

#include <cmath>

#include "sonora/dsp/lfo.h"
#include "sonora/dsp/math.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace tremolo;

const char* const kModes[] = {"tremolo", "auto-pan"};
const char* const kShapes[] = {"sine", "triangle", "soft square"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"mode", "Mode", Unit::Choice, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kModes, 2},
    {"rate", "Rate", Unit::Hertz, Taper::Log, 0.1f, 20.0f, 4.0f, 20.0f, true, false, false, "%.2f Hz", nullptr, 0},
    {"depth", "Depth", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 50.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"shape", "LFO shape", Unit::Choice, Taper::Stepped, 0.0f, 2.0f, 0.0f, 0.0f, false, false, true, "%.0f", kShapes, 3},
    {"spread", "Stereo spread", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 0.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
};

const Descriptor kDesc = {"org.sonora.tremolo", "Sonora Tremolo", "Modulation", 1, kParams, kParamCount, 1, 2, false};

constexpr double kSqrt2 = 1.41421356237309504880;
// tanh(4 x) / tanh(4): a square with a rise of about a tenth of a cycle.
constexpr double kSoftNorm = 1.0 / 0.99932929973906704;  // tanh(4)

double lfo_at(int shape, double phase) noexcept {
    if (shape == 2) return std::tanh(4.0 * dsp::Lfo::shape_at(dsp::LfoShape::Sine, phase)) * kSoftNorm;
    return dsp::Lfo::shape_at(shape == 1 ? dsp::LfoShape::Triangle : dsp::LfoShape::Sine, phase);
}

class Tremolo final : public ProcessorBase {
public:
    Tremolo() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override { return true; }
    void on_reset() noexcept override { lfo_.reset(0.0); }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        const bool pan = value(kMode) >= 0.5f;
        const int shape = static_cast<int>(value(kShape));
        for (std::uint32_t i = 0; i < frames; ++i) {
            lfo_.set_rate(smoother(kRate).next(), sample_rate());
            const double depth = smoother(kDepth).next() * 0.01;
            const double spread = smoother(kSpread).next() * 0.005;  // 100 % = half a cycle
            const double ph = lfo_.phase();
            if (pan && chs == 2) {
                const double p = depth * lfo_at(shape, ph);
                const double a = (p + 1.0) * dsp::kPi * 0.25;
                out[0][i] = static_cast<float>(in[0][i] * (kSqrt2 * std::cos(a)));
                out[1][i] = static_cast<float>(in[1][i] * (kSqrt2 * std::sin(a)));
            } else {
                for (std::uint32_t c = 0; c < chs; ++c) {
                    const double u = lfo_at(shape, ph + (c == 1 ? spread : 0.0));
                    const double g = 1.0 - depth * 0.5 * (1.0 - u);
                    out[c][i] = static_cast<float>(in[c][i] * g);
                }
            }
            lfo_.advance();
        }
    }

private:
    dsp::Lfo lfo_;
};

}  // namespace

std::unique_ptr<Processor> make_tremolo() { return std::make_unique<Tremolo>(); }

}  // namespace sonora::native
