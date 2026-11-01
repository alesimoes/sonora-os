// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/transformer.h"

#include <cmath>

namespace sonora::dsp {

namespace {

constexpr double kUMax = 1e6;

// The Langevin function and its derivative together. The closed form
// cancels badly near 0 (coth x and 1/x both blow up), so a series takes over
// under 0.1, where its first four terms are good to 1e-14. Above it one
// exp serves both: with e = exp(-2|x|), coth|x| = (1 + e) / (1 - e) and
// 1 / sinh^2 |x| = 4 e / (1 - e)^2, where a tanh and a sinh were two calls.
void langevin_both(double x, double& l, double& dl) noexcept {
    const double ax = std::fabs(x);
    if (ax < 0.1) {
        const double x2 = x * x;
        l = x * (1.0 / 3.0 - x2 * (1.0 / 45.0 - x2 * (2.0 / 945.0 - x2 / 4725.0)));
        dl = 1.0 / 3.0 - x2 * (1.0 / 15.0 - x2 * (2.0 / 189.0 - x2 / 675.0));
        return;
    }
    const double e = std::exp(-2.0 * ax);
    const double om = 1.0 - e;
    const double inv = 1.0 / ax;
    const double mag = (1.0 + e) / om - inv;
    l = x < 0.0 ? -mag : mag;
    dl = inv * inv - 4.0 * e / (om * om);
}

}  // namespace

bool OutputStage::prepare(double sample_rate, std::uint32_t channels, const TransformerCircuit& c,
                          const CoreParams& p) {
    if (!(sample_rate > 0.0) || channels < 1 || channels > kMaxChannels) return false;
    if (!(c.volts_per_unit > 0.0) || !(c.r_source > 0.0) || !(c.r_load > 0.0) || !(c.c_reservoir > 0.0) ||
        !(c.v_rail_idle > c.v_knee + 1.0) || !(c.r_supply >= 0.0) || !(c.i_quiescent >= 0.0))
        return false;
    if (!(p.l_core > 0.0) || !(p.m_sat > 0.0) || !(p.a > 0.0) || !(p.k > 0.0) || !(p.c >= 0.0 && p.c < 1.0) ||
        !(p.alpha >= 0.0))
        return false;
    ckt_ = c;
    p_ = p;
    channels_ = channels;
    t_ = 1.0 / sample_rate;
    g_ = c.r_load / (c.r_source + c.r_load);
    rp_ = c.r_source * c.r_load / (c.r_source + c.r_load);
    v_idle_ = c.v_rail_idle;
    set_sag(1.0);
    reset();
    return true;
}

void OutputStage::reset() noexcept SONORA_RT {
    for (Core& s : ch_) s = Core{};
    vres_ = v_idle_;
    rail_ = v_idle_ - ckt_.v_knee;
    max_iter_seen_ = 0;
}

void OutputStage::set_sag(double amount) noexcept SONORA_RT {
    sag_ = amount < 0.0 ? 0.0 : (amount > 1.0 ? 1.0 : amount);
    rsup_ = ckt_.r_supply * sag_;
    // The unloaded voltage that leaves v_idle_ at the quiescent current, so
    // the headroom at idle is the same at every sag setting.
    v_open_ = v_idle_ + ckt_.i_quiescent * rsup_;
}

OutputStage::Eval OutputStage::eval(const Core& s, double i) const noexcept SONORA_RT {
    const double dh = i - s.i;
    const double he = i + p_.alpha * s.m;
    const double x = he / p_.a;
    double lan, dlan;
    langevin_both(x, lan, dlan);
    const double man = p_.m_sat * lan;
    const double dman = p_.m_sat / p_.a * dlan;
    const double d = man - s.m_irr;
    const double delta = dh > 0.0 ? 1.0 : (dh < 0.0 ? -1.0 : 0.0);
    double m_irr = s.m_irr;
    double dm_irr = 0.0;
    if (delta * d > 0.0) {
        // dMirr/dH = (Man - Mirr) / (delta k - alpha (Man - Mirr)). The
        // denominator's floor is the guard against the non-physical region
        // (k < alpha |d|) in which the textbook form runs backwards.
        const double ad = std::fabs(d);
        double den = p_.k - p_.alpha * ad;
        const double floor_den = 0.05 * p_.k;
        if (den < floor_den) den = floor_den;
        const double f = ad / den;
        m_irr = s.m_irr + dh * f;
        if (delta * (m_irr - man) > 0.0) {
            // A step must not carry Mirr past the curve it is chasing.
            m_irr = man;
        } else {
            dm_irr = f + std::fabs(dh) * dman * p_.k / (den * den);
        }
    }
    Eval e;
    e.m_irr = m_irr;
    e.m = m_irr + p_.c * (man - m_irr);
    e.psi = p_.l_core * (i + e.m);
    e.dpsi = p_.l_core * (1.0 + (1.0 - p_.c) * dm_irr + p_.c * dman);
    return e;
}

double OutputStage::step(Core& s, double vs) noexcept SONORA_RT {
    const double half = 0.5 * t_ * rp_;
    const double rhs = s.psi + 0.5 * t_ * (g_ * vs + s.v);
    // F(i) = psi(i) - rhs + half i: strictly increasing with slope >= half,
    // so from F(i0) the root lies within |F(i0)| / half of i0.
    // The state is the converged point of the last step, so F(s.i) needs no
    // evaluation: s.psi is psi there. The first Newton step uses the slope
    // kept from that step (the core's slope moves little in one sample);
    // the bracket is then taken at the point it lands on, as before.
    const double slope = s.slope > 0.0 ? s.slope : p_.l_core;
    const double f0 = s.psi - rhs + half * s.i;
    double i = s.i - f0 / (slope + half);
    Eval e = eval(s, i);
    double f = e.psi - rhs + half * i;
    const double tol = 1e-11 * (1.0 + std::fabs(rhs));
    std::uint32_t n = 1;
    if (std::fabs(f) > tol) {
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
            double next = i - f / (e.dpsi + half);
            if (!(next > lo && next < hi)) next = 0.5 * (lo + hi);
            i = next;
            e = eval(s, i);
            f = e.psi - rhs + half * i;
            if (f > 0.0) hi = i; else lo = i;
            if (std::fabs(f) <= tol || hi - lo <= 1e-15 * (1.0 + std::fabs(i))) break;
        }
    }
    if (n > max_iter_seen_) max_iter_seen_ = n;
    const double v = g_ * vs - rp_ * i;
    s.psi = e.psi;
    s.i = i;
    s.v = v;
    s.m = e.m;
    s.m_irr = e.m_irr;
    s.slope = e.dpsi;
    return v;
}

