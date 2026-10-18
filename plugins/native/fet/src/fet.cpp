// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/fet.h"

#include <cmath>
#include <vector>

#include "sonora/dsp/delay_line.h"
#include "sonora/dsp/fet.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/oversampling.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace fet;

const char* const kOffOn[] = {"off", "on"};
const char* const kRatios[] = {"4:1", "8:1", "12:1", "20:1", "all buttons"};
const char* const kQualities[] = {"eco (no latency)", "standard (4x)", "high (8x)"};

// The attack and release controls are the networks' time constants with
// C22 (0.22 uF): R54 470 plus R55's 0-25k, and R57 270k plus R56's 0-5M.
// The ranges follow from the circuit (FetCircuit); the suite checks they
// match it.
constexpr float kAttackMinMs = 0.1034f, kAttackMaxMs = 5.6034f;
constexpr float kReleaseMinMs = 59.4f, kReleaseMaxMs = 1159.4f;

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"input", "Input", Unit::Decibel, Taper::Linear, -24.0f, 24.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
    {"output", "Output", Unit::Decibel, Taper::Linear, -24.0f, 24.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
    {"attack", "Attack", Unit::Milliseconds, Taper::Log, kAttackMinMs, kAttackMaxMs, 0.5f, 20.0f, true, false, false, "%.2f ms", nullptr, 0},
    {"release", "Release", Unit::Milliseconds, Taper::Log, kReleaseMinMs, kReleaseMaxMs, 250.0f, 20.0f, true, false, false, "%.0f ms", nullptr, 0},
    // Smoothed rather than declicked: the tap and the bias move over 20 ms.
    {"ratio", "Ratio", Unit::Choice, Taper::Stepped, 0.0f, 4.0f, 0.0f, 20.0f, true, false, false, "%.0f", kRatios, 5},
    {"link", "Stereo link", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 1.0f, 20.0f, true, false, false, "%.0f", kOffOn, 2},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"external_sc", "External key", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kOffOn, 2},
    {"quality", "Quality", Unit::Choice, Taper::Stepped, 0.0f, 2.0f, 1.0f, 0.0f, false, true, false, "%.0f", kQualities, 3},
};

const Descriptor kDesc = {"org.sonora.fet", "Sonora FET", "Dynamics", 1, kParams, kParamCount, 1, 2, true};

// The ratio, attack and release reach the circuit at this rate (base-rate
// samples): each is a smoothed resistance or tap, and the rest point is
// solved again with them; C22's time constants are 0.1 ms and longer.
constexpr std::uint32_t kControl = 16;
constexpr std::uint32_t kMaxCh = 2;
// The stereo adapter joins the two C22 nodes. Joined at 1 ohm, through
// link^4: the 20 ms ramp of the switch then spreads the joining (the
// equalising time C22 / 2g is 0.11 ms at 1 kohm, link 0.18, and 1.1 ms at
// link 0.1), where a linear law would join them in the ramp's first
// millisecond.
constexpr double kLinkSiemens = 1.0;

