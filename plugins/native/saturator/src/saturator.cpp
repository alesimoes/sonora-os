// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/saturator.h"

#include <cmath>
#include <vector>

#include "sonora/dsp/delay_line.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/oversampling.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace saturator;

const char* const kShapes[] = {"soft", "warm", "hard"};
const char* const kModes[] = {"saturate", "exciter"};
const char* const kQualities[] = {"eco (2x)", "standard (4x)", "high (8x)"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"drive", "Drive", Unit::Decibel, Taper::Linear, 0.0f, 36.0f, 12.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"shape", "Shape", Unit::Choice, Taper::Stepped, 0.0f, 2.0f, 0.0f, 0.0f, false, false, true, "%.0f", kShapes, 3},
    {"mode", "Mode", Unit::Choice, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kModes, 2},
    {"focus", "Focus", Unit::Hertz, Taper::Log, 500.0f, 8000.0f, 3000.0f, 20.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"tone", "Tone", Unit::Decibel, Taper::Linear, -12.0f, 12.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"output", "Output", Unit::Decibel, Taper::Linear, -24.0f, 12.0f, 0.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"quality", "Quality", Unit::Choice, Taper::Stepped, 0.0f, 2.0f, 1.0f, 0.0f, false, true, false, "%.0f", kQualities, 3},
};

const Descriptor kDesc = {"org.sonora.saturator", "Sonora Saturator", "Distortion", 1, kParams, kParamCount, 1, 2, false};

// The warm curve's constants, computed once at load (not per sample, and
// not as function-local statics, whose guard is a check on every call).
const double kWarmOffset = std::tanh(kWarmBias);
const double kWarmInvSlope = 1.0 / (1.0 - kWarmOffset * kWarmOffset);

// The curves, each with unit slope at the origin (the drive is divided out
// after them, so a small signal passes at unity gain). Templates, so the
// shape switch sits outside the per-sample loop.
struct Soft {
    static double f(double u) noexcept { return std::tanh(u); }
};
struct Warm {
    // tanh around a bias: the offset's own tanh is taken off, and the slope
    // there (1 - tanh^2 bias) divided out. Its mean is not zero for a loud
    // input; the DC blocker takes it.
    static double f(double u) noexcept { return (std::tanh(u + kWarmBias) - kWarmOffset) * kWarmInvSlope; }
};
struct Hard {
    // The cubic soft clipper u - u^3/3, which meets its flat top (2/3) with
    // matching value and zero slope at |u| = 1.
    static double f(double u) noexcept {
        const double c = u > 1.0 ? 1.0 : (u < -1.0 ? -1.0 : u);
        return c - c * c * c * (1.0 / 3.0);
    }
};

// A first-order high-pass at the oversampled rate with its pole at
// exp(-2 pi fc / fs): y[n] = x[n] - x[n-1] + r y[n-1].
struct DcBlocker {
    double r = 0.999, x1 = 0.0, y1 = 0.0;
    double tick(double x) noexcept SONORA_RT {
        const double y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        return y;
    }
    void reset() noexcept { x1 = y1 = 0.0; }
};

// One-pole low-pass, y += (1 - p)(x - y), pole p = exp(-2 pi f / fs).
struct OnePole {
    double y = 0.0;
    double tick(double p, double x) noexcept SONORA_RT {
        y = (1.0 - p) * x + p * y;
        return y;
    }
    void reset() noexcept { y = 0.0; }
};

