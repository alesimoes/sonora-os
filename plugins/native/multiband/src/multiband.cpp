// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/multiband.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/convolver.h"
#include "sonora/dsp/crossover.h"
#include "sonora/dsp/delay_line.h"
#include "sonora/dsp/dynamics.h"
#include "sonora/dsp/math.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace multiband;

const char* const kOffOn[] = {"off", "on"};
const char* const kBandCounts[] = {"3 bands", "4 bands"};
const char* const kPhases[] = {"minimum phase", "linear phase"};

// One band's seven parameters. Attack and release default shorter going up,
// as the ear and the material (slow low end, fast transients) want.
#define SONORA_MB_BAND(P, N, ATK, REL, RATIO)                                                                  \
    {P "_threshold", N " threshold", Unit::Decibel, Taper::Linear, -60.0f, 0.0f, -18.0f, 20.0f, true, false,   \
     false, "%.1f dB", nullptr, 0},                                                                            \
    {P "_ratio", N " ratio", Unit::Ratio, Taper::Log, 1.0f, 20.0f, RATIO, 20.0f, true, false, false,          \
     "%.1f:1", nullptr, 0},                                                                                    \
    {P "_attack", N " attack", Unit::Milliseconds, Taper::Log, 0.05f, 300.0f, ATK, 20.0f, true, false,        \
     false, "%.2f ms", nullptr, 0},                                                                            \
    {P "_release", N " release", Unit::Milliseconds, Taper::Log, 5.0f, 3000.0f, REL, 20.0f, true, false,      \
     false, "%.0f ms", nullptr, 0},                                                                            \
    {P "_makeup", N " make-up", Unit::Decibel, Taper::Linear, -12.0f, 24.0f, 0.0f, 20.0f, true, false,        \
     false, "%+.1f dB", nullptr, 0},                                                                           \
    {P "_solo", N " solo", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 20.0f, true, false, false, "%.0f", \
     kOffOn, 2},                                                                                               \
    {P "_bypass", N " bypass", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 20.0f, true, false, false,     \
     "%.0f", kOffOn, 2},

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    // The structure changes only at prepare(): the band count and phase
    // mode change the buffers and the latency (ADR-0011 §1.5).
    {"bands", "Bands", Unit::Choice, Taper::Stepped, 0.0f, 1.0f, 1.0f, 0.0f, false, true, false, "%.0f", kBandCounts, 2},
    {"phase", "Crossover phase", Unit::Choice, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, true, false, "%.0f", kPhases, 2},
    {"x1", "Crossover 1", Unit::Hertz, Taper::Log, 40.0f, 1000.0f, 150.0f, 0.0f, false, true, false, "%.0f Hz", nullptr, 0},
    {"x2", "Crossover 2", Unit::Hertz, Taper::Log, 150.0f, 8000.0f, 1000.0f, 0.0f, false, true, false, "%.0f Hz", nullptr, 0},
    {"x3", "Crossover 3", Unit::Hertz, Taper::Log, 1000.0f, 16000.0f, 5000.0f, 0.0f, false, true, false, "%.0f Hz", nullptr, 0},
    {"lookahead", "Look-ahead", Unit::Milliseconds, Taper::Linear, 0.0f, 10.0f, 0.0f, 0.0f, false, true, false, "%.1f ms", nullptr, 0},
    {"knee", "Knee", Unit::Decibel, Taper::Linear, 0.0f, 24.0f, 6.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"sc_hpf_on", "Side-chain high-pass", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 10.0f, true, false, false, "%.0f", kOffOn, 2},
    {"sc_hpf_freq", "Side-chain high-pass frequency", Unit::Hertz, Taper::Log, 20.0f, 500.0f, 80.0f, 20.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"auto_makeup", "Auto make-up", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 20.0f, true, false, false, "%.0f", kOffOn, 2},
    {"link", "Stereo link", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"out_gain", "Output", Unit::Decibel, Taper::Linear, -12.0f, 12.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
    SONORA_MB_BAND("b1", "Band 1", 30.0f, 300.0f, 2.0f)
    SONORA_MB_BAND("b2", "Band 2", 20.0f, 200.0f, 2.0f)
    SONORA_MB_BAND("b3", "Band 3", 10.0f, 150.0f, 2.0f)
    SONORA_MB_BAND("b4", "Band 4", 5.0f, 100.0f, 2.0f)
};
#undef SONORA_MB_BAND

