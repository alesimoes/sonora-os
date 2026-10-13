// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/delay.h"

#include <cmath>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/delay_line.h"
#include "sonora/dsp/math.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace delay;

const char* const kOffOn[] = {"off", "on"};
const char* const kNotes[] = {"1/64", "1/32 T", "1/32", "1/16 T", "1/32 D", "1/16", "1/8 T", "1/16 D", "1/8",
                              "1/4 T", "1/8 D", "1/4", "1/2 T", "1/4 D", "1/2", "1/1 T", "1/2 D", "1/1"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"sync", "Tempo sync", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 1.0f, 0.0f, true, false, false, "%.0f", kOffOn, 2},
    // Times are not ramped: a change crossfades taps (see delay.h).
    {"time_l", "Time left", Unit::Milliseconds, Taper::Log, 1.0f, static_cast<float>(kMaxTimeSeconds * 1e3), 375.0f, 0.0f, true, false, false, "%.1f ms", nullptr, 0},
    {"note_l", "Note left", Unit::Choice, Taper::Stepped, 0.0f, 17.0f, 11.0f, 0.0f, true, false, false, "%.0f", kNotes, kNoteCount},
    {"link", "Link right to left", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, true, false, false, "%.0f", kOffOn, 2},
    {"time_r", "Time right", Unit::Milliseconds, Taper::Log, 1.0f, static_cast<float>(kMaxTimeSeconds * 1e3), 500.0f, 0.0f, true, false, false, "%.1f ms", nullptr, 0},
    {"note_r", "Note right", Unit::Choice, Taper::Stepped, 0.0f, 17.0f, 10.0f, 0.0f, true, false, false, "%.0f", kNotes, kNoteCount},
    {"tempo", "Tempo (without host tempo)", Unit::Bpm, Taper::Linear, static_cast<float>(kMinTempoBpm), 300.0f, 120.0f, 0.0f, true, false, false, "%.1f BPM", nullptr, 0},
    {"feedback", "Feedback", Unit::Percent, Taper::Linear, 0.0f, 95.0f, 35.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"pingpong", "Ping-pong", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kOffOn, 2},
    {"hpf", "Feedback high-pass", Unit::Hertz, Taper::Log, 20.0f, 2000.0f, 20.0f, 20.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"lpf", "Feedback low-pass", Unit::Hertz, Taper::Log, 1000.0f, 20000.0f, 20000.0f, 20.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 30.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
};

const Descriptor kDesc = {"org.sonora.delay", "Sonora Delay", "Delay", 1, kParams, kParamCount, 1, 2, false};

constexpr std::uint32_t kControl = 16;
constexpr double kFadeSeconds = 0.030;

// One channel's tap: the current delay, and while a change is under way,
// the old one it crossfades from.
struct Tap {
    std::uint32_t cur = 1, old = 1;
    std::uint32_t fade = 0;  // samples into the fade; == len when idle
    // Read before this sample is pushed: read(0) is the previous input, so
    // a delay of n samples is read(n - 1).
    double read(const dsp::DelayLine& d, std::uint32_t len) const noexcept {
        if (fade >= len) return d.read(cur - 1);
        const double w = 0.5 - 0.5 * std::cos(dsp::kPi * (static_cast<double>(fade) / len));
        return (1.0 - w) * d.read(old - 1) + w * d.read(cur - 1);
    }
};

