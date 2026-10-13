// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The OS seam for loading plugin binaries (ADR-0008: platform code at the
// edges, behind a neutral header). Only src/linux/ implements it today.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace sonora::plugin_host {

// A shared library opened with local symbol binding, so two plugins that
// carry the same symbol names do not resolve into each other.
void* open_library(const std::string& path, std::string& error);
void* library_symbol(void* library, const char* name);
void close_library(void* library);

// The directory part of the platform's per-user and system VST3 locations
// (Linux: ~/.vst3, /usr/lib/vst3, /usr/local/lib/vst3), in search order.
std::vector<std::string> vst3_default_roots();
// The bundle subdirectory holding this build's binary ("x86_64-linux").
const char* vst3_architecture();

// Run `body` in a child process and return what it wrote to `out`. A scan
// runs plugin code (a VST3 module's static constructors, its factory), so it
// runs where a crash or a hang costs one entry of the list, not the list.
// The child's stdout goes to stderr: whatever a plugin prints there cannot
// end up inside the list.
struct IsolatedResult {
    std::string output;
    bool exited = false;   // false: killed by a signal or by the timeout
    int exit_code = 0;
    int signal = 0;        // the signal that ended it, when !exited
    bool timed_out = false;
};
IsolatedResult run_isolated(const std::function<int(std::FILE* out)>& body, uint64_t timeout_ns);

}  // namespace sonora::plugin_host