const Descriptor kDesc = {"org.sonora.multiband", "Sonora Multiband", "Dynamics", 1, kParams, kParamCount, 1, 2, false};

constexpr std::uint32_t kControl = 16;
constexpr std::uint32_t kMaxCh = 2;
// Partition of the linear-phase convolvers: the head runs directly, so the
// FFT's own delay is hidden and the latency is the FIR's centre alone.
constexpr std::uint32_t kPartition = 128;
// 6 is about 60 dB of stopband: the transition is about 2.9 fs / taps wide.
constexpr double kKaiserBeta = 6.0;

std::uint32_t fir_taps(double sample_rate) { return sample_rate >= 88200.0 ? 4097u : 2049u; }

class Multiband final : public ProcessorBase, public BandMeters {
public:
    Multiband() : ProcessorBase(kDesc) {
        for (auto& g : gr_) g.store(0.0f, std::memory_order_relaxed);
    }

    std::uint32_t band_count() const noexcept override { return nb_; }
    float band_gr_db(std::uint32_t band) const noexcept override {
        return band < kMaxBands ? gr_[band].load(std::memory_order_relaxed) : 0.0f;
    }

protected:
    bool on_prepare() override {
        if (channels() > kMaxCh) return false;
        const double fs = sample_rate();
        nb_ = value(kBands) >= 0.5f ? 4u : 3u;
        linear_ = value(kPhase) >= 0.5f;
        look_ = static_cast<std::uint32_t>(std::lround(value(kLookahead) * 1e-3 * fs));
        double hz[kMaxBands - 1] = {value(kX1), value(kX2), value(kX3)};
        dsp::order_crossovers(hz, nb_ - 1, fs);

        const std::size_t frames = max_frames();
        bands_.assign(static_cast<std::size_t>(kMaxBands) * kMaxCh * frames, 0.0f);
        lp_.assign(static_cast<std::size_t>(kMaxBands - 1) * frames, 0.0f);

        centre_ = 0;
        if (linear_) {
            const std::uint32_t taps = fir_taps(fs);
            centre_ = (taps - 1) / 2;
            std::vector<double> h(taps);
            for (std::uint32_t k = 0; k + 1 < nb_; ++k) {
                dsp::linear_phase_lowpass(h.data(), taps, hz[k], fs, kKaiserBeta);
                if (!kernel_[k].prepare(kPartition, taps) || !kernel_[k].set(h.data(), taps)) return false;
                for (std::uint32_t c = 0; c < kMaxCh; ++c) {
                    if (!conv_[c][k].prepare(kPartition, taps)) return false;
                }
            }
            for (auto& d : input_delay_) d.prepare(centre_);
        } else {
            bank_.prepare(fs, nb_, hz);
        }
        for (std::uint32_t b = 0; b < kMaxBands; ++b) {
            for (std::uint32_t c = 0; c < kMaxCh; ++c) look_delay_[b][c].prepare(look_ + 1);
        }
        update_filter(0, true);
        for (std::uint32_t b = 0; b < kMaxBands; ++b) update_times(b);
        set_latency(centre_ + look_);
        on_reset();
        return true;
    }

    void on_reset() noexcept override {
        bank_.reset();
        for (auto& row : conv_) {
            for (auto& cv : row) cv.reset();
        }
        for (auto& d : input_delay_) d.reset();
        for (std::uint32_t b = 0; b < kMaxBands; ++b) {
            for (std::uint32_t c = 0; c < kMaxCh; ++c) {
                look_delay_[b][c].reset();
                hpf_[b][c].reset();
                det_[b][c].reset(0.0);
            }
            gr_[b].store(0.0f, std::memory_order_relaxed);
        }
    }

