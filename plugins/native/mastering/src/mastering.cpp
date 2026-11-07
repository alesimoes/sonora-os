// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/mastering.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "sonora/dsp/loudness.h"
#include "sonora/dsp/math.h"
#include "sonora/native/compressor.h"
#include "sonora/native/eq.h"
#include "sonora/native/limiter.h"
#include "sonora/native/processor_base.h"
#include "sonora/native/saturator.h"

namespace sonora::native {

namespace {

using namespace mastering;

const char* const kOffOn[] = {"off", "on"};

constexpr ParamInfo db(const char* id, const char* name, float lo, float hi, float def, const char* fmt) {
    return {id, name, Unit::Decibel, Taper::Linear, lo, hi, def, 20.0f, true, false, false, fmt, nullptr, 0};
}
constexpr ParamInfo hz(const char* id, const char* name, float lo, float hi, float def) {
    return {id, name, Unit::Hertz, Taper::Log, lo, hi, def, 20.0f, true, false, false, "%.0f Hz", nullptr, 0};
}
constexpr ParamInfo pct(const char* id, const char* name, float lo, float hi, float def) {
    return {id, name, Unit::Percent, Taper::Linear, lo, hi, def, 20.0f, true, false, false, "%.0f %%", nullptr, 0};
}
constexpr ParamInfo ms(const char* id, const char* name, float lo, float hi, float def) {
    return {id, name, Unit::Milliseconds, Taper::Log, lo, hi, def, 20.0f, true, false, false, "%.0f ms", nullptr, 0};
}

// The defaults are the neutral console: every stage set so that it changes
// nothing (ratio 1, saturation mix 0, width 100 %), except the limiter at
// -1 dBTP, which only acts on a peak. Attack and release are the slow end
// of a mastering compressor.
const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    db("low_gain", "Low shelf", -9.0f, 9.0f, 0.0f, "%+.1f dB"),
    hz("lowmid_freq", "Low-mid frequency", 80.0f, 1000.0f, 300.0f),
    db("lowmid_gain", "Low-mid", -9.0f, 9.0f, 0.0f, "%+.1f dB"),
    hz("highmid_freq", "High-mid frequency", 1000.0f, 8000.0f, 3000.0f),
    db("highmid_gain", "High-mid", -9.0f, 9.0f, 0.0f, "%+.1f dB"),
    db("high_gain", "High shelf", -9.0f, 9.0f, 0.0f, "%+.1f dB"),
    db("comp_threshold", "Threshold", -40.0f, 0.0f, -18.0f, "%.1f dB"),
    {"comp_ratio", "Ratio", Unit::Ratio, Taper::Log, 1.0f, 6.0f, 1.0f, 20.0f, true, false, false, "%.2f:1", nullptr, 0},
    ms("comp_attack", "Attack", 5.0f, 100.0f, 30.0f),
    ms("comp_release", "Release", 50.0f, 1000.0f, 200.0f),
    pct("comp_mix", "Compressor mix", 0.0f, 100.0f, 100.0f),
    db("sat_drive", "Drive", 0.0f, 24.0f, 6.0f, "%.1f dB"),
    pct("sat_mix", "Saturation", 0.0f, 100.0f, 0.0f),
    pct("width", "Width", 0.0f, 200.0f, 100.0f),
    db("gain", "Gain", -12.0f, 12.0f, 0.0f, "%+.1f dB"),
    db("ceiling", "Ceiling", -6.0f, 0.0f, -1.0f, "%.1f dBTP"),
    ms("limit_release", "Limiter release", 10.0f, 1000.0f, 100.0f),
    db("target_lufs", "Target", -24.0f, -6.0f, -14.0f, "%.1f LUFS"),
    {"auto_level", "Auto-level", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 20.0f, true, false, false, "%.0f", kOffOn, 2},
};

const Descriptor kDesc = {"org.sonora.mastering", "Sonora Mastering Console", "Dynamics", 1,
                          kParams, kParamCount, 1, 2, false};

enum Stage : std::uint8_t { kEq, kComp, kSat, kLim, kStages };

struct Route {
    std::uint32_t console;
    Stage stage;
    std::uint32_t sub;
};

// Which parameter of which stage each console parameter moves. The stage
// smooths it (20 ms), so a change here is a step only to the stage's own
// smoother.
const Route kRoutes[] = {
    {kLowGain, kEq, eq::kLowShelfGain},
    {kLowMidFreq, kEq, static_cast<std::uint32_t>(eq::bell(0, 1))},
    {kLowMidGain, kEq, static_cast<std::uint32_t>(eq::bell(0, 2))},
    {kHighMidFreq, kEq, static_cast<std::uint32_t>(eq::bell(1, 1))},
    {kHighMidGain, kEq, static_cast<std::uint32_t>(eq::bell(1, 2))},
    {kHighGain, kEq, eq::kHighShelfGain},
    {kCompThreshold, kComp, compressor::kThreshold},
    {kCompRatio, kComp, compressor::kRatio},
    {kCompAttack, kComp, compressor::kAttack},
    {kCompRelease, kComp, compressor::kRelease},
    {kCompMix, kComp, compressor::kMix},
    {kSatDrive, kSat, saturator::kDrive},
    {kSatMix, kSat, saturator::kMix},
    {kCeiling, kLim, limiter::kCeiling},
    {kLimitRelease, kLim, limiter::kRelease},
};
constexpr std::size_t kRouteCount = sizeof kRoutes / sizeof kRoutes[0];

// The auto-level loop runs on the meter's 10 ms steps, every 100 ms, and
// not before the short-term window (3 s) is full.
constexpr std::uint64_t kLoopSteps = 10;
constexpr std::uint64_t kWarmSteps = 300;
// The trim follows its target with a one-pole of this time constant, so a
// step of the target is never a step of the audio.
constexpr double kTrimSmoothSeconds = 0.5;

class Mastering final : public ProcessorBase {
public:
    Mastering() : ProcessorBase(kDesc) {
        stage_[kEq] = make_eq();
        stage_[kComp] = make_compressor();
        stage_[kSat] = make_saturator();
        stage_[kLim] = make_limiter();
        // The stages that publish levels offered themselves to the host's
        // telemetry hand-over; inside this chain they are not the insert, so
        // they keep to their private blocks.
        for (auto& s : stage_) {
            if (auto* b = dynamic_cast<ProcessorBase*>(s.get())) b->telemetry().withhold();
        }
        // Fixed settings of the stages. The reconfiguring ones (the EQ's
        // mode, the saturator's quality) are read at their prepare().
        stage_[kEq]->set_param(eq::kMode, 0.0f);
        for (int b = 0; b < 2; ++b) {
            stage_[kEq]->set_param(static_cast<std::uint32_t>(eq::bell(b, 0)), 1.0f);
            stage_[kEq]->set_param(static_cast<std::uint32_t>(eq::bell(b, 3)), static_cast<float>(kBellQ));
        }
        stage_[kEq]->set_param(eq::kLowShelfOn, 1.0f);
        stage_[kEq]->set_param(eq::kLowShelfFreq, static_cast<float>(kLowShelfHz));
        stage_[kEq]->set_param(eq::kHighShelfOn, 1.0f);
        stage_[kEq]->set_param(eq::kHighShelfFreq, static_cast<float>(kHighShelfHz));
        stage_[kComp]->set_param(compressor::kKnee, 12.0f);
        stage_[kSat]->set_param(saturator::kQuality, 0.0f);
        stage_[kSat]->set_param(saturator::kShape, 0.0f);
        stage_[kSat]->set_param(saturator::kMode, 0.0f);
        stage_[kLim]->set_param(limiter::kInput, 0.0f);
    }

protected:
    bool on_prepare() override {
        // Current values first, so a stage prepares with what the console
        // holds, then the stages.
        for (const Route& r : kRoutes) stage_[r.stage]->set_param(r.sub, value(r.console));
        std::uint32_t lat = 0;
        for (auto& s : stage_) {
            if (!s->prepare(sample_rate(), max_frames(), channels())) return false;
            lat += s->latency();
        }
        set_latency(lat);
        if (!meter_.prepare(sample_rate(), channels(), nullptr, false)) return false;
        width_.assign(max_frames(), 1.0);
        gain_.assign(max_frames(), 1.0);
        trim_coef_ = 1.0 - std::exp(-1.0 / (kTrimSmoothSeconds * sample_rate()));
        trim_db_ = trim_target_db_ = 0.0;
        last_gain_db_ = 1e9;
        pending_.fill(0);
        return true;
    }

