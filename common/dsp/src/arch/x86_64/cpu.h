// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the x86-64 processor this runs on supports, for the kernels chosen
// at run time. One implementation per compiler family (cpu_gnu.cpp,
// cpu_msvc.cpp), picked by CMake: the two spell CPUID differently.
// Internal to common/dsp. Not real-time on the first call.
#pragma once

namespace sonora::dsp::arch {

// AVX2, and the OS saves the ymm registers across a context switch
// (OSXSAVE and XCR0 bits 1-2): without the second, an AVX2 instruction
// faults even on a processor that has it.
bool cpu_runs_avx2() noexcept;

}  // namespace sonora::dsp::arch
