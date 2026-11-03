// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/phaser.h"

#include <cmath>

#include "sonora/dsp/delay_line.h"
#include "sonora/dsp/lfo.h"
#include "sonora/dsp/math.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace phaser;

const char* const kModes[] = {"phaser", "flanger"};
const char* const kStageNames[] = {"2", "4", "6", "8", "12"};
constexpr int kStageCount[] = {2, 4, 6, 8, 12};
const char* const kShapes[] = {"sine", "triangle"};

// Depth 100 % sweeps the centre two octaves either way.
constexpr double kOctaves = 2.0;

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"mode", "Mode", Unit::Choice, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kModes, 2},
    {"rate", "Rate", Unit::Hertz, Taper::Log, 0.05f, 10.0f, 0.5f, 20.0f, true, false, false, "%.2f Hz", nullptr, 0},
    {"depth", "Depth", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 60.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"freq", "Frequency", Unit::Hertz, Taper::Log, 50.0f, 5000.0f, 700.0f, 20.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"delay", "Delay", Unit::Milliseconds, Taper::Log, 0.2f, 10.0f, 1.5f, 20.0f, true, false, false, "%.2f ms", nullptr, 0},
    {"stages", "Stages", Unit::Choice, Taper::Stepped, 0.0f, 4.0f, 1.0f, 0.0f, false, false, true, "%.0f", kStageNames, 5},
    {"spread", "Stereo spread", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 50.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"feedback", "Feedback", Unit::Percent, Taper::Linear, -85.0f, 85.0f, 30.0f, 20.0f, true, false, false, "%+.0f %%", nullptr, 0},
    {"shape", "LFO shape", Unit::Choice, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kShapes, 2},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 50.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
};

const Descriptor kDesc = {"org.sonora.phaser", "Sonora Phaser", "Modulation", 1, kParams, kParamCount, 1, 2, false};

constexpr double kMaxMs = 40.0 + 2.0;
constexpr double kMinDelaySamples = 2.0;  // Lagrange-3 reads need >= 1
constexpr int kMaxStages = 12;
constexpr int kControl = 4;

class Phaser final : public ProcessorBase {
public:
    Phaser() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        const auto n = static_cast<std::uint32_t>(std::ceil(kMaxMs * 1e-3 * sample_rate())) + 4;
        for (auto& l : line_) l.prepare(n);
        max_delay_ = n - 1;
        return true;
    }

    void on_reset() noexcept override {
        for (auto& l : line_) l.reset();
        for (auto& c : state_) {
            for (double& s : c) s = 0.0;
        }
        for (double& y : last_) y = 0.0;
        lfo_.reset(0.0);
        control_ = 0;
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        const bool flanger = value(kMode) >= 0.5f;
        const int stages = kStageCount[static_cast<int>(value(kStages))];
        const auto shape = value(kShape) >= 0.5f ? dsp::LfoShape::Triangle : dsp::LfoShape::Sine;
        const double ms = sample_rate() * 1e-3;
        const double f_max = 0.45 * sample_rate();
        for (std::uint32_t i = 0; i < frames; ++i) {
            lfo_.set_rate(smoother(kRate).next(), sample_rate());
            const double depth = smoother(kDepth).next() * 0.01 * kOctaves;
            const double freq = smoother(kFrequency).next();
            const double delay = smoother(kDelay).next() * ms;
            const double spread = smoother(kSpread).next() * 0.005;  // 100 % = half a cycle
            const double fb = smoother(kFeedback).next() * 0.01;
            const double mix = smoother(kMix).next() * 0.01;
            for (std::uint32_t c = 0; c < chs; ++c) {
                double sweep = 1.0;
                if (flanger || control_ == 0) sweep = std::exp2(depth * lfo_.value(shape, c == 1 ? spread : 0.0));
                const double x = in[c][i];
                double wet;
                if (flanger) {
                    double d = delay * sweep;
                    if (d < kMinDelaySamples) d = kMinDelaySamples;
                    if (d > max_delay_) d = max_delay_;
                    // Read before the push: a delay of d samples is d - 1 back.
                    wet = line_[c].read_lagrange3(d - 1.0);
                    line_[c].push(static_cast<float>(x + fb * wet));
                } else {
                    // The coefficient is recomputed every kControl samples:
                    // the sweep is at most 10 Hz, and the tan and exp2 are
                    // the cost that pushes the per-block time over class A.
                    if (control_ == 0) {
                        double f = freq * sweep;
                        if (f > f_max) f = f_max;
                        const double t = std::tan(dsp::kPi * f / sample_rate());
                        coeff_[c] = (t - 1.0) / (t + 1.0);
                    }
                    const double a = coeff_[c];
                    // The previous output closes the loop, so it has one
                    // sample of delay and |fb| < 1 keeps it stable.
                    double v = x + fb * last_[c];
                    for (int k = 0; k < stages; ++k) {
                        const double y = a * v + state_[c][k];
                        state_[c][k] = v - a * y;
                        v = y;
                    }
                    last_[c] = v;
                    wet = v;
                }
                out[c][i] = static_cast<float>(x + mix * (wet - x));
            }
            lfo_.advance();
            control_ = (control_ + 1) % kControl;
        }
    }

private:
    double state_[2][kMaxStages] = {};
    double last_[2] = {};
    double coeff_[2] = {};
    int control_ = 0;
    dsp::DelayLine line_[2];
    dsp::Lfo lfo_;
    double max_delay_ = 1.0;
};

}  // namespace

std::unique_ptr<Processor> make_phaser() { return std::make_unique<Phaser>(); }

}  // namespace sonora::native
