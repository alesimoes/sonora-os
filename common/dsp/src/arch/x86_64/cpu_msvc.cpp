// SPDX-License-Identifier: GPL-3.0-or-later
//
// cpu.h for MSVC: CPUID and XGETBV by hand (Intel SDM vol. 1, "Detection
// of Intel AVX2").
#include "cpu.h"

#include <immintrin.h>
#include <intrin.h>

namespace sonora::dsp::arch {

bool cpu_runs_avx2() noexcept {
    int r[4] = {0, 0, 0, 0};
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    const bool osxsave = (r[2] & (1 << 27)) != 0;
    const bool avx = (r[2] & (1 << 28)) != 0;
    if (!osxsave || !avx) return false;
    // XCR0 bit 1 (SSE state) and bit 2 (AVX state): the OS saves the ymm registers.
    if ((_xgetbv(0) & 0x6u) != 0x6u) return false;
    __cpuidex(r, 7, 0);
    return (r[1] & (1 << 5)) != 0;
}

}  // namespace sonora::dsp::arch
