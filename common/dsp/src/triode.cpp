// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/triode.h"

#include <cmath>
#include <complex>

#include "sonora/dsp/math.h"
#include "triode_solve.h"

namespace sonora::dsp {

namespace {

constexpr std::uint32_t N = TriodeStage::kNodes;
constexpr std::uint32_t M = TriodeStage::kCaps;
constexpr int kGnd = -1;
constexpr int kVin = -2;     // the input source's node, a known voltage
constexpr int kSupply = -3;  // B+, a known voltage

// log(1 + exp(x)) and its derivative, without overflow for large x.
inline double softplus(double x) noexcept {
    return x > 0.0 ? x + std::log1p(std::exp(-x)) : std::log1p(std::exp(x));
}
inline double logistic(double x) noexcept {
    if (x >= 0.0) return 1.0 / (1.0 + std::exp(-x));
    const double e = std::exp(x);
    return e / (1.0 + e);
}

// Dempwolf's two laws as functions of one variable each: the cathode
// current of u = Vpk / mu + Vgk, and the grid current of Vgk.
inline void dempwolf_ik(const TriodeDevice& d, double u, double& y, double& dy) noexcept {
    const double x = d.c * u;
    const double s = softplus(x) / d.c;
    if (!(s > 0.0)) {
        y = dy = 0.0;
        return;
    }
    const double p = std::pow(s, d.gamma - 1.0);
    y = d.g * p * s;
    dy = d.g * d.gamma * p * logistic(x);
}
inline void dempwolf_ig(const TriodeDevice& d, double v, double& y, double& dy) noexcept {
    const double x = d.cg * v;
    const double s = softplus(x) / d.cg;
    if (!(s > 0.0)) {
        y = d.ig0;
        dy = 0.0;
        return;
    }
    const double p = std::pow(s, d.xi - 1.0);
    y = d.gg * p * s + d.ig0;
    dy = d.gg * d.xi * p * logistic(x);
}

struct Two {
    int a, b;  // terminals: a node index, or kGnd / kVin / kSupply
};
// The capacitors, in the order of TriodeStage::j_.
constexpr Two kCapNodes[M] = {
    {kVin, TriodeStage::kA},              // Cin
    {TriodeStage::kK, kGnd},              // Ck
    {TriodeStage::kP, TriodeStage::kO},   // Cout
    {TriodeStage::kG, TriodeStage::kP},   // Cgp
};

void cap_values(const TriodeStageCircuit& c, const TriodeDevice& d, double out[M]) {
    out[0] = c.c_in;
    out[1] = c.c_cathode;
    out[2] = c.c_out;
    out[3] = d.c_gp;
}

// The resistors' conductance stamps, and what B+ injects.
void stamp_resistors(const TriodeStageCircuit& c, double g[N][N], double rhs[N]) {
    const auto stamp = [&](int a, int b, double r) {
        const double y = 1.0 / r;
        if (a >= 0) g[a][a] += y;
        if (b >= 0) g[b][b] += y;
        if (a >= 0 && b >= 0) {
            g[a][b] -= y;
            g[b][a] -= y;
        }
        if (a >= 0 && b == kSupply) rhs[a] += y * c.supply_v;
    };
    stamp(TriodeStage::kA, kGnd, c.r_leak);
    stamp(TriodeStage::kA, TriodeStage::kG, c.r_grid);
    stamp(TriodeStage::kK, kGnd, c.r_cathode);
    stamp(TriodeStage::kP, kSupply, c.r_plate);
    stamp(TriodeStage::kO, kGnd, c.r_load);
}

// In place, Gauss-Jordan with partial pivoting. Returns false if singular.
template <class T>
bool invert(T a[N][N]) {
    T inv[N][N] = {};
    for (std::uint32_t i = 0; i < N; ++i) inv[i][i] = T(1);
    for (std::uint32_t col = 0; col < N; ++col) {
        std::uint32_t piv = col;
        for (std::uint32_t r = col + 1; r < N; ++r) {
            if (std::abs(a[r][col]) > std::abs(a[piv][col])) piv = r;
        }
        if (std::abs(a[piv][col]) == 0.0) return false;
        for (std::uint32_t k = 0; k < N; ++k) {
            std::swap(a[col][k], a[piv][k]);
            std::swap(inv[col][k], inv[piv][k]);
        }
        const T d = a[col][col];
        for (std::uint32_t k = 0; k < N; ++k) {
            a[col][k] /= d;
            inv[col][k] /= d;
        }
        for (std::uint32_t r = 0; r < N; ++r) {
            if (r == col) continue;
            const T f = a[r][col];
            if (f == T(0)) continue;
            for (std::uint32_t k = 0; k < N; ++k) {
                a[r][k] -= f * a[col][k];
                inv[r][k] -= f * inv[col][k];
            }
        }
    }
    for (std::uint32_t i = 0; i < N; ++i)
        for (std::uint32_t k = 0; k < N; ++k) a[i][k] = inv[i][k];
    return true;
}

// The port voltages Vgk and Vpk from the node voltages (rows of Nv).
constexpr double kNv[2][N] = {
    {0.0, 1.0, -1.0, 0.0, 0.0},  // Vgk
    {0.0, 0.0, -1.0, 1.0, 0.0},  // Vpk
};
// Where the currents (ip, ig) flow: ip from the plate, ig from the grid,
// each into the cathode. The devices inject -kPort^T i.
constexpr double kPort[2][N] = {
    {0.0, 0.0, -1.0, 1.0, 0.0},  // ip: plate to cathode
    {0.0, 1.0, -1.0, 0.0, 0.0},  // ig: grid to cathode
};

}  // namespace

TriodeCurrents triode_currents(TriodeLaw law, const TriodeDevice& d, double vgk, double vpk) noexcept {
    TriodeCurrents r;
    if (law == TriodeLaw::Dempwolf) {
        double ik, dik, ig, dig;
        dempwolf_ik(d, vpk / d.mu + vgk, ik, dik);
        dempwolf_ig(d, vgk, ig, dig);
        r.ip = ik - ig;
        r.ig = ig;
        r.dip_dvgk = dik - dig;
        r.dip_dvpk = dik / d.mu;
        r.dig_dvgk = dig;
        return r;
    }
    // Koren: E1 and Ip as published; (1 + sgn(E1)) is 2 where E1 > 0.
    const double s = std::sqrt(d.k_kvb + vpk * vpk);
    const double x = d.k_kp * (1.0 / d.k_mu + vgk / s);
    const double sp = softplus(x);
    const double e1 = vpk / d.k_kp * sp;
    if (e1 > 0.0) {
        const double sig = logistic(x);
        const double de_dvgk = vpk * sig / s;
        const double de_dvpk = sp / d.k_kp - vpk * sig * vgk * vpk / (s * s * s);
        const double pw = std::pow(e1, d.k_ex - 1.0);
        r.ip = 2.0 * pw * e1 / d.k_kg1;
        const double dip_de = 2.0 * d.k_ex * pw / d.k_kg1;
        r.dip_dvgk = dip_de * de_dvgk;
        r.dip_dvpk = dip_de * de_dvpk;
    }
    if (vgk > 0.0) {
        r.ig = vgk / d.k_rgi;
        r.dig_dvgk = 1.0 / d.k_rgi;
    }
    return r;
}

TriodeCurrents TriodeStage::currents(double vgk, double vpk) const noexcept { return law(vgk, vpk); }

void TriodeStage::build_dc_point_matrices(double s_dc[N][N]) const {
    double rhs[N] = {};
    for (std::uint32_t i = 0; i < N; ++i)
        for (std::uint32_t k = 0; k < N; ++k) s_dc[i][k] = 0.0;
    stamp_resistors(ckt_, s_dc, rhs);
    invert(s_dc);
}

bool TriodeStage::prepare(double sample_rate, TriodeLaw law, const TriodeStageCircuit& circuit,
                          const TriodeDevice& device, bool tabulated) {
    law_ = law;
    ckt_ = circuit;
    dev_ = device;
    fs_ = sample_rate;
    inv_mu_ = 1.0 / dev_.mu;
    // Two stages share matrices in tick2() only when everything they are
    // built from is equal: a hash over those values (FNV-1a over their
    // bytes; prepare() is deterministic, so equal inputs give equal
    // matrices).
    {
        std::uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](const void* p, std::size_t n) {
            const auto* c = static_cast<const unsigned char*>(p);
            for (std::size_t i = 0; i < n; ++i) h = (h ^ c[i]) * 1099511628211ull;
        };
        const double values[] = {sample_rate, circuit.supply_v, circuit.r_plate, circuit.r_cathode,
                                 circuit.c_cathode, circuit.c_in, circuit.r_leak, circuit.r_grid,
                                 circuit.c_out, circuit.r_load, device.g, device.mu, device.gamma, device.c,
                                 device.gg, device.xi, device.cg, device.ig0, device.k_mu, device.k_ex,
                                 device.k_kg1, device.k_kp, device.k_kvb, device.k_rgi, device.c_gp};
        mix(values, sizeof values);
        const int flags[] = {static_cast<int>(law), tabulated ? 1 : 0};
        mix(flags, sizeof flags);
        model_key_ = h;
    }
    tabulated_ = tabulated && law == TriodeLaw::Dempwolf;
    if (tabulated_) {
        // 1/128 V: the error of a cubic Hermite interpolant is h^4 / 384
        // times the fourth derivative, about 1e-14 A here (test_triode
        // measures it against the exact law).
        const auto build = [&](Table& t, double lo, double hi, bool grid) {
            const std::uint32_t per_volt = 128;
            const auto n = static_cast<std::uint32_t>((hi - lo) * per_volt) + 1;
            const double h = 1.0 / per_volt;
            t.lo = lo;
            t.inv_h = per_volt;
            t.hi = lo + (n - 1) * h;
            t.last = static_cast<int>(n) - 2;
            std::vector<double> f(n), df(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                const double x = lo + i * h;
                if (grid) dempwolf_ig(dev_, x, f[i], df[i]);
                else dempwolf_ik(dev_, x, f[i], df[i]);
            }
            // Hermite on [0, 1] with end slopes scaled by h.
            t.c.assign(static_cast<std::size_t>(n - 1) * 4, 0.0);
            for (std::uint32_t i = 0; i + 1 < n; ++i) {
                const double f0 = f[i], f1 = f[i + 1], d0 = df[i] * h, d1 = df[i + 1] * h;
                double* k = &t.c[static_cast<std::size_t>(i) * 4];
                k[0] = f0;
                k[1] = d0;
                k[2] = 3.0 * (f1 - f0) - 2.0 * d0 - d1;
                k[3] = 2.0 * (f0 - f1) + d0 + d1;
            }
        };
        // Each table spans |C x| <= 36 and a little more, the range where
        // the softplus is neither of its asymptotes; tail_ik and tail_ig
        // are exact beyond it.
        // The grid table also reaches 12 V: with the input bounded to
        // kMaxInputV the grid conducts up to about 5 V (and overshoots on
        // a hot edge), where the tail would be a pow per pass.
        const double ku = std::ceil(36.0 / dev_.c) + 1.0;
        const double kg = std::ceil(36.0 / dev_.cg) + 1.0;
        build(ik_, -ku, ku, false);
        build(ig_, -kg, kg > 12.0 ? kg : 12.0, true);
    }

