// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/varimu.h"

#include <cmath>
#include <vector>

#include "sonora/dsp/delay_line.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/oversampling.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace varimu;

const char* const kOffOn[] = {"off", "on"};
const char* const kTimes[] = {"1", "2", "3", "4", "5 (program)", "6 (program)"};
const char* const kQualities[] = {"eco (no latency)", "standard (2x)", "high (4x)"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"input", "Input", Unit::Decibel, Taper::Linear, -12.0f, 24.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
    // The rectified peak level at which the cell has given 2 dB.
    {"threshold", "Threshold", Unit::Decibel, Taper::Linear, -50.0f, 0.0f, -20.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"time_constant", "Time constant", Unit::Choice, Taper::Stepped, 0.0f, 5.0f, 1.0f, 0.0f, true, false, false, "%.0f", kTimes, 6},
    {"sc_hpf_on", "Side-chain high-pass", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 10.0f, true, false, false, "%.0f", kOffOn, 2},
    {"sc_hpf_freq", "Side-chain high-pass frequency", Unit::Hertz, Taper::Log, 20.0f, 500.0f, 80.0f, 20.0f, true, false, false, "%.0f Hz", nullptr, 0},
    {"link", "Stereo link", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"output", "Output", Unit::Decibel, Taper::Linear, -12.0f, 24.0f, 0.0f, 20.0f, true, false, false, "%+.1f dB", nullptr, 0},
    {"iron", "Output transformer", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 0.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"external_sc", "External key", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true, "%.0f", kOffOn, 2},
    {"quality", "Quality", Unit::Choice, Taper::Stepped, 0.0f, 2.0f, 1.0f, 0.0f, false, true, false, "%.0f", kQualities, 3},
};

const Descriptor kDesc = {"org.sonora.varimu", "Sonora Vari-Mu", "Dynamics", 1, kParams, kParamCount, 1, 2, true};

constexpr std::uint32_t kMaxCh = 2;
constexpr std::uint32_t kControl = 16;

// The cell: gain = (1 - u)^1.5, the three-halves law of the remote-cutoff
// tube's transconductance. The bias u rises with the rectified level as
// u = kUCeiling z / (1 + z), z = rho level / threshold: the bias network
// saturates (the grid draws current as it nears cutoff), which keeps the
// output rising with the input, and the cell's reduction approaches 40 dB
// (the ceiling, (1 - u)^1.5 = 0.01) without a corner. rho puts 2 dB of
// reduction at the threshold: u = 1 - 10^(-2/30).
constexpr double kUThreshold = 0.14228643;
constexpr double kUCeiling = 0.95360;
constexpr double kRho = kUThreshold / (kUCeiling - kUThreshold);

// Fixed attack/release pairs and the program-dependent ones: attack,
// fast release, slow release (0: single network), all seconds.
struct Times {
    double attack, fast, slow;
};
constexpr Times kTimeTable[6] = {
    {0.0002, 0.3, 0.0}, {0.0002, 0.8, 0.0}, {0.0004, 2.0, 0.0},
    {0.0008, 5.0, 0.0}, {0.0004, 0.3, 3.0}, {0.0008, 0.8, 8.0},
};
// The slow network charges from the fast one through a resistor.
constexpr double kSlowCharge = 0.5;

// Cell curvature: a gentle cubic and an even term that grows with the
// reduction (the stage works nearer cutoff, where its curve bends more).
// Generic values of ours; degree 3, so at 2x nothing it makes folds back.
constexpr double kK3 = 0.01;
constexpr double kK2Rest = 0.004;
constexpr double kK2Reduction = 0.03;
// Where the grid's swing is bounded, full scale = 1 (+14 dBFS): the cubic
// above is monotonic up to sqrt(1 / (3 kK3)) = 5.8.
constexpr double kCellRail = 5.0;

// Iron: the flux is the lowpassed voltage; the core saturates when it
// reaches kFluxSat; the primary inductance rolls off below kPrimaryHz.
constexpr double kFluxHz = 200.0;
constexpr double kFluxSat = 3.0;
constexpr double kPrimaryHz = 12.0;
// The cell's output coupling capacitor, which also removes the DC the even
// term makes.
// The ripple filter between the networks and the grid: the rectifier
// charges the capacitor on each peak, and the ripple that leaves on the
// bias would modulate the gain at the tone's own rate. The first-order
// corner is above anything the slowest attack (0.2 ms) needs.
constexpr double kRippleHz = 3000.0;
constexpr double kDcHz = 4.0;

struct Channel {
    double hp1 = 0.0, hp2 = 0.0;  // key high-pass states
    double e1 = 0.0, e2 = 0.0;    // fast and slow networks
    double rip = 0.0;             // the ripple filter's state
    double gprev = 1.0;
    double dc = 0.0;              // cell output DC (even term)
    double flux = 0.0, prim = 0.0;
};

