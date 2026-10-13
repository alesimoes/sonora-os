// SPDX-License-Identifier: GPL-3.0-or-later
// Must FAIL tests/rt_reachability.cmake: an "audio thread" object that calls
// a common/platform function other than wake_all. Never linked.
#include "sonora/platform/process.h"

#include <cstdint>

int64_t canary_platform() { return sonora::platform::current_process_id(); }