    void on_reset() noexcept override {
        for (auto& s : stage_) s->reset();
        meter_.reset();
        trim_db_ = trim_target_db_ = 0.0;
        last_steps_ = 0;
        last_gain_db_ = 1e9;
    }

    void on_param(std::uint32_t index, bool jump) noexcept override {
        for (const Route& r : kRoutes) {
            if (r.console != index) continue;
            if (jump) {
                stage_[r.stage]->set_param(r.sub, value(index));
                pending_[index] = 0;
            } else {
                pend_value_[index] = value(index);
                pending_[index] = 1;
            }
        }
        if (index == kAutoLevel && value(kAutoLevel) < 0.5f) trim_target_db_ = 0.0;
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        const float* src[2] = {in[0], chs > 1 ? in[1] : nullptr};
        run(kEq, src, out, frames);
        const float* io[2] = {out[0], chs > 1 ? out[1] : nullptr};
        run(kComp, io, out, frames);
        run(kSat, io, out, frames);
        width_and_gain(out, frames);
        run(kLim, io, out, frames);
        meter_.process(io, frames);
        if (value(kAutoLevel) >= 0.5f) update_trim();
    }

private:
    void run(Stage s, const float* const* in, float* const* out, std::uint32_t frames) noexcept SONORA_RT {
        ParamEvent ev[8];
        std::uint32_t n = 0;
        for (std::size_t i = 0; i < kRouteCount; ++i) {
            const Route& r = kRoutes[i];
            if (r.stage != s || !pending_[r.console] || n >= 8) continue;
            ev[n++] = {0, r.sub, pend_value_[r.console]};
        }
        // Cleared per stage: every route of this stage has been sent.
        for (const Route& r : kRoutes) {
            if (r.stage == s) pending_[r.console] = 0;
        }
        AudioBuffers b{in, out, nullptr, channels(), 0, frames};
        stage_[s]->process(b, ev, n, transport());
    }