class Saturator final : public ProcessorBase {
public:
    Saturator() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        long q = std::lround(value(kQuality));
        q = q < 0 ? 0 : (q > 2 ? 2 : q);
        factor_ = kFactors[q];
        const double passband = std::fmin(0.4535, 20000.0 / sample_rate());
        if (!os_.prepare(factor_, channels(), max_frames(), passband)) return false;
        latency_ = os_.latency();
        set_latency(latency_);
        for (auto& l : dry_) l.prepare(latency_);
        for (auto& d : dc_) d.r = std::exp(-dsp::kTwoPi * kDcHz / (sample_rate() * factor_));
        tone_pole_ = std::exp(-dsp::kTwoPi * kToneHz / sample_rate());
        scaled_.assign(max_frames(), 0.0f);
        wet_.assign(max_frames(), 0.0f);
        for (auto* v : {&drive_, &inv_drive_, &tone_, &mix_, &post_, &pole_}) v->assign(max_frames(), 0.0);
        return true;
    }

    void on_reset() noexcept override {
        os_.reset();
        for (auto& l : dry_) l.reset();
        for (auto& d : dc_) d.reset();
        for (auto& c : split_) {
            for (auto& f : c) f.reset();
        }
        for (auto& f : tone_lp_) f.reset();
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const long sh = std::lround(value(kShape));
        const bool exciter = value(kMode) >= 0.5f;
        // A parameter that is not moving gives the same value every sample:
        // its gain once per block, the same bits as once per sample.
        auto gains = [frames](ParamSmoother& sm, double* dst, auto&& f) noexcept {
            if (sm.moving()) {
                for (std::uint32_t k = 0; k < frames; ++k) dst[k] = f(sm.next());
            } else {
                const double g = f(sm.next());
                for (std::uint32_t k = 0; k < frames; ++k) dst[k] = g;
            }
        };
        const double fs = sample_rate();
        gains(smoother(kDrive), drive_.data(), [](double v) noexcept { return dsp::db_to_gain(v); });
        for (std::uint32_t k = 0; k < frames; ++k) inv_drive_[k] = 1.0 / drive_[k];
        gains(smoother(kTone), tone_.data(), [](double v) noexcept { return dsp::db_to_gain(v); });
        gains(smoother(kMix), mix_.data(), [](double v) noexcept { return 0.01 * v; });
        gains(smoother(kOutput), post_.data(), [](double v) noexcept { return dsp::db_to_gain(v); });
        gains(smoother(kFocus), pole_.data(), [fs](double hz) noexcept { return std::exp(-dsp::kTwoPi * hz / fs); });

        const std::uint32_t chs = channels();
        const std::uint32_t n = frames * factor_;
        for (std::uint32_t c = 0; c < chs; ++c) {
            // Ahead of the curve: the whole signal, or its high part (two
            // one-pole high-passes in cascade).
            for (std::uint32_t k = 0; k < frames; ++k) {
                double s = in[c][k];
                if (exciter) {
                    const double h1 = s - split_[c][0].tick(pole_[k], s);
                    s = h1 - split_[c][1].tick(pole_[k], h1);
                }
                scaled_[k] = static_cast<float>(s * drive_[k]);
            }
            double* x = os_.up(c, scaled_.data(), frames);
            switch (sh) {
                case 1: shape<Warm>(x, n, exciter, c); break;
                case 2: shape<Hard>(x, n, exciter, c); break;
                default: shape<Soft>(x, n, exciter, c); break;
            }
            os_.down(c, wet_.data(), frames);
            for (std::uint32_t k = 0; k < frames; ++k) {
                dry_[c].push(in[c][k]);
                const double d = dry_[c].read(latency_);
                const double w = wet_[k] * inv_drive_[k];
                // Tone: the part above the corner scaled, the rest left.
                const double lo = tone_lp_[c].tick(tone_pole_, w);
                const double toned = lo + tone_[k] * (w - lo);
                const double y = exciter ? d + mix_[k] * toned : d + mix_[k] * (toned - d);
                out[c][k] = static_cast<float>(y * post_[k]);
            }
        }
    }

private:
    // Saturate mode keeps f(u); exciter mode keeps what the curve added to
    // the split part, f(u) - u, which is added to the dry signal.
    template <class Curve>
    void shape(double* x, std::uint32_t n, bool exciter, std::uint32_t c) noexcept SONORA_RT {
        DcBlocker& dc = dc_[c];
        if (exciter) {
            for (std::uint32_t i = 0; i < n; ++i) x[i] = dc.tick(Curve::f(x[i]) - x[i]);
        } else {
            for (std::uint32_t i = 0; i < n; ++i) x[i] = dc.tick(Curve::f(x[i]));
        }
    }

    dsp::Oversampler os_;
    dsp::DelayLine dry_[2];
    DcBlocker dc_[2];
    OnePole split_[2][2];
    OnePole tone_lp_[2];
    std::uint32_t factor_ = 4, latency_ = 0;
    double tone_pole_ = 0.0;
    std::vector<float> scaled_, wet_;
    std::vector<double> drive_, inv_drive_, tone_, mix_, post_, pole_;
};

}  // namespace

std::unique_ptr<Processor> make_saturator() { return std::make_unique<Saturator>(); }

}  // namespace sonora::native
