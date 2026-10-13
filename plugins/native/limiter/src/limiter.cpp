// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/limiter.h"

#include <cmath>
#include <vector>

#include "sonora/dsp/limiter.h"
#include "sonora/dsp/math.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace limiter;

const char* const kOffOn[] = {"off", "on"};
const char* const kLinks[] = {"unlinked", "linked"};

// The guard's own look-ahead: long enough that its ramps put little energy
// near Nyquist (2 ms measured no better on full-band noise), short enough
// to keep the chain's latency down.
constexpr double kGuardLookaheadSeconds = 0.001;

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"input", "Input", Unit::Decibel, Taper::Linear, 0.0f, 24.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
    {"ceiling", "Ceiling", Unit::Decibel, Taper::Linear, -12.0f, 0.0f, -1.0f, 20.0f, true, false, false, "%.1f dBTP", nullptr, 0},
    {"release", "Release", Unit::Milliseconds, Taper::Log, 1.0f, 1000.0f, 50.0f, 20.0f, true, false, false, "%.0f ms", nullptr, 0},
    {"lookahead", "Look-ahead", Unit::Milliseconds, Taper::Linear, 1.0f, 5.0f, 1.5f, 0.0f, false, true, false, "%.1f ms", nullptr, 0},
    // Switching detection changes the need, not the audio path: the gain
    // ramps to the new need through the same averaging, so no declick.
    {"true_peak", "True peak", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 1.0f, 0.0f, true, false, false, "%.0f", kOffOn, 2},
    {"link", "Stereo link", Unit::Choice, Taper::Stepped, 0.0f, 1.0f, 1.0f, 0.0f, true, false, false, "%.0f", kLinks, 2},
};

const Descriptor kDesc = {"org.sonora.limiter", "Sonora Limiter", "Dynamics", 1, kParams, kParamCount, 1, 2, false};

class Limiter final : public ProcessorBase {
public:
    Limiter() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        const auto la = static_cast<std::uint32_t>(std::lround(value(kLookahead) * 1e-3 * sample_rate()));
        const auto gla = static_cast<std::uint32_t>(std::lround(kGuardLookaheadSeconds * sample_rate()));
        if (!lim_.prepare(sample_rate(), channels(), la)) return false;
        if (!guard_.prepare(sample_rate(), channels(), max_frames(), gla)) return false;
        ceiling_.assign(max_frames(), 1.0);
        set_latency(lim_.latency() + guard_.latency());
        return true;
    }

    void on_reset() noexcept override {
        lim_.reset();
        guard_.reset();
    }

    void on_param(std::uint32_t index, bool) noexcept override {
        if (index == kTruePeak) {
            lim_.set_true_peak(value(kTruePeak) >= 0.5f);
            guard_.set_true_peak(value(kTruePeak) >= 0.5f);
        }
        if (index == kLink) {
            lim_.set_linked(value(kLink) >= 0.5f);
            guard_.set_linked(value(kLink) >= 0.5f);
        }
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        float frame[dsp::LookaheadLimiter::kMaxChannels];
        for (std::uint32_t i = 0; i < frames; ++i) {
            const double drive = dsp::db_to_gain(smoother(kInput).next());
            ceiling_[i] = dsp::db_to_gain(smoother(kCeiling).next());
            lim_.set_ceiling(ceiling_[i]);
            // The release coefficient is an exp(); recomputed only while
            // the release is being moved.
            if (smoother(kRelease).moving() || i == 0) {
                const double r = smoother(kRelease).next() * 1e-3;
                lim_.set_release(r);
                guard_.set_release(r);
            } else {
                smoother(kRelease).next();
            }
            for (std::uint32_t c = 0; c < chs; ++c) frame[c] = static_cast<float>(in[c][i] * drive);
            lim_.process_frame(frame);
            for (std::uint32_t c = 0; c < chs; ++c) out[c][i] = frame[c];
        }
        // The ceiling guarantee, on what the look-ahead stage made.
        guard_.process(out, ceiling_.data(), frames);
    }

private:
    dsp::LookaheadLimiter lim_;
    dsp::TruePeakGuard guard_;
    std::vector<double> ceiling_;  // per sample of the block, sized in prepare
};

}  // namespace

std::unique_ptr<Processor> make_limiter() { return std::make_unique<Limiter>(); }

}  // namespace sonora::native
