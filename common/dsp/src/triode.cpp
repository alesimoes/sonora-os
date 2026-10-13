// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/triode.h"

#include <cmath>
#include <complex>

#include "sonora/dsp/math.h"

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
    {TriodeStage::kG, TriodeStage::kK},   // Cgk
    {TriodeStage::kG, TriodeStage::kP},   // Cgp
    {TriodeStage::kP, TriodeStage::kK},   // Cpk
};

void cap_values(const TriodeStageCircuit& c, const TriodeDevice& d, double out[M]) {
    out[0] = c.c_in;
    out[1] = c.c_cathode;
    out[2] = c.c_out;
    out[3] = d.c_gk;
    out[4] = d.c_gp;
    out[5] = d.c_pk;
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

inline bool TriodeStage::Table::eval(double x, double& y, double& dy) const noexcept {
    if (!(x >= lo && x < hi)) return false;
    const double t = (x - lo) * inv_h;
    const int i = static_cast<int>(t);
    const double s = t - static_cast<double>(i);
    const double* k = &c[static_cast<std::size_t>(i) * 4];
    y = k[0] + s * (k[1] + s * (k[2] + s * k[3]));
    dy = (k[1] + s * (2.0 * k[2] + s * (3.0 * k[3]))) * inv_h;
    return true;
}

namespace {
// The exact law where the tables end: rare (cut-off far below the bias,
// or an absurd input), and kept out of line so the common path inlines.
void exact_ik(const TriodeDevice& d, double u, double& y, double& dy) noexcept { dempwolf_ik(d, u, y, dy); }
void exact_ig(const TriodeDevice& d, double v, double& y, double& dy) noexcept { dempwolf_ig(d, v, y, dy); }
}  // namespace

TriodeCurrents TriodeStage::currents(double vgk, double vpk) const noexcept { return law(vgk, vpk); }

inline TriodeCurrents TriodeStage::law(double vgk, double vpk) const noexcept {
    if (!tabulated_) return triode_currents(law_, dev_, vgk, vpk);
    double ik, dik, ig, dig;
    const double u = vpk * inv_mu_ + vgk;
    if (!ik_.eval(u, ik, dik)) exact_ik(dev_, u, ik, dik);
    if (!ig_.eval(vgk, ig, dig)) exact_ig(dev_, vgk, ig, dig);
    TriodeCurrents r;
    r.ip = ik - ig;
    r.ig = ig;
    r.dip_dvgk = dik - dig;
    r.dip_dvpk = dik * inv_mu_;
    r.dig_dvgk = dig;
    return r;
}

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
        // u = Vpk / mu + Vgk reaches about 5.5 V and Vgk about 3 V at the
        // drive processor's extremes; below -5 V both currents are under
        // 1e-11 A. Outside the range the exact law is used.
        build(ik_, -5.0, 8.0, false);
        build(ig_, -5.0, 8.0, true);
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

    // Packed for tick(): rows p0, p1, out, J'0..5 (and a zero pad).
    for (std::uint32_t col = 0; col < kCols; ++col)
        for (std::uint32_t r = 0; r < kRows; ++r) w_[col][r] = 0.0;
    for (std::uint32_t col = 0; col < kCols; ++col) {
        const auto pick = [&](const double* j_part, double u_part, double c_part) {
            return col < M ? j_part[col] : (col == M ? u_part : c_part);
        };
        w_[col][0] = pick(pj[0], pu[0], pc[0]);
        w_[col][1] = pick(pj[1], pu[1], pc[1]);
        w_[col][2] = pick(oj, ou, oc);
        for (std::uint32_t m = 0; m < M; ++m) w_[col][3 + m] = pick(aj[m], au[m], ac[m]);
    }
    for (std::uint32_t r = 0; r < kRows; ++r) post_[0][r] = post_[1][r] = 0.0;
    for (std::uint32_t c = 0; c < 2; ++c) {
        post_[c][2] = oi[c];
        for (std::uint32_t m = 0; m < M; ++m) post_[c][3 + m] = ai[m][c];
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
    // The history sources at rest: no capacitor current, J = gc v.
    for (std::uint32_t m = 0; m < M; ++m) {
        const int a = kCapNodes[m].a, b = kCapNodes[m].b;
        const double va = a >= 0 ? op_v_[a] : 0.0;  // vin = 0, ground = 0
        const double vb = b >= 0 ? op_v_[b] : 0.0;
        op_j_[m] = gc[m] * (va - vb);
    }
    reset();
    return true;
}

void TriodeStage::reset() noexcept {
    for (std::uint32_t m = 0; m < M; ++m) j_[m] = op_j_[m];
    i_[0] = op_i_[0];
    i_[1] = op_i_[1];
    v_[0] = op_port_[0];
    v_[1] = op_port_[1];
    ji_[0] = op_ji_[0];
    ji_[1] = op_ji_[1];
    ji_[2] = op_ji_[2];
    for (int k = 0; k < 4; ++k) inv_[k] = op_inv_[k];
    last_iter_ = max_iter_ = 0;
}

namespace {
// acc += w x, row by row: each row's sum keeps its own order, so packed
// arithmetic gives the same bits as scalar.
inline void mac_rows(double* acc, const double* w, double x) noexcept {
    for (std::uint32_t r = 0; r < TriodeStage::kRows; ++r) acc[r] += w[r] * x;
}
}  // namespace

// One sample of L independent stages in lockstep. Every lane does exactly
// the arithmetic tick() does for it alone, in the same order, and a lane
// that has converged stops changing, so a stage's output never depends on
// the stage beside it (test_triode checks tick2 against tick to the bit).
// What the lockstep buys is instruction-level parallelism: one sample of
// one stage is a chain of dependent operations (sums, a division, a table
// lookup, another division) that leaves most of a core idle.
template <int L>
void TriodeStage::step(TriodeStage* const* st, const double* vin, double* out) noexcept {
    alignas(16) double base[L][kRows];
    double p[L][2], v[L][2], i[L][2];
    for (int l = 0; l < L; ++l) {
        const TriodeStage& s = *st[l];
        // base = W (J, vin, 1): column by column, each row its own sum in
        // a fixed order, so it vectorises without changing the arithmetic.
        for (std::uint32_t r = 0; r < kRows; ++r) base[l][r] = s.w_[M + 1][r];
        mac_rows(base[l], s.w_[M], vin[l]);
        for (std::uint32_t col = 0; col < M; ++col) mac_rows(base[l], s.w_[col], s.j_[col]);
        p[l][0] = base[l][0];
        p[l][1] = base[l][1];
        // Predictor: one Newton step from last sample's solution with last
        // sample's Jacobian and its inverse, which costs no evaluation of
        // the law and no division. For a smoothly moving input it lands
        // within millivolts.
        const double ji00 = s.ji_[0], ji01 = s.ji_[1], ji10 = s.ji_[2];
        const double e0 = s.i_[0] - ji00 * s.v_[0] - ji01 * s.v_[1], e1 = s.i_[1] - ji10 * s.v_[0];
        const double r0 = p[l][0] + s.k_[0][0] * e0 + s.k_[0][1] * e1;
        const double r1 = p[l][1] + s.k_[1][0] * e0 + s.k_[1][1] * e1;
        // v = (I - K Ji)^-1 r, the inverse kept from last sample.
        v[l][0] = s.inv_[0] * r0 + s.inv_[1] * r1;
        v[l][1] = s.inv_[2] * r0 + s.inv_[3] * r1;
    }
    bool done[L];
    std::uint32_t it[L];
    TriodeCurrents c[L];
    for (int l = 0; l < L; ++l) {
        done[l] = false;
        it[l] = 0;
    }
    for (std::uint32_t pass = 0; pass < kMaxIterations; ++pass) {
        bool all = true;
        for (int l = 0; l < L; ++l) {
            if (done[l]) continue;
            const TriodeStage& s = *st[l];
            ++it[l];
            c[l] = s.law(v[l][0], v[l][1]);
            const TriodeCurrents& cc = c[l];
            i[l][0] = cc.ip;
            i[l][1] = cc.ig;
            const double f0 = p[l][0] + s.k_[0][0] * i[l][0] + s.k_[0][1] * i[l][1] - v[l][0];
            const double f1 = p[l][1] + s.k_[1][0] * i[l][0] + s.k_[1][1] * i[l][1] - v[l][1];
            // JF = K Ji - I, and -JF = I - K Ji, whose inverse is the next
            // sample's predictor.
            const double j00 = s.k_[0][0] * cc.dip_dvgk + s.k_[0][1] * cc.dig_dvgk - 1.0;
            const double j01 = s.k_[0][0] * cc.dip_dvpk;
            const double j10 = s.k_[1][0] * cc.dip_dvgk + s.k_[1][1] * cc.dig_dvgk;
            const double j11 = s.k_[1][0] * cc.dip_dvpk - 1.0;
            const double det = j00 * j11 - j01 * j10;
            if (!(std::fabs(det) > 1e-300)) {
                done[l] = true;
                continue;
            }
            const double inv = 1.0 / det;
            double d0 = (-f0 * j11 + f1 * j01) * inv;
            double d1 = (-f1 * j00 + f0 * j10) * inv;
            // A bounded step: the laws are monotone, but a step of
            // hundreds of volts from an absurd input could leave the
            // tables' range for nothing.
            const double a0 = std::fabs(d0), a1 = std::fabs(d1);
            const double big = a0 > a1 ? a0 : a1;
            if (big > 50.0) {
                d0 *= 50.0 / big;
                d1 *= 50.0 / big;
            }
            v[l][0] += d0;
            v[l][1] += d1;
            // The currents at the new point, to first order: the step is
            // the Newton step, so this is exact to the square of the step.
            i[l][0] += cc.dip_dvgk * d0 + cc.dip_dvpk * d1;
            i[l][1] += cc.dig_dvgk * d0;
            // Newton converges quadratically: after a step of 1e-5 V the
            // error left is of order 1e-10 V at the grid (the laws'
            // curvature is at most ~10 /V), under -170 dB at the
            // processor's output. The plate moves ~100 times the grid, so
            // its threshold is 100 times wider.
            if (std::fabs(d0) < 1e-5 && std::fabs(d1) < 1e-3) {
                done[l] = true;
                // (I - K Ji)^-1 = -JF^-1
                TriodeStage& w = *st[l];
                w.inv_[0] = -j11 * inv;
                w.inv_[1] = j01 * inv;
                w.inv_[2] = j10 * inv;
                w.inv_[3] = -j00 * inv;
            } else {
                all = false;
            }
        }
        if (all) break;
    }
    for (int l = 0; l < L; ++l) {
        TriodeStage& s = *st[l];
        s.last_iter_ = it[l];
        if (it[l] > s.max_iter_) s.max_iter_ = it[l];
        if (!(std::isfinite(i[l][0]) && std::isfinite(i[l][1]) && std::isfinite(v[l][0]) &&
              std::isfinite(v[l][1]))) {
            // Never seen; but a non-finite state would never recover, so
            // the stage restarts from its operating point instead.
            s.reset();
            out[l] = 0.0;
            continue;
        }
        s.v_[0] = v[l][0];
        s.v_[1] = v[l][1];
        s.ji_[0] = c[l].dip_dvgk;
        s.ji_[1] = c[l].dip_dvpk;
        s.ji_[2] = c[l].dig_dvgk;
        mac_rows(base[l], s.post_[0], i[l][0]);
        mac_rows(base[l], s.post_[1], i[l][1]);
        out[l] = base[l][2];
        for (std::uint32_t m = 0; m < M; ++m) s.j_[m] = base[l][3 + m];
        s.i_[0] = i[l][0];
        s.i_[1] = i[l][1];
    }
}

double TriodeStage::tick(double vin) noexcept {
    TriodeStage* const st[1] = {this};
    double out;
    step<1>(st, &vin, &out);
    return out;
}

void TriodeStage::tick2(TriodeStage& a, TriodeStage& b, double vin_a, double vin_b, double& out_a,
                        double& out_b) noexcept {
    TriodeStage* const st[2] = {&a, &b};
    const double vin[2] = {vin_a, vin_b};
    double out[2];
    step<2>(st, vin, out);
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
