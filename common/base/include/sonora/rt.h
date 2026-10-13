// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Marks a function that runs on the audio thread (ADR-0007 §2, and its first
// 2026-10-12 update). Declare it as `noexcept SONORA_RT`, in that order,
// before `override` or `= 0`:
//
//     virtual void process(const AudioBlock& block) noexcept SONORA_RT = 0;
//     void process(const AudioBlock& block) noexcept SONORA_RT override;
//
// Under a Clang that has it, this is [[clang::nonblocking]], which RTSan
// (-fsanitize=realtime) checks at run time and -Wfunction-effects checks at
// compile time. It is part of the function's type there, so an override that
// leaves it out does not override under that build; GCC never sees the
// difference. Elsewhere it expands to nothing, and the rt-safety-auditor
// starts its call graph from every function that carries it.
#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::nonblocking)
#define SONORA_RT [[clang::nonblocking]]
#endif
#endif

#ifndef SONORA_RT
#define SONORA_RT
#endif
