// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/tube.h"

#include <cmath>
#include <vector>

#include "sonora/dsp/delay_line.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/oversampling.h"
#include "sonora/dsp/triode.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace tube;

const char* const kOffOn[] = {"off", "on"};
const char* const kQualities[] = {"eco (2x)", "standard (4x)", "high (8x)"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"drive", "Drive", Unit::Decibel, Taper::Linear, -12.0f, 36.0f, 6.0f, 30.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"output", "Output", Unit::Decibel, Taper::Linear, -24.0f, 12.0f, 0.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    // Switching it changes the level by the drive: declicked by the base.
    {"auto_level", "Auto level", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 1.0f, 0.0f, true, false, true, "%.0f", kOffOn, 2},
    {"quality", "Quality", Unit::Choice, Taper::Stepped, 0.0f, 2.0f, 1.0f, 0.0f, false, true, false, "%.0f", kQualities, 3},
};

const Descriptor kDesc = {"org.sonora.tube", "Sonora Tube Drive", "Distortion", 1, kParams, kParamCount, 1, 2, false};

class Tube final : public ProcessorBase {
public:
    Tube() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        long q = std::lround(value(kQuality));
        q = q < 0 ? 0 : (q > 2 ? 2 : q);
        factor_ = kFactors[q];
        const std::uint32_t chs = channels();
        // The protected band is 20 kHz at every rate (0.4535 of 44.1 kHz),
        // not a fixed fraction of the rate: at 48 and 96 kHz the
        // half-bands get a wider transition and so fewer taps, and the
        // latency is shorter. The aliasing bar is measured to 20 kHz.
        const double passband = std::fmin(0.4535, 20000.0 / sample_rate());
        if (!os_.prepare(factor_, chs, max_frames(), passband)) return false;
        const double os_rate = sample_rate() * factor_;
        for (std::uint32_t c = 0; c < chs; ++c) {
            if (!stage_[c].prepare(os_rate)) return false;
        }
        double mag = 1.0, phase = 0.0;
        stage_[0].small_signal_gain(1000.0, mag, phase);
        // Divides out the gain and the stage's inversion.
        norm_ = -1.0 / mag;
        latency_ = os_.latency();
        set_latency(latency_);
        for (std::uint32_t c = 0; c < chs; ++c) dry_[c].prepare(latency_ + 1);
        drive_line_.assign(latency_ + 1, 1.0);
        scaled_.assign(max_frames(), 0.0f);
        wet_.assign(max_frames(), 0.0f);
        drive_.assign(max_frames(), 0.0);
        post_.assign(max_frames(), 0.0);
        mix_.assign(max_frames(), 0.0);
        return true;
    }

    void on_reset() noexcept override {
        os_.reset();
        for (auto& s : stage_) s.reset();
        for (auto& d : dry_) d.reset();
        const double g = dsp::db_to_gain(smoother(kDrive).current());
        for (double& v : drive_line_) v = g;
        drive_pos_ = 0;
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        // The per-sample gains first: the smoothers advance once per
        // sample, whatever the channel count.
        const bool auto_level = value(kAutoLevel) >= 0.5f;
        const std::uint32_t n_line = static_cast<std::uint32_t>(drive_line_.size());
        for (std::uint32_t k = 0; k < frames; ++k) {
            const double g = dsp::db_to_gain(smoother(kDrive).next());
            drive_[k] = g;
            // Auto level divides by the drive the sample now leaving the
            // oversampler went in with: `latency_` samples ago.
            drive_line_[drive_pos_] = g;
            drive_pos_ = drive_pos_ + 1 == n_line ? 0 : drive_pos_ + 1;
            const double g_then = drive_line_[drive_pos_];
            const double level = dsp::db_to_gain(smoother(kOutput).next());
            post_[k] = norm_ * level / (auto_level ? g_then : 1.0);
            mix_[k] = 0.01 * smoother(kMix).next();
        }
        const std::uint32_t chs = channels();
        const std::uint32_t n = frames * factor_;
        double* x[2] = {nullptr, nullptr};
        for (std::uint32_t c = 0; c < chs; ++c) {
            for (std::uint32_t k = 0; k < frames; ++k) scaled_[k] = static_cast<float>(in[c][k] * drive_[k]);
            x[c] = os_.up(c, scaled_.data(), frames);
        }
        // Stereo: both stages in lockstep (triode.h, tick2), the same
        // numbers as one at a time, in less time.
        if (chs == 2) {
            for (std::uint32_t i = 0; i < n; ++i) dsp::TriodeStage::tick2(stage_[0], stage_[1], x[0][i], x[1][i], x[0][i], x[1][i]);
        } else {
            for (std::uint32_t i = 0; i < n; ++i) x[0][i] = stage_[0].tick(x[0][i]);
        }
        for (std::uint32_t c = 0; c < chs; ++c) {
            os_.down(c, wet_.data(), frames);
            for (std::uint32_t k = 0; k < frames; ++k) {
                dry_[c].push(in[c][k]);
                const double dry = dry_[c].read(latency_);
                const double wet = wet_[k] * post_[k];
                out[c][k] = static_cast<float>(dry + mix_[k] * (wet - dry));
            }
        }
    }

private:
    dsp::Oversampler os_;
    dsp::TriodeStage stage_[2];
    dsp::DelayLine dry_[2];
    std::uint32_t factor_ = 4;
    std::uint32_t latency_ = 0;
    double norm_ = -1.0;
    std::vector<double> drive_line_;
    std::uint32_t drive_pos_ = 0;
    std::vector<float> scaled_, wet_;
    std::vector<double> drive_, post_, mix_;
};

}  // namespace

std::unique_ptr<Processor> make_tube() { return std::make_unique<Tube>(); }

}  // namespace sonora::native