    void on_param(std::uint32_t index, bool jump) noexcept override {
        if (index == kScHpfFreq) update_filter(0, jump);
        if (jump && index >= kBandBase) {
            const std::uint32_t which = (index - kBandBase) % kPerBand;
            if (which == kAttack || which == kRelease) update_times((index - kBandBase) / kPerBand);
        }
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        split(in, frames);
        const std::uint32_t chs = channels();
        const std::uint32_t nb = nb_;
        double sum[kMaxCh];
        for (std::uint32_t pos = 0; pos < frames; pos += kControl) {
            const std::uint32_t m = frames - pos < kControl ? frames - pos : kControl;
            if (smoother(kScHpfFreq).moving()) update_filter(m, false);
            else smoother(kScHpfFreq).advance(m);
            bool moving = smoother(kKnee).moving() || smoother(kScHpfOn).moving() ||
                          smoother(kAutoMakeup).moving() || smoother(kLink).moving() ||
                          smoother(kOutGain).moving();
            for (std::uint32_t b = 0; b < nb; ++b) {
                const bool times = smoother(band_param(b, kAttack)).moving() || smoother(band_param(b, kRelease)).moving();
                smoother(band_param(b, kAttack)).advance(m);
                smoother(band_param(b, kRelease)).advance(m);
                if (times) update_times(b);
                for (std::uint32_t i = kThreshold; i < kPerBand; ++i) {
                    if (i != kAttack && i != kRelease) moving = moving || smoother(band_param(b, i)).moving();
                }
            }
            if (!moving) fetch(false);
            for (std::uint32_t i = 0; i < m; ++i) {
                const std::uint32_t k = pos + i;
                if (moving) fetch(true);
                const dsp::SvfState::Derived& d = hpf_ramp_.next();
                const double link = glob_.link;
                const bool linked = link >= 0.9999;
                double any_solo = 0.0;
                for (std::uint32_t b = 0; b < nb; ++b) any_solo = std::fmax(any_solo, bp_[b].solo);
                for (std::uint32_t c = 0; c < chs; ++c) sum[c] = 0.0;
                for (std::uint32_t b = 0; b < nb; ++b) {
                    const BandP& q = bp_[b];
                    double key[kMaxCh];
                    for (std::uint32_t c = 0; c < chs; ++c) {
                        const double x = band(b, c)[k];
                        const double h = hpf_[b][c].tick(d, x);
                        key[c] = x + glob_.hpw * (h - x);
                    }
                    double red[kMaxCh];
                    if (linked) {
                        double peak = 0.0;
                        for (std::uint32_t c = 0; c < chs; ++c) peak = std::fmax(peak, std::fabs(key[c]));
                        const double x = dsp::gain_to_db(peak, -400.0);
                        const double cdb = x - dsp::compressor_curve_db(x, q.thr, q.ratio, glob_.knee);
                        // Both detectors keep running, so leaving the link
                        // finds them in step.
                        for (std::uint32_t c = 0; c < chs; ++c) red[c] = det_[b][c].process(cdb);
                    } else {
                        double lvl[kMaxCh];
                        double lmax = -400.0;
                        for (std::uint32_t c = 0; c < chs; ++c) {
                            lvl[c] = dsp::gain_to_db(std::fabs(key[c]), -400.0);
                            lmax = std::fmax(lmax, lvl[c]);
                        }
                        for (std::uint32_t c = 0; c < chs; ++c) {
                            const double x = link * lmax + (1.0 - link) * lvl[c];
                            const double cdb = x - dsp::compressor_curve_db(x, q.thr, q.ratio, glob_.knee);
                            red[c] = det_[b][c].process(cdb);
                        }
                    }
                    // Giannoulis et al.: make-up for a 0 dBFS input, halved.
                    const double auto_db = glob_.autow == 0.0
                                               ? 0.0
                                               : -0.5 * dsp::compressor_curve_db(0.0, q.thr, q.ratio, glob_.knee) * glob_.autow;
                    const double wt = 1.0 - any_solo + any_solo * q.solo;
                    for (std::uint32_t c = 0; c < chs; ++c) {
                        double a = band(b, c)[k];
                        if (look_ > 0) {
                            look_delay_[b][c].push(static_cast<float>(a));
                            a = look_delay_[b][c].read(look_);
                        }
                        const double gc = dsp::db_to_gain(q.makeup + auto_db - red[c]);
                        const double g = gc + q.byp * (1.0 - gc);
                        sum[c] += a * g * wt;
                    }
                    if (k + 1 == frames) {
                        // The block's last value, for the telemetry region.
                        double r = red[0];
                        for (std::uint32_t c = 1; c < chs; ++c) r = std::fmax(r, red[c]);
                        gr_[b].store(static_cast<float>(r), std::memory_order_relaxed);
                    }
                }
                for (std::uint32_t c = 0; c < chs; ++c) out[c][k] = static_cast<float>(sum[c] * glob_.out);
            }
        }
    }

private:
    struct Glob {
        double knee = 0.0, autow = 0.0, hpw = 0.0, link = 1.0, out = 1.0;
    };
    struct BandP {
        double thr = 0.0, ratio = 1.0, makeup = 0.0, byp = 0.0, solo = 0.0;
    };

