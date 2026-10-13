// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/compressor.h"

#include <cmath>
#include <vector>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/dynamics.h"
#include "sonora/dsp/math.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace compressor;

const char* const kOffOn[] = {"off", "on"};
const char* const kDetectors[] = {"peak", "RMS"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"threshold", "Threshold", Unit::Decibel, Taper::Linear, -60.0f, 0.0f, -20.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"ratio", "Ratio", Unit::Ratio, Taper::Log, 1.0f, 20.0f, 4.0f, 20.0f, true, false, false, "%.1f:1", nullptr, 0},
    {"knee", "Knee", Unit::Decibel, Taper::Linear, 0.0f, 24.0f, 6.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"attack", "Attack", Unit::Milliseconds, Taper::Log, 0.05f, 300.0f, 10.0f, 20.0f, true, false, false, "%.2f ms", nullptr, 0},
    {"release", "Release", Unit::Milliseconds, Taper::Log, 5.0f, 3000.0f, 150.0f, 20.0f, true, false, false, "%.0f ms", nullptr, 0},
    {"makeup", "Make-up", Unit::Decibel, Taper::Linear, -12.0f, 24.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
    {"auto_makeup", "Auto make-up", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 20.0f, true, false, false, "%.0f", kOffOn, 2},
    {"detector", "Detector", Unit::Choice, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kDetectors, 2},
    {"sc_hpf_on", "Side-chain high-pass", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 10.0f, true, false, false, "%.0f", kOffOn, 2},
    {"sc_hpf_freq", "Side-chain high-pass frequency", Unit::Hertz, Taper::Log, 20.0f, 500.0f, 80.0f, 20.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"link", "Stereo link", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"external_sc", "External key", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kOffOn, 2},
};

const Descriptor kDesc = {"org.sonora.compressor", "Sonora Compressor", "Dynamics", 1, kParams, kParamCount, 1, 2, true};

constexpr std::uint32_t kControl = 16;
constexpr double kRmsSeconds = 0.010;
constexpr std::uint32_t kMaxCh = 2;

class Compressor final : public ProcessorBase {
public:
    Compressor() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        for (auto& m : ms_) m.set_time(kRmsSeconds, sample_rate());
        update_filter(0, true);
        update_times();
        return true;
    }

    void on_reset() noexcept override {
        for (std::uint32_t c = 0; c < kMaxCh; ++c) {
            hpf_[c].reset();
            ms_[c].reset();
            det_[c].reset(0.0);
        }
        gr_db_ = 0.0;
    }

    void on_param(std::uint32_t index, bool jump) noexcept override {
        if (index == kScHpfFreq) update_filter(0, jump);
        if (jump && (index == kAttack || index == kRelease)) update_times();
    }

    void render(const float* const* in, float* const* out, const float* const* sc,
                std::uint32_t, std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        const float* const* key = value(kExternalSc) >= 0.5f ? sc : in;
        const bool rms = value(kDetector) >= 0.5f;
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
                const double ratio = smoother(kRatio).next();
                const double knee = smoother(kKnee).next();
                const double makeup = smoother(kMakeup).next();
                const double autow = smoother(kAutoMakeup).next();
                const double hpw = smoother(kScHpfOn).next();
                const double link = smoother(kLink).next() * 0.01;
                const double mix = smoother(kMix).next() * 0.01;
                const dsp::SvfState::Derived& d = hpf_ramp_.next();

                // Level of each channel's key, in dB.
                double lvl[kMaxCh];
                double lmax = -400.0;
                for (std::uint32_t c = 0; c < chs; ++c) {
                    double s = key[c][k];
                    const double h = hpf_[c].tick(d, s);
                    s += hpw * (h - s);
                    lvl[c] = rms ? 10.0 * std::log10(ms_[c].process(s) + 1e-30)
                                 : dsp::gain_to_db(std::fabs(s), -400.0);
                    if (lvl[c] > lmax) lmax = lvl[c];
                }
                // Giannoulis et al.: make-up for a 0 dBFS input, halved,
                // so automatic make-up does not overshoot on loud material.
                const double auto_db = -0.5 * dsp::compressor_curve_db(0.0, thr, ratio, knee) * autow;
                for (std::uint32_t c = 0; c < chs; ++c) {
                    const double x = link * lmax + (1.0 - link) * lvl[c];
                    const double cdb = x - dsp::compressor_curve_db(x, thr, ratio, knee);
                    const double red = det_[c].process(cdb);
                    const double g = dsp::db_to_gain(makeup + auto_db - red);
                    const double dry = in[c][k];
                    const double wet = dry * g;
                    out[c][k] = static_cast<float>(dry + mix * (wet - dry));
                    if (c == 0) gr_db_ = red;
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
        for (auto& det : det_) det.set_times(a, r, sample_rate());
    }

    dsp::SvfRamp hpf_ramp_;
    dsp::SvfState hpf_[kMaxCh];
    dsp::MeanSquare ms_[kMaxCh];
    dsp::BranchingSmoother det_[kMaxCh];
    double gr_db_ = 0.0;
};

}  // namespace

std::unique_ptr<Processor> make_compressor() { return std::make_unique<Compressor>(); }

}  // namespace sonora::native
