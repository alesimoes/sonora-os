// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/gate.h"

#include <cmath>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/dynamics.h"
#include "sonora/dsp/math.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace gate;

const char* const kOffOn[] = {"off", "on"};
const char* const kModes[] = {"gate", "expander"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"mode", "Mode", Unit::Choice, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kModes, 2},
    {"threshold", "Threshold", Unit::Decibel, Taper::Linear, -80.0f, 0.0f, -40.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"range", "Range", Unit::Decibel, Taper::Linear, -80.0f, 0.0f, -80.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"ratio", "Ratio", Unit::Ratio, Taper::Log, 1.0f, 10.0f, 2.0f, 20.0f, true, false, false, "1:%.1f", nullptr, 0},
    {"knee", "Knee", Unit::Decibel, Taper::Linear, 0.0f, 24.0f, 6.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"attack", "Attack", Unit::Milliseconds, Taper::Log, 0.01f, 100.0f, 0.5f, 20.0f, true, false, false, "%.2f ms", nullptr, 0},
    {"hold", "Hold", Unit::Milliseconds, Taper::Linear, 0.0f, 2000.0f, 20.0f, 20.0f, true, false, false, "%.0f ms", nullptr, 0},
    {"release", "Release", Unit::Milliseconds, Taper::Log, 1.0f, 5000.0f, 100.0f, 20.0f, true, false, false, "%.0f ms", nullptr, 0},
    {"hysteresis", "Hysteresis", Unit::Decibel, Taper::Linear, 0.0f, 20.0f, 4.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"key_hpf_on", "Key high-pass", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 10.0f, true, false, false, "%.0f", kOffOn, 2},
    {"key_hpf_freq", "Key high-pass frequency", Unit::Hertz, Taper::Log, 20.0f, 2000.0f, 80.0f, 20.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"key_lpf_on", "Key low-pass", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 10.0f, true, false, false, "%.0f", kOffOn, 2},
    {"key_lpf_freq", "Key low-pass frequency", Unit::Hertz, Taper::Log, 500.0f, 20000.0f, 12000.0f, 20.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"link", "Stereo link", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"external_sc", "External key", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kOffOn, 2},
    {"listen", "Key listen", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kOffOn, 2},
};

const Descriptor kDesc = {"org.sonora.gate", "Sonora Gate", "Dynamics", 1, kParams, kParamCount, 1, 2, true};

constexpr std::uint32_t kControl = 16;
constexpr std::uint32_t kMaxCh = 2;
constexpr double kFollowerFall = 0.010;