    // ---- the operating point: capacitors open, input at 0 V ----
    double sdc[N][N];
    build_dc_point_matrices(sdc);
    double rhs[N] = {};
    {
        double gtmp[N][N] = {};
        stamp_resistors(ckt_, gtmp, rhs);
    }
    // v = pdc + Kdc i
    double sni[N][2];
    for (std::uint32_t r = 0; r < N; ++r) {
        for (std::uint32_t c = 0; c < 2; ++c) {
            double acc = 0.0;
            for (std::uint32_t k = 0; k < N; ++k) acc -= sdc[r][k] * kPort[c][k];
            sni[r][c] = acc;
        }
    }
    double vdc_lin[N];
    for (std::uint32_t r = 0; r < N; ++r) {
        double acc = 0.0;
        for (std::uint32_t k = 0; k < N; ++k) acc += sdc[r][k] * rhs[k];
        vdc_lin[r] = acc;
    }
    double pdc[2], kdc[2][2];
    for (std::uint32_t a = 0; a < 2; ++a) {
        double acc = 0.0;
        for (std::uint32_t k = 0; k < N; ++k) acc += kNv[a][k] * vdc_lin[k];
        pdc[a] = acc;
        for (std::uint32_t c = 0; c < 2; ++c) {
            double s = 0.0;
            for (std::uint32_t k = 0; k < N; ++k) s += kNv[a][k] * sni[k][c];
            kdc[a][c] = s;
        }
    }
    // Damped Newton from a cut-off guess; the curves are monotone, so it
    // converges, and the bound keeps the first steps from overshooting.
    double v[2] = {-1.0, 0.5 * ckt_.supply_v};
    bool converged = false;
    for (int it = 0; it < 500; ++it) {
        const TriodeCurrents c = currents(v[0], v[1]);
        const double i[2] = {c.ip, c.ig};
        const double ji[2][2] = {{c.dip_dvgk, c.dip_dvpk}, {c.dig_dvgk, 0.0}};
        double f[2], jf[2][2];
        for (std::uint32_t a = 0; a < 2; ++a) {
            f[a] = pdc[a] + kdc[a][0] * i[0] + kdc[a][1] * i[1] - v[a];
            for (std::uint32_t b = 0; b < 2; ++b)
                jf[a][b] = kdc[a][0] * ji[0][b] + kdc[a][1] * ji[1][b] - (a == b ? 1.0 : 0.0);
        }
        const double det = jf[0][0] * jf[1][1] - jf[0][1] * jf[1][0];
        if (!(std::fabs(det) > 0.0)) break;
        double d0 = (-f[0] * jf[1][1] + f[1] * jf[0][1]) / det;
        double d1 = (-f[1] * jf[0][0] + f[0] * jf[1][0]) / det;
        const double big = std::fmax(std::fabs(d0), std::fabs(d1));
        if (big > 10.0) {
            d0 *= 10.0 / big;
            d1 *= 10.0 / big;
        }
        v[0] += d0;
        v[1] += d1;
        if (std::fabs(d0) < 1e-13 && std::fabs(d1) < 1e-11) {
            converged = true;
            break;
        }
    }
    if (!converged || !std::isfinite(v[0]) || !std::isfinite(v[1])) return false;
    const TriodeCurrents c0 = currents(v[0], v[1]);
    op_i_[0] = c0.ip;
    op_i_[1] = c0.ig;
    op_port_[0] = v[0];
    op_port_[1] = v[1];
    op_ji_[0] = c0.dip_dvgk;
    op_ji_[1] = c0.dip_dvpk;
    op_ji_[2] = c0.dig_dvgk;
    for (std::uint32_t r = 0; r < N; ++r) op_v_[r] = vdc_lin[r] + sni[r][0] * op_i_[0] + sni[r][1] * op_i_[1];

