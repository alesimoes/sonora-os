// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/passiveeq.h"

#include <array>
#include <cmath>
#include <vector>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/transformer.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace passiveeq;

const char* const kLfNames[] = {"20 Hz", "30 Hz", "60 Hz", "100 Hz"};
const char* const kHfNames[] = {"3 kHz", "4 kHz", "5 kHz", "8 kHz", "10 kHz", "12 kHz", "16 kHz"};
const char* const kHfAttenNames[] = {"5 kHz", "10 kHz", "20 kHz"};
const char* const kOffOn[] = {"off", "on"};

constexpr ParamInfo db_knob(const char* id, const char* name, float lo, float hi, float def) {
    return {id, name, Unit::Decibel, Taper::Linear, lo, hi, def, 20.0f, true, false, false, "%.1f dB", nullptr, 0};
}
constexpr ParamInfo pick(const char* id, const char* name, float n, const char* const* names, int count) {
    return {id, name, Unit::Choice, Taper::Stepped, 0.0f, n - 1.0f, 0.0f, 0.0f, false, true, false, "%.0f", names, count};
}

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    db_knob("lf_boost", "Low boost", 0.0f, 14.0f, 0.0f),
    db_knob("lf_atten", "Low cut", 0.0f, 16.0f, 0.0f),
    pick("lf_freq", "Low frequency", 4.0f, kLfNames, 4),
    db_knob("hf_boost", "High boost", 0.0f, 16.0f, 0.0f),
    {"hf_q", "High bandwidth (Q)", Unit::Q, Taper::Log, 0.3f, 3.0f, 0.8f, 20.0f, true, false, false, "%.2f", nullptr, 0},
    pick("hf_freq", "High boost frequency", 7.0f, kHfNames, 7),
    db_knob("hf_atten", "High cut", 0.0f, 20.0f, 0.0f),
    pick("hf_atten_freq", "High cut frequency", 3.0f, kHfAttenNames, 3),
    pick("iron", "Iron", 2.0f, kOffOn, 2),
    db_knob("drive", "Drive", 0.0f, 24.0f, 6.0f),
    db_knob("output", "Output", -12.0f, 12.0f, 0.0f),
};

const Descriptor kDesc = {"org.sonora.passiveeq", "Sonora Passive EQ", "EQ", 1, kParams, kParamCount, 1, 2, false};

constexpr std::uint32_t kControl = 16;  // samples between retunes
constexpr int kBands = 4;

// The transformer processor's large core, with the same Jiles-Atherton
// shape ratios (see transformer.cpp for the derivation).
dsp::CoreParams large_core(double r_parallel) {
    constexpr double corner_hz = 5.0, flux_vs = 1.2;
    dsp::CoreParams p;
    const double chi = 4000.0 / 3.0;
    const double chi_low = p.c * chi / (1.0 - p.alpha * chi);
    const double l_low = r_parallel / (dsp::kTwoPi * corner_hz);
    p.l_core = l_low / (1.0 + chi_low);
    p.m_sat = flux_vs / p.l_core;
    p.a = p.m_sat / 4000.0;
    p.k = 7.5e-5 * p.m_sat;
    return p;
}

class PassiveEq final : public ProcessorBase {
public:
    PassiveEq() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        iron_ = value(kIron) >= 0.5f;
        st_.assign(channels(), {});
        work_.assign(static_cast<std::size_t>(channels()) * max_frames(), 0.0);
        if (iron_) {
            dsp::TransformerCircuit ckt;
            ckt.volts_per_unit = 100.0;
            ckt.r_source = 600.0;
            ckt.r_load = 10000.0;
            ckt.v_rail_idle = 1e6;
            ckt.r_supply = 0.0;
            const double rp = ckt.r_source * ckt.r_load / (ckt.r_source + ckt.r_load);
            if (!stage_.prepare(sample_rate(), channels(), ckt, large_core(rp), 1)) return false;
            for (auto* v : {&drive_, &inv_drive_, &post_}) v->assign(max_frames(), 0.0);
            for (std::uint32_t ch = 0; ch < 2; ++ch) {
                x_[ch].assign(max_frames(), 0.0);
                y_[ch].assign(max_frames(), 0.0);
            }
        }
        dirty_ = true;
        retune(0, true);
        set_latency(0);
        return true;
    }

    void on_reset() noexcept override {
        for (auto& ch : st_)
            for (auto& s : ch) s.reset();
        if (iron_) stage_.reset();
    }

    void on_param(std::uint32_t index, bool jump) noexcept override {
        if (index == kLfFreq || index == kHfFreq || index == kHfAttenFreq) dirty_ = true;
        if (jump && !st_.empty()) {
            dirty_ = true;
            retune(0, true);
        }
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        for (std::uint32_t pos = 0; pos < frames; pos += kControl) {
            const std::uint32_t m = frames - pos < kControl ? frames - pos : kControl;
            render_block(in, out, pos, m);
        }
    }

