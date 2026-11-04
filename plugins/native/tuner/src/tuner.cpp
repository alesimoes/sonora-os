// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/tuner.h"

#include <algorithm>
#include <cmath>

#include "sonora/native/processor_base.h"
#include "sonora/native/tuner_pitch.h"

namespace sonora::native {

namespace {

using namespace tuner;

const char* const kKeys[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* const kScales[] = {"chromatic", "major", "minor", "harmonic minor", "major pentatonic", "minor pentatonic"};
const char* const kRanges[] = {"low (65-500 Hz)", "medium (100-800 Hz)", "high (160-1100 Hz)"};

// Key and scale are stepped and reach the audio only through the eased
// correction, which is why they need neither a ramp nor a declick.
const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"key", "Key", Unit::Choice, Taper::Stepped, 0.0f, 11.0f, 0.0f, 0.0f, true, false, false, "%.0f", kKeys, 12},
    {"scale", "Scale", Unit::Choice, Taper::Stepped, 0.0f, 5.0f, 0.0f, 0.0f, true, false, false, "%.0f", kScales, 6},
    {"speed", "Retune speed", Unit::Milliseconds, Taper::Linear, 0.0f, 400.0f, 50.0f, 20.0f, true, false, false, "%.0f ms", nullptr, 0},
    {"amount", "Amount", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"range", "Voice range", Unit::Choice, Taper::Stepped, 0.0f, 2.0f, 1.0f, 0.0f, false, true, false, "%.0f", kRanges, 3},
    {"reference", "Reference A4", Unit::Hertz, Taper::Linear, 415.0f, 466.0f, 440.0f, 50.0f, true, false, false, "%.1f Hz", nullptr, 0},
};

const Descriptor kDesc = {"org.sonora.tuner", "Sonora Tuner", "Pitch", 1, kParams, kParamCount, 1, 2, false};

class Tuner final : public ProcessorBase {
public:
    Tuner() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        long r = std::lround(value(kRange));
        r = r < 0 ? 0 : (r > 2 ? 2 : r);
        if (!yin_.prepare(sample_rate(), kRangeMinHz[r], kRangeMaxHz[r])) return false;
        if (!shifter_.prepare(sample_rate(), kRangeMinHz[r], channels())) return false;
        set_latency(shifter_.latency());
        hop_s_ = yin_.hop_seconds();
        on_reset();
        return true;
    }

    void on_reset() noexcept override {
        yin_.reset();
        shifter_.reset();
        n_period_ = 0;
        voiced_run_ = 0;
        unvoiced_s_ = 1e9;
        have_pitch_ = false;
        note_ = -1;
        cents_ = 0.0;
        ratio_ = 1.0;
        snap_ = false;
        period_ = std::max(shifter_.min_period(), std::min(shifter_.max_period(), sample_rate() / 200.0));
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        const float inv_chs = 1.0f / static_cast<float>(chs);
        for (std::uint32_t k = 0; k < frames; ++k) {
            const double amount = smoother(kAmount).next();
            const double speed = smoother(kSpeed).next();
            const double ref = smoother(kReference).next();
            float x[2] = {0.0f, 0.0f}, y[2] = {0.0f, 0.0f};
            float mono = 0.0f;
            for (std::uint32_t c = 0; c < chs; ++c) {
                x[c] = in[c][k];
                mono += x[c];
            }
            if (yin_.push(mono * inv_chs)) hop(amount, speed, ref);
            shifter_.tick(x, y, period_, ratio_, snap_);
            for (std::uint32_t c = 0; c < chs; ++c) out[c][k] = y[c];
        }
    }

private:
    // One detector result: the period the shifter's grains follow, and the
    // correction toward the chosen note.
    void hop(double amount, double speed_ms, double ref_hz) noexcept SONORA_RT {
        const YinDetector::Result& r = yin_.result();
        if (r.voiced) {
            ++voiced_run_;
            // Median of three periods: one wrong frame (an octave slip at
            // an onset) never reaches the shifter.
            if (n_period_ < 3) {
                periods_[n_period_++] = r.period;
            } else {
                periods_[0] = periods_[1];
                periods_[1] = periods_[2];
                periods_[2] = r.period;
            }
        } else {
            voiced_run_ = 0;
            n_period_ = 0;
        }
        const double max_c = kMaxCents;
        if (voiced_run_ >= kConfirmFrames) {
            const double p = n_period_ >= 3 ? median3(periods_[0], periods_[1], periods_[2]) : periods_[n_period_ - 1];
            period_ = std::max(shifter_.min_period(), std::min(shifter_.max_period(), p));
            have_pitch_ = true;
            unvoiced_s_ = 0.0;
            const double midi = 69.0 + 12.0 * std::log2(sample_rate() / (period_ * ref_hz));
            const int tonic = static_cast<int>(std::lround(value(kKey)));
            const long sc = std::lround(value(kScale));
            const unsigned mask = kScaleMasks[sc < 0 ? 0 : (sc >= static_cast<long>(kScaleCount) ? kScaleCount - 1 : sc)];
            note_ = nearest_note(midi, tonic, mask, note_, kHysteresisSemitones);
            const double target = (static_cast<double>(note_) - midi) * amount;  // semitones x 100 x (amount / 100)
            const double alpha = speed_ms < 1.0 ? 1.0 : 1.0 - std::exp(-hop_s_ / (speed_ms * 1e-3));
            cents_ += alpha * (target - cents_);
        } else if (voiced_run_ == 0) {
            unvoiced_s_ += hop_s_;
            if (unvoiced_s_ > kHoldSeconds) {
                // The voice is gone: the correction eases back to nothing
                // and the next note starts from the nearest, not the last.
                cents_ -= cents_ * (1.0 - std::exp(-hop_s_ / kReleaseSeconds));
                note_ = -1;
            }
        }
        cents_ = std::max(-max_c, std::min(max_c, cents_));
        ratio_ = std::exp2(cents_ / 1200.0);
        snap_ = have_pitch_ && unvoiced_s_ <= kHoldSeconds;
    }

    static double median3(double a, double b, double c) noexcept {
        return std::max(std::min(a, b), std::min(std::max(a, b), c));
    }

    YinDetector yin_;
    PsolaShifter shifter_;
    double hop_s_ = 0.005;
    double periods_[3] = {0.0, 0.0, 0.0};
    int n_period_ = 0, voiced_run_ = 0, note_ = -1;
    double unvoiced_s_ = 1e9, cents_ = 0.0, ratio_ = 1.0, period_ = 240.0;
    bool have_pitch_ = false, snap_ = false;
};

}  // namespace

std::unique_ptr<Processor> make_tuner() { return std::make_unique<Tuner>(); }

}  // namespace sonora::native
