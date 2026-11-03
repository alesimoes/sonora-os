// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/mixbus.h"

#include <cmath>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/dynamics.h"
#include "sonora/dsp/math.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace mixbus {
double ratio_at(double position) noexcept {
    if (!(position > 0.0)) return kRatios[0];
    if (position >= kRatioCount - 1) return kRatios[kRatioCount - 1];
    const auto i = static_cast<std::uint32_t>(position);
    const double t = position - i;
    return kRatios[i] * std::exp(t * std::log(kRatios[i + 1] / kRatios[i]));
}
}  // namespace mixbus

namespace {

using namespace mixbus;

const char* const kOffOn[] = {"off", "on"};
const char* const kRatioNames[] = {"1.5:1", "2:1", "4:1", "10:1"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"threshold", "Threshold", Unit::Decibel, Taper::Linear, -40.0f, 0.0f, -12.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"ratio", "Ratio", Unit::Choice, Taper::Stepped, 0.0f, 3.0f, 1.0f, 20.0f, true, false, false, "%.0f", kRatioNames, kRatioCount},
    {"knee", "Knee", Unit::Decibel, Taper::Linear, 0.0f, 12.0f, 3.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"attack", "Attack", Unit::Milliseconds, Taper::Log, 0.1f, 30.0f, 10.0f, 20.0f, true, false, false, "%.1f ms", nullptr, 0},
    {"release", "Release", Unit::Milliseconds, Taper::Log, 50.0f, 1200.0f, 300.0f, 20.0f, true, false, false, "%.0f ms", nullptr, 0},
    {"auto_release", "Auto release", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 20.0f, true, false, false, "%.0f", kOffOn, 2},
    {"sc_hpf_on", "Side-chain high-pass", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 10.0f, true, false, false, "%.0f", kOffOn, 2},
    {"sc_hpf_freq", "Side-chain high-pass frequency", Unit::Hertz, Taper::Log, 20.0f, 300.0f, 90.0f, 20.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"makeup", "Make-up", Unit::Decibel, Taper::Linear, -6.0f, 18.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"width", "Width", Unit::Percent, Taper::Linear, 0.0f, 200.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
};

const Descriptor kDesc = {"org.sonora.mixbus", "Sonora Mix Bus", "Dynamics", 1, kParams, kParamCount, 1, 2, false};

// Control rate for the side-chain filter's coefficient ramp and the
// detector times: cheap, and a 16-sample step of a time constant is not
// audible (the zipper check holds it).
constexpr std::uint32_t kControl = 16;
constexpr std::uint32_t kMaxCh = 2;

class MixBus final : public ProcessorBase {
public:
    MixBus() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        update_filter(0, true);
        update_times();
        return true;
    }

    void on_reset() noexcept override {
        for (auto& h : hpf_) h.reset();
        det_.reset(0.0);
        fast_.reset(0.0);
        slow_.reset(0.0);
    }

    void on_param(std::uint32_t index, bool jump) noexcept override {
        if (index == kScHpfFreq) update_filter(0, jump);
        if (jump && (index == kAttack || index == kRelease)) update_times();
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        for (std::uint32_t pos = 0; pos < frames; pos += kControl) {
            const std::uint32_t m = frames - pos < kControl ? frames - pos : kControl;
            if (smoother(kScHpfFreq).moving()) update_filter(m, false);
            else smoother(kScHpfFreq).advance(m);
            const bool times_moving = smoother(kAttack).moving() || smoother(kRelease).moving();
            smoother(kAttack).advance(m);
            smoother(kRelease).advance(m);
            if (times_moving) update_times();
            for (std::uint32_t i = 0; i < m; ++i) {
                const std::uint32_t k = pos + i;
                const double thr = smoother(kThreshold).next();
                const double ratio = ratio_at(smoother(kRatio).next());
                const double knee = smoother(kKnee).next();
                const double autow = smoother(kAutoRelease).next();
                const double hpw = smoother(kScHpfOn).next();
                const double makeup = smoother(kMakeup).next();
                const double mix = smoother(kMix).next() * 0.01;
                const double width = smoother(kWidth).next() * 0.01;
                const dsp::SvfState::Derived& d = hpf_ramp_.next();

                // Fully linked: the louder channel's key sets the reduction.
                double key = 0.0;
                for (std::uint32_t c = 0; c < chs; ++c) {
                    double s = in[c][k];
                    const double h = hpf_[c].tick(d, s);
                    s += hpw * (h - s);
                    key = std::fmax(key, std::fabs(s));
                }
                const double x = dsp::gain_to_db(key, -400.0);
                const double cdb = x - dsp::compressor_curve_db(x, thr, ratio, knee);
                // Every path runs whatever the switch, so turning auto
                // release on or off crossfades two settled states.
                const double manual = det_.process(cdb);
                const double fast = fast_.process(cdb);
                const double slow = slow_.process(cdb);
                const double automatic = fast > slow ? fast : slow;
                const double red = manual + autow * (automatic - manual);
                const double g = dsp::db_to_gain(makeup - red);

                float y[kMaxCh];
                for (std::uint32_t c = 0; c < chs; ++c) {
                    const double dry = in[c][k];
                    y[c] = static_cast<float>(dry + mix * (dry * g - dry));
                }
                if (chs == 2) {
                    // From the rounded floats: at 100 % the sum and the
                    // difference are exact in double, so the stage is the
                    // identity to the bit.
                    const double mid = 0.5 * (static_cast<double>(y[0]) + y[1]);
                    const double side = 0.5 * (static_cast<double>(y[0]) - y[1]) * width;
                    out[0][k] = static_cast<float>(mid + side);
                    out[1][k] = static_cast<float>(mid - side);
                } else {
                    out[0][k] = y[0];
                }
            }
        }
    }

private:
    void update_filter(std::uint32_t m, bool jump) noexcept {
        const double f = jump ? smoother(kScHpfFreq).current() : smoother(kScHpfFreq).advance(m);
        const auto c = dsp::to_svf(dsp::design_matched({dsp::Shape::HighPass, f, 0.7071067811865476, 0.0}, sample_rate()));
        if (jump) hpf_ramp_.set(c);
        else hpf_ramp_.ramp_to(c, m);
    }
    void update_times() noexcept {
        const double a = smoother(kAttack).current() * 1e-3;
        const double r = smoother(kRelease).current() * 1e-3;
        det_.set_times(a, r, sample_rate());
        fast_.set_times(a, kAutoFastRelease, sample_rate());
        slow_.set_times(kAutoSlowAttack, kAutoSlowRelease, sample_rate());
    }

    dsp::SvfRamp hpf_ramp_;
    dsp::SvfState hpf_[kMaxCh];
    dsp::BranchingSmoother det_, fast_, slow_;
};

}  // namespace

std::unique_ptr<Processor> make_mixbus() { return std::make_unique<MixBus>(); }

}  // namespace sonora::native
