// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bench's heap counter, a copy of engine/core's playback_test.h counter
// (the bench includes no test header): heap_counter.cpp replaces the global
// allocators for the executable, and counts only on the thread while
// t_counting is set, i.e. inside Session::process(). The supervisor's
// threads and the driver loop allocate uncounted.
#pragma once

namespace sonora::bench {

extern thread_local bool t_counting;

long heap_calls();
void reset_heap_calls();
// What the counter covers in this build, for the report.
const char* heap_counter_scope();

}  // namespace sonora::bench