class VariMu final : public ProcessorBase {
public:
    VariMu() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        long q = std::lround(value(kQuality));
        q = q < 0 ? 0 : (q > 2 ? 2 : q);
        factor_ = kFactors[q];
        const std::uint32_t chs = channels();
        latency_ = 0;
        if (factor_ > 1) {
            const double passband = std::fmin(0.4535, 20000.0 / sample_rate());
            if (!os_.prepare(factor_, chs, max_frames(), passband)) return false;
            latency_ = os_.latency();
        }
        set_latency(latency_);
        for (std::uint32_t c = 0; c < chs; ++c) {
            dry_[c].prepare(latency_ + 1);
            xin_[c].assign(max_frames(), 0.0f);
            gain_[c].assign(max_frames(), 1.0);
            buf_[c].assign(factor_ == 1 ? max_frames() : 0, 0.0);
        }
        wet_.assign(max_frames(), 0.0f);
        out_gain_.assign(max_frames(), 1.0);
        iron_.assign(max_frames(), 0.0);
        mix_.assign(max_frames(), 1.0);
        const double fs = sample_rate();
        a_flux_ = 1.0 - std::exp(-dsp::kTwoPi * kFluxHz / fs);
        a_prim_ = 1.0 - std::exp(-dsp::kTwoPi * kPrimaryHz / fs);
        a_rip_ = 1.0 - std::exp(-dsp::kTwoPi * kRippleHz / fs);
        a_dc_ = 1.0 - std::exp(-dsp::kTwoPi * kDcHz / fs);
        gin_db_ = thr_db_ = out_db_ = 1e9;
        update_times();
        update_filter();
        return true;
    }

    void on_reset() noexcept override {
        os_.reset();
        for (auto& ch : ch_) ch = Channel{};
        for (auto& d : dry_) d.reset();
    }

    void on_param(std::uint32_t index, bool) noexcept override {
        if (index == kTimeConstant) update_times();
    }

    void render(const float* const* in, float* const* out, const float* const* sc, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        const bool keyed = value(kExternalSc) >= 0.5f;
        const std::uint32_t f = factor_;

        for (std::uint32_t pos = 0; pos < frames; pos += kControl) {
            const std::uint32_t m = frames - pos < kControl ? frames - pos : kControl;
            smoother(kScHpfFreq).advance(m);
            update_filter();
            for (std::uint32_t k = pos; k < pos + m; ++k) {
                // exp() only when the smoothed value moved: three of them
                // per sample were most of the cost of the base rate.
                const double gin = cached_gain(smoother(kInput).next(), gin_db_, gin_);
                const double thr = cached_gain(smoother(kThreshold).next(), thr_db_, thr_);
                const double hpw = smoother(kScHpfOn).next();
                const double link = smoother(kLink).next() * 0.01;
                out_gain_[k] = cached_gain(smoother(kOutput).next(), out_db_, outg_);
                iron_[k] = smoother(kIron).next() * 0.01;
                mix_[k] = smoother(kMix).next() * 0.01;
                const double scale = kRho / thr;

                double r[kMaxCh];
                double rmax = 0.0;
                for (std::uint32_t c = 0; c < chs; ++c) {
                    Channel& s = ch_[c];
                    const double x = keyed ? sc[c][k] : in[c][k] * gin;
                    // Two one-pole high-passes: 12 dB/octave in the key.
                    s.hp1 += a_hpf_ * (x - s.hp1);
                    const double h1 = x - s.hp1;
                    s.hp2 += a_hpf_ * (h1 - s.hp2);
                    const double key = x + hpw * ((h1 - s.hp2) - x);
                    r[c] = std::fabs(key);
                    if (r[c] > rmax) rmax = r[c];
                    xin_[c][k] = static_cast<float>(in[c][k] * gin);
                }
                for (std::uint32_t c = 0; c < chs; ++c) {
                    Channel& s = ch_[c];
                    const double lvl = link * rmax + (1.0 - link) * r[c];
                    // The rectifier charges the fast capacitor through the
                    // attack resistance when the key is above it, and the
                    // capacitor discharges through the release resistance.
                    if (lvl > s.e1) s.e1 += a_att_ * (lvl - s.e1);
                    else s.e1 -= a_rel1_ * s.e1;
                    double ctl = s.e1;
                    if (slow_) {
                        if (s.e1 > s.e2) s.e2 += a_charge_ * (s.e1 - s.e2);
                        else s.e2 -= a_rel2_ * s.e2;
                        if (s.e2 > ctl) ctl = s.e2;
                    }
                    s.rip += a_rip_ * (ctl - s.rip);
                    const double z = s.rip * scale;
                    const double u = kUCeiling * z / (1.0 + z);
                    const double v = 1.0 - u;
                    gain_[c][k] = v * std::sqrt(v);
                }
            }
        }

        for (std::uint32_t c = 0; c < chs; ++c) {
            Channel& s = ch_[c];
            double* y;
            if (f > 1) {
                y = os_.up(c, xin_[c].data(), frames);
            } else {
                y = buf_[c].data();
                for (std::uint32_t k = 0; k < frames; ++k) y[k] = xin_[c][k];
            }
            double g0 = s.gprev;
            for (std::uint32_t k = 0; k < frames; ++k) {
                const double g1 = gain_[c][k];
                const double k2 = kK2Rest + kK2Reduction * (1.0 - g1);
                for (std::uint32_t j = 0; j < f; ++j) {
                    // The gain moves in a ramp across the base-rate step.
                    const double g = g0 + (g1 - g0) * static_cast<double>(j + 1) / static_cast<double>(f);
                    // The grid swing is bounded first (smoothly, so the cubic
                    // below never turns over on a hot input), then the curve.
                    const double raw = y[k * f + j] / kCellRail;
                    const double r2 = raw * raw;
                    const double x = y[k * f + j] / std::sqrt(std::sqrt(1.0 + r2 * r2));
                    y[k * f + j] = g * (x + k2 * x * x - kK3 * x * x * x);
                }
                g0 = g1;
            }
            s.gprev = g0;
            if (f > 1) os_.down(c, wet_.data(), frames);
            for (std::uint32_t k = 0; k < frames; ++k) {
                double w = f > 1 ? static_cast<double>(wet_[k]) : y[k];
                // The even term moves the cell's mean with the level.
                s.dc += a_dc_ * (w - s.dc);
                w = (w - s.dc) * out_gain_[k];
                // The transformer: primary inductance, then flux saturation.
                s.flux += a_flux_ * (w - s.flux);
                s.prim += a_prim_ * (w - s.prim);
                const double fr = s.flux / kFluxSat;
                const double lost = s.flux - kFluxSat * fr / std::sqrt(1.0 + fr * fr);
                const double iron = (w - s.prim) - lost;
                w += iron_[k] * (iron - w);
                dry_[c].push(in[c][k]);
                const double dry = dry_[c].read(latency_);
                out[c][k] = static_cast<float>(dry + mix_[k] * (w - dry));
            }
        }
    }

