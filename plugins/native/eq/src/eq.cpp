// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/eq.h"

#include <array>
#include <complex>
#include <cmath>
#include <vector>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/oversampling.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace eq;

const char* const kOffOn[] = {"off", "on"};
const char* const kModes[] = {"zero latency", "analog matched (2x)"};
const char* const kSlopeNames[] = {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct"};

// On/off is a 10 ms crossfade, frequencies, gains and Qs ramp over 20 ms:
// long enough that a full-range sweep at block rate leaves no step above
// -60 dBFS (the zipper check), short enough to follow automation.
constexpr ParamInfo toggle(const char* id, const char* name, float def) {
    return {id, name, Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, def, 10.0f, true, false, false, "%.0f", kOffOn, 2};
}
constexpr ParamInfo freq(const char* id, const char* name, float lo, float hi, float def) {
    return {id, name, Unit::Hertz, Taper::Log, lo, hi, def, 20.0f, true, false, false, "%.0f Hz", nullptr, 0};
}
constexpr ParamInfo gain(const char* id, const char* name) {
    return {id, name, Unit::Decibel, Taper::Linear, -24.0f, 24.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0};
}
constexpr ParamInfo qual(const char* id, const char* name, float lo, float hi, float def) {
    return {id, name, Unit::Q, Taper::Log, lo, hi, def, 20.0f, true, false, false, "%.2f", nullptr, 0};
}
constexpr ParamInfo slope(const char* id, const char* name) {
    return {id, name, Unit::Choice, Taper::Stepped, 0.0f, 5.0f, 1.0f, 0.0f, false, false, true, "%.0f", kSlopeNames, 6};
}

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"mode", "Mode", Unit::Choice, Taper::Stepped, 0.0f, 1.0f, 1.0f, 0.0f, false, true, false, "%.0f", kModes, 2},
    {"output", "Output", Unit::Decibel, Taper::Linear, -24.0f, 24.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
    toggle("hpf_on", "High-pass", 0.0f),
    freq("hpf_freq", "High-pass frequency", 10.0f, 2000.0f, 80.0f),
    slope("hpf_slope", "High-pass slope"),
    toggle("lpf_on", "Low-pass", 0.0f),
    freq("lpf_freq", "Low-pass frequency", 1000.0f, 22000.0f, 18000.0f),
    slope("lpf_slope", "Low-pass slope"),
    toggle("low_shelf_on", "Low shelf", 1.0f),
    freq("low_shelf_freq", "Low shelf frequency", 20.0f, 2000.0f, 100.0f),
    gain("low_shelf_gain", "Low shelf gain"),
    qual("low_shelf_q", "Low shelf Q", 0.5f, 1.5f, 0.7071f),
    toggle("high_shelf_on", "High shelf", 1.0f),
    freq("high_shelf_freq", "High shelf frequency", 1000.0f, 20000.0f, 10000.0f),
    gain("high_shelf_gain", "High shelf gain"),
    qual("high_shelf_q", "High shelf Q", 0.5f, 1.5f, 0.7071f),
    toggle("bell1_on", "Bell 1", 1.0f),
    freq("bell1_freq", "Bell 1 frequency", 20.0f, 20000.0f, 250.0f),
    gain("bell1_gain", "Bell 1 gain"),
    qual("bell1_q", "Bell 1 Q", 0.1f, 18.0f, 1.0f),
    toggle("bell2_on", "Bell 2", 1.0f),
    freq("bell2_freq", "Bell 2 frequency", 20.0f, 20000.0f, 1000.0f),
    gain("bell2_gain", "Bell 2 gain"),
    qual("bell2_q", "Bell 2 Q", 0.1f, 18.0f, 1.0f),
    toggle("bell3_on", "Bell 3", 1.0f),
    freq("bell3_freq", "Bell 3 frequency", 20.0f, 20000.0f, 3000.0f),
    gain("bell3_gain", "Bell 3 gain"),
    qual("bell3_q", "Bell 3 Q", 0.1f, 18.0f, 1.0f),
    toggle("bell4_on", "Bell 4", 1.0f),
    freq("bell4_freq", "Bell 4 frequency", 20.0f, 20000.0f, 8000.0f),
    gain("bell4_gain", "Bell 4 gain"),
    qual("bell4_q", "Bell 4 Q", 0.1f, 18.0f, 1.0f),
};