    // ---- the discrete-time matrices ----
    const double t = 1.0 / sample_rate;
    double cv[M];
    cap_values(ckt_, dev_, cv);
    double gc[M];
    for (std::uint32_t m = 0; m < M; ++m) gc[m] = 2.0 * cv[m] / t;  // trapezoidal companion

    double g[N][N] = {};
    double c_rhs[N] = {};
    stamp_resistors(ckt_, g, c_rhs);
    double bj[N][M] = {}, bu[N] = {};
    double nc[M][N] = {}, ncu[M] = {};
    for (std::uint32_t m = 0; m < M; ++m) {
        const int a = kCapNodes[m].a, b = kCapNodes[m].b;
        if (a >= 0) g[a][a] += gc[m];
        if (b >= 0) g[b][b] += gc[m];
        if (a >= 0 && b >= 0) {
            g[a][b] -= gc[m];
            g[b][a] -= gc[m];
        }
        // The history source J: KCL at a reads gc (va - vb) + ... = J.
        if (a >= 0) bj[a][m] += 1.0;
        if (b >= 0) bj[b][m] -= 1.0;
        if (a == kVin && b >= 0) bu[b] += gc[m];
        // The capacitor's voltage, va - vb.
        if (a >= 0) nc[m][a] += 1.0;
        if (b >= 0) nc[m][b] -= 1.0;
        if (a == kVin) ncu[m] += 1.0;
    }
    if (!invert(g)) return false;
    // S times each right-hand side.
    double sbj[N][M], sbu[N], sc[N], sni2[N][2];
    for (std::uint32_t r = 0; r < N; ++r) {
        for (std::uint32_t m = 0; m < M; ++m) {
            double acc = 0.0;
            for (std::uint32_t k = 0; k < N; ++k) acc += g[r][k] * bj[k][m];
            sbj[r][m] = acc;
        }
        double au = 0.0, ac = 0.0;
        for (std::uint32_t k = 0; k < N; ++k) {
            au += g[r][k] * bu[k];
            ac += g[r][k] * c_rhs[k];
        }
        sbu[r] = au;
        sc[r] = ac;
        for (std::uint32_t c = 0; c < 2; ++c) {
            double acc = 0.0;
            for (std::uint32_t k = 0; k < N; ++k) acc -= g[r][k] * kPort[c][k];
            sni2[r][c] = acc;
        }
    }
    const auto rows = [&](const double (*sel)[N], std::uint32_t nrows, double* out_j, double* out_u, double* out_c,
                          double* out_i) {
        // out_j: nrows x M, out_u: nrows, out_c: nrows, out_i: nrows x 2
        for (std::uint32_t a = 0; a < nrows; ++a) {
            for (std::uint32_t m = 0; m < M; ++m) {
                double acc = 0.0;
                for (std::uint32_t k = 0; k < N; ++k) acc += sel[a][k] * sbj[k][m];
                out_j[a * M + m] = acc;
            }
            double u = 0.0, cc = 0.0;
            for (std::uint32_t k = 0; k < N; ++k) {
                u += sel[a][k] * sbu[k];
                cc += sel[a][k] * sc[k];
            }
            out_u[a] = u;
            out_c[a] = cc;
            for (std::uint32_t c = 0; c < 2; ++c) {
                double acc = 0.0;
                for (std::uint32_t k = 0; k < N; ++k) acc += sel[a][k] * sni2[k][c];
                out_i[a * 2 + c] = acc;
            }
        }
    };
    double pj[2][M], pu[2], pc[2];
    rows(kNv, 2, &pj[0][0], pu, pc, &k_[0][0]);
    double qj[M][M], qu[M], qc[M], qi[M][2];
    rows(nc, M, &qj[0][0], qu, qc, &qi[0][0]);
    double aj[M][M], au[M], ac[M], ai[M][2];
    for (std::uint32_t m = 0; m < M; ++m) {
        const double two = 2.0 * gc[m];
        for (std::uint32_t n = 0; n < M; ++n) aj[m][n] = two * qj[m][n] - (m == n ? 1.0 : 0.0);
        au[m] = two * (qu[m] + ncu[m]);
        ac[m] = two * qc[m];
        ai[m][0] = two * qi[m][0];
        ai[m][1] = two * qi[m][1];
    }
    double sel_o[1][N] = {{0.0, 0.0, 0.0, 0.0, 1.0}};
    double oj[M], ou, oc, oi[2];
    rows(sel_o, 1, oj, &ou, &oc, oi);
    for (std::uint32_t a = 0; a < 2; ++a)
        for (std::uint32_t b = 0; b < 2; ++b) dup_[kDupK + 2 * a + b][0] = dup_[kDupK + 2 * a + b][1] = k_[a][b];

