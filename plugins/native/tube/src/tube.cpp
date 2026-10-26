// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/tube.h"

#include <cmath>
#include <complex>
#include <vector>

#include "sonora/dsp/delay_line.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/noise.h"
#include "sonora/dsp/oversampling.h"
#include "sonora/dsp/supply.h"
#include "sonora/dsp/triode.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace tube;

const char* const kOffOn[] = {"off", "on"};
const char* const kQualities[] = {"eco (2x)", "standard (4x)", "high (8x)"};
const char* const kMainsChoices[] = {"50 Hz", "60 Hz"};
constexpr double kMainsHz[] = {50.0, 60.0};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"drive", "Drive", Unit::Decibel, Taper::Linear, -12.0f, 36.0f, 6.0f, 30.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"output", "Output", Unit::Decibel, Taper::Linear, -24.0f, 12.0f, 0.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    // Switching it changes the level by the drive: declicked by the base.
    {"auto_level", "Auto level", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 1.0f, 0.0f, true, false, true, "%.0f", kOffOn, 2},
    {"quality", "Quality", Unit::Choice, Taper::Stepped, 0.0f, 2.0f, 1.0f, 0.0f, false, true, false, "%.0f", kQualities, 3},
    // Slice 5. 100 % is the level the circuit makes, 0 % digitally silent
    // (ADR-0011 §3.3); off by default until the owner rules on item 7.
    {"noise", "Noise", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 0.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"hum", "Hum", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 0.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"mains", "Mains", Unit::Choice, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, true, false, "%.0f", kMainsChoices, 2},
};

// The noise generators' seeds, two per channel: fixed, so a render is the
// same every time (ADR-0011 §3.3, §3.4).
constexpr std::uint64_t kNoiseSeed = 0x536F6E6F72614E31ull;
// The leaky integrator that turns the grid leak's current noise into its
// equivalent at the input (prepare_noise): its corner, far under the band.
constexpr double kLeakCornerHz = 0.5;
// Ripple harmonics: this many integrated, then those within 60 dB of the
// strongest and under 20 kHz kept.
constexpr std::uint32_t kRippleHarmonics = 24;
constexpr double kHumFloor = 1e-3;

const Descriptor kDesc = {"org.sonora.tube", "Sonora Tube Drive", "Distortion", 1, kParams, kParamCount, 1, 2, false};

