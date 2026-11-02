// SPDX-License-Identifier: GPL-3.0-or-later
//
// AArch64: FPCR.FZ (bit 24). With FPCR.AH clear, the state every OS gives
// a thread, FZ flushes subnormal inputs and outputs of single- and
// double-precision arithmetic alike, so it is x86's FTZ and DAZ in one bit
// (Arm Architecture Reference Manual for A-profile, DDI 0487, "FPCR",
// "Flushing denormalized numbers to zero"). Half precision has FZ16 (bit
// 19); nothing in common/dsp computes in half, so it is left alone.
//
// Most AArch64 cores have no subnormal slow path, but some do, and the tape
// and transformer state decays through the subnormal range of double (a
// pre-emphasis tail runs about 46 oversampled samples there): flushing
// makes that the same arithmetic as on x86-64, where the goldens are made.
//
// FPCR is read and written with MRS/MSR, which standard C++ cannot name:
// GCC/Clang inline assembly. CMake builds this file only for those
// compilers (common/dsp/CMakeLists.txt); MSVC on ARM64 would need its own
// file (_ReadStatusReg/_WriteStatusReg).
#include "sonora/dsp/denormals.h"

#include <cstdint>

namespace sonora::dsp {

namespace {

constexpr std::uint64_t kFz = std::uint64_t{1} << 24;

inline std::uint64_t read_fpcr() noexcept {
    std::uint64_t v;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
    return v;
}

inline void write_fpcr(std::uint64_t v) noexcept {
    __asm__ __volatile__("msr fpcr, %0" : : "r"(v));
}

}  // namespace

// FPCR's defined fields all sit in its low 32 bits (the upper half is
// RES0), so FpState's 32 bits hold the whole state.
FpState enable_flush_to_zero() noexcept {
    FpState saved;
    const std::uint64_t fpcr = read_fpcr();
    saved.bits = static_cast<std::uint32_t>(fpcr);
    if ((fpcr & kFz) == 0) write_fpcr(fpcr | kFz);
    return saved;
}

void restore_fp_state(FpState state) noexcept {
    // An MSR to FPCR can be costly on some cores; skip it when nothing changes.
    const std::uint64_t now = read_fpcr();
    const std::uint64_t want = (now & ~std::uint64_t{0xffffffffu}) | state.bits;
    if (want != now) write_fpcr(want);
}

bool flush_to_zero_supported() noexcept { return true; }

bool flush_to_zero_active() noexcept { return (read_fpcr() & kFz) != 0; }

}  // namespace sonora::dsp
