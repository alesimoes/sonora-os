// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two doubles in lockstep, portable: the two-lane V that triode_solve.h
// and transformer_solve.h are written over (each header lists what it
// uses; this type is their union). Every operation is the scalar one, lane
// by lane, so each lane gives the bits the one-lane step gives; what two
// lanes gain is two independent dependency chains for the core to overlap.
// Internal to common/dsp; arch/x86_64/f64x2.h is the same type in SSE2.
#pragma once

#include <cmath>

namespace sonora::dsp::lanes_generic {

struct F64x2 {
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
    F64x2() noexcept : x{0.0, 0.0} {}
    explicit F64x2(double v) noexcept : x{v, v} {}
    F64x2(double a, double b) noexcept : x{a, b} {}
    static F64x2 load(const double* p) noexcept { return F64x2(p[0], p[1]); }
    static F64x2 set(const double* p) noexcept { return F64x2(p[0], p[1]); }
    static F64x2 dup(const double* p) noexcept { return F64x2(p[0], p[1]); }
    double get(int i) const noexcept { return x[i]; }
    void store(double* p) const noexcept {
        p[0] = x[0];
        p[1] = x[1];
    }
    static Mask none() noexcept { return {{false, false}}; }
    F64x2 operator+(F64x2 o) const noexcept { return F64x2(x[0] + o.x[0], x[1] + o.x[1]); }
    F64x2 operator-(F64x2 o) const noexcept { return F64x2(x[0] - o.x[0], x[1] - o.x[1]); }
    F64x2 operator*(F64x2 o) const noexcept { return F64x2(x[0] * o.x[0], x[1] * o.x[1]); }
    F64x2 operator/(F64x2 o) const noexcept { return F64x2(x[0] / o.x[0], x[1] / o.x[1]); }
    F64x2 neg() const noexcept { return F64x2(-x[0], -x[1]); }
    F64x2 abs() const noexcept { return F64x2(std::fabs(x[0]), std::fabs(x[1])); }
    F64x2 sqrt() const noexcept { return F64x2(std::sqrt(x[0]), std::sqrt(x[1])); }
    static F64x2 max(F64x2 a, F64x2 b) noexcept {
        return F64x2(a.x[0] > b.x[0] ? a.x[0] : b.x[0], a.x[1] > b.x[1] ? a.x[1] : b.x[1]);
    }
    Mask operator>(F64x2 o) const noexcept { return {{x[0] > o.x[0], x[1] > o.x[1]}}; }
    Mask operator<(F64x2 o) const noexcept { return {{x[0] < o.x[0], x[1] < o.x[1]}}; }
    Mask operator<=(F64x2 o) const noexcept { return {{x[0] <= o.x[0], x[1] <= o.x[1]}}; }
    Mask operator==(F64x2 o) const noexcept { return {{x[0] == o.x[0], x[1] == o.x[1]}}; }
    static F64x2 select(Mask m, F64x2 a, F64x2 b) noexcept {
        return F64x2(m.b[0] ? a.x[0] : b.x[0], m.b[1] ? a.x[1] : b.x[1]);
    }
};

}  // namespace sonora::dsp::lanes_generic