    // Packed for tick(): rows p0, p1, out, J'0..5 (and a zero pad).
    for (std::uint32_t col = 0; col < kCols; ++col) {
        for (std::uint32_t r = 0; r < kRows; ++r) w_[col][r] = 0.0;
    }
    for (std::uint32_t col = 0; col < kCols; ++col) {
        const auto pick = [&](const double* j_part, double u_part, double c_part) {
            return col < M ? j_part[col] : (col == M ? u_part : c_part);
        };
        wp_[0][col] = pick(pj[0], pu[0], pc[0]);
        wp_[1][col] = pick(pj[1], pu[1], pc[1]);
        w_[col][0] = pick(oj, ou, oc);
        for (std::uint32_t m = 0; m < M; ++m) w_[col][1 + m] = pick(aj[m], au[m], ac[m]);
    }
    for (std::uint32_t r = 0; r < kRows; ++r) post_[0][r] = post_[1][r] = 0.0;
    const auto put = [&](std::uint32_t i, double v) { dup_[i][0] = dup_[i][1] = v; };
    for (std::uint32_t c = 0; c < 2; ++c) {
        post_[c][0] = oi[c];
        for (std::uint32_t m = 0; m < M; ++m) post_[c][1 + m] = ai[m][c];
    }
    for (std::uint32_t col = 0; col < kCols; ++col) {
        put(kDupP + col, wp_[0][col]);
        put(kDupP + kCols + col, wp_[1][col]);
        for (std::uint32_t r = 0; r < 1 + M; ++r) put(kDupW + r * kCols + col, w_[col][r]);
    }
    for (std::uint32_t r = 0; r < 1 + M; ++r) {
        put(kDupPost + 2 * r, post_[0][r]);
        put(kDupPost + 2 * r + 1, post_[1][r]);
    }

