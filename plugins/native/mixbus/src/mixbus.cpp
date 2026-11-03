// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/mixbus.h"

#include <cmath>
#include <vector>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/delay_line.h"
#include "sonora/dsp/dynamics.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/oversampling.h"
#include "sonora/dsp/transformer.h"
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
const char* const kQualities[] = {"eco (2x)", "standard (4x)", "high (8x)"};
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
    // Part 2. Quality changes the latency, so it reconfigures.
    {"quality", "Quality", Unit::Choice, Taper::Stepped, 0.0f, 2.0f, 1.0f, 0.0f, false, true, false, "%.0f", kQualities, 3},
    // 0 % is the compressor's output delayed by the latency, exactly.
    {"colour", "Colour", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 0.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"sag", "Sag", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
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
        long q = std::lround(value(kQuality));
        q = q < 0 ? 0 : (q > 2 ? 2 : q);
        factor_ = kFactors[q];
        const std::uint32_t chs = channels();
        // The protected band is 20 kHz at every rate, as the tube's.
        const double passband = std::fmin(0.4535, 20000.0 / sample_rate());
        if (!os_.prepare(factor_, chs, max_frames(), passband)) return false;
        // The core steps at the base rate in eco and at twice it otherwise,
        // the driver's clip at the oversampled rate: the figures the tube's
        // output stage was measured at (brain/research/native-tube-n2.md,
        // "Output stage at class B").
        xf_sub_ = factor_ == 2 ? 2 : factor_ / 2;
        if (!out_stage_.prepare(sample_rate() * factor_, chs, {}, {}, xf_sub_)) return false;
        latency_ = os_.latency();
        set_latency(latency_);
        for (std::uint32_t c = 0; c < kMaxCh; ++c) {
            comp_[c].assign(max_frames(), 0.0f);
            xf_out_[c].assign(static_cast<std::size_t>(max_frames()) * factor_, 0.0);
            wet_[c].assign(max_frames(), 0.0f);
            dry_[c].prepare(latency_ + 1);
        }
        width_.assign(max_frames(), 0.0);
        amount_.assign(max_frames(), 0.0);
        sag_.assign(max_frames(), 0.0);
        xf_active_ = false;
        return true;
    }

    void on_reset() noexcept override {
        for (auto& h : hpf_) h.reset();
        det_.reset(0.0);
        fast_.reset(0.0);
        slow_.reset(0.0);
        os_.reset();
        out_stage_.reset();
        for (auto& d : dry_) d.reset();
        xf_active_ = false;
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

                for (std::uint32_t c = 0; c < chs; ++c) {
                    const double dry = in[c][k];
                    comp_[c][k] = static_cast<float>(dry + mix * (dry * g - dry));
                }
                width_[k] = width;
            }
        }
        colour(frames);
        for (std::uint32_t k = 0; k < frames; ++k) {
            if (chs == 2) {
                // From the rounded floats: at 100 % the sum and the
                // difference are exact in double, so the stage is the
                // identity to the bit.
                const double mid = 0.5 * (static_cast<double>(wet_[0][k]) + wet_[1][k]);
                const double side = 0.5 * (static_cast<double>(wet_[0][k]) - wet_[1][k]) * width_[k];
                out[0][k] = static_cast<float>(mid + side);
                out[1][k] = static_cast<float>(mid - side);
            } else {
                out[0][k] = wet_[0][k];
            }
        }
    }

private:
    // The colour stage on comp_ into wet_ (both delayed by latency_). The
    // oversampler always runs, so its history is warm when the amount
    // leaves 0; only the transformer's solve, the cost, waits for it. At 0
    // and not moving the output is comp_ delayed, to the bit, and the
    // stage is dropped to rest so the next engagement starts from it
    // (tube.cpp does the same).
    void colour(std::uint32_t frames) noexcept {
        const std::uint32_t chs = channels();
        ParamSmoother& cs = smoother(kColour);
        const bool active = cs.moving() || cs.current() != 0.0;
        double* x[kMaxCh] = {nullptr, nullptr};
        for (std::uint32_t c = 0; c < chs; ++c) x[c] = os_.up(c, comp_[c].data(), frames);
        if (active) {
            for (std::uint32_t k = 0; k < frames; ++k) {
                amount_[k] = 0.01 * cs.next();
                sag_[k] = 0.01 * smoother(kSag).next();
            }
            xf_active_ = true;
            const double* ip[kMaxCh] = {x[0], x[1]};
            double* op[kMaxCh] = {xf_out_[0].data(), xf_out_[1].data()};
            const std::uint32_t n = frames * factor_;
            out_stage_.process_block(ip, op, n / xf_sub_, sag_.data(), factor_ / xf_sub_);
            for (std::uint32_t c = 0; c < chs; ++c) {
                const double* y = op[c];
                for (std::uint32_t k = 0; k < frames; ++k) {
                    const double a = amount_[k];
                    for (std::uint32_t j = 0; j < factor_; ++j) {
                        const std::uint32_t i = k * factor_ + j;
                        x[c][i] += a * (y[i] - x[c][i]);
                    }
                }
            }
        } else {
            smoother(kSag).advance(frames);
            if (xf_active_) {
                out_stage_.reset();
                xf_active_ = false;
            }
        }
        for (std::uint32_t c = 0; c < chs; ++c) {
            os_.down(c, wet_[c].data(), frames);
            for (std::uint32_t k = 0; k < frames; ++k) {
                dry_[c].push(comp_[c][k]);
                if (!active) wet_[c][k] = dry_[c].read(latency_);
            }
        }
    }

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
    // The colour stage and its blocks.
    dsp::Oversampler os_;
    dsp::OutputStage out_stage_;
    dsp::DelayLine dry_[kMaxCh];
    std::uint32_t factor_ = 4, xf_sub_ = 2, latency_ = 0;
    bool xf_active_ = false;
    std::vector<float> comp_[kMaxCh], wet_[kMaxCh];
    std::vector<double> xf_out_[kMaxCh], width_, amount_, sag_;
};

}  // namespace

std::unique_ptr<Processor> make_mixbus() { return std::make_unique<MixBus>(); }

}  // namespace sonora::native