const Descriptor kDesc = {"org.sonora.eq", "Sonora EQ", "EQ", 1, kParams, kParamCount, 1, 2, false};

// Stages: 0-3 high-pass sections, 4-7 low-pass, 8 low shelf, 9 high
// shelf, 10-13 bells. Groups: 0 HPF, 1 LPF, 2 LS, 3 HS, 4-7 bells.
constexpr int kStages = 14;
constexpr int kGroups = 8;
constexpr std::uint32_t kControl = 16;  // base-rate samples between retunes

// What an editor draws (ui_hints.h): sections per band, level meters and the
// magnitude response of the whole EQ, which every band's controls shape.
constexpr std::uint32_t kMeterIn = 0, kMeterOut = 1;
constexpr std::uint32_t kCurvePoints = 128;
constexpr float kCurveFmin = 20.0f, kCurveFmax = 20000.0f;
constexpr float kCurveDbMin = -24.0f, kCurveDbMax = 24.0f;
const MeterDecl kMeters[] = {kInputMeter, kOutputMeter};
const CurveDecl kCurves[] = {{"response", "Frequency response", "Hz", "dB", kCurvePoints, true, kCurveFmin,
                              kCurveFmax, kCurveDbMin, kCurveDbMax}};
const ParamHint kHints[] = {
    {"mode", "Output", Widget::Choice, nullptr},
    {"output", "Output", Widget::Knob, "response"},
    {"hpf_on", "High-pass", Widget::Switch, "response"},
    {"hpf_freq", "High-pass", Widget::Knob, "response"},
    {"hpf_slope", "High-pass", Widget::Choice, "response"},
    {"lpf_on", "Low-pass", Widget::Switch, "response"},
    {"lpf_freq", "Low-pass", Widget::Knob, "response"},
    {"lpf_slope", "Low-pass", Widget::Choice, "response"},
    {"low_shelf_on", "Low shelf", Widget::Switch, "response"},
    {"low_shelf_freq", "Low shelf", Widget::Knob, "response"},
    {"low_shelf_gain", "Low shelf", Widget::Knob, "response"},
    {"low_shelf_q", "Low shelf", Widget::Knob, "response"},
    {"high_shelf_on", "High shelf", Widget::Switch, "response"},
    {"high_shelf_freq", "High shelf", Widget::Knob, "response"},
    {"high_shelf_gain", "High shelf", Widget::Knob, "response"},
    {"high_shelf_q", "High shelf", Widget::Knob, "response"},
    {"bell1_on", "Bell 1", Widget::Switch, "response"},
    {"bell1_freq", "Bell 1", Widget::Knob, "response"},
    {"bell1_gain", "Bell 1", Widget::Knob, "response"},
    {"bell1_q", "Bell 1", Widget::Knob, "response"},
    {"bell2_on", "Bell 2", Widget::Switch, "response"},
    {"bell2_freq", "Bell 2", Widget::Knob, "response"},
    {"bell2_gain", "Bell 2", Widget::Knob, "response"},
    {"bell2_q", "Bell 2", Widget::Knob, "response"},
    {"bell3_on", "Bell 3", Widget::Switch, "response"},
    {"bell3_freq", "Bell 3", Widget::Knob, "response"},
    {"bell3_gain", "Bell 3", Widget::Knob, "response"},
    {"bell3_q", "Bell 3", Widget::Knob, "response"},
    {"bell4_on", "Bell 4", Widget::Switch, "response"},
    {"bell4_freq", "Bell 4", Widget::Knob, "response"},
    {"bell4_gain", "Bell 4", Widget::Knob, "response"},
    {"bell4_q", "Bell 4", Widget::Knob, "response"},
};
const UiHints kUi = {kHints, sizeof kHints / sizeof kHints[0], kMeters, 2, kCurves, 1};

