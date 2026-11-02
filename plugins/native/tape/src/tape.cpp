// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/tape.h"

#include <cmath>
#include <vector>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/delay_line.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/oversampling.h"
#include "sonora/dsp/tape.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace tape;

const char* const kSpeeds[] = {"7.5 in/s", "15 in/s", "30 in/s"};
const char* const kEqs[] = {"NAB", "IEC"};
const char* const kOffOn[] = {"off", "on"};
const char* const kQualities[] = {"eco (2x)", "standard (4x)", "high (8x)"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"drive", "Drive", Unit::Decibel, Taper::Linear, -12.0f, 24.0f, 0.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"output", "Output", Unit::Decibel, Taper::Linear, -24.0f, 12.0f, 0.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"speed", "Speed", Unit::Choice, Taper::Stepped, 0.0f, 2.0f, 1.0f, 0.0f, false, false, true, "%.0f", kSpeeds, 3},
    {"eq", "EQ standard", Unit::Choice, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kEqs, 2},
    {"bump", "Head bump", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 50.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"motion", "Motion", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, true, false, "%.0f", kOffOn, 2},
    {"wow", "Wow", Unit::Percent, Taper::Linear, 0.0f, static_cast<float>(kWowMaxPercent), 0.05f, 20.0f, true, false, false, "%.3f %%", nullptr, 0},
    {"flutter", "Flutter", Unit::Percent, Taper::Linear, 0.0f, static_cast<float>(kFlutterMaxPercent), 0.03f, 20.0f, true, false, false, "%.3f %%", nullptr, 0},
    {"quality", "Quality", Unit::Choice, Taper::Stepped, 0.0f, 2.0f, 1.0f, 0.0f, false, true, false, "%.0f", kQualities, 3},
};

const Descriptor kDesc = {"org.sonora.tape", "Sonora Tape", "Distortion", 1, kParams, kParamCount, 1, 2, false};

// The field per unit of input after drive, in units of the model's a: 0 dBFS
// at 0 dB drive reaches H = 2 a, where the Langevin curve is 20 % under
// its tangent.
constexpr double kFieldScale = 2.0;
// The delay that holds the motion's swing: both sines at their maxima
// (peak speed deviation / (2 pi f), in seconds), and a margin for the
// interpolator.
constexpr double kMotionSwingS = kWowMaxPercent * 0.01 / (dsp::kTwoPi * kWowHz) +
                                 kFlutterMaxPercent * 0.01 / (dsp::kTwoPi * kFlutterHz);
constexpr std::uint32_t kMotionMargin = 3;

