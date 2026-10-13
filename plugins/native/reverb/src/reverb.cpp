// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/reverb.h"

#include <cmath>
#include <vector>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/delay_line.h"
#include "sonora/dsp/fdn.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/smoothing.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace reverb;

const char* const kOffOn[] = {"off", "on"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    // Pre-delay and size are not ramped: a change crossfades taps (reverb.h).
    {"predelay", "Pre-delay", Unit::Milliseconds, Taper::Linear, 0.0f, static_cast<float>(kMaxPredelayMs), 10.0f, 0.0f, true, false, false, "%.1f ms", nullptr, 0},
    {"decay", "Decay", Unit::Seconds, Taper::Log, 0.2f, 20.0f, 2.0f, 50.0f, true, false, false, "%.2f s", nullptr, 0},
    {"size", "Size", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 70.0f, 0.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"diffusion", "Diffusion", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 70.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"low_ratio", "Low decay", Unit::None, Taper::Log, 0.5f, 2.0f, 1.0f, 50.0f, true, false, false, "%.2f x", nullptr, 0},
    {"low_freq", "Low crossover", Unit::Hertz, Taper::Log, 50.0f, 1000.0f, 250.0f, 50.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"high_ratio", "High decay", Unit::None, Taper::Log, 0.1f, 1.0f, 0.5f, 50.0f, true, false, false, "%.2f x", nullptr, 0},
    {"high_freq", "High crossover", Unit::Hertz, Taper::Log, 1000.0f, 16000.0f, 5000.0f, 50.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"low_cut", "Low cut", Unit::Hertz, Taper::Log, 20.0f, 1000.0f, 20.0f, 20.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"high_cut", "High cut", Unit::Hertz, Taper::Log, 1000.0f, 20000.0f, 20000.0f, 20.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"width", "Width", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    // Ramped inside the processor (50 ms), so a toggle is never a step.
    {"freeze", "Freeze", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, true, false, false, "%.0f", kOffOn, 2},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 25.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
};

const Descriptor kDesc = {"org.sonora.reverb", "Sonora Reverb", "Reverb", 1, kParams, kParamCount, 1, 2, false};

// Coefficients (decay filters, wet filters) and taps are updated every
// kControl samples, counted across render() calls, so the output does not
// depend on how a host splits its blocks.
constexpr std::uint32_t kControl = 32;
constexpr double kFadeSeconds = 0.030;
constexpr double kFreezeSeconds = 0.050;
constexpr std::uint32_t kSizeSteps = 101;  // size in whole percent
constexpr std::uint32_t kAllpasses = 4;
// The diffusers' delays, in ms, a little apart between the channels so the
// two inputs are smeared differently. Ratios near the golden mean keep
// their echoes from lining up.
constexpr double kDiffuserMs[2][kAllpasses] = {{1.31, 2.17, 3.53, 5.71}, {1.43, 2.29, 3.67, 5.89}};

// A pre-delay tap and, while a change is under way, the one it crossfades
// from (as the delay's taps do). Read after the push: read(0) is this
// sample, so a pre-delay of 0 is direct.
struct Tap {
    std::uint32_t cur = 0, old = 0;
    std::uint32_t fade = 0;  // samples into the fade; == len when idle
    double read(const dsp::DelayLine& d, std::uint32_t len) const noexcept {
        if (fade >= len) return d.read(cur);
        const double w = 0.5 - 0.5 * std::cos(dsp::kPi * (static_cast<double>(fade) / len));
        return (1.0 - w) * d.read(old) + w * d.read(cur);
    }
};

class Reverb final : public ProcessorBase {
public:
    Reverb() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        const double fs = sample_rate();
        table_.assign(static_cast<std::size_t>(kSizeSteps) * dsp::Fdn::kLines, 0u);
        std::uint32_t longest = 1;
        for (std::uint32_t s = 0; s < kSizeSteps; ++s) {
            std::uint32_t* m = &table_[static_cast<std::size_t>(s) * dsp::Fdn::kLines];
            dsp::Fdn::lengths_for(s / 100.0, fs, m);
            for (std::uint32_t i = 0; i < dsp::Fdn::kLines; ++i) longest = m[i] > longest ? m[i] : longest;
        }
        fdn_.prepare(fs, longest, channels() == 2);
        const auto pre = static_cast<std::uint32_t>(std::ceil(kMaxPredelayMs * 1e-3 * fs)) + 1;
        for (int c = 0; c < 2; ++c) {
            pre_[c].prepare(pre);
            for (std::uint32_t a = 0; a < kAllpasses; ++a) {
                ap_[c][a].prepare(static_cast<std::uint32_t>(std::lround(kDiffuserMs[c][a] * 1e-3 * fs)));
            }
        }
        fade_len_ = static_cast<std::uint32_t>(std::lround(kFadeSeconds * fs));
        freeze_.set_ramp_samples(static_cast<std::uint32_t>(std::lround(kFreezeSeconds * fs)));
        prepared_ = true;
        return true;
    }

    void on_reset() noexcept override {
        fdn_.set_lengths(size_lengths(), 0, true);
        fdn_.set_decay(decay_spec(false), 0);
        fdn_.reset();
        for (int c = 0; c < 2; ++c) {
            pre_[c].reset();
            for (auto& a : ap_[c]) a.reset();
            hp_[c].reset();
            lp_[c].reset();
            tap_[c].cur = tap_[c].old = predelay_samples();
            tap_[c].fade = fade_len_;
        }
        update_filters(0, true);
        freeze_.reset(value(kFreeze) >= 0.5f ? 1.0 : 0.0);
        ctl_left_ = 0;
    }

    void on_param(std::uint32_t index, bool jump) noexcept override {
        if (index == kFreeze) {
            const double f = value(kFreeze) >= 0.5f ? 1.0 : 0.0;
            if (jump) freeze_.reset(f);
            else freeze_.set_target(f);
            return;
        }
        // Outside playback a value takes effect at once; before prepare()
        // there is nothing to apply it to, and on_reset() will.
        if (!jump || !prepared_) return;
        if (index == kSize || index == kDecay || index == kLowRatio || index == kLowFreq ||
            index == kHighRatio || index == kHighFreq) {
            fdn_.set_lengths(size_lengths(), 0, true);
            fdn_.set_decay(decay_spec(false), 0);
        } else if (index == kLowCut || index == kHighCut) {
            update_filters(0, true);
        } else if (index == kPredelay) {
            for (auto& t : tap_) {
                t.cur = t.old = predelay_samples();
                t.fade = fade_len_;
            }
        }
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        const double g_span = kMaxDiffusion - kMinDiffusion;
        for (std::uint32_t k = 0; k < frames; ++k) {
            if (ctl_left_ == 0) {
                control();
                ctl_left_ = kControl;
            }
            --ctl_left_;
            const double g = kMinDiffusion + g_span * 0.01 * smoother(kDiffusion).next();
            const double width = 0.01 * smoother(kWidth).next();
            const double mix = 0.01 * smoother(kMix).next();
            const double frz = freeze_.next();
            const dsp::SvfState::Derived& dh = hp_ramp_.next();
            const dsp::SvfState::Derived& dl = lp_ramp_.next();

            double x[2] = {0.0, 0.0};
            for (std::uint32_t c = 0; c < chs; ++c) {
                pre_[c].push(in[c][k]);
                double v = tap_[c].read(pre_[c], fade_len_);
                if (tap_[c].fade < fade_len_) ++tap_[c].fade;
                for (auto& a : ap_[c]) v = a.tick(v, g);
                x[c] = v;
            }
            double wl, wr;
            fdn_.tick(x[0], x[1], frz, wl, wr);
            if (chs == 2) {
                const double m = 0.5 * (wl + wr), s = 0.5 * width * (wl - wr);
                wl = m + s;
                wr = m - s;
            }
            const double wet[2] = {wl, wr};
            for (std::uint32_t c = 0; c < chs; ++c) {
                const double w = lp_[c].tick(dl, hp_[c].tick(dh, wet[c]));
                const double dry = in[c][k];
                out[c][k] = static_cast<float>(dry + mix * (w - dry));
            }
        }
    }

private:
    const std::uint32_t* size_lengths() const noexcept {
        long s = std::lround(value(kSize));
        s = s < 0 ? 0 : (s > 100 ? 100 : s);
        return &table_[static_cast<std::size_t>(s) * dsp::Fdn::kLines];
    }

    std::uint32_t predelay_samples() const noexcept {
        const long n = std::lround(value(kPredelay) * 1e-3 * sample_rate());
        return static_cast<std::uint32_t>(n < 0 ? 0 : n);
    }

    // advance: move the smoothers kControl samples on (playback); else
    // read where they are.
    dsp::DecaySpec decay_spec(bool advance) noexcept {
        auto at = [&](std::uint32_t i) {
            return advance ? smoother(i).advance(kControl) : smoother(i).current();
        };
        dsp::DecaySpec s;
        s.t60_s = at(kDecay);
        s.low_ratio = at(kLowRatio);
        s.low_hz = at(kLowFreq);
        s.high_ratio = at(kHighRatio);
        s.high_hz = at(kHighFreq);
        return s;
    }

    void control() noexcept {
        // Size: a crossfade to the new lengths, started only from rest; a
        // fast sweep becomes a chain of clean 30 ms crossfades.
        // Held while freeze is on or on its way: nothing absorbs then, so a
        // longer tap re-reading history and a shorter one dropping some
        // would move the held energy, and sweeps ratchet it (measured at
        // the network, -11 to +3.5 dB over 50 round trips, test_fdn.cpp).
        // The new size applies once freeze is fully off.
        bool redesign = false;
        const bool frozen = freeze_.current() != 0.0 || freeze_.target() != 0.0;
        if (!fdn_.fading() && !frozen) {
            const std::uint32_t* want = size_lengths();
            const std::uint32_t* have = fdn_.lengths();
            bool differ = false;
            for (std::uint32_t i = 0; i < dsp::Fdn::kLines; ++i) differ |= want[i] != have[i];
            if (differ) {
                fdn_.set_lengths(want, fade_len_, false);
                redesign = true;  // absorption follows each line's length
            }
        }
        const std::uint32_t want_pre = predelay_samples();
        for (auto& t : tap_) {
            if (want_pre != t.cur && t.fade >= fade_len_) {
                t.old = t.cur;
                t.cur = want_pre;
                t.fade = 0;
            }
        }
        const bool moving = smoother(kDecay).moving() || smoother(kLowRatio).moving() ||
                            smoother(kLowFreq).moving() || smoother(kHighRatio).moving() ||
                            smoother(kHighFreq).moving();
        if (moving || redesign) fdn_.set_decay(decay_spec(true), kControl);
        if (smoother(kLowCut).moving() || smoother(kHighCut).moving()) update_filters(kControl, false);
    }

    void update_filters(std::uint32_t m, bool jump) noexcept {
        const double fh = jump ? smoother(kLowCut).current() : smoother(kLowCut).advance(m);
        const double fl = jump ? smoother(kHighCut).current() : smoother(kHighCut).advance(m);
        const double q = 0.7071067811865476;
        const auto ch = dsp::to_svf(dsp::design_matched({dsp::Shape::HighPass, fh, q, 0.0}, sample_rate()));
        const auto cl = dsp::to_svf(dsp::design_matched({dsp::Shape::LowPass, fl, q, 0.0}, sample_rate()));
        if (jump) {
            hp_ramp_.set(ch);
            lp_ramp_.set(cl);
        } else {
            hp_ramp_.ramp_to(ch, m);
            lp_ramp_.ramp_to(cl, m);
        }
    }

    bool prepared_ = false;
    std::vector<std::uint32_t> table_;  // kSizeSteps x kLines lengths
    dsp::Fdn fdn_;
    dsp::DelayLine pre_[2];
    Tap tap_[2];
    dsp::SchroederAllpass ap_[2][kAllpasses];
    dsp::SvfRamp hp_ramp_, lp_ramp_;
    dsp::SvfState hp_[2], lp_[2];
    dsp::LinearSmoother freeze_;
    std::uint32_t fade_len_ = 1;
    std::uint32_t ctl_left_ = 0;
};

}  // namespace

std::unique_ptr<Processor> make_reverb() { return std::make_unique<Reverb>(); }

}  // namespace sonora::native
