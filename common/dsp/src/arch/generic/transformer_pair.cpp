// SPDX-License-Identifier: GPL-3.0-or-later
//
// The output stage's two channels in lockstep, portable: the two-lane V of
// OutputStage::process_lanes (transformer_solve.h) as two doubles. The
// operations are the one-channel step's, lane by lane, so the results are
// the same bits; what the pair gains is two independent dependency chains
// for the core to overlap, with no branch between them.
#include "../../transformer_solve.h"

namespace sonora::dsp {

namespace {

struct L2 {
    static constexpr int kLanes = 2;
    struct Mask {
        bool b[2];
        Mask operator&(Mask o) const noexcept { return {{b[0] && o.b[0], b[1] && o.b[1]}}; }
        bool lane(int i) const noexcept { return b[i]; }
    };
    double x[2];
    L2() noexcept : x{0.0, 0.0} {}
    explicit L2(double v) noexcept : x{v, v} {}
    L2(double a, double b) noexcept : x{a, b} {}
    static L2 pack(const double* p) noexcept { return L2(p[0], p[1]); }
    double get(int i) const noexcept { return x[i]; }
    L2 operator+(L2 o) const noexcept { return L2(x[0] + o.x[0], x[1] + o.x[1]); }
    L2 operator-(L2 o) const noexcept { return L2(x[0] - o.x[0], x[1] - o.x[1]); }
    L2 operator*(L2 o) const noexcept { return L2(x[0] * o.x[0], x[1] * o.x[1]); }
    L2 operator/(L2 o) const noexcept { return L2(x[0] / o.x[0], x[1] / o.x[1]); }
    L2 neg() const noexcept { return L2(-x[0], -x[1]); }
    L2 abs() const noexcept { return L2(std::fabs(x[0]), std::fabs(x[1])); }
    L2 sqrt() const noexcept { return L2(std::sqrt(x[0]), std::sqrt(x[1])); }
    Mask operator<(L2 o) const noexcept { return {{x[0] < o.x[0], x[1] < o.x[1]}}; }
    Mask operator>(L2 o) const noexcept { return {{x[0] > o.x[0], x[1] > o.x[1]}}; }
    Mask operator<=(L2 o) const noexcept { return {{x[0] <= o.x[0], x[1] <= o.x[1]}}; }
    static L2 select(Mask m, L2 a, L2 b) noexcept { return L2(m.b[0] ? a.x[0] : b.x[0], m.b[1] ? a.x[1] : b.x[1]); }
};

}  // namespace

void transformer_process_pair(OutputStage& st, const double* const* in, double* const* out, std::uint32_t steps,
                              const double* sag, std::uint32_t steps_per_sag) noexcept SONORA_RT {
    st.process_lanes<L2>(in, out, steps, sag, steps_per_sag);
}

}  // namespace sonora::dsp