class Eq final : public ProcessorBase {
public:
    Eq() : ProcessorBase(kDesc) { publish_levels(kMeterIn, kMeterOut); }
    const UiHints* ui_hints() const noexcept override { return &kUi; }

protected:
    // The response of the bands as they are set (the targets, not the
    // smoothed values in flight), recomputed only when a parameter moved.
    void on_publish(Telemetry& t) noexcept override {
        bool changed = !curve_valid_;
        for (std::uint32_t i = kMode; i < kParamCount; ++i) {
            if (value(i) != curve_params_[i]) {
                curve_params_[i] = value(i);
                changed = true;
            }
        }
        if (!changed) return;
        curve_valid_ = true;
        dsp::BiquadCoeffs sections[kStages];
        int n = 0;
        const auto add_filter = [&](bool high_pass, std::uint32_t on, std::uint32_t freq, std::uint32_t slope) {
            if (value(on) < 0.5f) return;
            dsp::FilterSpec secs[5];
            const int count = dsp::butterworth_sections(sections_for(static_cast<int>(value(slope))), high_pass,
                                                        value(freq), secs);
            for (int k = 0; k < count && n < kStages; ++k) sections[n++] = dsp::design_matched(secs[k], irate_);
        };
        const auto add_band = [&](dsp::Shape shape, std::uint32_t on, std::uint32_t freq, std::uint32_t gain_i,
                                  std::uint32_t q) {
            const double eff = value(gain_i) * (value(on) >= 0.5f ? 1.0 : 0.0);
            if (eff == 0.0 || n >= kStages) return;
            sections[n++] = dsp::design_matched({shape, value(freq), value(q), eff}, irate_);
        };
        add_filter(true, kHpfOn, kHpfFreq, kHpfSlope);
        add_filter(false, kLpfOn, kLpfFreq, kLpfSlope);
        add_band(dsp::Shape::LowShelf, kLowShelfOn, kLowShelfFreq, kLowShelfGain, kLowShelfQ);
        add_band(dsp::Shape::HighShelf, kHighShelfOn, kHighShelfFreq, kHighShelfGain, kHighShelfQ);
        for (int b = 0; b < 4; ++b)
            add_band(dsp::Shape::Peak, bell(b, 0), bell(b, 1), bell(b, 2), bell(b, 3));
        const double out_db = value(kOutput);
        const double ratio = static_cast<double>(kCurveFmax) / kCurveFmin;
        for (std::uint32_t i = 0; i < kCurvePoints; ++i) {
            const double f = kCurveFmin * std::pow(ratio, static_cast<double>(i) / (kCurvePoints - 1));
            double mag = 1.0;
            for (int s = 0; s < n; ++s) mag *= std::abs(dsp::response(sections[s], f, irate_));
            curve_[i] = static_cast<float>(out_db + dsp::gain_to_db(mag, -200.0));
        }
        t.curve(0, curve_, kCurvePoints);
    }

    bool on_prepare() override {
        factor_ = value(kMode) >= 0.5f ? 2u : 1u;
        irate_ = sample_rate() * factor_;
        if (!os_.prepare(factor_, channels(), max_frames(), 0.4535, 100.0)) return false;
        set_latency(os_.latency());
        st_.assign(channels(), {});
        work_.assign(static_cast<std::size_t>(channels()) * max_frames(), 0.0);
        bufs_.assign(channels(), nullptr);
        for (int g = 0; g < kGroups; ++g) dirty_[g] = true;
        hpf_sections_ = sections_for(static_cast<int>(value(kHpfSlope)));
        lpf_sections_ = sections_for(static_cast<int>(value(kLpfSlope)));
        retune(0, true);
        curve_valid_ = false;
        return true;
    }

    void on_reset() noexcept override {
        os_.reset();
        for (auto& ch : st_) {
            for (auto& s : ch) s.reset();
        }
    }

    void on_param(std::uint32_t index, bool jump) noexcept override {
        if (index == kHpfSlope) {
            // Applied at the bottom of a declick (or outside playback): the
            // section count changes, so the old sections' state is dropped.
            hpf_sections_ = sections_for(static_cast<int>(value(kHpfSlope)));
            for (auto& ch : st_) {
                for (int s = 0; s < 4; ++s) ch[s].reset();
            }
        } else if (index == kLpfSlope) {
            lpf_sections_ = sections_for(static_cast<int>(value(kLpfSlope)));
            for (auto& ch : st_) {
                for (int s = 4; s < 8; ++s) ch[s].reset();
            }
        }
        const int g = group_of(index);
        if (g >= 0) dirty_[g] = true;
        if (jump && !st_.empty()) retune(0, true);
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        for (std::uint32_t pos = 0; pos < frames; pos += kControl) {
            const std::uint32_t m = frames - pos < kControl ? frames - pos : kControl;
            render_block(in, out, pos, m);
        }
    }

private:
    static int sections_for(int slope_index) noexcept {
        const int order = kSlopes[dsp::clamp(slope_index, 0, 5)] / 6;
        return order;  // the order; sections are computed from it
    }