    // The predictor's inverse at rest, (I - K Ji)^-1.
    {
        const double a00 = 1.0 - (k_[0][0] * op_ji_[0] + k_[0][1] * op_ji_[2]), a01 = -k_[0][0] * op_ji_[1];
        const double a10 = -(k_[1][0] * op_ji_[0] + k_[1][1] * op_ji_[2]), a11 = 1.0 - k_[1][0] * op_ji_[1];
        const double inv = 1.0 / (a00 * a11 - a01 * a10);
        op_inv_[0] = a11 * inv;
        op_inv_[1] = -a01 * inv;
        op_inv_[2] = -a10 * inv;
        op_inv_[3] = a00 * inv;
    }
    // The predictor at the operating point: v = op_inv (p + K (i - J v)).
    {
        const double e0 = op_i_[0] - (op_ji_[0] * op_port_[0] + op_ji_[1] * op_port_[1]);
        const double e1 = op_i_[1] - op_ji_[2] * op_port_[0];
        const double vals[7] = {k_[0][0] * e0 + k_[0][1] * e1, k_[1][0] * e0 + k_[1][1] * e1, op_inv_[0], op_inv_[1],
                                op_inv_[2], op_inv_[3], op_port_[0]};
        for (std::uint32_t i = 0; i < 7; ++i) dup_[kDupOp + i][0] = dup_[kDupOp + i][1] = vals[i];
    }
    // ... and at a point in grid current: Vgk +0.6 V, Vpk 50 V, where the
    // full-drive traces' solutions in grid current lie (0.4 to 0.9 V,
    // 34 to 58 V).
    {
        const double vg = 0.6, vp = 50.0;
        const TriodeCurrents g = currents(vg, vp);
        const double a00 = 1.0 - (k_[0][0] * g.dip_dvgk + k_[0][1] * g.dig_dvgk), a01 = -k_[0][0] * g.dip_dvpk;
        const double a10 = -(k_[1][0] * g.dip_dvgk + k_[1][1] * g.dig_dvgk), a11 = 1.0 - k_[1][0] * g.dip_dvpk;
        const double inv = 1.0 / (a00 * a11 - a01 * a10);
        const double e0 = g.ip - (g.dip_dvgk * vg + g.dip_dvpk * vp);
        const double e1 = g.ig - g.dig_dvgk * vg;
        const double vals[6] = {k_[0][0] * e0 + k_[0][1] * e1, k_[1][0] * e0 + k_[1][1] * e1, a11 * inv, -a01 * inv,
                                -a10 * inv, a00 * inv};
        for (std::uint32_t i = 0; i < 6; ++i) dup_[kDupGrid + i][0] = dup_[kDupGrid + i][1] = vals[i];
    }
    // The history sources at rest: no capacitor current, J = gc v.
    for (std::uint32_t m = 0; m < M; ++m) {
        const int a = kCapNodes[m].a, b = kCapNodes[m].b;
        const double va = a >= 0 ? op_v_[a] : 0.0;  // vin = 0, ground = 0
        const double vb = b >= 0 ? op_v_[b] : 0.0;
        op_j_[m] = gc[m] * (va - vb);
    }
    reset();
    // What the stage puts out at rest: one solve of silence from the
    // operating point, after which reset() puts the state back.
    at_rest_ = false;
    rest_out_ = tick(0.0);
    reset();
    return true;
}