class Delay final : public ProcessorBase {
public:
    Delay() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        max_ = static_cast<std::uint32_t>(std::ceil(kMaxSeconds * sample_rate())) + 1;
        fade_len_ = static_cast<std::uint32_t>(std::lround(kFadeSeconds * sample_rate()));
        for (auto& l : line_) l.prepare(max_);
        update_filters(0, true);
        return true;
    }

    void on_reset() noexcept override {
        for (int c = 0; c < 2; ++c) {
            line_[c].reset();
            hp_[c].reset();
            lp_[c].reset();
            tap_[c].cur = tap_[c].old = target(c);
            tap_[c].fade = fade_len_;
        }
    }

    void on_param(std::uint32_t index, bool jump) noexcept override {
        if (jump && (index == kHpf || index == kLpf)) update_filters(0, true);
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        const bool ping = chs == 2 && value(kPingPong) >= 0.5f;
        for (std::uint32_t pos = 0; pos < frames; pos += kControl) {
            const std::uint32_t m = frames - pos < kControl ? frames - pos : kControl;
            if (smoother(kHpf).moving() || smoother(kLpf).moving()) update_filters(m, false);
            else {
                smoother(kHpf).advance(m);
                smoother(kLpf).advance(m);
            }
            for (std::uint32_t c = 0; c < chs; ++c) {
                Tap& t = tap_[c];
                const std::uint32_t want = target(c);
                // A new time starts a fade only from rest: during a fade the
                // target is re-read when it ends, so a fast sweep becomes a
                // chain of clean 30 ms crossfades.
                if (want != t.cur && t.fade >= fade_len_) {
                    t.old = t.cur;
                    t.cur = want;
                    t.fade = 0;
                }
            }
            for (std::uint32_t i = 0; i < m; ++i) {
                const std::uint32_t k = pos + i;
                const double fb = smoother(kFeedback).next() * 0.01;
                const double mix = smoother(kMix).next() * 0.01;
                const dsp::SvfState::Derived& dh = hp_ramp_.next();
                const dsp::SvfState::Derived& dl = lp_ramp_.next();
                double wet[2];
                for (std::uint32_t c = 0; c < chs; ++c) {
                    const double tap = tap_[c].read(line_[c], fade_len_);
                    if (tap_[c].fade < fade_len_) ++tap_[c].fade;
                    // Filters in the loop shape every repeat; the wet output
                    // is the filtered repeat, as it is fed back.
                    wet[c] = lp_[c].tick(dl, hp_[c].tick(dh, tap));
                }
                for (std::uint32_t c = 0; c < chs; ++c) {
                    double into;
                    if (ping) {
                        // Mono sum into the left line; each line feeds the
                        // other, so repeats alternate left, right, left.
                        into = c == 0 ? 0.5 * (in[0][k] + in[1][k]) + fb * wet[1] : fb * wet[0];
                    } else {
                        into = in[c][k] + fb * wet[c];
                    }
                    line_[c].push(static_cast<float>(into));
                    const double dry = in[c][k];
                    out[c][k] = static_cast<float>(dry + mix * (wet[c] - dry));
                }
            }
        }
    }

private:
    // The wanted delay of channel c in whole samples.
    std::uint32_t target(int c) noexcept {
        const bool right = c == 1 && value(kLink) < 0.5f;
        double sec;
        if (value(kSync) >= 0.5f) {
            double bpm = transport().tempo_valid && transport().tempo_bpm > 0.0
                             ? transport().tempo_bpm
                             : static_cast<double>(value(kTempo));
            // Below the floor the longest notes would not fit the line
            // (delay.h). Bounded, and reported for this block.
            if (bpm < kMinTempoBpm) {
                bpm = kMinTempoBpm;
                note_bounded();
            }
            const auto idx = static_cast<std::uint32_t>(value(right ? kNoteR : kNoteL));
            sec = kNoteBeats[idx < kNoteCount ? idx : kNoteCount - 1] * 60.0 / bpm;
        } else {
            sec = value(right ? kTimeR : kTimeL) * 1e-3;
        }
        double n = std::round(sec * sample_rate());
        if (n < 1.0) n = 1.0;
        // Unreachable within the parameters' ranges; reported if it ever is.
        if (n > max_ - 1) {
            n = max_ - 1;
            note_bounded();
        }
        return static_cast<std::uint32_t>(n);
    }

    void update_filters(std::uint32_t m, bool jump) noexcept {
        const double fh = jump ? smoother(kHpf).current() : smoother(kHpf).advance(m);
        const double fl = jump ? smoother(kLpf).current() : smoother(kLpf).advance(m);
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

    std::uint32_t max_ = 1;
    std::uint32_t fade_len_ = 1;
    dsp::DelayLine line_[2];
    Tap tap_[2];
    dsp::SvfRamp hp_ramp_, lp_ramp_;
    dsp::SvfState hp_[2], lp_[2];
};

}  // namespace

std::unique_ptr<Processor> make_delay() { return std::make_unique<Delay>(); }

}  // namespace sonora::native