class Fet final : public ProcessorBase {
public:
    Fet() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        long q = std::lround(value(kQuality));
        q = q < 0 ? 0 : (q > 2 ? 2 : q);
        factor_ = kFactors[q];
        const std::uint32_t chs = channels();
        latency_ = 0;
        if (factor_ > 1) {
            // The input's channels, then the key's: both at the high rate,
            // so a keyed side chain sees what an unkeyed one would. The
            // protected band is 20 kHz at every rate, as in the tube.
            const double passband = std::fmin(0.4535, 20000.0 / sample_rate());
            if (!os_.prepare(factor_, 2 * chs, max_frames(), passband)) return false;
            latency_ = os_.latency();
        }
        set_latency(latency_);
        for (auto& ch : ch_) ch.prepare(sample_rate() * factor_);
        for (std::uint32_t c = 0; c < chs; ++c) {
            dry_[c].prepare(latency_ + 1);
            // The base-rate copy eco processes in place; the oversampler
            // holds the high-rate one.
            buf_[c].assign(factor_ == 1 ? max_frames() : 0, 0.0);
        }
        wet_.assign(max_frames(), 0.0f);
        gin_.assign(max_frames(), 0.0);
        gout_.assign(max_frames(), 0.0);
        link_.assign(max_frames(), 0.0);
        mix_.assign(max_frames(), 0.0);
        update_controls(true);
        for (auto& ch : ch_) ch.reset();
        return true;
    }

    void on_reset() noexcept override {
        os_.reset();
        for (auto& ch : ch_) ch.reset();
        for (auto& d : dry_) d.reset();
    }

    void render(const float* const* in, float* const* out, const float* const* sc, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        const bool keyed = value(kExternalSc) >= 0.5f;
        const dsp::FetCircuit& k = ch_[0].circuit();
        const double fsv = k.full_scale_v;
        // The divider's output normalised so 0 dB in and out is unity at rest.
        const double norm = 1.0 / (ch_[0].rest_gain() * fsv);
        const std::uint32_t f = factor_;
        double* x[kMaxCh] = {nullptr, nullptr};
        const double* key[kMaxCh] = {nullptr, nullptr};
        for (std::uint32_t c = 0; c < chs; ++c) {
            if (f > 1) {
                x[c] = os_.up(c, in[c], frames);
                // Upsampled whether or not it is used, so switching the
                // key on never starts the side chain from a stale history.
                key[c] = os_.up(chs + c, sc[c], frames);
            } else {
                double* b = buf_[c].data();
                for (std::uint32_t n = 0; n < frames; ++n) b[n] = in[c][n];
                x[c] = b;
            }
        }
        // The smoothers advance once per base-rate sample; a settled gain is
        // converted once a block rather than per sample: an exp each, two
        // per base-rate sample, in a mode held to class A.
        const bool in_moving = smoother(kInput).moving(), out_moving = smoother(kOutput).moving();
        const double gin_still = in_moving ? 0.0 : dsp::db_to_gain(smoother(kInput).current()) * fsv;
        const double gout_still = out_moving ? 0.0 : dsp::db_to_gain(smoother(kOutput).current());
        for (std::uint32_t n = 0; n < frames; ++n) {
            gin_[n] = in_moving ? dsp::db_to_gain(smoother(kInput).next()) * fsv : gin_still;
            gout_[n] = out_moving ? dsp::db_to_gain(smoother(kOutput).next()) : gout_still;
            const double l = smoother(kLink).next();
            link_[n] = kLinkSiemens * (l * l) * (l * l);
            mix_[n] = smoother(kMix).next() * 0.01;
        }
        double* const* y = x;
        // Never a silent clamp (ProcessorBase): a block in which the input
        // reached the circuit's limit at R5, or a solve step ran out of
        // iterations or was not finite, is reported as bounded.
        const std::uint64_t hits_before = ch_[0].cap_hits() + ch_[1].cap_hits();
        const double limit = k.input_limit_v;
        double peak = 0.0;
        for (std::uint32_t pos = 0; pos < frames; pos += kControl) {
            const std::uint32_t m = frames - pos < kControl ? frames - pos : kControl;
            // Settled controls leave the circuit as it is: the smoothers
            // advance and the circuit is updated only while one of the three
            // moves, or when one was set by a jump since the last update (a
            // host's change at load arrives settled; without this check the
            // circuit kept its old controls, and the host renders differed).
            const bool moving =
                smoother(kAttack).moving() || smoother(kRelease).moving() || smoother(kRatio).moving();
            if (moving) {
                smoother(kAttack).advance(m);
                smoother(kRelease).advance(m);
                smoother(kRatio).advance(m);
            }
            if (moving || smoother(kAttack).current() != applied_[0] || smoother(kRelease).current() != applied_[1] ||
                smoother(kRatio).current() != applied_[2])
                update_controls(false);
            for (std::uint32_t n = pos; n < pos + m; ++n) {
                const double gi = gin_[n], gl = link_[n];
                for (std::uint32_t j = n * f; j < (n + 1) * f; ++j) {
                    // The side chain follows the input's own divider, or the
                    // key through a second divider on the same gate (fet.h,
                    // External key), whose drain the solve finds with it.
                    double loop[kMaxCh] = {0.0, 0.0};
                    for (std::uint32_t c = 0; c < chs; ++c) {
                        loop[c] = (keyed ? (f > 1 ? key[c][j] : sc[c][n]) : y[c][j]) * gi;
                        peak = std::fmax(peak, std::fabs(loop[c]));
                        if (keyed) peak = std::fmax(peak, std::fabs(y[c][j] * gi));
                    }
                    if (chs == 2) dsp::FetChannel::step_pair(ch_[0], ch_[1], loop[0], loop[1], gl);
                    else ch_[0].step(loop[0]);
                    for (std::uint32_t c = 0; c < chs; ++c)
                        y[c][j] = (keyed ? ch_[c].drain(y[c][j] * gi) : ch_[c].loop_drain()) * norm;
                }
            }
        }
        if (peak > limit || ch_[0].cap_hits() + ch_[1].cap_hits() != hits_before) note_bounded();
        for (std::uint32_t c = 0; c < chs; ++c) {
            if (f > 1) os_.down(c, wet_.data(), frames);
            for (std::uint32_t n = 0; n < frames; ++n) {
                // The dry signal delayed by the oversampler's latency, so
                // the mix never combs.
                // Eco has no latency, and the dry signal is the input.
                double dry = in[c][n];
                if (latency_ > 0) {
                    dry_[c].push(in[c][n]);
                    dry = dry_[c].read(latency_);
                }
                const double w = (f > 1 ? static_cast<double>(wet_[n]) : y[c][n]) * gout_[n];
                out[c][n] = static_cast<float>(dry + mix_[n] * (w - dry));
            }
        }
    }

private:
    void update_controls(bool jump) noexcept {
        auto v = [&](std::uint32_t id) { return jump ? smoother(id).target() : smoother(id).current(); };
        const dsp::FetCircuit& k = ch_[0].circuit();
        applied_[0] = v(kAttack);
        applied_[1] = v(kRelease);
        applied_[2] = v(kRatio);
        const double r55 = applied_[0] * 1e-3 / k.c22 - k.r54;
        const double r56 = applied_[1] * 1e-3 / k.c22 - k.r57;
        for (auto& ch : ch_) ch.set_controls(applied_[2], r55, r56);
    }

    dsp::Oversampler os_;
    dsp::FetChannel ch_[kMaxCh];
    dsp::DelayLine dry_[kMaxCh];
    std::vector<double> buf_[kMaxCh];
    std::vector<float> wet_;
    std::vector<double> gin_, gout_, link_, mix_;
    std::uint32_t factor_ = 4;
    std::uint32_t latency_ = 0;
    // The attack, release and ratio values the circuit was last set to.
    double applied_[3] = {-1.0, -1.0, -1.0};
};

}  // namespace

std::unique_ptr<Processor> make_fet() { return std::make_unique<Fet>(); }

}  // namespace sonora::native