void TriodeStage::reset() noexcept {
    restore_operating_point();
    last_iter_ = max_iter_ = 0;
}

void TriodeStage::restore_operating_point() noexcept {
    at_rest_ = true;
    for (std::uint32_t m = 0; m < M; ++m) j_[m] = op_j_[m];
    i_[0] = op_i_[0];
    i_[1] = op_i_[1];
    v_[0] = op_port_[0];
    v_[1] = op_port_[1];
    ji_[0] = op_ji_[0];
    ji_[1] = op_ji_[1];
    ji_[2] = op_ji_[2];
    for (int k = 0; k < 4; ++k) inv_[k] = op_inv_[k];
}

namespace {

// One lane: tick()'s V for TriodeStage::solve (triode_solve.h). The
// two-lane V is in src/arch/<arch>/triode_pair.cpp; both do the same
// operations, so a stage gives the same bits alone or paired.
struct V1 {
    static constexpr int kLanes = 1;
    struct Mask {
        bool b;
        Mask operator&(Mask o) const noexcept { return {b && o.b}; }
        Mask operator|(Mask o) const noexcept { return {b || o.b}; }
        Mask operator~() const noexcept { return {!b}; }
        bool lane(int) const noexcept { return b; }
        bool all() const noexcept { return b; }
        bool any() const noexcept { return b; }
    };
    double x;
    V1() noexcept : x(0.0) {}
    explicit V1(double v) noexcept : x(v) {}
    static V1 load(const double* p) noexcept { return V1(p[0]); }
    static V1 set(const double* p) noexcept { return V1(p[0]); }
    static V1 dup(const double* p) noexcept { return V1(p[0]); }
    double get(int) const noexcept { return x; }
    void store(double* p) const noexcept { p[0] = x; }
    static Mask none() noexcept { return {false}; }
    V1 operator+(V1 o) const noexcept { return V1(x + o.x); }
    V1 operator-(V1 o) const noexcept { return V1(x - o.x); }
    V1 operator*(V1 o) const noexcept { return V1(x * o.x); }
    V1 operator/(V1 o) const noexcept { return V1(x / o.x); }
    V1 neg() const noexcept { return V1(-x); }
    V1 abs() const noexcept { return V1(std::fabs(x)); }
    static V1 max(V1 a, V1 b) noexcept { return a.x > b.x ? a : b; }
    Mask operator>(V1 o) const noexcept { return {x > o.x}; }
    Mask operator<(V1 o) const noexcept { return {x < o.x}; }
    Mask operator==(V1 o) const noexcept { return {x == o.x}; }
    static V1 select(Mask m, V1 a, V1 b) noexcept { return m.b ? a : b; }
};

}  // namespace

