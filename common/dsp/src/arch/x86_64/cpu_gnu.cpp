// SPDX-License-Identifier: GPL-3.0-or-later
//
// cpu.h for GCC and Clang: their runtime's CPU model, which reports AVX2
// only when XGETBV says the OS saves the ymm state (libgcc cpuinfo.c,
// compiler-rt cpu_model.c), so the OS check is included.
#include "cpu.h"

namespace sonora::dsp::arch {

bool cpu_runs_avx2() noexcept {
    // Needed only before the runtime's own constructor has run (a static
    // initialiser that loads a capture); harmless after.
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") != 0;
}

}  // namespace sonora::dsp::arch