void OutputStage::tick(const double* in, double* out) noexcept SONORA_RT {
    const double vpu = ckt_.volts_per_unit;
    const double rail = rail_;
    double drawn = ckt_.i_quiescent;
    for (std::uint32_t c = 0; c < channels_; ++c) {
        double x = in[c];
        if (!std::isfinite(x)) x = 0.0;
        // |u| is capped before u^4: past 1e6 the soft clip is flat at the
        // rail to 1e-6 relative, and u^4 (and x * vpu) can no longer reach
        // inf, which would make vs = inf / inf = NaN and poison the state.
        double u = x * vpu / rail;
        if (!(u < kUMax)) u = kUMax;
        if (!(u > -kUMax)) u = -kUMax;
        const double u2 = u * u;
        const double vs = rail * u / std::sqrt(std::sqrt(1.0 + u2 * u2));
        const double v = step(ch_[c], vs);
        // The primary's current, from the driver: push-pull draws it from
        // the rail on both half cycles.
        drawn += std::fabs(vs - v) / ckt_.r_source;
        const double y = v / (g_ * vpu);
        out[c] = std::isfinite(y) ? y : 0.0;
    }
    if (rsup_ <= 1e-9) {
        vres_ = v_idle_;
    } else {
        // Backward Euler on C dV/dt = (Vopen - V) / Rsup - I: stable at any
        // step, however light the source resistance.
        const double a = t_ / ckt_.c_reservoir;
        vres_ = (vres_ + a * (v_open_ / rsup_ - drawn)) / (1.0 + a / rsup_);
    }
    rail_ = vres_ - ckt_.v_knee;
    if (rail_ < 10.0) rail_ = 10.0;
}

}  // namespace sonora::dsp
