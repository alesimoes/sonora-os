// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/fet.h"

#include <cmath>

#include "sonora/dsp/math.h"

namespace sonora::dsp {

namespace {

// exp() of a junction voltage over nVt is bounded here: the side chain's
// swing bounds the emf, so a converged junction never sees more than about
// 0.8 V (20 nVt); the bound only keeps a Newton overshoot finite.
constexpr double kMaxExpArg = 80.0;

inline double bounded_exp(double x) noexcept { return std::exp(x < kMaxExpArg ? x : kMaxExpArg); }

// A junction further reverse than this many nVt carries -Is and nothing
// measurable (pair_current() says why).
constexpr double kOffNvt = 12.0;

// The ohmic channel with lambda, solved as ohmic_drain() says, with
// inv = 1 / (g0 + g) given.
inline double ohmic_drain_with(double b, double g0, double gch, double lambda, double inv) noexcept {
    const double a0 = b * g0 * inv;
    const double eps = gch * lambda * a0 * inv;
    return a0 * (1.0 - eps + 2.0 * eps * eps);
}

// The ohmic channel with lambda, g a (1 + lambda a) = (b - a) g0, solved
// by its series in eps = g lambda a0 / (g0 + g) to second order: one
// division and no square root, where the quadratic's formula takes both.
// eps is lambda times the drain (under 1e-2 at any reduction the channel
// stays ohmic at), so the series' remainder, about 5 eps^3, is under 5e-6
// of the drain and in practice 1e-12: within the solve's own tolerance.
inline double ohmic_drain(double b, double g0, double gch, double lambda) noexcept {
    return ohmic_drain_with(b, g0, gch, lambda, 1.0 / (g0 + gch));
}

// SPICE's junction step limit (pnjlim, Nagel 1975): a forward step that
// would multiply the current by more than e^(step / nVt) is taken on a log
// scale instead, so Newton walks up an exponential rather than overshooting.
inline double limit_step(double u_new, double u_old, double nvt, double vcrit) noexcept {
    if (u_new > vcrit && std::fabs(u_new - u_old) > 2.0 * nvt) {
        if (u_old > 0.0) {
            const double arg = 1.0 + (u_new - u_old) / nvt;
            return arg > 0.0 ? u_old + nvt * std::log(arg) : vcrit;
        }
        return nvt * std::log(u_new / nvt);
    }
    return u_new;
}

}  // namespace

// The linear part of one channel's step: Vc' = p + q I' alone, Vq' = alpha +
// beta_q Vc', and the attack network's history h / gz.
struct FetChannel::Linear {
    double p, q, r, k, alpha, beta_q, hz;  // p = q r, k = 1 / q
};

void FetChannel::prepare(double sample_rate, const FetCircuit& c, const JfetParams& j, const DiodeParams& d) noexcept {
    c_ = c;
    j_ = j;
    d_ = d;
    dt_ = 1.0 / sample_rate;
    nvt_ = d.n * d.vt;
    vcrit_ = nvt_ * std::log(nvt_ / (1.41421356237 * d.is));
    ratio_ = 0.0;
    r55_ = 0.0;
    r56_ = 0.0;
    update();
    reset();
}

void FetChannel::reset() noexcept {
    vc_ = vq_ = v_target_;
    // The rest equilibrium: the leakage the release resistor balances.
    i_det_ = (v_target_ - v_rest_) * gr_;
    f22_ = 0.0;
    f9_ = 0.0;
    vz_ = i_det_ * c_.r54;
    i27_ = 0.0;
    u_ = vb_ - v_target_ - (c_.r54 + r55_) * i_det_;
    vd_ = 0.0;
    s_last_ = 0.0;
    vg_last_ = v_target_;
    iu_last_ = 2.0 * d_.is * bounded_exp(u_ * inv_nvt_) * inv_nvt_;
    iterations_ = 0;
}

void FetChannel::set_ratio(double position) noexcept { set_controls(position, r55_, r56_); }
void FetChannel::set_attack_ohms(double r55) noexcept { set_controls(ratio_, r55, r56_); }
void FetChannel::set_release_ohms(double r56) noexcept { set_controls(ratio_, r55_, r56); }

void FetChannel::set_controls(double position, double r55, double r56) noexcept {
    const double p = clamp(position, 0.0, static_cast<double>(kFetAllButtons));
    const double a = clamp(r55, 0.0, c_.r55_max), r = clamp(r56, 0.0, c_.r56_max);
    if (p == ratio_ && a == r55_ && r == r56_) return;
    ratio_ = p;
    r55_ = a;
    r56_ = r;
    update();
}

void FetChannel::ratio_network(const FetCircuit& c, double position, double& tap, double& offset_v) noexcept {
    const double p = clamp(position, 0.0, static_cast<double>(kFetAllButtons));
    const double string = c.r19 + c.r20 + c.r21 + c.r22;
    const double ladder = c.r76 + c.r75 + c.r73 + c.r68 + c.r64;
    const double i_l = c.bias_drive_v / (c.r35 + ladder);
    // [M] Fig. 5: the A sections take the side chain from the string's
    // taps (20:1 at its top, 4:1 above R22); the B sections take the bias
    // from the ladder (4:1 nearest the rest node, after R76). All four in
    // short R19-R21 and R75-R68: the side chain from the top of R22 alone,
    // the bias from R76's tap with more current in the shortened ladder.
    double taps[kFetAllButtons + 1], offs[kFetAllButtons + 1];
    const double den = c.rs_in + string;
    taps[0] = c.r22 / den;
    taps[1] = (c.r21 + c.r22) / den;
    taps[2] = (c.r20 + c.r21 + c.r22) / den;
    taps[3] = string / den;
    taps[4] = c.r22 / (c.rs_in + c.r22);
    offs[0] = i_l * c.r76;
    offs[1] = i_l * (c.r76 + c.r75);
    offs[2] = i_l * (c.r76 + c.r75 + c.r73);
    offs[3] = i_l * (c.r76 + c.r75 + c.r73 + c.r68);
    offs[4] = c.bias_drive_v / (c.r35 + c.r76 + c.r64) * c.r76;
    const int lo = static_cast<int>(std::floor(p));
    const int hi = lo < kFetAllButtons ? lo + 1 : lo;
    const double w = p - lo;
    tap = taps[lo] + w * (taps[hi] - taps[lo]);
    offset_v = offs[lo] + w * (offs[hi] - offs[lo]);
}

double FetChannel::channel_current(const JfetParams& j, double vg0, double vds, double* slope,
                                   double* slope_gate) noexcept {
    // [SH], SPICE's JFET level 1 for either sign of vds, with the LN gate
    // vg0 + vds / 2. Measured from the terminal acting as the source, the
    // gate is vg0 + |vds| / 2 either way, so the law is odd in vds.
    const double a = std::fabs(vds);
    const double v0 = vg0 - j.vto;
    const double vgst = v0 + 0.5 * a;
    if (!(vgst > 0.0)) {
        if (slope != nullptr) *slope = 0.0;
        if (slope_gate != nullptr) *slope_gate = 0.0;
        return 0.0;
    }
    const double betap = j.beta * (1.0 + j.lambda * a);
    double h, dh, dg;
    if (vgst <= a) {
        // Saturated: beta' vgst^2.
        h = betap * vgst * vgst;
        dh = j.beta * j.lambda * vgst * vgst + betap * vgst;
        dg = 2.0 * betap * vgst;
    } else {
        // Ohmic: beta' a (2 vgst - a), and 2 vgst - a = 2 v0: the LN
        // circuit's cancellation, linear but for lambda.
        h = 2.0 * v0 * betap * a;
        dh = 2.0 * v0 * j.beta * (1.0 + 2.0 * j.lambda * a);
        dg = 2.0 * betap * a;
    }
    if (slope != nullptr) *slope = dh;
    if (slope_gate != nullptr) *slope_gate = vds < 0.0 ? -dg : dg;
    return vds < 0.0 ? -h : h;
}

double FetChannel::rest_for(const DiodeParams& d, double target, double offset_v, double ra, double rr) noexcept {
    // At rest the pair's leakage, 2 Is (e^(u / nVt) - 1) with the junction
    // at u = -offset - d (1 + ra / rr), equals the release current d / rr,
    // d = target - rest. Increasing in d on the left, decreasing on the
    // right: one root, Newton from 0.
    const double nvt = d.n * d.vt;
    const double kk = 1.0 + ra / rr;
    double x = 0.0;
    for (int it = 0; it < 40; ++it) {
        const double y = (-offset_v - x * kk) / nvt;
        const double e = y < -kOffNvt ? 0.0 : bounded_exp(y);
        const double h = x / rr - 2.0 * d.is * (e - 1.0);
        const double dh = 1.0 / rr + 2.0 * d.is * e * kk / nvt;
        const double step = h / dh;
        x -= step;
        if (std::fabs(step) < 1e-15) break;
    }
    return target - x;
}

void FetChannel::update() noexcept {
    ratio_network(c_, ratio_, k_, offset_);
    const double ra = c_.r54 + r55_;
    const double rr = c_.r57 + r56_;
    gr_ = 1.0 / rr;
    gq_ = 1.0 / (c_.r6 + c_.r7);
    gz_ = 1.0 / c_.r54 + 2.0 * c_.c27 / dt_;
    // [M]'s "Q" bias: the channel's conductance at rest such that the
    // divider loses rest_loss_db; the ohmic channel is 2 beta (Vg - vto).
    const double loss = std::pow(10.0, c_.rest_loss_db / 20.0);
    rest_gain_ = 1.0 / loss;
    v_target_ = j_.vto + (loss - 1.0) / c_.r5 / (2.0 * j_.beta);
    v_rest_ = rest_for(d_, v_target_, offset_, ra, rr);
    vb_ = v_rest_ - offset_;
    sc_gain_ = c_.sc_gain * k_ * c_.preamp_gain;
    k9_ = 2.0 * c_.c9 / dt_;
    inv_kk9_ = 1.0 / (k9_ + gq_);
    beta_q_ = gq_ * inv_kk9_;
    k22_ = 2.0 * c_.c22 / dt_;
    k_c22_ = k22_ + gr_ + gq_ * (1.0 - beta_q_);
    q_ = 1.0 / k_c22_;
    k27_ = 2.0 * c_.c27 / dt_;
    inv_gz_ = 1.0 / gz_;
    inv_nvt_ = 1.0 / nvt_;
    g0_ = 1.0 / c_.r5;
    r_att_ = r55_ + inv_gz_;
}

double FetChannel::drain(double vin) const noexcept {
    // (vin - vd) / R5 = I(vd): the left side falls and the right rises
    // with vd, so the root is in [0, vin] and unique; Newton from the
    // ohmic solution without lambda (exact at small signal), bracketed.
    // Stops on the residual relative to R5's current: a step-based stop
    // let 2e-8 through at deep reduction, where the channel's slope is a
    // hundred times R5's.
    const double b = std::fabs(vin);
    if (!(b > 0.0)) return 0.0;
    const double vg0 = std::fmin(0.5 * (vc_ + vq_), c_.gate_max_v);
    const double v0 = vg0 - j_.vto;
    const double g0 = 1.0 / c_.r5;
    double a = v0 > 0.0 ? b / (1.0 + c_.r5 * 2.0 * j_.beta * v0) : b;
    const double tol = 1e-13 * b * g0;
    double lo = 0.0, hi = b;
    for (int it = 0; it < 40; ++it) {
        double dh;
        const double h = channel_current(j_, vg0, a, &dh);
        const double f = (b - a) * g0 - h;
        if (std::fabs(f) <= tol) break;
        if (f > 0.0) lo = a;
        else hi = a;
        double next = a + f / (g0 + dh);
        if (!(next > lo && next < hi)) next = 0.5 * (lo + hi);
        a = next;
    }
    return vin < 0.0 ? -a : a;
}

double FetChannel::pair_current(double u, double s_abs, double* du, double* ds) const noexcept {
    // The higher anode's junction at u, the other's at u - 2|s|.
    // Both junctions more than 12 nVt reverse (0.54 V): their forward terms
    // are under 7e-6 Is (2e-14 A, moving C22 by about 1e-7 V through the
    // release resistor), and the exps are skipped. In limiting the
    // rectifier conducts only near the peaks, so most samples take this
    // path; so do 12:1 and 20:1 at rest. rest_for() applies the same cut,
    // so the rest point is the step's own equilibrium.
    const double y = u * inv_nvt_;
    if (y < -kOffNvt) {
        *du = 0.0;
        *ds = 0.0;
        return -2.0 * d_.is;
    }
    const double e = d_.is * bounded_exp(y);
    // The lower junction, at u - 2|s|, under the same cut: its term matters
    // only near the drive's zero crossings, and its exp is skipped on the
    // rest (the cost of 8:1 at moderate reduction, where the higher
    // junction hovers near the cut and every sample is solved).
    const double x = 2.0 * s_abs * inv_nvt_;
    const double em = y - x < -kOffNvt ? 0.0 : std::exp(-x);
    *du = e * (1.0 + em) * inv_nvt_;
    *ds = -2.0 * e * em * inv_nvt_;
    return e * (1.0 + em) - 2.0 * d_.is;
}

void FetChannel::linear_step(Linear& l) const noexcept {
    // Trapezoidal on C9 and C22. C9: Vq' = alpha + beta_q Vc'. C22:
    // K Vc' = (2 C22 / dt) Vc + f22 + Gr Vrest + Gq alpha + I', so
    // Vc' = p + q I'. The attack network R54 || C27 in series with R55:
    // the rectifier's node is w = Vc' + R55 I' + vz', vz' = (I' + h) / gz
    // (trapezoidal on C27), so it adds h / gz to the emf and R55 + 1 / gz
    // to the resistance.
    l.alpha = (k9_ * vq_ + f9_) * inv_kk9_;
    l.beta_q = beta_q_;
    l.q = q_;
    l.k = k_c22_;
    l.r = k22_ * vc_ + f22_ + gr_ * v_rest_ + gq_ * l.alpha;
    l.p = q_ * l.r;
    l.hz = (k27_ * vz_ + i27_) * inv_gz_;
}

void FetChannel::commit(const Linear& l, double i_det, double vc_new, double link_i, double vd) noexcept {
    const double vz_new = i_det * inv_gz_ + l.hz;
    i27_ = k27_ * (vz_new - vz_) - i27_;
    vz_ = vz_new;
    const double vq_new = l.alpha + l.beta_q * vc_new;
    f9_ = gq_ * (vc_new - vq_new);
    f22_ = i_det - gr_ * (vc_new - v_rest_) - f9_ + link_i;
    vc_ = vc_new;
    vq_ = vq_new;
    i_det_ = i_det;
    vd_ = vd;
}

template <int N>
void FetChannel::solve(FetChannel* const* ch, const double* vin, double g_link) noexcept {
    // Unknowns: the drain magnitude a_c and the junction voltage u_c of each
    // channel. With the linear network eliminated, Vc' = P + Q I(u, s(a)):
    //   F1_c = (b_c - a_c) / R5 - I_ch(a_c; Vg0(Vc'_c))   (the divider)
    //   F2_c = u_c - E_c(a_c) + sum_j R_cj I_j            (the rectifier)
    // E_c = Vb + |s_c| - P_c - h_c / gz_c, R = Q + diag(R55 + 1 / gz).
    Linear lin[N];
    double P[N], Q[N][N], R[N][N];
    for (int c = 0; c < N; ++c) ch[c]->linear_step(lin[c]);
    if constexpr (N == 2) {
        // [M]: the stereo adapter joins the two control nodes. The C22
        // equations gain -g (Vc_a - Vc_b) and its mirror: Vc' = Q (r + I').
        // Q changes only with the link's conductance and the channels'
        // constants, so it is kept in the first channel between samples
        // (a division a sample saved).
        FetChannel& f0 = *ch[0];
        if (!(g_link == f0.link_g_ && lin[0].k == f0.link_ka_ && lin[N - 1].k == f0.link_kb_)) {
            const double ka = lin[0].k + g_link, kb = lin[N - 1].k + g_link;
            const double inv = 1.0 / (ka * kb - g_link * g_link);
            f0.link_q_[0] = kb * inv;
            f0.link_q_[1] = ka * inv;
            f0.link_q_[2] = g_link * inv;
            f0.link_g_ = g_link;
            f0.link_ka_ = lin[0].k;
            f0.link_kb_ = lin[N - 1].k;
        }
        Q[0][0] = f0.link_q_[0];
        Q[N - 1][N - 1] = f0.link_q_[1];
        Q[0][N - 1] = Q[N - 1][0] = f0.link_q_[2];
        const double ra = lin[0].r, rb = lin[N - 1].r;
        P[0] = Q[0][0] * ra + Q[0][N - 1] * rb;
        P[N - 1] = Q[N - 1][0] * ra + Q[N - 1][N - 1] * rb;
    } else {
        P[0] = lin[0].p;
        Q[0][0] = lin[0].q;
    }
    double b[N], sg[N], a[N], u[N], vg_before[N];
    for (int c = 0; c < N; ++c) {
        const FetChannel& f = *ch[c];
        for (int j = 0; j < N; ++j) R[c][j] = Q[c][j] + (c == j ? f.r_att_ : 0.0);
        b[c] = std::fabs(vin[c]);
        // NaN is no input: a sanitising caller never sends one, and here it
        // must not reach a state.
        if (!(b[c] < 1e300)) b[c] = 0.0;
        sg[c] = vin[c] < 0.0 ? -1.0 : 1.0;
        // Starts. The divider: the ohmic channel with lambda, solved in
        // closed form, g a (1 + lambda a) = (b - a) / R5, at the gate
        // extrapolated a sample from its last two values (it moves with
        // the rectifier's ripple). The junction: moved with the drive by
        // du/ds = 1 / (1 + R dI/du), the rectifier's own sensitivity at the
        // last solution (exact while the diodes are off, where the junction
        // follows the drive). Without these most samples took a second
        // iteration (92 % on programme at 8:1, with the drain's start
        // missing lambda's 2e-5).
        const double vg_now = 0.5 * (f.vc_ + f.vq_);
        vg_before[c] = vg_now;
        const double v0 = std::fmin(2.0 * vg_now - f.vg_last_, f.c_.gate_max_v) - f.j_.vto;
        a[c] = v0 > 0.0 ? ohmic_drain(b[c], f.g0_, 2.0 * f.j_.beta * v0, f.j_.lambda) : b[c];
        // Bounded at SPICE's critical junction voltage: from a junction far
        // forward Newton walks down one nVt an iteration (a jump of the
        // drive predicted 12 V and ran into the 60-iteration cap).
        const double sp = std::fmin(f.sc_gain_ * a[c], f.c_.sc_swing_v);
        u[c] = std::fmin(f.u_ + (sp - f.s_last_) / (1.0 + R[c][c] * f.iu_last_), std::fmax(f.u_, f.vcrit_));
    }
    // The rectifier off. In limiting it conducts only near the peaks; the
    // rest of the time both junctions sit more than kOffNvt reverse, where
    // pair_current() gives the constant -2 Is. Then nothing is implicit:
    // the capacitors follow from that current, the gate from them, and the
    // drain from the ohmic channel in closed form. The result is the root
    // the Newton path would find, so the path is taken whenever its own
    // answer confirms it (every junction off, every channel ohmic), and
    // most samples need no iteration and no exp. Tried only when the starts
    // already put every junction off, so a sample that conducts pays
    // nothing for it.
    bool try_off = true;
    for (int c = 0; c < N; ++c) try_off &= u[c] * ch[c]->inv_nvt_ < -kOffNvt;
    if (try_off) {
        double I_off[N], vc_off[N], a_off[N], u_off[N], s_off[N];
        bool off = true;
        for (int c = 0; c < N; ++c) I_off[c] = -2.0 * ch[c]->d_.is;
        for (int c = 0; c < N && off; ++c) {
            const FetChannel& f = *ch[c];
            double vc = P[c], rsum = 0.0;
            for (int j = 0; j < N; ++j) {
                vc += Q[c][j] * I_off[j];
                rsum += R[c][j] * I_off[j];
            }
            vc_off[c] = vc;
            const double v0 = std::fmin(0.5 * ((1.0 + lin[c].beta_q) * vc + lin[c].alpha), f.c_.gate_max_v) - f.j_.vto;
            if (!(v0 > 0.0)) {
                off = false;
                break;
            }
            const double ad = ohmic_drain(b[c], f.g0_, 2.0 * f.j_.beta * v0, f.j_.lambda);
            // Ohmic while vgs - vto exceeds vds: a < 2 v0.
            if (!(ad < 2.0 * v0)) {
                off = false;
                break;
            }
            const double s = std::fmin(f.sc_gain_ * ad, f.c_.sc_swing_v);
            const double uo = f.vb_ + s - P[c] - lin[c].hz - rsum;
            off = uo * f.inv_nvt_ < -kOffNvt;
            a_off[c] = ad;
            u_off[c] = uo;
            s_off[c] = s;
        }
        if (off) {
            for (int c = 0; c < N; ++c) {
                const double link = N == 2 ? -g_link * (vc_off[c] - vc_off[N - 1 - c]) : 0.0;
                ch[c]->u_ = u_off[c];
                ch[c]->vg_last_ = vg_before[c];
                ch[c]->s_last_ = s_off[c];
                ch[c]->iu_last_ = 0.0;
                ch[c]->iterations_ = 0;
                ch[c]->commit(lin[c], I_off[c], vc_off[c], link, sg[c] * a_off[c]);
            }
            return;
        }
    }
    double I[N], Iu[N], Is[N], dS[N];
    int it = 0;
    for (; it < 60; ++it) {
        for (int c = 0; c < N; ++c) {
            const FetChannel& f = *ch[c];
            const double sv = f.sc_gain_ * a[c];
            const bool free = sv < f.c_.sc_swing_v;
            dS[c] = free ? f.sc_gain_ : 0.0;
            I[c] = f.pair_current(u[c], free ? sv : f.c_.sc_swing_v, &Iu[c], &Is[c]);
        }
        // The Newton step by a Schur complement on the junctions. The
        // Jacobian keeps every term but the slope of one channel's
        // rectifier in the other's drive (the drives only meet through the
        // joined C22), so the divider rows are diagonal in the drains:
        //   A da + B du = F1,  C da + D du = F2,  A and C diagonal,
        //   (D - C A^-1 B) du = F2 - C A^-1 F1.
        // The residuals are exact, so the root is; only the path differs.
        double A[N], B[N][N], C[N], D[N][N], F1[N], F2[N], da[N], du[N];
        for (int c = 0; c < N; ++c) {
            const FetChannel& f = *ch[c];
            double vc = P[c], rsum = 0.0;
            for (int j = 0; j < N; ++j) {
                vc += Q[c][j] * I[j];
                rsum += R[c][j] * I[j];
            }
            const double vraw = 0.5 * ((1.0 + lin[c].beta_q) * vc + lin[c].alpha);
            const bool clamped = vraw > f.c_.gate_max_v;
            const double cg = clamped ? 0.0 : 0.5 * (1.0 + lin[c].beta_q);
            double ha, hv;
            const double h = channel_current(f.j_, clamped ? f.c_.gate_max_v : vraw, a[c], &ha, &hv);
            const double s = dS[c] > 0.0 ? f.sc_gain_ * a[c] : f.c_.sc_swing_v;
            F1[c] = h - (b[c] - a[c]) * f.g0_;
            F2[c] = (f.vb_ + s - P[c] - lin[c].hz) - u[c] - rsum;
            A[c] = -hv * cg * Q[c][c] * Is[c] * dS[c] - (f.g0_ + ha);
            C[c] = R[c][c] * Is[c] * dS[c] - dS[c];
            for (int j = 0; j < N; ++j) {
                B[c][j] = -hv * cg * Q[c][j] * Iu[j];
                D[c][j] = R[c][j] * Iu[j] + (c == j ? 1.0 : 0.0);
            }
        }
        // The rows scaled by A (S' = A S, G' = A G), so the step takes one
        // division, of det(S') times the product of the A's, from which
        // 1 / A_c follows too: three divisions became one, on the chain
        // every sample waits for.
        double S[N][N], G[N], inv_a[N];
        for (int c = 0; c < N; ++c) {
            for (int j = 0; j < N; ++j) S[c][j] = A[c] * D[c][j] - C[c] * B[c][j];
            G[c] = A[c] * F2[c] - C[c] * F1[c];
        }
        if constexpr (N == 1) {
            const double inv = 1.0 / (S[0][0] * A[0]);
            du[0] = G[0] * A[0] * inv;
            inv_a[0] = S[0][0] * inv;
        } else {
            const double det = S[0][0] * S[N - 1][N - 1] - S[0][N - 1] * S[N - 1][0];
            const double pa = A[0] * A[N - 1];
            const double inv = 1.0 / (det * pa);
            du[0] = (G[0] * S[N - 1][N - 1] - S[0][N - 1] * G[N - 1]) * pa * inv;
            du[N - 1] = (S[0][0] * G[N - 1] - S[N - 1][0] * G[0]) * pa * inv;
            inv_a[0] = det * A[N - 1] * inv;
            inv_a[N - 1] = det * A[0] * inv;
        }
        for (int c = 0; c < N; ++c) {
            double bu = 0.0;
            for (int j = 0; j < N; ++j) bu += B[c][j] * du[j];
            da[c] = (F1[c] - bu) * inv_a[c];
        }
        double dx[2 * N];
        for (int c = 0; c < N; ++c) {
            dx[c] = da[c];
            dx[N + c] = du[c];
        }
        // Newton converges quadratically here, so a small enough correction
        // is applied and the loop stops without a confirming evaluation:
        // under 1e-4 V on the junction it leaves about 1e-7 V (the
        // junction's curvature is 1 / nVt; 2e-6 of the rectifier's
        // current), and under 1e-5 of the drain it leaves under 3e-8 of it
        // at the saturated channel's steepest. On programme at 8:1 this
        // took the mean from 1.94 iterations a sample (1e-6 on both) to
        // 1.07.
        bool done = true;
        for (int c = 0; c < N; ++c) {
            const FetChannel& f = *ch[c];
            const double an = clamp(a[c] + dx[c], 0.0, b[c]);
            const double un = limit_step(u[c] + dx[N + c], u[c], f.nvt_, f.vcrit_);
            done &= std::fabs(un - u[c]) < 1e-4 && un == u[c] + dx[N + c] && std::fabs(an - a[c]) <= 1e-5 * b[c] &&
                    an == a[c] + dx[c];
            // The current at the corrected point, to first order (the
            // correction's square is below the tolerance).
            I[c] += Iu[c] * (un - u[c]) + Is[c] * dS[c] * (an - a[c]);
            a[c] = an;
            u[c] = un;
        }
        if (done) break;
        if (it == 59) {
            // Not converged: the current at the last point, exactly.
            for (int c = 0; c < N; ++c) {
                const FetChannel& f = *ch[c];
                const double sv = f.sc_gain_ * a[c];
                I[c] = f.pair_current(u[c], sv < f.c_.sc_swing_v ? sv : f.c_.sc_swing_v, &Iu[c], &Is[c]);
            }
        }
    }
    double vcn[N];
    for (int c = 0; c < N; ++c) {
        vcn[c] = P[c];
        for (int j = 0; j < N; ++j) vcn[c] += Q[c][j] * I[j];
    }
    for (int c = 0; c < N; ++c) {
        const double link = N == 2 ? -g_link * (vcn[c] - vcn[N - 1 - c]) : 0.0;
        ch[c]->u_ = u[c];
        ch[c]->vg_last_ = vg_before[c];
        ch[c]->s_last_ = std::fmin(ch[c]->sc_gain_ * a[c], ch[c]->c_.sc_swing_v);
        ch[c]->iu_last_ = Iu[c];
        // The loop ran out (it == 60) only if no correction met the stop:
        // the step still commits its last point, and the miss is counted
        // for the tests to hold at zero.
        ch[c]->iterations_ = it < 60 ? it + 1 : 60;
        if (it >= 60) ++ch[c]->cap_hits_;
        ch[c]->commit(lin[c], I[c], vcn[c], link, sg[c] * a[c]);
    }
}

void FetChannel::step(double loop_vin) noexcept {
    FetChannel* self[1] = {this};
    solve<1>(self, &loop_vin, 0.0);
}

void FetChannel::step_pair(FetChannel& a, FetChannel& b, double vin_a, double vin_b, double g_link) noexcept {
    // Unlinked (g 0) the coupling terms vanish and the two are independent;
    // solved in one loop all the same, so the two channels' dependency
    // chains overlap.
    FetChannel* both[2] = {&a, &b};
    const double vin[2] = {vin_a, vin_b};
    solve<2>(both, vin, g_link > 0.0 ? g_link : 0.0);
}

double FetChannel::gain_reduction_db() const noexcept {
    const double vg0 = std::fmin(0.5 * (vc_ + vq_), c_.gate_max_v);
    const double g = 2.0 * j_.beta * std::fmax(vg0 - j_.vto, 0.0);
    return 20.0 * std::log10(rest_gain_ * (1.0 + c_.r5 * g));
}

}  // namespace sonora::dsp
