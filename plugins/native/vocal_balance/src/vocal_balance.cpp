// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/vocal_balance.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/dynamics.h"
#include "sonora/dsp/math.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace vocal_balance;

const char* const kBandCounts[] = {"3", "4", "5"};
const char* const kListens[] = {"off", "delta", "solo"};

#define VB_FREQ(id, name, def) \
    {id, name, Unit::Hertz, Taper::Log, 100.0f, 16000.0f, def, 20.0f, true, false, false, "%.0f Hz", nullptr, 0}
#define VB_CUT(id, name) \
    {id, name, Unit::Decibel, Taper::Linear, 0.0f, 18.0f, 9.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0}

// Default frequencies are the zones a vocal usually needs: low body, nasal
// middle, presence, harshness, and the top of the sibilant region.
const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"bands", "Bands", Unit::Choice, Taper::Stepped, 0.0f, 2.0f, 1.0f, 0.0f, false, false, true, "%.0f", kBandCounts, 3},
    {"depth", "Depth", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 60.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"sharpness", "Sharpness", Unit::Q, Taper::Log, 1.0f, 16.0f, 4.0f, 20.0f, true, false, false, "%.2f", nullptr, 0},
    {"selectivity", "Selectivity", Unit::Q, Taper::Log, 2.0f, 16.0f, 6.0f, 20.0f, true, false, false, "%.1f", nullptr, 0},
    {"threshold", "Threshold", Unit::Decibel, Taper::Linear, 0.0f, 24.0f, 6.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"attack", "Attack", Unit::Milliseconds, Taper::Log, 0.5f, 50.0f, 8.0f, 20.0f, true, false, false, "%.1f ms", nullptr, 0},
    {"release", "Release", Unit::Milliseconds, Taper::Log, 10.0f, 1000.0f, 80.0f, 20.0f, true, false, false, "%.0f ms", nullptr, 0},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"output", "Output", Unit::Decibel, Taper::Linear, -12.0f, 12.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
    {"listen", "Listen", Unit::Choice, Taper::Stepped, 0.0f, 2.0f, 0.0f, 0.0f, false, false, true, "%.0f", kListens, 3},
    VB_FREQ("freq1", "Band 1 frequency", 280.0f),
    VB_FREQ("freq2", "Band 2 frequency", 1000.0f),
    VB_FREQ("freq3", "Band 3 frequency", 3300.0f),
    VB_FREQ("freq4", "Band 4 frequency", 4600.0f),
    VB_FREQ("freq5", "Band 5 frequency", 7500.0f),
    VB_CUT("cut1", "Band 1 max cut"),
    VB_CUT("cut2", "Band 2 max cut"),
    VB_CUT("cut3", "Band 3 max cut"),
    VB_CUT("cut4", "Band 4 max cut"),
    VB_CUT("cut5", "Band 5 max cut"),
};

const Descriptor kDesc = {"org.sonora.vocal_balance", "Sonora Vocal Balance", "EQ", 1, kParams, kParamCount, 1, 2, false};

// Reductions are computed every kControl samples: the detector's windows
// are milliseconds long, so a finer rate buys nothing and the logs and the
// bell redesigns are the expensive part.
constexpr std::uint32_t kControl = 16;
constexpr std::uint32_t kMaxCh = 2;
// Under -80 dBFS (rms) of local level the ratio of two small numbers means
// nothing; no reduction.
constexpr double kGateMs = 1e-8;
constexpr double kKneeDb = 6.0;

