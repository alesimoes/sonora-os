// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace sonora {

// Locks every page the process has, and every page it maps from now on, into
// RAM (mlockall MCL_CURRENT | MCL_FUTURE), so the audio thread never faults
// on the session's buffers. Call before the session is constructed.
//
// Needs RLIMIT_MEMLOCK unlimited, and raises the soft limit to the hard one
// to get it. With a finite limit MCL_FUTURE would turn the first mapping past
// it -- a thread stack, a capture ring -- into a failed allocation later, so
// that is refused here instead, with `why` naming the limit.
bool lock_memory(std::string& why);

// VmLck from /proc/self/status, in kB; -1 if it cannot be read.
long locked_kb();

}  // namespace sonora
