// SPDX-License-Identifier: GPL-3.0-or-later
// What tests/rt_reachability.cmake must refuse: an "audio thread" object
// that reaches common/platform beyond wake_all, allocates and locks. Never
// linked into anything; plugin_client_rt_reachability_canary runs the check
// on it and passes only when the check fails.
#include "sonora/platform/process.h"

#include <mutex>
#include <string>

namespace {
std::mutex m;
}

std::string canary_callback() {
    std::lock_guard<std::mutex> lock(m);
    auto* leak = new int(1);
    delete leak;
    return sonora::platform::executable_dir();
}