class Tape final : public ProcessorBase {
public:
    Tape() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        long q = std::lround(value(kQuality));
        q = q < 0 ? 0 : (q > 2 ? 2 : q);
        factor_ = kFactors[q];
        const std::uint32_t chs = channels();
        const double passband = std::fmin(0.4535, 20000.0 / sample_rate());
        if (!os_.prepare(factor_, chs, max_frames(), passband)) return false;
        const double os_rate = sample_rate() * factor_;
        for (int s = 0; s < 3; ++s) {
            const auto speed = static_cast<dsp::TapeSpeed>(s);
            lp_[s] = dsp::design_matched_one_pole(0.0, dsp::tape_head_loss_corner_hz(speed), os_rate);
            dsp::FilterSpec bp;
            bp.shape = dsp::Shape::BandPass;
            bp.freq_hz = dsp::tape_bump_hz(speed);
            bp.q = dsp::kTapeBumpQ;
            bump_bp_[s] = dsp::design_matched(bp, sample_rate());
            for (int e = 0; e < 2; ++e) {
                const double tau = dsp::tape_eq_tau(speed, static_cast<dsp::TapeEq>(e));
                pre_[s][e] = dsp::design_matched_one_pole(1.0 / (dsp::kTwoPi * tau), dsp::kTapePreemphasisLimitHz, os_rate);
            }
        }
        for (auto& h : hyst_) h.set_params(dsp::JilesAthertonParams{});
        // Unity small-signal gain: what a tiny input makes, divided out.
        norm_ = 1.0 / (hyst_[0].initial_susceptibility() * kFieldScale);
        motion_ = value(kMotion) >= 0.5f;
        motion_delay_ = motion_ ? static_cast<std::uint32_t>(std::ceil(kMotionSwingS * sample_rate())) + kMotionMargin : 0;
        latency_ = os_.latency() + motion_delay_;
        set_latency(latency_);
        if (motion_) {
            // The read sits at the centre plus or minus the swing, and the
            // centre is the swing: twice the swing back, not once.
            for (auto& l : line_) l.prepare(2 * motion_delay_ + 4);
        }
        scaled_.assign(max_frames(), 0.0f);
        wet_.assign(max_frames(), 0.0f);
        drive_.assign(max_frames(), 0.0);
        post_.assign(max_frames(), 0.0);
        bump_.assign(max_frames(), 0.0);
        tau_.assign(max_frames(), 0.0);
        speed_ = -1;
        select(1, 0);
        return true;
    }

    void on_reset() noexcept override {
        os_.reset();
        for (auto& h : hyst_) h.reset();
        for (auto& f : pre_state_) f.reset();
        for (auto& f : de_state_) f.reset();
        for (auto& f : lp_state_) f.reset();
        for (auto& b : bump_state_) b.reset();
        if (motion_) {
            for (auto& l : line_) l.reset();
        }
        wow_phase_ = 0.0;
        flutter_phase_ = 0.0;
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const long sp = std::lround(value(kSpeed)), eq = std::lround(value(kEq));
        select(static_cast<int>(sp < 0 ? 0 : (sp > 2 ? 2 : sp)), static_cast<int>(eq < 0 ? 0 : (eq > 1 ? 1 : eq)));
        const double bump_gain = dsp::db_to_gain(dsp::kTapeBumpMaxDb) - 1.0;
        const double fs = sample_rate();
        const double wow_inc = tape::kWowHz / fs, flutter_inc = tape::kFlutterHz / fs;
        for (std::uint32_t k = 0; k < frames; ++k) {
            drive_[k] = dsp::db_to_gain(smoother(kDrive).next()) * kFieldScale;
            post_[k] = norm_ * dsp::db_to_gain(smoother(kOutput).next());
            bump_[k] = bump_gain * 0.01 * smoother(kBump).next();
            if (motion_) {
                // Delay in samples: the integral of the speed deviation,
                // eps / (2 pi f) in seconds for a sine of peak eps.
                const double wow = 0.01 * smoother(kWow).next() * fs / (dsp::kTwoPi * tape::kWowHz);
                const double flt = 0.01 * smoother(kFlutter).next() * fs / (dsp::kTwoPi * tape::kFlutterHz);
                tau_[k] = wow * std::sin(dsp::kTwoPi * wow_phase_) + flt * std::sin(dsp::kTwoPi * flutter_phase_);
                wow_phase_ += wow_inc;
                if (wow_phase_ >= 1.0) wow_phase_ -= 1.0;
                flutter_phase_ += flutter_inc;
                if (flutter_phase_ >= 1.0) flutter_phase_ -= 1.0;
            }
        }
        const std::uint32_t chs = channels();
        const std::uint32_t n = frames * factor_;
        for (std::uint32_t c = 0; c < chs; ++c) {
            for (std::uint32_t k = 0; k < frames; ++k) scaled_[k] = static_cast<float>(in[c][k] * drive_[k]);
            double* x = os_.up(c, scaled_.data(), frames);
            dsp::TapeHysteresis& h = hyst_[c];
            dsp::OnePoleZero& pre = pre_state_[c];
            dsp::OnePoleInverse& de = de_state_[c];
            dsp::OnePoleZero& lp = lp_state_[c];
            for (std::uint32_t i = 0; i < n; ++i) {
                const double m = h.tick(pre.tick(x[i]));
                x[i] = lp.tick(de.tick(m));
            }
            os_.down(c, wet_.data(), frames);
            dsp::BiquadState& bp = bump_state_[c];
            for (std::uint32_t k = 0; k < frames; ++k) {
                const double w = wet_[k];
                double y = w + bump_[k] * bp.tick(bump_bp_[speed_], w);
                if (motion_) {
                    line_[c].push(static_cast<float>(y));
                    y = line_[c].read_lagrange3(static_cast<double>(motion_delay_) + tau_[k]);
                }
                out[c][k] = static_cast<float>(y * post_[k]);
            }
        }
    }

private:
    // Moves the filters to a speed and an EQ standard. The states carry
    // over; the base fades the output around the change (declick).
    void select(int speed, int eq) noexcept SONORA_RT {
        if (speed == speed_ && eq == eq_) return;
        speed_ = speed;
        eq_ = eq;
        const dsp::OnePoleZero& pre = pre_[speed][eq];
        const dsp::OnePoleZero& lp = lp_[speed];
        for (std::uint32_t c = 0; c < 2; ++c) {
            carry(pre_state_[c], pre);
            de_state_[c].ib0 = 1.0 / pre.b0;
            de_state_[c].b1 = pre.b1;
            de_state_[c].p = pre.p;
            carry(lp_state_[c], lp);
        }
    }
    static void carry(dsp::OnePoleZero& st, const dsp::OnePoleZero& c) noexcept {
        st.b0 = c.b0;
        st.b1 = c.b1;
        st.p = c.p;
    }

    dsp::Oversampler os_;
    dsp::TapeHysteresis hyst_[2];
    dsp::OnePoleZero pre_[3][2], lp_[3];
    dsp::BiquadCoeffs bump_bp_[3];
    dsp::OnePoleZero pre_state_[2], lp_state_[2];
    dsp::OnePoleInverse de_state_[2];
    dsp::BiquadState bump_state_[2];
    dsp::DelayLine line_[2];
    int speed_ = -1, eq_ = -1;
    bool motion_ = false;
    std::uint32_t factor_ = 4, latency_ = 0, motion_delay_ = 0;
    double norm_ = 1.0;
    double wow_phase_ = 0.0, flutter_phase_ = 0.0;
    std::vector<float> scaled_, wet_;
    std::vector<double> drive_, post_, bump_, tau_;
};

}  // namespace

std::unique_ptr<Processor> make_tape() { return std::make_unique<Tape>(); }

}  // namespace sonora::native
