// SPDX-License-Identifier: GPL-3.0-or-later
//
// Flush-to-zero and denormals-are-zero for the audio thread: the same bits
// the plugin host sets on its processing thread (plugins/host, ipc_linux.cpp),
// MXCSR on x86 and FPCR.FZ on AArch64. A fade, a ramp to a muted fader or a
// delay line decaying into subnormals costs up to a hundred cycles per
// operation on x86 without them, a spike that lands in the callback.
#pragma once

#include <cstdint>

#include "sonora/rt.h"

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace sonora {

// Sets the mode on the calling thread if it is not set already. One
// register read is nothing next to a block, so the audio thread calls this
// at the top of every block rather than trust whichever thread a backend
// happens to call it from; only the first call writes.
inline void flush_denormals_on_this_thread() noexcept SONORA_RT {
#if defined(__x86_64__) || defined(__i386__)
    // FTZ is bit 15, DAZ bit 6.
    const unsigned csr = _mm_getcsr();
    if ((csr & 0x8040u) != 0x8040u) _mm_setcsr(csr | 0x8040u);
#elif defined(__aarch64__)
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    const uint64_t fz = uint64_t(1) << 24;  // flushes denormal inputs and outputs alike
    if ((fpcr & fz) == 0) __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | fz));
#endif
}

}  // namespace sonora