private:
    static double cached_gain(double db, double& last_db, double& gain) noexcept {
        if (db != last_db) {
            last_db = db;
            gain = dsp::db_to_gain(db);
        }
        return gain;
    }
    void update_times() noexcept {
        long t = std::lround(value(kTimeConstant));
        t = t < 0 ? 0 : (t > 5 ? 5 : t);
        const Times& tm = kTimeTable[t];
        const double fs = sample_rate();
        a_att_ = 1.0 - std::exp(-1.0 / (tm.attack * fs));
        a_rel1_ = 1.0 - std::exp(-1.0 / (tm.fast * fs));
        slow_ = tm.slow > 0.0;
        a_charge_ = 1.0 - std::exp(-1.0 / (kSlowCharge * fs));
        a_rel2_ = slow_ ? 1.0 - std::exp(-1.0 / (tm.slow * fs)) : 0.0;
    }
    void update_filter() noexcept {
        a_hpf_ = 1.0 - std::exp(-dsp::kTwoPi * smoother(kScHpfFreq).current() / sample_rate());
    }

    dsp::Oversampler os_;
    Channel ch_[kMaxCh];
    dsp::DelayLine dry_[kMaxCh];
    std::vector<float> xin_[kMaxCh];
    std::vector<double> gain_[kMaxCh];
    std::vector<double> buf_[kMaxCh];
    std::vector<float> wet_;
    std::vector<double> out_gain_, iron_, mix_;
    std::uint32_t factor_ = 2;
    std::uint32_t latency_ = 0;
    double a_att_ = 0.0, a_rel1_ = 0.0, a_rel2_ = 0.0, a_charge_ = 0.0;
    double a_rip_ = 0.0, a_hpf_ = 0.0, a_flux_ = 0.0, a_prim_ = 0.0, a_dc_ = 0.0;
    bool slow_ = false;
    double gin_db_ = 1e9, thr_db_ = 1e9, out_db_ = 1e9;
    double gin_ = 1.0, thr_ = 1.0, outg_ = 1.0;
};

}  // namespace

std::unique_ptr<Processor> make_varimu() { return std::make_unique<VariMu>(); }

}  // namespace sonora::native