double TriodeStage::tick(double vin) noexcept {
    TriodeStage* const st[1] = {this};
    double out;
    solve<V1>(st, &vin, &out);
    return out;
}

void TriodeStage::tick2(TriodeStage& a, TriodeStage& b, double vin_a, double vin_b, double& out_a,
                        double& out_b) noexcept {
    // The pair shares one set of matrices, so the two must have been
    // prepared alike; otherwise each runs alone, with the same results.
    if (a.model_key_ != b.model_key_) {
        out_a = a.tick(vin_a);
        out_b = b.tick(vin_b);
        return;
    }
    TriodeStage* const st[2] = {&a, &b};
    const double vin[2] = {vin_a, vin_b};
    double out[2];
    triode_tick_pair(st, vin, out);
    out_a = out[0];
    out_b = out[1];
}

void TriodeStage::small_signal_gain(double f_hz, double& magnitude, double& phase_deg) const {
    using C = std::complex<double>;
    const C jw(0.0, kTwoPi * f_hz);
    double gr[N][N] = {};
    double rhs_unused[N] = {};
    stamp_resistors(ckt_, gr, rhs_unused);
    C y[N][N];
    for (std::uint32_t r = 0; r < N; ++r)
        for (std::uint32_t k = 0; k < N; ++k) y[r][k] = gr[r][k];
    C b[N] = {};
    double cv[M];
    cap_values(ckt_, dev_, cv);
    for (std::uint32_t m = 0; m < M; ++m) {
        const int a = kCapNodes[m].a, bb = kCapNodes[m].b;
        const C ym = jw * cv[m];
        if (a >= 0) y[a][a] += ym;
        if (bb >= 0) y[bb][bb] += ym;
        if (a >= 0 && bb >= 0) {
            y[a][bb] -= ym;
            y[bb][a] -= ym;
        }
        if (a == kVin && bb >= 0) b[bb] += ym;  // a 1 V source
    }
    // The devices, linearised: the injection -kPort^T Ji Nv V moves left.
    const TriodeCurrents c = currents(op_port_[0], op_port_[1]);
    const double ji[2][2] = {{c.dip_dvgk, c.dip_dvpk}, {c.dig_dvgk, 0.0}};
    for (std::uint32_t r = 0; r < N; ++r) {
        for (std::uint32_t k = 0; k < N; ++k) {
            double acc = 0.0;
            for (std::uint32_t a = 0; a < 2; ++a)
                for (std::uint32_t bb = 0; bb < 2; ++bb) acc += kPort[a][r] * ji[a][bb] * kNv[bb][k];
            y[r][k] += acc;
        }
    }
    invert(y);
    C vo = 0.0;
    for (std::uint32_t k = 0; k < N; ++k) vo += y[kO][k] * b[k];
    magnitude = std::abs(vo);
    phase_deg = std::arg(vo) * 180.0 / kPi;
}

}  // namespace sonora::dsp
