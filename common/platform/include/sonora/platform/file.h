// SPDX-License-Identifier: GPL-3.0-or-later
//
// Files the engine creates and must not share (ADR-0008 §3; the exclusive
// create it names as `create_exclusive()`). Not for the audio thread.
#pragma once

#include <cstdio>
#include <string>

namespace sonora::platform {

// Creates `path` for binary writing and returns it open, or null with errno
// set. Exclusive: a name that exists is refused with EEXIST and left as it
// is, never truncated. Not inherited: a process this one starts later does
// not get the file (POSIX: O_CLOEXEC, set atomically with the open).
std::FILE* create_exclusive(const std::string& path);

}  // namespace sonora::platform