    float* band(std::uint32_t b, std::uint32_t c) noexcept {
        return bands_.data() + (static_cast<std::size_t>(b) * kMaxCh + c) * max_frames();
    }

    // Every smoother's value for the next sample (advance) or, in a chunk
    // where none is moving, its settled value once: the exp() of the log
    // tapers is then paid per chunk and not per sample.
    void fetch(bool advance) noexcept {
        auto v = [&](std::uint32_t i) { return advance ? smoother(i).next() : smoother(i).current(); };
        glob_.knee = v(kKnee);
        glob_.autow = v(kAutoMakeup);
        glob_.hpw = v(kScHpfOn);
        glob_.link = v(kLink) * 0.01;
        glob_.out = dsp::db_to_gain(v(kOutGain));
        for (std::uint32_t b = 0; b < nb_; ++b) {
            BandP& q = bp_[b];
            q.thr = v(band_param(b, kThreshold));
            q.ratio = v(band_param(b, kRatio));
            q.makeup = v(band_param(b, kMakeup));
            q.solo = v(band_param(b, kSolo));
            q.byp = v(band_param(b, kBandBypass));
        }
    }

    // The input into nb_ bands in bands_, over the whole block.
    void split(const float* const* in, std::uint32_t frames) noexcept {
        const std::uint32_t chs = channels();
        const std::uint32_t nx = nb_ - 1;
        if (linear_) {
            for (std::uint32_t c = 0; c < chs; ++c) {
                for (std::uint32_t k = 0; k < nx; ++k) {
                    conv_[c][k].process(kernel_[k], in[c], lp_.data() + static_cast<std::size_t>(k) * max_frames(), frames);
                }
                for (std::uint32_t i = 0; i < frames; ++i) {
                    input_delay_[c].push(in[c][i]);
                    double prev = 0.0;
                    for (std::uint32_t k = 0; k < nx; ++k) {
                        const double cur = lp_[static_cast<std::size_t>(k) * max_frames() + i];
                        band(k, c)[i] = static_cast<float>(cur - prev);
                        prev = cur;
                    }
                    // The input, a whole centre later, less the last low-pass:
                    // the bands telescope to the delayed input.
                    band(nx, c)[i] = static_cast<float>(static_cast<double>(input_delay_[c].read(centre_)) - prev);
                }
            }
        } else {
            double out[kMaxBands];
            for (std::uint32_t c = 0; c < chs; ++c) {
                for (std::uint32_t i = 0; i < frames; ++i) {
                    bank_.process(c, in[c][i], out);
                    for (std::uint32_t b = 0; b < nb_; ++b) band(b, c)[i] = static_cast<float>(out[b]);
                }
            }
        }
    }

    void update_filter(std::uint32_t m, bool jump) noexcept {
        const double f = jump ? smoother(kScHpfFreq).current() : smoother(kScHpfFreq).advance(m);
        const auto c = dsp::to_svf(dsp::design_matched({dsp::Shape::HighPass, f, 0.7071067811865476, 0.0}, sample_rate()));
        if (jump) hpf_ramp_.set(c);
        else hpf_ramp_.ramp_to(c, m);
    }
    void update_times(std::uint32_t b) noexcept {
        const double a = smoother(band_param(b, kAttack)).current() * 1e-3;
        const double r = smoother(band_param(b, kRelease)).current() * 1e-3;
        for (auto& det : det_[b]) det.set_times(a, r, sample_rate());
    }

    std::uint32_t nb_ = 4, look_ = 0, centre_ = 0;
    bool linear_ = false;
    std::vector<float> bands_, lp_;
    dsp::LrBank bank_;
    dsp::ConvolutionKernel kernel_[kMaxBands - 1];
    dsp::Convolver conv_[kMaxCh][kMaxBands - 1];
    dsp::DelayLine input_delay_[kMaxCh];
    dsp::DelayLine look_delay_[kMaxBands][kMaxCh];
    dsp::SvfRamp hpf_ramp_;
    dsp::SvfState hpf_[kMaxBands][kMaxCh];
    dsp::BranchingSmoother det_[kMaxBands][kMaxCh];
    Glob glob_;
    BandP bp_[kMaxBands];
    std::atomic<float> gr_[kMaxBands];
};

}  // namespace

std::unique_ptr<Processor> make_multiband() { return std::make_unique<Multiband>(); }

}  // namespace sonora::native
