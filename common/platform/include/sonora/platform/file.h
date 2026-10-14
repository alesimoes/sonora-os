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

// Flushes `f`'s buffer and asks the OS to put its data, and the size needed
// to read it back, on the medium (POSIX: fdatasync). Blocks for as long as
// the device takes, which on a USB stick can be long: never from the audio
// thread. False with errno set if either step failed. A file the OS cannot
// sync at all (EINVAL: a pipe, a special file) counts as synced, since there
// is nothing more this call could do for it.
bool sync_data(std::FILE* f);

// Makes the entries of directory `path` durable, so a file just created in
// it is still found after a power cut (POSIX: fsync on the directory). False
// with errno set on failure.
bool sync_directory(const std::string& path);

}  // namespace sonora::platform