class Tube final : public ProcessorBase {
public:
    Tube() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        long q = std::lround(value(kQuality));
        q = q < 0 ? 0 : (q > 2 ? 2 : q);
        factor_ = kFactors[q];
        const std::uint32_t chs = channels();
        // The protected band is 20 kHz at every rate (0.4535 of 44.1 kHz),
        // not a fixed fraction of the rate: at 48 and 96 kHz the
        // half-bands get a wider transition and so fewer taps, and the
        // latency is shorter. The aliasing bar is measured to 20 kHz.
        const double passband = std::fmin(0.4535, 20000.0 / sample_rate());
        if (!os_.prepare(factor_, chs, max_frames(), passband)) return false;
        const double os_rate = sample_rate() * factor_;
        for (std::uint32_t c = 0; c < chs; ++c) {
            if (!stage_[c].prepare(os_rate)) return false;
        }
        double mag = 1.0, phase = 0.0;
        stage_[0].small_signal_gain(1000.0, mag, phase);
        // Divides out the gain and the stage's inversion.
        norm_ = -1.0 / mag;
        latency_ = os_.latency();
        set_latency(latency_);
        for (std::uint32_t c = 0; c < chs; ++c) dry_[c].prepare(latency_ + 1);
        drive_line_.assign(latency_ + 1, 1.0);
        scaled_.assign(max_frames(), 0.0f);
        wet_.assign(max_frames(), 0.0f);
        drive_.assign(max_frames(), 0.0);
        post_.assign(max_frames(), 0.0);
        mix_.assign(max_frames(), 0.0);
        noise_gain_.assign(max_frames(), 0.0);
        hum_.assign(max_frames(), 0.0);
        return prepare_noise() && prepare_hum();
    }

    // The stage's noise as sources at its input, in volts at vin (where
    // the drive's output is). The grid leak's thermal current flows into
    // node a, which Cin ties to the input: at vin it is that current
    // through Cin, i / (j w Cin), brown noise, made by integrating white
    // noise (with a corner at kLeakCornerHz so it cannot wander). Every
    // other source is white at the grid or refers to it as white across
    // the band (the plate and load resistors through the stage's own
    // gain): their sum at 1 kHz, over the gain there, is one white source.
    // The suite holds the result to ngspice's noise analysis of the same
    // circuit, band by band.
    bool prepare_noise() {
        const dsp::TriodeStage& st = stage_[0];
        double s[dsp::TriodeStage::kNoiseSources];
        st.small_signal_noise(1000.0, s);
        double others = 0.0;
        for (std::uint32_t k = 0; k < dsp::TriodeStage::kNoiseSources; ++k)
            if (k != dsp::TriodeStage::kNoiseRLeak) others += s[k];
        double mag = 1.0, phase = 0.0;
        st.small_signal_gain(1000.0, mag, phase);
        const double fs = sample_rate();
        // White noise of density e (V/sqrt(Hz)), sampled at fs: e^2 fs / 2.
        const double half_band = std::sqrt(0.5 * fs);
        white_sigma_ = std::sqrt(others) / mag * half_band;
        const dsp::TriodeStageCircuit ckt;
        const double i_leak = std::sqrt(4.0 * dsp::kBoltzmann * dsp::kNoiseTemperatureK / ckt.r_leak);
        // y[n] = a y[n-1] + (T / Cin) i[n]: at f well above the corner,
        // i / (j w Cin).
        leak_sigma_ = i_leak * half_band / (fs * ckt.c_in);
        leak_pole_ = std::exp(-dsp::kTwoPi * kLeakCornerHz / fs);
        return std::isfinite(white_sigma_) && std::isfinite(leak_sigma_);
    }

    // The supply's ripple at the load: the reservoir's harmonics from
    // integrating the bridge (supply.h), through the filter loaded by the
    // stage, through the stage from B+ to the load.
    bool prepare_hum() {
        long m = std::lround(value(kMains));
        m = m < 0 ? 0 : (m > 1 ? 1 : m);
        dsp::SupplyCircuit sc;
        sc.mains_hz = kMainsHz[m];
        dsp::SupplyRipple ripple;
        if (!dsp::simulate_supply(sc, kRippleHarmonics, ripple)) return false;
        std::vector<double> freq;
        std::vector<std::complex<double>> amp;
        double strongest = 0.0;
        for (std::uint32_t k = 0; k < kRippleHarmonics; ++k) {
            const double f = ripple.ripple_hz * (k + 1);
            std::complex<double> gain, admittance;
            stage_[0].small_signal_supply(f, gain, admittance);
            const std::complex<double> a =
                ripple.reservoir[k] * dsp::supply_filter_transfer(sc, f, admittance) * gain;
            freq.push_back(f);
            amp.push_back(a);
            strongest = std::fmax(strongest, std::abs(a));
        }
        std::vector<double> kf;
        std::vector<std::complex<double>> ka;
        for (std::size_t k = 0; k < freq.size(); ++k) {
            if (freq[k] >= 20000.0 || std::abs(amp[k]) < kHumFloor * strongest) continue;
            kf.push_back(freq[k]);
            ka.push_back(amp[k]);
        }
        hum_bank_.prepare(sample_rate(), kf, ka);
        return true;
    }

    void on_reset() noexcept override {
        os_.reset();
        for (auto& s : stage_) s.reset();
        for (auto& d : dry_) d.reset();
        const double g = dsp::db_to_gain(smoother(kDrive).current());
        for (double& v : drive_line_) v = g;
        drive_pos_ = 0;
        for (std::uint32_t c = 0; c < 2; ++c) {
            white_[c].seed_with(kNoiseSeed + 2 * c);
            leak_[c].seed_with(kNoiseSeed + 2 * c + 1);
            leak_y_[c] = 0.0;
        }
        hum_bank_.reset();
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        // The per-sample gains first: the smoothers advance once per
        // sample, whatever the channel count.
        const bool auto_level = value(kAutoLevel) >= 0.5f;
        const std::uint32_t n_line = static_cast<std::uint32_t>(drive_line_.size());
        for (std::uint32_t k = 0; k < frames; ++k) {
            const double g = dsp::db_to_gain(smoother(kDrive).next());
            drive_[k] = g;
            // Auto level divides by the drive the sample now leaving the
            // oversampler went in with: `latency_` samples ago.
            drive_line_[drive_pos_] = g;
            drive_pos_ = drive_pos_ + 1 == n_line ? 0 : drive_pos_ + 1;
            const double g_then = drive_line_[drive_pos_];
            const double level = dsp::db_to_gain(smoother(kOutput).next());
            post_[k] = norm_ * level / (auto_level ? g_then : 1.0);
            mix_[k] = 0.01 * smoother(kMix).next();
        }
        // Noise and hum: nothing is generated, and nothing added, while a
        // level is at 0 and staying there, so 0 % is the signal path
        // exactly as without them (ADR-0011 §3.3, digitally silent).
        const bool noise_on = level_active(kNoise, noise_gain_.data(), frames);
        const bool hum_on = level_active(kHum, hum_.data(), frames);
        if (hum_on) {
            // One supply feeds both channels' stages: the same hum on each.
            for (std::uint32_t k = 0; k < frames; ++k) hum_[k] *= hum_bank_.next();
            hum_bank_.renormalise();
        }
        const std::uint32_t chs = channels();
        const std::uint32_t n = frames * factor_;
        double* x[2] = {nullptr, nullptr};
        for (std::uint32_t c = 0; c < chs; ++c) {
            if (noise_on) {
                double y = leak_y_[c];
                for (std::uint32_t k = 0; k < frames; ++k) {
                    y = leak_pole_ * y + leak_sigma_ * leak_[c].next();
                    const double e = white_sigma_ * white_[c].next() + y;
                    scaled_[k] = static_cast<float>(in[c][k] * drive_[k] + noise_gain_[k] * e);
                }
                leak_y_[c] = y;
            } else {
                for (std::uint32_t k = 0; k < frames; ++k) scaled_[k] = static_cast<float>(in[c][k] * drive_[k]);
                leak_y_[c] = 0.0;
            }
            x[c] = os_.up(c, scaled_.data(), frames);
        }
        // Stereo: both stages in lockstep (triode.h, tick2), the same
        // numbers as one at a time, in less time.
        if (chs == 2) {
            for (std::uint32_t i = 0; i < n; ++i) dsp::TriodeStage::tick2(stage_[0], stage_[1], x[0][i], x[1][i], x[0][i], x[1][i]);
        } else {
            for (std::uint32_t i = 0; i < n; ++i) x[0][i] = stage_[0].tick(x[0][i]);
        }
        for (std::uint32_t c = 0; c < chs; ++c) {
            os_.down(c, wet_.data(), frames);
            for (std::uint32_t k = 0; k < frames; ++k) {
                dry_[c].push(in[c][k]);
                const double dry = dry_[c].read(latency_);
                const double wet = (hum_on ? wet_[k] + hum_[k] : wet_[k]) * post_[k];
                out[c][k] = static_cast<float>(dry + mix_[k] * (wet - dry));
            }
        }
    }

