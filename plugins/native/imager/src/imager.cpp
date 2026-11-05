// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/imager.h"

#include <cmath>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/math.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace imager;

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"low_xover", "Low crossover", Unit::Hertz, Taper::Log, 40.0f, 500.0f, 150.0f, 20.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"high_xover", "High crossover", Unit::Hertz, Taper::Log, 1000.0f, 12000.0f, 4000.0f, 20.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"width_low", "Low width", Unit::Percent, Taper::Linear, 0.0f, 200.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"width_mid", "Mid width", Unit::Percent, Taper::Linear, 0.0f, 200.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"width_high", "High width", Unit::Percent, Taper::Linear, 0.0f, 200.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
};

const Descriptor kDesc = {"org.sonora.imager", "Sonora Imager", "Stereo", 1, kParams, kParamCount, 1, 2, false};

constexpr std::uint32_t kControl = 16;
constexpr std::uint32_t kMaxCh = 2;
constexpr double kButterworthQ = 0.7071067811865476;

// Two cascaded Butterworth sections: one Linkwitz-Riley fourth-order leg.
struct Lr4 {
    dsp::SvfState s[2];
    double tick(const dsp::SvfState::Derived& d, double x) noexcept {
        return s[1].tick(d, s[0].tick(d, x));
    }
    void reset() noexcept { s[0].reset(); s[1].reset(); }
};

struct Chan {
    Lr4 lo1, hi1;      // the lower crossover on the input
    Lr4 lo2, hi2;      // the upper crossover on the lower crossover's high leg
    Lr4 ap_lo, ap_hi;  // the upper crossover's all-pass on the low band
    void reset() noexcept {
        lo1.reset(); hi1.reset(); lo2.reset(); hi2.reset(); ap_lo.reset(); ap_hi.reset();
    }
};

class Imager final : public ProcessorBase {
public:
    Imager() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        update_filters(0, true);
        return true;
    }
    void on_reset() noexcept override {
        for (auto& c : ch_) c.reset();
    }
    void on_param(std::uint32_t index, bool jump) noexcept override {
        if (jump && (index == kLowXover || index == kHighXover)) update_filters(0, true);
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        for (std::uint32_t pos = 0; pos < frames; pos += kControl) {
            const std::uint32_t m = frames - pos < kControl ? frames - pos : kControl;
            if (smoother(kLowXover).moving() || smoother(kHighXover).moving()) {
                update_filters(m, false);
            } else {
                smoother(kLowXover).advance(m);
                smoother(kHighXover).advance(m);
            }
            for (std::uint32_t i = 0; i < m; ++i) {
                const std::uint32_t k = pos + i;
                const double w[3] = {smoother(kWidthLow).next() * 0.01, smoother(kWidthMid).next() * 0.01,
                                     smoother(kWidthHigh).next() * 0.01};
                const dsp::SvfState::Derived& dl1 = lp1_.next();
                const dsp::SvfState::Derived& dh1 = hp1_.next();
                const dsp::SvfState::Derived& dl2 = lp2_.next();
                const dsp::SvfState::Derived& dh2 = hp2_.next();
                double band[kMaxCh][3];
                for (std::uint32_t c = 0; c < chs; ++c) {
                    Chan& s = ch_[c];
                    const double x = in[c][k];
                    const double lo = s.lo1.tick(dl1, x);
                    const double rest = s.hi1.tick(dh1, x);
                    band[c][0] = s.ap_lo.tick(dl2, lo) + s.ap_hi.tick(dh2, lo);
                    band[c][1] = s.lo2.tick(dl2, rest);
                    band[c][2] = s.hi2.tick(dh2, rest);
                }
                if (chs == 1) {
                    out[0][k] = static_cast<float>(band[0][0] + band[0][1] + band[0][2]);
                    continue;
                }
                double l = 0.0, r = 0.0;
                for (int b = 0; b < 3; ++b) {
                    const double mid = 0.5 * (band[0][b] + band[1][b]);
                    const double side = 0.5 * (band[0][b] - band[1][b]) * w[b];
                    l += mid + side;
                    r += mid - side;
                }
                out[0][k] = static_cast<float>(l);
                out[1][k] = static_cast<float>(r);
            }
        }
    }

private:
    void update_filters(std::uint32_t m, bool jump) noexcept {
        const double f1 = jump ? smoother(kLowXover).current() : smoother(kLowXover).advance(m);
        double f2 = jump ? smoother(kHighXover).current() : smoother(kHighXover).advance(m);
        const double top = dsp::kMaxFreqRatio * sample_rate();
        if (f2 > top) f2 = top;
        // Bilinear, prewarped at the corner: a frequency warping maps the
        // analog identity LP4 + HP4 = all-pass exactly, so the bands sum
        // flat at every frequency. A matched design agrees with the analog
        // magnitude only at the corner; its phase differs elsewhere and the
        // sum ripples near Nyquist (measured 1 dB at 12 kHz, 44.1 kHz). The
        // crossover's own cramping near Nyquist does not matter here.
        const auto design = [&](dsp::Shape sh, double f) {
            return dsp::to_svf(dsp::design_bilinear({sh, f, kButterworthQ, 0.0}, sample_rate()));
        };
        const auto a = design(dsp::Shape::LowPass, f1), b = design(dsp::Shape::HighPass, f1);
        const auto c = design(dsp::Shape::LowPass, f2), d = design(dsp::Shape::HighPass, f2);
        if (jump) {
            lp1_.set(a); hp1_.set(b); lp2_.set(c); hp2_.set(d);
        } else {
            lp1_.ramp_to(a, m); hp1_.ramp_to(b, m); lp2_.ramp_to(c, m); hp2_.ramp_to(d, m);
        }
    }

    dsp::SvfRamp lp1_, hp1_, lp2_, hp2_;
    Chan ch_[kMaxCh];
};

}  // namespace

std::unique_ptr<Processor> make_imager() { return std::make_unique<Imager>(); }

}  // namespace sonora::native
