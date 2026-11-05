// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/transformer.h"

#include <cmath>
#include <vector>

#include "sonora/dsp/math.h"
#include "sonora/dsp/transformer.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace {

using namespace transformer;

const char* const kCoreNames[] = {"small", "line input", "large"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"drive", "Drive", Unit::Decibel, Taper::Linear, 0.0f, 24.0f, 6.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"core", "Core", Unit::Choice, Taper::Stepped, 0.0f, 2.0f, 1.0f, 0.0f, false, true, false, "%.0f", kCoreNames, 3},
    {"output", "Output", Unit::Decibel, Taper::Linear, -12.0f, 12.0f, 0.0f, 20.0f, true, false, false, "%.1f dB", nullptr, 0},
};

const Descriptor kDesc = {"org.sonora.transformer", "Sonora Transformer", "Distortion", 1, kParams, kParamCount, 1, 2, false};

// The Jiles-Atherton constants of a core. All the currents scale together
// (Ms, a, k) with the inductance the other way, which leaves the shape of
// the loop alone and moves the two things the specification names: the
// saturation flux Lc Ms, and the low-field inductance. The shape ratios are
// those of common/dsp's output stage default (Ms / a = 4000, k / Ms = 7.5e-5,
// c = 0.3, alpha = 2e-5), whose behaviour test_transformer holds.
dsp::CoreParams core_params(const CoreSpec& spec, double r_parallel) {
    dsp::CoreParams p;
    const double chi = 4000.0 / 3.0;  // Ms / 3a, the anhysteretic slope at the origin
    // The reversible part moves on a demagnetised core below the coercive
    // field: the low-field slope is c times the anhysteretic one, with the
    // domain coupling's feedback.
    const double chi_low = p.c * chi / (1.0 - p.alpha * chi);
    const double l_low = r_parallel / (dsp::kTwoPi * spec.corner_hz);
    p.l_core = l_low / (1.0 + chi_low);
    p.m_sat = spec.flux_vs / p.l_core;
    p.a = p.m_sat / 4000.0;
    p.k = 7.5e-5 * p.m_sat;
    return p;
}

class Transformer final : public ProcessorBase {
public:
    Transformer() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        long c = std::lround(value(kCore));
        c = c < 0 ? 0 : (c > 2 ? 2 : c);
        dsp::TransformerCircuit ckt;
        ckt.volts_per_unit = kVoltsPerUnit;
        ckt.r_source = kRSource;
        ckt.r_load = kRLoad;
        // A stiff supply a long way above anything the input reaches: the
        // driver is a source, and the core is the only nonlinearity.
        ckt.v_rail_idle = 1e6;
        ckt.r_supply = 0.0;
        const double rp = kRSource * kRLoad / (kRSource + kRLoad);
        if (!stage_.prepare(sample_rate(), channels(), ckt, core_params(kCores[c], rp), 1)) return false;
        for (auto* v : {&drive_, &inv_drive_, &post_}) v->assign(max_frames(), 0.0);
        for (std::uint32_t ch = 0; ch < 2; ++ch) {
            x_[ch].assign(max_frames(), 0.0);
            y_[ch].assign(max_frames(), 0.0);
        }
        set_latency(0);
        return true;
    }

    void on_reset() noexcept override { stage_.reset(); }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        auto gains = [frames](ParamSmoother& sm, double* dst) noexcept {
            if (sm.moving()) {
                for (std::uint32_t k = 0; k < frames; ++k) dst[k] = dsp::db_to_gain(sm.next());
            } else {
                const double g = dsp::db_to_gain(sm.next());
                for (std::uint32_t k = 0; k < frames; ++k) dst[k] = g;
            }
        };
        gains(smoother(kDrive), drive_.data());
        gains(smoother(kOutput), post_.data());
        for (std::uint32_t k = 0; k < frames; ++k) inv_drive_[k] = post_[k] / drive_[k];

        const std::uint32_t chs = channels();
        const double* ip[2] = {x_[0].data(), x_[1].data()};
        double* op[2] = {y_[0].data(), y_[1].data()};
        for (std::uint32_t c = 0; c < chs; ++c)
            for (std::uint32_t k = 0; k < frames; ++k) x_[c][k] = static_cast<double>(in[c][k]) * drive_[k];
        stage_.process_block(ip, op, frames, nullptr, 1);
        for (std::uint32_t c = 0; c < chs; ++c)
            for (std::uint32_t k = 0; k < frames; ++k) out[c][k] = static_cast<float>(y_[c][k] * inv_drive_[k]);
    }

private:
    dsp::OutputStage stage_;
    std::vector<double> x_[2], y_[2], drive_, inv_drive_, post_;
};

}  // namespace

std::unique_ptr<Processor> make_transformer() { return std::make_unique<Transformer>(); }

}  // namespace sonora::native
