// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tape's two channels in lockstep, portable: the two-lane V of
// TapeSolve (tape_solve.h) as two doubles. The operations are
// TapeHysteresis::tick()'s, lane by lane, so the results are the same
// bits; what the pair gains is two independent dependency chains for the
// core to overlap.
#include "../../tape_solve.h"

namespace sonora::dsp {

namespace {

struct V2 {
    static constexpr int kLanes = 2;
    struct Mask {
        bool b[2];
        Mask operator&(Mask o) const noexcept { return {{b[0] && o.b[0], b[1] && o.b[1]}}; }
        Mask operator|(Mask o) const noexcept { return {{b[0] || o.b[0], b[1] || o.b[1]}}; }
        Mask operator~() const noexcept { return {{!b[0], !b[1]}}; }
        bool any() const noexcept { return b[0] || b[1]; }
    };
    double x[2];
    V2() noexcept : x{0.0, 0.0} {}
    explicit V2(double v) noexcept : x{v, v} {}
    V2(double a, double b) noexcept : x{a, b} {}
    static V2 from(const double* p) noexcept { return V2(p[0], p[1]); }
    double get(int i) const noexcept { return x[i]; }
    V2 operator+(V2 o) const noexcept { return V2(x[0] + o.x[0], x[1] + o.x[1]); }
    V2 operator-(V2 o) const noexcept { return V2(x[0] - o.x[0], x[1] - o.x[1]); }
    V2 operator*(V2 o) const noexcept { return V2(x[0] * o.x[0], x[1] * o.x[1]); }
    V2 operator/(V2 o) const noexcept { return V2(x[0] / o.x[0], x[1] / o.x[1]); }
    V2 neg() const noexcept { return V2(-x[0], -x[1]); }
    V2 abs() const noexcept { return V2(std::fabs(x[0]), std::fabs(x[1])); }
    Mask operator<(V2 o) const noexcept { return {{x[0] < o.x[0], x[1] < o.x[1]}}; }
    Mask operator>(V2 o) const noexcept { return {{x[0] > o.x[0], x[1] > o.x[1]}}; }
    Mask operator<=(V2 o) const noexcept { return {{x[0] <= o.x[0], x[1] <= o.x[1]}}; }
    Mask operator==(V2 o) const noexcept { return {{x[0] == o.x[0], x[1] == o.x[1]}}; }
    static V2 select(Mask m, V2 a, V2 b) noexcept { return V2(m.b[0] ? a.x[0] : b.x[0], m.b[1] ? a.x[1] : b.x[1]); }
    static V2 exp2_magic(V2 t) noexcept {
        return V2(tape_detail::V1::exp2_bits(t.x[0]), tape_detail::V1::exp2_bits(t.x[1]));
    }
    static V2 trunc(V2 u, int* idx) noexcept {
        idx[0] = static_cast<int>(u.x[0]);
        idx[1] = static_cast<int>(u.x[1]);
        return V2(static_cast<double>(idx[0]), static_cast<double>(idx[1]));
    }
    static void rows4(const double* const* r, V2& c0, V2& c1, V2& c2, V2& c3) noexcept {
        c0 = V2(r[0][0], r[1][0]);
        c1 = V2(r[0][1], r[1][1]);
        c2 = V2(r[0][2], r[1][2]);
        c3 = V2(r[0][3], r[1][3]);
    }
};

}  // namespace

void tape_run_pair(TapeHysteresis& a, TapeHysteresis& b, double* xa, double* xb, std::uint32_t n) noexcept {
    TapeHysteresis* st[2] = {&a, &b};
    double* x[2] = {xa, xb};
    TapeSolve<V2>::run(st, x, n);
}

}  // namespace sonora::dsp
