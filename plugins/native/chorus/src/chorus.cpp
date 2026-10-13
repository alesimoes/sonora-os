// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/chorus.h"

#include <cmath>

#include "sonora/dsp/delay_line.h"
#include "sonora/dsp/lfo.h"
#include "sonora/dsp/math.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace chorus;

const char* const kShapes[] = {"sine", "triangle"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"rate", "Rate", Unit::Hertz, Taper::Log, 0.05f, 10.0f, 0.8f, 20.0f, true, false, false, "%.2f Hz", nullptr, 0},
    {"depth", "Depth", Unit::Milliseconds, Taper::Linear, 0.0f, 10.0f, 2.5f, 20.0f, true, false, false, "%.2f ms", nullptr, 0},
    {"delay", "Delay", Unit::Milliseconds, Taper::Linear, 1.0f, 30.0f, 8.0f, 50.0f, true, false, false, "%.1f ms", nullptr, 0},
    {"voices", "Voices", Unit::None, Taper::Stepped, 1.0f, 4.0f, 2.0f, 0.0f, false, false, true, "%.0f", nullptr, 0},
    {"spread", "Stereo spread", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 50.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"feedback", "Feedback", Unit::Percent, Taper::Linear, -70.0f, 70.0f, 0.0f, 20.0f, true, false, false, "%+.0f %%", nullptr, 0},
    {"shape", "LFO shape", Unit::Choice, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kShapes, 2},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 50.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
};

const Descriptor kDesc = {"org.sonora.chorus", "Sonora Chorus", "Modulation", 1, kParams, kParamCount, 1, 2, false};

constexpr double kMaxMs = 30.0 + 10.0 + 2.0;
constexpr double kMinDelaySamples = 2.0;  // Lagrange-3 reads need >= 1

class Chorus final : public ProcessorBase {
public:
    Chorus() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        const auto n = static_cast<std::uint32_t>(std::ceil(kMaxMs * 1e-3 * sample_rate())) + 4;
        for (auto& l : line_) l.prepare(n);
        max_delay_ = n - 1;
        return true;
    }

    void on_reset() noexcept override {
        for (auto& l : line_) l.reset();
        lfo_.reset(0.0);
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        const int voices = static_cast<int>(value(kVoices));
        const auto shape = value(kShape) >= 0.5f ? dsp::LfoShape::Triangle : dsp::LfoShape::Sine;
        const double ms = sample_rate() * 1e-3;
        // Voices are averaged with equal power, so adding one does not jump
        // the level.
        const double norm = 1.0 / std::sqrt(static_cast<double>(voices));
        for (std::uint32_t i = 0; i < frames; ++i) {
            lfo_.set_rate(smoother(kRate).next(), sample_rate());
            const double depth = smoother(kDepth).next() * ms;
            const double centre = smoother(kDelay).next() * ms;
            const double spread = smoother(kSpread).next() * 0.005;  // 100 % = half a cycle
            const double fb = smoother(kFeedback).next() * 0.01;
            const double mix = smoother(kMix).next() * 0.01;
            if (spread != spread_ || voices != voices_) set_offsets(voices, spread);
            // One sin and one cos per sample; each voice's phase is a
            // rotation of them: sin(t + p) = sin t cos p + cos t sin p.
            double s0 = 0.0, c0 = 0.0;
            if (shape == dsp::LfoShape::Sine) {
                s0 = std::sin(dsp::kTwoPi * lfo_.phase());
                c0 = std::cos(dsp::kTwoPi * lfo_.phase());
            }
            for (std::uint32_t c = 0; c < chs; ++c) {
                double wet = 0.0;
                double first = 0.0;
                for (int v = 0; v < voices; ++v) {
                    const double l = shape == dsp::LfoShape::Sine
                                         ? s0 * cos_[c][v] + c0 * sin_[c][v]
                                         : lfo_.value(shape, off_[c][v]);
                    double d = centre + depth * l;
                    if (d < kMinDelaySamples) d = kMinDelaySamples;
                    if (d > max_delay_) d = max_delay_;
                    // Read before the push: the previous input is read(0),
                    // so a delay of d samples is d - 1 back.
                    const double tap = line_[c].read_lagrange3(d - 1.0);
                    if (v == 0) first = tap;
                    wet += tap;
                }
                wet *= norm;
                const double x = in[c][i];
                // Signed feedback from the first voice: positive thickens,
                // negative hollows out, as in a flanger's two polarities.
                line_[c].push(static_cast<float>(x + fb * first));
                out[c][i] = static_cast<float>(x + mix * (wet - x));
            }
            lfo_.advance();
        }
    }

private:
    // Voice v's phase offset on channel c, in cycles, with its sine and
    // cosine; recomputed only when the spread or the voice count changes.
    void set_offsets(int voices, double spread) noexcept {
        voices_ = voices;
        spread_ = spread;
        for (int c = 0; c < 2; ++c) {
            for (int v = 0; v < 4; ++v) {
                off_[c][v] = static_cast<double>(v) / voices + (c == 1 ? spread : 0.0);
                sin_[c][v] = std::sin(dsp::kTwoPi * off_[c][v]);
                cos_[c][v] = std::cos(dsp::kTwoPi * off_[c][v]);
            }
        }
    }

    double off_[2][4] = {}, sin_[2][4] = {}, cos_[2][4] = {};
    double spread_ = -1.0;
    int voices_ = 0;
    dsp::DelayLine line_[2];
    dsp::Lfo lfo_;
    double max_delay_ = 1.0;
};

}  // namespace

std::unique_ptr<Processor> make_chorus() { return std::make_unique<Chorus>(); }

}  // namespace sonora::native