    static int group_of(std::uint32_t index) noexcept {
        if (index >= kHpfOn && index <= kHpfSlope) return 0;
        if (index >= kLpfOn && index <= kLpfSlope) return 1;
        if (index >= kLowShelfOn && index <= kLowShelfQ) return 2;
        if (index >= kHighShelfOn && index <= kHighShelfQ) return 3;
        if (index >= kBell1On && index < kParamCount) return 4 + static_cast<int>((index - kBell1On) / 4);
        return -1;
    }

    // Advances the band parameters by m base samples and moves each band
    // whose parameters move to its new design, ramped over the block.
    void retune(std::uint32_t m, bool jump) noexcept {
        const std::uint32_t ramp = jump ? 0 : m * factor_;
        // High- and low-pass: Butterworth cascades.
        for (int g = 0; g < 2; ++g) {
            const std::uint32_t f_idx = g == 0 ? kHpfFreq : kLpfFreq;
            const std::uint32_t on_idx = g == 0 ? kHpfOn : kLpfOn;
            ParamSmoother& fs = smoother(f_idx);
            const bool moving = fs.moving();
            const double f = jump ? fs.current() : fs.advance(m);
            w_from_[g] = smoother(on_idx).current();
            w_to_[g] = jump ? w_from_[g] : smoother(on_idx).advance(m);
            if (!(dirty_[g] || moving)) continue;
            dirty_[g] = fs.moving();
            dsp::FilterSpec secs[5];
            const int order = g == 0 ? hpf_sections_ : lpf_sections_;
            const int n = dsp::butterworth_sections(order, g == 0, f, secs);
            n_secs_[g] = n;
            for (int k = 0; k < 4; ++k) {
                const int stage = 4 * g + k;
                if (k < n) {
                    const auto c = dsp::to_svf(dsp::design_matched(secs[k], irate_));
                    if (ramp == 0) ramp_[stage].set(c);
                    else ramp_[stage].ramp_to(c, ramp);
                }
            }
        }
        // Shelves and bells: the gain is scaled by the smoothed on/off, so
        // switching a band is a gain ramp to 0 dB, not a step.
        for (int g = 2; g < kGroups; ++g) {
            std::uint32_t on_i, f_i, g_i, q_i;
            dsp::Shape shape;
            if (g == 2) {
                on_i = kLowShelfOn; f_i = kLowShelfFreq; g_i = kLowShelfGain; q_i = kLowShelfQ;
                shape = dsp::Shape::LowShelf;
            } else if (g == 3) {
                on_i = kHighShelfOn; f_i = kHighShelfFreq; g_i = kHighShelfGain; q_i = kHighShelfQ;
                shape = dsp::Shape::HighShelf;
            } else {
                on_i = bell(g - 4, 0); f_i = bell(g - 4, 1); g_i = bell(g - 4, 2); q_i = bell(g - 4, 3);
                shape = dsp::Shape::Peak;
            }
            const bool moving = smoother(on_i).moving() || smoother(f_i).moving() ||
                                smoother(g_i).moving() || smoother(q_i).moving();
            const double on = jump ? smoother(on_i).current() : smoother(on_i).advance(m);
            const double fr = jump ? smoother(f_i).current() : smoother(f_i).advance(m);
            const double gd = jump ? smoother(g_i).current() : smoother(g_i).advance(m);
            const double q = jump ? smoother(q_i).current() : smoother(q_i).advance(m);
            if (!(dirty_[g] || moving)) continue;
            dirty_[g] = smoother(on_i).moving() || smoother(f_i).moving() ||
                        smoother(g_i).moving() || smoother(q_i).moving();
            const double eff = gd * on;
            const int stage = 6 + g;  // 8, 9, 10..13
            const bool was_active = active_[stage];
            active_[stage] = eff != 0.0 || dirty_[g];
            if (!active_[stage]) {
                ramp_[stage].set(dsp::SvfCoeffs{});
                continue;
            }
            if (!was_active) {
                // A band that was skipped resumes from rest, at 0 dB.
                for (auto& ch : st_) ch[stage].reset();
                ramp_[stage].set(dsp::to_svf(dsp::BiquadCoeffs{}));
            }
            const auto c = dsp::to_svf(dsp::design_matched({shape, fr, q, eff}, irate_));
            if (ramp == 0) ramp_[stage].set(c);
            else ramp_[stage].ramp_to(c, ramp);
        }
    }