class Gate final : public ProcessorBase {
public:
    Gate() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        for (auto& f : env_) f.set_release(kFollowerFall, sample_rate());
        update_filters(0, true);
        update_times();
        return true;
    }

    void on_reset() noexcept override {
        for (std::uint32_t c = 0; c < kMaxCh; ++c) {
            hpf_[c].reset();
            lpf_[c].reset();
            env_[c].reset();
            // Start closed, as a gate does before the first note.
            det_[c].reset(-value(kRange));
            open_[c] = false;
            hold_left_[c] = 0;
        }
    }

    void on_param(std::uint32_t index, bool jump) noexcept override {
        if (!jump) return;
        if (index == kKeyHpfFreq || index == kKeyLpfFreq) update_filters(0, true);
        if (index == kAttack || index == kRelease) update_times();
    }

    void render(const float* const* in, float* const* out, const float* const* sc,
                std::uint32_t, std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        const float* const* key = value(kExternalSc) >= 0.5f ? sc : in;
        const bool expander = value(kMode) >= 0.5f;
        const bool listen = value(kListen) >= 0.5f;
        for (std::uint32_t pos = 0; pos < frames; pos += kControl) {
            const std::uint32_t m = frames - pos < kControl ? frames - pos : kControl;
            const bool fmov = smoother(kKeyHpfFreq).moving() || smoother(kKeyLpfFreq).moving();
            if (fmov) update_filters(m, false);
            else {
                smoother(kKeyHpfFreq).advance(m);
                smoother(kKeyLpfFreq).advance(m);
            }
            const bool tmov = smoother(kAttack).moving() || smoother(kRelease).moving();
            smoother(kAttack).advance(m);
            smoother(kRelease).advance(m);
            if (tmov) update_times();
            for (std::uint32_t i = 0; i < m; ++i) {
                const std::uint32_t k = pos + i;
                const double thr = smoother(kThreshold).next();
                const double range = smoother(kRange).next();
                const double ratio = smoother(kRatio).next();
                const double knee = smoother(kKnee).next();
                const double hold = smoother(kHold).next() * 1e-3 * sample_rate();
                const double hyst = smoother(kHysteresis).next();
                const double hpw = smoother(kKeyHpfOn).next();
                const double lpw = smoother(kKeyLpfOn).next();
                const double link = smoother(kLink).next() * 0.01;
                const dsp::SvfState::Derived& dh = hpf_ramp_.next();
                const dsp::SvfState::Derived& dl = lpf_ramp_.next();

                double keyv[kMaxCh], lvl[kMaxCh];
                double lmax = -400.0;
                for (std::uint32_t c = 0; c < chs; ++c) {
                    double s = key[c][k];
                    const double h = hpf_[c].tick(dh, s);
                    s += hpw * (h - s);
                    const double l = lpf_[c].tick(dl, s);
                    s += lpw * (l - s);
                    keyv[c] = s;
                    // The expander reads the instantaneous level, as the
                    // Giannoulis design does, so its times measure as set;
                    // the gate's open/close decision reads the envelope.
                    const double e = expander ? std::fabs(s) : env_[c].process(s);
                    lvl[c] = dsp::gain_to_db(e, -400.0);
                    if (lvl[c] > lmax) lmax = lvl[c];
                }
                for (std::uint32_t c = 0; c < chs; ++c) {
                    const double x = link * lmax + (1.0 - link) * lvl[c];
                    double atten;  // dB, >= 0
                    if (expander) {
                        atten = x - dsp::expander_curve_db(x, thr, ratio, knee);
                    } else {
                        if (!open_[c] && x >= thr) {
                            open_[c] = true;
                            hold_left_[c] = hold;
                        } else if (open_[c]) {
                            if (x >= thr - hyst) hold_left_[c] = hold;
                            else if (hold_left_[c] > 0.0) hold_left_[c] -= 1.0;
                            else open_[c] = false;
                        }
                        atten = open_[c] ? 0.0 : -range;
                    }
                    if (atten > -range) atten = -range;
                    const double a = det_[c].process(atten);
                    const double g = dsp::db_to_gain(-a);
                    out[c][k] = static_cast<float>(listen ? keyv[c] : in[c][k] * g);
                }
            }
        }
    }

private:
    void update_filters(std::uint32_t m, bool jump) noexcept {
        const double fh = jump ? smoother(kKeyHpfFreq).current() : smoother(kKeyHpfFreq).advance(m);
        const double fl = jump ? smoother(kKeyLpfFreq).current() : smoother(kKeyLpfFreq).advance(m);
        const double q = 0.7071067811865476;
        const auto ch = dsp::to_svf(dsp::design_matched({dsp::Shape::HighPass, fh, q, 0.0}, sample_rate()));
        const auto cl = dsp::to_svf(dsp::design_matched({dsp::Shape::LowPass, fl, q, 0.0}, sample_rate()));
        if (jump) {
            hpf_ramp_.set(ch);
            lpf_ramp_.set(cl);
        } else {
            hpf_ramp_.ramp_to(ch, m);
            lpf_ramp_.ramp_to(cl, m);
        }
    }
    void update_times() noexcept {
        // Rising attenuation closes the gate: the release time. Falling
        // attenuation opens it: the attack time.
        const double a = smoother(kAttack).current() * 1e-3;
        const double r = smoother(kRelease).current() * 1e-3;
        for (auto& d : det_) d.set_times(r, a, sample_rate());
    }

    dsp::SvfRamp hpf_ramp_, lpf_ramp_;
    dsp::SvfState hpf_[kMaxCh], lpf_[kMaxCh];
    dsp::PeakFollower env_[kMaxCh];
    dsp::BranchingSmoother det_[kMaxCh];
    bool open_[kMaxCh] = {false, false};
    double hold_left_[kMaxCh] = {0.0, 0.0};
};

}  // namespace

std::unique_ptr<Processor> make_gate() { return std::make_unique<Gate>(); }

}  // namespace sonora::native
