// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/transformer.h"

#include <cmath>

#include "transformer_solve.h"

namespace sonora::dsp {

namespace {

using transformer_detail::kLanPerUnit;

// The Langevin function and its derivative, exactly, for the table
// (prepare). The closed form cancels badly near 0 (coth x and 1/x both blow
// up), so a series takes over under 0.1, where its first four terms are
// good to 1e-14.
void langevin_exact(double x, double& l, double& dl) noexcept {
    const double ax = std::fabs(x);
    if (ax < 0.1) {
        const double x2 = x * x;
        l = x * (1.0 / 3.0 - x2 * (1.0 / 45.0 - x2 * (2.0 / 945.0 - x2 / 4725.0)));
        dl = 1.0 / 3.0 - x2 * (1.0 / 15.0 - x2 * (2.0 / 189.0 - x2 / 675.0));
        return;
    }
    const double t = std::tanh(ax);
    const double sh = std::sinh(ax);
    const double mag = 1.0 / t - 1.0 / ax;
    l = x < 0.0 ? -mag : mag;
    dl = 1.0 / (ax * ax) - 1.0 / (sh * sh);
}

}  // namespace

bool OutputStage::prepare(double sample_rate, std::uint32_t channels, const TransformerCircuit& c,
                          const CoreParams& p, std::uint32_t substeps) {
    if (!(sample_rate > 0.0) || channels < 1 || channels > kMaxChannels) return false;
    if (substeps < 1 || substeps > kMaxSubsteps) return false;
    if (!(c.volts_per_unit > 0.0) || !(c.r_source > 0.0) || !(c.r_load > 0.0) || !(c.c_reservoir > 0.0) ||
        !(c.v_rail_idle > c.v_knee + 1.0) || !(c.r_supply >= 0.0) || !(c.i_quiescent >= 0.0))
        return false;
    if (!(p.l_core > 0.0) || !(p.m_sat > 0.0) || !(p.a > 0.0) || !(p.k > 0.0) || !(p.c >= 0.0 && p.c < 1.0) ||
        !(p.alpha >= 0.0))
        return false;
    ckt_ = c;
    p_ = p;
    channels_ = channels;
    substeps_ = substeps;
    inv_substeps_ = 1.0 / substeps;
    t_ = 1.0 / sample_rate;
    t_step_ = t_ * substeps;
    g_ = c.r_load / (c.r_source + c.r_load);
    rp_ = c.r_source * c.r_load / (c.r_source + c.r_load);
    half_ = 0.5 * t_step_ * rp_;
    inv_a_ = 1.0 / p.a;
    inv_rs_ = 1.0 / c.r_source;
    inv_out_ = 1.0 / (g_ * c.volts_per_unit);
    for (std::uint32_t k = 0; k <= kLangevinSegments; ++k)
        langevin_exact(static_cast<double>(k) / kLanPerUnit, lan_[k][0], lan_[k][1]);
    set_solve_tolerance(kDefaultSolveTol);
    // The demagnetised core's slope, on the anhysteretic curve, for the
    // first step after a reset.
    inv_slope_rest_ = 1.0 / (p.l_core * (1.0 + p.m_sat / (3.0 * p.a)) + half_);
    v_idle_ = c.v_rail_idle;
    sag_ = -1.0;  // so set_sag recomputes
    set_sag(1.0);
    reset();
    return true;
}

void OutputStage::set_solve_tolerance(double output_tol) noexcept {
    // A step of di in the magnetising current moves the output by
    // rp di / (g vpu) in signal units (1.0 = full scale).
    di_tol_ = output_tol * g_ * ckt_.volts_per_unit / rp_;
}

void OutputStage::reset() noexcept SONORA_RT {
    for (Core& s : ch_) {
        s = Core{};
        s.inv_slope = inv_slope_rest_;
    }
    vres_ = v_idle_;
    rail_ = v_idle_ - ckt_.v_knee;
    rail_drv_ = rail_;
    max_iter_seen_ = 0;
    evals_ = steps_ = 0;
}

void OutputStage::set_sag(double amount) noexcept SONORA_RT {
    amount = amount < 0.0 ? 0.0 : (amount > 1.0 ? 1.0 : amount);
    // The processor sets it every sample; the divisions below only when it moves.
    if (amount == sag_) return;
    sag_ = amount;
    rsup_ = ckt_.r_supply * sag_;
    // The unloaded voltage that leaves v_idle_ at the quiescent current, so
    // the headroom at idle is the same at every sag setting.
    v_open_ = v_idle_ + ckt_.i_quiescent * rsup_;
    if (rsup_ > 1e-9) {
        // Backward Euler on C dV/dt = (Vopen - V) / Rsup - I over one step:
        // V' = (V + a (Vopen / Rsup - I)) / (1 + a / Rsup), a = T / C.
        sup_a_ = t_step_ / ckt_.c_reservoir;
        sup_open_ = v_open_ / rsup_;
        sup_inv_den_ = 1.0 / (1.0 + sup_a_ / rsup_);
    }
}

void OutputStage::refine(const Core& s, double rhs, double& i, EvalV<Lane1>& e, double& f, double& inv,
                         double& di, std::uint32_t& n) const noexcept SONORA_RT {
    const double half = half_;
    const CoefV<Lane1> k = coef<Lane1>();
    double lo, hi;
    if (f > 0.0) {
        hi = i;
        lo = i - f / half;
    } else {
        lo = i;
        hi = i - f / half;
    }
    while (n < kMaxIter) {
        ++n;
        double next = i + di;
        if (!(next > lo && next < hi)) next = 0.5 * (lo + hi);
        i = next;
        e = eval_lanes<Lane1>(k, Lane1(s.i), Lane1(s.m), Lane1(s.m_irr), Lane1(i));
        f = e.psi.x - rhs + half * i;
        if (f > 0.0) hi = i; else lo = i;
        inv = 1.0 / (e.dpsi.x + half);
        di = -(f * inv);
        if (std::fabs(di) <= di_tol_ || hi - lo <= 1e-15 * (1.0 + std::fabs(i))) break;
    }
    // Stopped on the bracket (a kink the step cannot settle on) or on the
    // bound: no extrapolation past the last evaluated point.
    if (!(std::fabs(di) <= di_tol_)) di = 0.0;
}

void OutputStage::process(const double* const* in, double* const* out) noexcept SONORA_RT {
    process_block(in, out, 1, nullptr, 1);
}

void OutputStage::process_block(const double* const* in, double* const* out, std::uint32_t steps, const double* sag,
                                std::uint32_t steps_per_sag) noexcept SONORA_RT {
    if (steps_per_sag < 1) steps_per_sag = 1;
    if (channels_ == 2 && pair_) {
        transformer_process_pair(*this, in, out, steps, sag, steps_per_sag);
    } else {
        process_lanes<Lane1>(in, out, steps, sag, steps_per_sag);
    }
}

void OutputStage::tick(const double* in, double* out) noexcept SONORA_RT {
    if (substeps_ != 1) {
        for (std::uint32_t c = 0; c < channels_; ++c) out[c] = 0.0;
        return;
    }
    const double* ip[kMaxChannels] = {in, in + 1};
    double* op[kMaxChannels] = {out, out + 1};
    process(ip, op);
}

}  // namespace sonora::dsp
