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

// Sets the mode on the calling thread for one block and puts the thread's
// own back after it. Under pw-jack the block runs on PipeWire's data loop,
// shared with other nodes that expect IEEE arithmetic
// (cpu.zero.denormals defaults to false). A register read per block is
// nothing next to the block; the writes happen only when the host's mode
// differs.
class FlushDenormals {
public:
    FlushDenormals() noexcept SONORA_RT {
#if defined(__x86_64__) || defined(__i386__)
        saved_ = _mm_getcsr();
        if ((saved_ & kFtzDaz) != kFtzDaz) _mm_setcsr(saved_ | kFtzDaz);
#elif defined(__aarch64__)
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(saved_));
        if ((saved_ & kFz) == 0) __asm__ __volatile__("msr fpcr, %0" : : "r"(saved_ | kFz));
#endif
    }
    ~FlushDenormals() {
#if defined(__x86_64__) || defined(__i386__)
        if ((saved_ & kFtzDaz) != kFtzDaz) _mm_setcsr(saved_);
#elif defined(__aarch64__)
        if ((saved_ & kFz) == 0) __asm__ __volatile__("msr fpcr, %0" : : "r"(saved_));
#endif
    }
    FlushDenormals(const FlushDenormals&) = delete;
    FlushDenormals& operator=(const FlushDenormals&) = delete;

private:
#if defined(__x86_64__) || defined(__i386__)
    static constexpr unsigned kFtzDaz = 0x8040u;  // FTZ is bit 15, DAZ bit 6
    unsigned saved_ = 0;
#elif defined(__aarch64__)
    // Flushes denormal inputs and outputs alike.
    static constexpr uint64_t kFz = uint64_t(1) << 24;
    uint64_t saved_ = 0;
#endif
};

}  // namespace sonora
