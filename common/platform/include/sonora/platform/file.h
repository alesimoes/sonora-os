// SPDX-License-Identifier: GPL-3.0-or-later
//
// Files the engine creates and must not share (ADR-0008 §3; the exclusive
// create it names as `create_exclusive()`). Not for the audio thread.
#pragma once

#include <cstdint>
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

// Where this user's regenerable data goes, the application's own folder in
// it, not created: POSIX $XDG_CACHE_HOME/sonora when that is an absolute
// path, else $HOME/.cache/sonora; empty when neither is known. (Windows,
// later: %LOCALAPPDATA%\Sonora.) Deleting it costs a rebuild, never data.
std::string user_cache_dir();

// What a fingerprint needs of one path, following symlinks to their
// target (a yabridge bundle links into a Wine prefix).
struct PathStatus {
    bool regular = false;
    bool directory = false;
    uint64_t size = 0;
    // Nanoseconds since the epoch: Unix's on POSIX; elsewhere the file
    // clock's own, comparable only with stamps of the same platform.
    // `changed` is the inode change time
    // (POSIX ctime), which cp -p, rsync -t, tar and unzip cannot set back
    // the way they set the modification time; 0 where there is none.
    int64_t modified_ns = 0;
    int64_t changed_ns = 0;
};
// False when the path, or a symlink's target, does not exist or cannot be
// read.
bool path_status(const std::string& path, PathStatus& out);

// An advisory lock on a folder: one engine per session folder, because a
// second one would run crash recovery over the first one's open takes.
// Held until the object is destroyed or the process ends, however it ends,
// so a killed engine never leaves its session locked. Each FolderLock is
// its own holder, in this process or another. Not inherited by a process
// this one starts. Advisory: it keeps out only programs that ask for it.
// POSIX: flock on the folder itself, so nothing is written into it.
class FolderLock {
public:
    enum class Result { Held, Busy, Failed };

    FolderLock() = default;
    ~FolderLock();
    FolderLock(const FolderLock&) = delete;
    FolderLock& operator=(const FolderLock&) = delete;

    // Never waits. Busy: another holder has it. Failed: the folder could
    // not be opened or locked. Either way `error` says why. Where the
    // platform has no lock, Held without holding anything: no exclusion is
    // claimed there.
    Result acquire(const std::string& path, std::string& error);
    bool held() const { return handle_ != -1; }

private:
    intptr_t handle_ = -1;
};

}  // namespace sonora::platform