    void render_block(const float* const* in, float* const* out, std::uint32_t pos,
                      std::uint32_t m) noexcept {
        retune(m, false);
        const std::uint32_t n = m * factor_;
        const std::uint32_t chs = channels();
        for (std::uint32_t c = 0; c < chs; ++c) {
            if (factor_ == 1) {
                double* w = &work_[static_cast<std::size_t>(c) * max_frames()];
                for (std::uint32_t i = 0; i < m; ++i) w[i] = in[c][pos + i];
                bufs_[c] = w;
            } else {
                bufs_[c] = os_.up(c, in[c] + pos, m);
            }
        }
        // Which stages run this block: the filters while switched on or
        // fading, the bands while not at 0 dB.
        bool run_f[2];
        for (int g = 0; g < 2; ++g) {
            run_f[g] = w_from_[g] > 0.0 || w_to_[g] > 0.0;
            if (!run_f[g] && !was_running_[g]) continue;
            if (!run_f[g]) {
                for (auto& ch : st_) {
                    for (int k = 0; k < 4; ++k) ch[4 * g + k].reset();
                }
            }
            was_running_[g] = run_f[g];
        }
        const double inv_n = 1.0 / n;
        const dsp::SvfState::Derived* d[kStages];
        for (std::uint32_t i = 0; i < n; ++i) {
            for (int s = 0; s < kStages; ++s) d[s] = &ramp_[s].next();
            const double t = (i + 1) * inv_n;
            const double wh = w_from_[0] + (w_to_[0] - w_from_[0]) * t;
            const double wl = w_from_[1] + (w_to_[1] - w_from_[1]) * t;
            for (std::uint32_t c = 0; c < chs; ++c) {
                auto& st = st_[c];
                double v = bufs_[c][i];
                if (run_f[0]) {
                    double y = v;
                    for (int k = 0; k < n_secs_[0]; ++k) y = st[k].tick(*d[k], y);
                    v += wh * (y - v);
                }
                if (run_f[1]) {
                    double y = v;
                    for (int k = 0; k < n_secs_[1]; ++k) y = st[4 + k].tick(*d[4 + k], y);
                    v += wl * (y - v);
                }
                for (int s = 8; s < kStages; ++s) {
                    if (active_[s]) v = st[s].tick(*d[s], v);
                }
                bufs_[c][i] = v;
            }
        }
        ParamSmoother& og = smoother(kOutput);
        for (std::uint32_t c = 0; c < chs; ++c) {
            if (factor_ == 1) {
                for (std::uint32_t i = 0; i < m; ++i) out[c][pos + i] = static_cast<float>(bufs_[c][i]);
            } else {
                os_.down(c, out[c] + pos, m);
            }
        }
        for (std::uint32_t i = 0; i < m; ++i) {
            const auto g = static_cast<float>(dsp::db_to_gain(og.next()));
            for (std::uint32_t c = 0; c < chs; ++c) out[c][pos + i] *= g;
        }
    }

    std::uint32_t factor_ = 1;
    double irate_ = 48000.0;
    dsp::Oversampler os_;
    dsp::SvfRamp ramp_[kStages];
    bool active_[kStages] = {};
    bool dirty_[kGroups] = {};
    int hpf_sections_ = 2, lpf_sections_ = 2;
    int n_secs_[2] = {1, 1};
    double w_from_[2] = {0.0, 0.0}, w_to_[2] = {0.0, 0.0};
    bool was_running_[2] = {false, false};
    std::vector<std::array<dsp::SvfState, kStages>> st_;
    std::vector<double> work_;
    std::vector<double*> bufs_;
    float curve_[kCurvePoints] = {};
    float curve_params_[kParamCount] = {};
    bool curve_valid_ = false;
};

}  // namespace

const Descriptor& eq_descriptor() { return kDesc; }
std::unique_ptr<Processor> make_eq() { return std::make_unique<Eq>(); }

}  // namespace sonora::native
