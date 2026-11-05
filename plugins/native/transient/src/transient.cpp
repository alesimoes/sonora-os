// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/transient.h"

#include <cmath>

#include "sonora/dsp/dynamics.h"
#include "sonora/dsp/math.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace transient;

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"attack", "Attack", Unit::Decibel, Taper::Linear, -15.0f, 15.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
    {"sustain", "Sustain", Unit::Decibel, Taper::Linear, -15.0f, 15.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
    {"speed", "Speed", Unit::Ratio, Taper::Log, 0.25f, 4.0f, 1.0f, 20.0f, true, false, false, "%.2fx", nullptr, 0},
    {"link", "Stereo link", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"gain", "Output gain", Unit::Decibel, Taper::Linear, -12.0f, 12.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
};

const Descriptor kDesc = {"org.sonora.transient", "Sonora Transient", "Dynamics", 1, kParams, kParamCount, 1, 2, false};

constexpr std::uint32_t kControl = 16;
constexpr std::uint32_t kMaxCh = 2;
// Time constants at speed 1 x, seconds. The fast follower finds the onset
// within about a millisecond; its release is long enough that a 100 Hz
// tone's half-wave ripple stays inside the dead zone. The slow follower
// sets how long "attack" lasts (tens of ms) and how fast a decay must be
// to count as sustain.
constexpr double kFastAttack = 0.0005, kFastRelease = 0.020;
constexpr double kSlowAttack = 0.030, kSlowRelease = 0.120;
constexpr double kGainSmooth = 0.002;
constexpr double kDeadZoneDb = 2.0;   // |ratio| below which nothing is applied
constexpr double kFullDb = 6.0;       // ... and the width over which the weight reaches 1
constexpr double kFloorDb = -66.0;    // below this slow level, weights fade out
constexpr double kFloorFadeDb = 6.0;  // so the noise floor of a silent take is not pumped

class Transient final : public ProcessorBase {
public:
    Transient() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        gs_ = dsp::time_constant_coeff(kGainSmooth, sample_rate());
        update_times();
        return true;
    }
    void on_reset() noexcept override {
        for (std::uint32_t c = 0; c < kMaxCh; ++c) {
            fast_[c].reset();
            slow_[c].reset();
            g_[c] = 0.0;
        }
    }
    void on_param(std::uint32_t index, bool jump) noexcept override {
        if (jump && index == kSpeed) update_times();
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        for (std::uint32_t pos = 0; pos < frames; pos += kControl) {
            const std::uint32_t m = frames - pos < kControl ? frames - pos : kControl;
            if (smoother(kSpeed).moving()) {
                smoother(kSpeed).advance(m);
                update_times();
            }
            for (std::uint32_t i = 0; i < m; ++i) {
                const std::uint32_t k = pos + i;
                const double a_db = smoother(kAttack).next();
                const double s_db = smoother(kSustain).next();
                const double link = smoother(kLink).next() * 0.01;
                const double out_g = dsp::db_to_gain(smoother(kGain).next());
                double peak = 0.0;
                for (std::uint32_t c = 0; c < chs; ++c) peak = std::fmax(peak, std::fabs(static_cast<double>(in[c][k])));
                for (std::uint32_t c = 0; c < chs; ++c) {
                    const double x = in[c][k];
                    const double det = link * peak + (1.0 - link) * std::fabs(x);
                    const double f = fast_[c].process(det);
                    const double sl = slow_[c].process(det);
                    const double f_db = dsp::gain_to_db(f, -200.0);
                    const double s_lvl = dsp::gain_to_db(sl, -200.0);
                    const double diff = f_db - s_lvl;
                    double wa = (diff - kDeadZoneDb) / kFullDb;
                    double ws = (-diff - kDeadZoneDb) / kFullDb;
                    wa = wa < 0.0 ? 0.0 : (wa > 1.0 ? 1.0 : wa);
                    ws = ws < 0.0 ? 0.0 : (ws > 1.0 ? 1.0 : ws);
                    // A silent take has no transients: fade the shaping out
                    // under the floor rather than lift the noise.
                    double fl = (f_db - kFloorDb) / kFloorFadeDb;
                    fl = fl < 0.0 ? 0.0 : (fl > 1.0 ? 1.0 : fl);
                    const double target = fl * (a_db * wa + s_db * ws);
                    g_[c] = gs_ * g_[c] + (1.0 - gs_) * target;
                    out[c][k] = static_cast<float>(x * dsp::db_to_gain(g_[c]) * out_g);
                }
            }
        }
    }

private:
    void update_times() noexcept {
        const double sp = smoother(kSpeed).current();
        for (auto& f : fast_) f.set_times(kFastAttack / sp, kFastRelease / sp, sample_rate());
        for (auto& s : slow_) s.set_times(kSlowAttack / sp, kSlowRelease / sp, sample_rate());
    }

    dsp::BranchingSmoother fast_[kMaxCh], slow_[kMaxCh];
    double g_[kMaxCh] = {0.0, 0.0};
    double gs_ = 0.0;
};

}  // namespace

std::unique_ptr<Processor> make_transient() { return std::make_unique<Transient>(); }

}  // namespace sonora::native