private:
    static int choice(float v, int n) noexcept {
        const long i = std::lround(v);
        return static_cast<int>(i < 0 ? 0 : (i >= n ? n - 1 : i));
    }

    bool params_moving() noexcept {
        for (std::uint32_t p : {kLfBoost, kLfAtten, kHfBoost, kHfQ, kHfAtten})
            if (smoother(p).moving()) return true;
        return false;
    }

    // Advances the knobs by m samples and ramps each section to its new design.
    void retune(std::uint32_t m, bool jump) noexcept {
        const bool moving = params_moving();
        auto get = [&](std::uint32_t p) { return jump ? smoother(p).current() : smoother(p).advance(m); };
        const double lb = get(kLfBoost), la = get(kLfAtten), hb = get(kHfBoost), hq = get(kHfQ), ha = get(kHfAtten);
        if (!(dirty_ || moving)) return;
        dirty_ = false;
        const double lf = kLfFreqs[choice(value(kLfFreq), 4)];
        const double hf = kHfFreqs[choice(value(kHfFreq), 7)];
        const double haf = kHfAttenFreqs[choice(value(kHfAttenFreq), 3)];
        const dsp::FilterSpec specs[kBands] = {
            {dsp::Shape::LowShelf, lf, kLfBoostQ, lb},
            {dsp::Shape::LowShelf, lf * kCutRatio, kLfCutQ, -la * kCutScale},
            {dsp::Shape::Peak, hf, hq, hb},
            {dsp::Shape::HighShelf, haf, kHfCutQ, -ha},
        };
        const std::uint32_t ramp = jump ? 0 : m;
        for (int b = 0; b < kBands; ++b) {
            // A corner at or above Nyquist cannot be designed at this rate;
            // it is moved just under it.
            dsp::FilterSpec s = specs[b];
            s.freq_hz = std::fmin(s.freq_hz, 0.45 * sample_rate());
            const auto c = dsp::to_svf(dsp::design_matched(s, sample_rate()));
            if (ramp == 0) ramp_[b].set(c);
            else ramp_[b].ramp_to(c, ramp);
        }
    }

    void render_block(const float* const* in, float* const* out, std::uint32_t pos, std::uint32_t m) noexcept {
        retune(m, false);
        const std::uint32_t chs = channels();
        const dsp::SvfState::Derived* d[kBands];
        for (std::uint32_t c = 0; c < chs; ++c) {
            double* w = &work_[static_cast<std::size_t>(c) * max_frames()];
            for (std::uint32_t i = 0; i < m; ++i) w[i] = in[c][pos + i];
        }
        // The ramps are shared by the channels, so they advance once a sample.
        for (std::uint32_t i = 0; i < m; ++i) {
            for (int b = 0; b < kBands; ++b) d[b] = &ramp_[b].next();
            for (std::uint32_t c = 0; c < chs; ++c) {
                double& v = work_[static_cast<std::size_t>(c) * max_frames() + i];
                for (int b = 0; b < kBands; ++b) v = st_[c][b].tick(*d[b], v);
            }
        }
        ParamSmoother& og = smoother(kOutput);
        ParamSmoother& dg = smoother(kDrive);
        if (iron_) {
            for (std::uint32_t k = 0; k < m; ++k) {
                drive_[k] = dsp::db_to_gain(dg.next());
                post_[k] = dsp::db_to_gain(og.next());
                inv_drive_[k] = post_[k] / drive_[k];
            }
            const double* ip[2] = {x_[0].data(), x_[1].data()};
            double* op[2] = {y_[0].data(), y_[1].data()};
            for (std::uint32_t c = 0; c < chs; ++c)
                for (std::uint32_t k = 0; k < m; ++k)
                    x_[c][k] = work_[static_cast<std::size_t>(c) * max_frames() + k] * drive_[k];
            stage_.process_block(ip, op, m, nullptr, 1);
            for (std::uint32_t c = 0; c < chs; ++c)
                for (std::uint32_t k = 0; k < m; ++k) out[c][pos + k] = static_cast<float>(y_[c][k] * inv_drive_[k]);
        } else {
            for (std::uint32_t k = 0; k < m; ++k) {
                const double g = dsp::db_to_gain(og.next());
                for (std::uint32_t c = 0; c < chs; ++c)
                    out[c][pos + k] = static_cast<float>(work_[static_cast<std::size_t>(c) * max_frames() + k] * g);
            }
            dg.advance(m);
        }
    }

    bool iron_ = false;
    bool dirty_ = true;
    dsp::SvfRamp ramp_[kBands];
    std::vector<std::array<dsp::SvfState, kBands>> st_;
    std::vector<double> work_;
    dsp::OutputStage stage_;
    std::vector<double> x_[2], y_[2], drive_, inv_drive_, post_;
};

}  // namespace

std::unique_ptr<Processor> make_passiveeq() { return std::make_unique<PassiveEq>(); }

}  // namespace sonora::native