private:
    // Advances a level's smoother over the block and writes its gain per
    // sample (0..1) to `gain`. False, with nothing written, when the level
    // is 0 and not moving.
    bool level_active(std::uint32_t index, double* gain, std::uint32_t frames) noexcept SONORA_RT {
        ParamSmoother& sm = smoother(index);
        if (!sm.moving() && sm.current() == 0.0) return false;
        for (std::uint32_t k = 0; k < frames; ++k) gain[k] = 0.01 * sm.next();
        return true;
    }

    dsp::Oversampler os_;
    dsp::TriodeStage stage_[2];
    dsp::DelayLine dry_[2];
    std::uint32_t factor_ = 4;
    std::uint32_t latency_ = 0;
    double norm_ = -1.0;
    std::vector<double> drive_line_;
    std::uint32_t drive_pos_ = 0;
    std::vector<float> scaled_, wet_;
    std::vector<double> drive_, post_, mix_;
    // Noise (prepare_noise) and hum (prepare_hum).
    std::vector<double> noise_gain_, hum_;
    dsp::GaussianNoise white_[2], leak_[2];
    double leak_y_[2] = {0.0, 0.0};
    double white_sigma_ = 0.0, leak_sigma_ = 0.0, leak_pole_ = 0.0;
    dsp::HumBank hum_bank_;
};

}  // namespace

std::unique_ptr<Processor> make_tube() { return std::make_unique<Tube>(); }

}  // namespace sonora::native