    // Width (mid/side) and the gain ahead of the limiter, one pass. Both
    // are held at one value per block when nothing moves.
    void width_and_gain(float* const* out, std::uint32_t frames) noexcept SONORA_RT {
        ParamSmoother& ws = smoother(kWidth);
        ParamSmoother& gs = smoother(kGain);
        const bool stereo = channels() > 1;
        const bool trim_moves = std::fabs(trim_target_db_ - trim_db_) > 1e-7;
        for (std::uint32_t k = 0; k < frames; ++k) {
            width_[k] = ws.next() * 0.01;
            if (trim_moves) trim_db_ += (trim_target_db_ - trim_db_) * trim_coef_;
            const double gdb = gs.next() + trim_db_;
            if (gdb != last_gain_db_) {
                last_gain_db_ = gdb;
                last_gain_ = dsp::db_to_gain(gdb);
            }
            gain_[k] = last_gain_;
        }
        if (!trim_moves) trim_db_ = trim_target_db_;
        for (std::uint32_t k = 0; k < frames; ++k) {
            double l = out[0][k];
            if (!stereo) {
                out[0][k] = static_cast<float>(l * gain_[k]);
                continue;
            }
            double r = out[1][k];
            if (width_[k] != 1.0) {
                const double mid = 0.5 * (l + r);
                const double side = 0.5 * (l - r) * width_[k];
                l = mid + side;
                r = mid - side;
            }
            out[0][k] = static_cast<float>(l * gain_[k]);
            out[1][k] = static_cast<float>(r * gain_[k]);
        }
    }

    // Every 100 ms, once the 3 s window is full: a slow integrator on the
    // short-term loudness error, so the trim never chases a transient.
    void update_trim() noexcept SONORA_RT {
        const std::uint64_t steps = meter_.steps();
        if (steps < kWarmSteps || steps - last_steps_ < kLoopSteps) return;
        last_steps_ = steps;
        const double st = meter_.short_term_lufs();
        if (st < -70.0) return;  // silence: nothing to steer by
        const double err = static_cast<double>(value(kTargetLufs)) - st;
        const double step = std::fmax(-kTrimStepDb, std::fmin(kTrimStepDb, kTrimGain * err));
        trim_target_db_ = std::fmax(-kTrimRangeDb, std::fmin(kTrimRangeDb, trim_target_db_ + step));
    }

    std::unique_ptr<Processor> stage_[kStages];
    dsp::LoudnessMeter meter_;
    std::array<float, kParamCount> pend_value_{};
    std::array<std::uint8_t, kParamCount> pending_{};
    std::vector<double> width_, gain_;
    double trim_db_ = 0.0, trim_target_db_ = 0.0, trim_coef_ = 0.0;
    double last_gain_db_ = 1e9, last_gain_ = 1.0;
    std::uint64_t last_steps_ = 0;
};

}  // namespace

std::unique_ptr<Processor> make_mastering() { return std::make_unique<Mastering>(); }
const Descriptor& mastering_descriptor() { return kDesc; }

}  // namespace sonora::native