// 10 log10(x), x > 0: the atanh series on the mantissa, as in the
// de-esser (error under 3e-8 of ln m; the libm log10 was a fifth of that
// block's cost, measured there).
inline double ten_log10(double x) noexcept {
    std::uint64_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    int e = static_cast<int>((bits >> 52) & 0x7ff) - 1023;
    bits = (bits & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
    double m;
    std::memcpy(&m, &bits, sizeof m);
    if (m > 1.4142135623730951) {
        m *= 0.5;
        ++e;
    }
    const double z = (m - 1.0) / (m + 1.0);
    const double z2 = z * z;
    const double ln_m = 2.0 * z * (1.0 + z2 * (1.0 / 3.0 + z2 * (0.2 + z2 * (1.0 / 7.0))));
    constexpr double kLn2 = 0.6931471805599453;
    constexpr double kTenOverLn10 = 4.342944819032518;
    return kTenOverLn10 * (ln_m + e * kLn2);
}

struct Band {
    dsp::SvfRamp narrow, wide, bell;
    dsp::SvfState nb[kMaxCh], wb[kMaxCh], bl[kMaxCh];
    dsp::MeanSquare nms[kMaxCh], wms[kMaxCh];
    double msn[kMaxCh] = {0.0, 0.0};
    double msw[kMaxCh] = {0.0, 0.0};
    double red = 0.0;       // smoothed, linked reduction in dB
    double bell_red = 0.0;  // the reduction the bell was last designed for
};

class VocalBalance final : public ProcessorBase {
public:
    VocalBalance() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        update_times();
        for (std::uint32_t i = 0; i < kMaxBands; ++i) {
            design_detectors(i, 0, true);
            design_bell(i, 0, true);
        }
        out_gain_ = dsp::db_to_gain(smoother(kOutput).current());
        mix_ = smoother(kMix).current() * 0.01;
        return true;
    }

    void on_reset() noexcept override {
        for (std::uint32_t i = 0; i < kMaxBands; ++i) {
            Band& b = bands_[i];
            for (std::uint32_t c = 0; c < kMaxCh; ++c) {
                b.nb[c].reset();
                b.wb[c].reset();
                b.bl[c].reset();
                b.nms[c].reset();
                b.wms[c].reset();
                b.msn[c] = b.msw[c] = 0.0;
            }
            b.red = 0.0;
            design_detectors(i, 0, true);
            design_bell(i, 0, true);
        }
    }

    void on_param(std::uint32_t index, bool jump) noexcept override {
        if (!jump) return;
        if (index == kSharpness || index == kSelectivity) {
            for (std::uint32_t i = 0; i < kMaxBands; ++i) {
                design_detectors(i, 0, true);
                design_bell(i, 0, true, true);
            }
        } else if (index >= kFreq1 && index < kFreq1 + kMaxBands) {
            design_detectors(index - kFreq1, 0, true);
            design_bell(index - kFreq1, 0, true, true);
        } else if (index == kAttack || index == kRelease) {
            update_times();
        } else if (index == kOutput) {
            out_gain_ = dsp::db_to_gain(smoother(kOutput).current());
        } else if (index == kMix) {
            mix_ = smoother(kMix).current() * 0.01;
        } else if (index == kBands) {
            // A band that was off holds the state of whenever it last ran;
            // starting it from rest avoids a burst.
            const std::uint32_t n = active_bands();
            for (std::uint32_t i = n; i < kMaxBands; ++i) clear_band(i);
        }
    }

    void render(const float* const* in, float* const* out, const float* const*,
                std::uint32_t, std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        const std::uint32_t nb = active_bands();
        const int listen = static_cast<int>(value(kListen) + 0.5f);
        for (std::uint32_t pos = 0; pos < frames; pos += kControl) {
            const std::uint32_t m = frames - pos < kControl ? frames - pos : kControl;
            bool dirty = smoother(kSharpness).moving() || smoother(kSelectivity).moving();
            smoother(kSharpness).advance(m);
            smoother(kSelectivity).advance(m);
            double cut[kMaxBands];
            bool fmove[kMaxBands];
            for (std::uint32_t i = 0; i < kMaxBands; ++i) {
                fmove[i] = smoother(kFreq1 + i).moving();
                smoother(kFreq1 + i).advance(m);
                cut[i] = smoother(kCut1 + i).advance(m);
            }
            const double depth = smoother(kDepth).advance(m) * 0.01;
            const double thr = smoother(kThreshold).advance(m);
            const bool tmov = smoother(kAttack).moving() || smoother(kRelease).moving();
            smoother(kAttack).advance(m);
            smoother(kRelease).advance(m);
            if (tmov) update_times();
            double mix_end = mix_;
            if (smoother(kMix).moving()) mix_end = smoother(kMix).advance(m) * 0.01;
            else smoother(kMix).advance(m);
            double out_end = out_gain_;
            if (smoother(kOutput).moving()) out_end = dsp::db_to_gain(smoother(kOutput).advance(m));
            else smoother(kOutput).advance(m);
            const double inv = 1.0 / static_cast<double>(m);
            const double mix_step = (mix_end - mix_) * inv;
            const double out_step = (out_end - out_gain_) * inv;

            // The bell takes the reduction reached at the end of the last
            // block: a lag of at most kControl samples (0.33 ms at 48 kHz).
            for (std::uint32_t i = 0; i < nb; ++i) {
                if (dirty || fmove[i]) design_detectors(i, m, false);
                design_bell(i, m, false, dirty || fmove[i]);
            }

            for (std::uint32_t i = 0; i < m; ++i) {
                const std::uint32_t k = pos + i;
                double wet[kMaxCh], solo[kMaxCh];
                for (std::uint32_t c = 0; c < chs; ++c) {
                    wet[c] = in[c][k];
                    solo[c] = 0.0;
                }
                for (std::uint32_t b = 0; b < nb; ++b) {
                    Band& bd = bands_[b];
                    const dsp::SvfState::Derived& dn = bd.narrow.next();
                    const dsp::SvfState::Derived& dw = bd.wide.next();
                    const dsp::SvfState::Derived& dl = bd.bell.next();
                    for (std::uint32_t c = 0; c < chs; ++c) {
                        const double x = in[c][k];
                        const double n = bd.nb[c].tick(dn, x);
                        const double w = bd.wb[c].tick(dw, x);
                        bd.msn[c] = bd.nms[c].process(n);
                        bd.msw[c] = bd.wms[c].process(w);
                        solo[c] += n;
                        wet[c] = bd.bl[c].tick(dl, wet[c]);
                    }
                }
                mix_ += mix_step;
                out_gain_ += out_step;
                for (std::uint32_t c = 0; c < chs; ++c) {
                    const double dry = in[c][k];
                    double y;
                    if (listen == 1) y = (dry - wet[c]) * mix_;
                    else if (listen == 2) y = solo[c];
                    else y = dry + mix_ * (wet[c] - dry);
                    out[c][k] = static_cast<float>(y * out_gain_);
                }
            }
            mix_ = mix_end;
            out_gain_ = out_end;

            // The control step: the excess of the narrow level over the
            // wide, per band, then the reduction through the knee, depth and
            // the band's cap, and the branching smoother.
            const double ratio_q = kWideQ / smoother(kSelectivity).current();
            const double bias = ten_log10(ratio_q / (1.0 - ratio_q));
            for (std::uint32_t i = 0; i < nb; ++i) {
                Band& bd = bands_[i];
                double target = 0.0;
                for (std::uint32_t c = 0; c < chs; ++c) {
                    if (bd.msw[c] < kGateMs) continue;
                    // The surroundings are the wide level less the narrow
                    // one: a peak adds the same power to both, so it cancels
                    // out of the surroundings and the excess is not capped by
                    // the peak dominating the wide band-pass. Floor 30 dB.
                    double around = bd.msw[c] - bd.msn[c];
                    const double floor_ms = 1e-3 * bd.msw[c];
                    if (around < floor_ms) around = floor_ms;
                    const double excess = ten_log10((bd.msn[c] + 1e-30) / around) - bias;
                    const double x = excess - thr;
                    double r;
                    if (x <= -0.5 * kKneeDb) r = 0.0;
                    else if (x < 0.5 * kKneeDb) r = (x + 0.5 * kKneeDb) * (x + 0.5 * kKneeDb) / (2.0 * kKneeDb);
                    else r = x;
                    r *= depth;
                    if (r > cut[i]) r = cut[i];
                    if (r > target) target = r;
                }
                const double a = (target > bd.red ? att_ : rel_);
                const double am = m == kControl ? a : std::pow(a, static_cast<double>(m) * (1.0 / kControl));
                bd.red = am * bd.red + (1.0 - am) * target;
            }
        }
    }

private:
    std::uint32_t active_bands() const noexcept {
        int n = 3 + static_cast<int>(value(kBands) + 0.5f);
        if (n > static_cast<int>(kMaxBands)) n = kMaxBands;
        return static_cast<std::uint32_t>(n);
    }
    void clear_band(std::uint32_t i) noexcept {
        Band& b = bands_[i];
        for (std::uint32_t c = 0; c < kMaxCh; ++c) {
            b.nb[c].reset();
            b.wb[c].reset();
            b.bl[c].reset();
            b.nms[c].reset();
            b.wms[c].reset();
            b.msn[c] = b.msw[c] = 0.0;
        }
        b.red = 0.0;
        design_bell(i, 0, true, true);
    }
    double band_freq(std::uint32_t i) noexcept {
        const double f = smoother(kFreq1 + i).current();
        const double top = 0.45 * sample_rate();
        return f < top ? f : top;
    }
    // The estimate of a narrow band's power needs a window that holds many
    // of its own ring times: 12 periods, and never under 5 ms, which at
    // selectivity 6 and 4 kHz leaves the log ratio within about 1.3 dB
    // (relative std 1/sqrt(B T), B = pi f0 / 2Q).
    void design_detectors(std::uint32_t i, std::uint32_t m, bool jump) noexcept {
        Band& b = bands_[i];
        const double f = band_freq(i);
        const double sel = smoother(kSelectivity).current();
        const auto n = dsp::to_svf(dsp::design_matched({dsp::Shape::BandPass, f, sel, 0.0}, sample_rate()));
        const auto w = dsp::to_svf(dsp::design_matched({dsp::Shape::BandPass, f, kWideQ, 0.0}, sample_rate()));
        if (jump) {
            b.narrow.set(n);
            b.wide.set(w);
        } else {
            b.narrow.ramp_to(n, m);
            b.wide.ramp_to(w, m);
        }
        double tau = 12.0 / f;
        if (tau < 5e-3) tau = 5e-3;
        for (std::uint32_t c = 0; c < kMaxCh; ++c) {
            b.nms[c].set_time(tau, sample_rate());
            b.wms[c].set_time(tau, sample_rate());
        }
    }
    // Zero reduction is a 0 dB bell, the identity. Below 0.002 dB of change
    // (and with the frequency and width still) the last design stands.
    void design_bell(std::uint32_t i, std::uint32_t m, bool jump, bool force = false) noexcept {
        Band& b = bands_[i];
        if (!jump && !force && std::fabs(b.red - b.bell_red) < 0.002) return;
        b.bell_red = b.red;
        const double q = smoother(kSharpness).current();
        const auto c = dsp::to_svf(dsp::design_matched({dsp::Shape::Peak, band_freq(i), q, -b.red}, sample_rate()));
        if (jump) b.bell.set(c);
        else b.bell.ramp_to(c, m);
    }
    // Per-control-step coefficients: the time constants at the control rate.
    void update_times() noexcept {
        const double step_rate = sample_rate() / static_cast<double>(kControl);
        att_ = dsp::time_constant_coeff(smoother(kAttack).current() * 1e-3, step_rate);
        rel_ = dsp::time_constant_coeff(smoother(kRelease).current() * 1e-3, step_rate);
    }

    Band bands_[kMaxBands];
    double att_ = 0.0, rel_ = 0.0;
    double mix_ = 1.0;
    double out_gain_ = 1.0;
};

}  // namespace

std::unique_ptr<Processor> make_vocal_balance() { return std::make_unique<VocalBalance>(); }

}  // namespace sonora::native
