// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two triode stages in lockstep, portable: the two-lane V of
// TriodeStage::solve (triode_solve.h) as two doubles. The operations are
// tick()'s, lane by lane, so the results are the same bits; what the pair
// gains is two independent dependency chains for the core to overlap.
#include "../../triode_solve.h"

namespace sonora::dsp {

namespace {

struct V2 {
    static constexpr int kLanes = 2;
    struct Mask {
        bool b[2];
        Mask operator&(Mask o) const noexcept { return {{b[0] && o.b[0], b[1] && o.b[1]}}; }
        Mask operator|(Mask o) const noexcept { return {{b[0] || o.b[0], b[1] || o.b[1]}}; }
        Mask operator~() const noexcept { return {{!b[0], !b[1]}}; }
        bool lane(int i) const noexcept { return b[i]; }
        bool all() const noexcept { return b[0] && b[1]; }
        bool any() const noexcept { return b[0] || b[1]; }
    };
    double x[2];
    V2() noexcept : x{0.0, 0.0} {}
    explicit V2(double v) noexcept : x{v, v} {}
    V2(double a, double b) noexcept : x{a, b} {}
    static V2 load(const double* p) noexcept { return V2(p[0], p[1]); }
    static V2 set(const double* p) noexcept { return V2(p[0], p[1]); }
    static V2 dup(const double* p) noexcept { return V2(p[0], p[1]); }
    double get(int i) const noexcept { return x[i]; }
    void store(double* p) const noexcept {
        p[0] = x[0];
        p[1] = x[1];
    }
    static Mask none() noexcept { return {{false, false}}; }
    V2 operator+(V2 o) const noexcept { return V2(x[0] + o.x[0], x[1] + o.x[1]); }
    V2 operator-(V2 o) const noexcept { return V2(x[0] - o.x[0], x[1] - o.x[1]); }
    V2 operator*(V2 o) const noexcept { return V2(x[0] * o.x[0], x[1] * o.x[1]); }
    V2 operator/(V2 o) const noexcept { return V2(x[0] / o.x[0], x[1] / o.x[1]); }
    V2 neg() const noexcept { return V2(-x[0], -x[1]); }
    V2 abs() const noexcept { return V2(std::fabs(x[0]), std::fabs(x[1])); }
    static V2 max(V2 a, V2 b) noexcept {
        return V2(a.x[0] > b.x[0] ? a.x[0] : b.x[0], a.x[1] > b.x[1] ? a.x[1] : b.x[1]);
    }
    Mask operator>(V2 o) const noexcept { return {{x[0] > o.x[0], x[1] > o.x[1]}}; }
    Mask operator<(V2 o) const noexcept { return {{x[0] < o.x[0], x[1] < o.x[1]}}; }
    Mask operator==(V2 o) const noexcept { return {{x[0] == o.x[0], x[1] == o.x[1]}}; }
    static V2 select(Mask m, V2 a, V2 b) noexcept { return V2(m.b[0] ? a.x[0] : b.x[0], m.b[1] ? a.x[1] : b.x[1]); }
};

}  // namespace

void triode_tick_pair(TriodeStage* const* st, const double* vin, double* out) noexcept {
    TriodeStage::solve<V2>(st, vin, out);
}

}  // namespace sonora::dsp
