// SPDX-License-Identifier: GPL-3.0-or-later
//
// FolderLock, which keeps a second engine off a session folder: the second
// would run crash recovery over the first one's open takes and cut a frame
// it was half-way through writing. Each holder is its own lock, in this
// process or another; a holder that dies, however, frees it; and nothing
// is written into the folder.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "sonora/platform/file.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::platform::FolderLock;

// What a child process gets trying the lock, as its exit status: 0 held,
// 1 busy, 2 failed.
int try_in_child(const std::string& dir) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        FolderLock lock;
        std::string error;
        const FolderLock::Result r = lock.acquire(dir, error);
        ::_exit(r == FolderLock::Result::Held ? 0 : r == FolderLock::Result::Busy ? 1 : 2);
    }
    int status = 0;
    if (pid < 0 || ::waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sonora_test_folder_lock";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string error;

    {
        FolderLock first;
        CHECK(first.acquire(dir.string(), error) == FolderLock::Result::Held);
        CHECK(first.held());

        // Another holder in this process, and one in another process.
        FolderLock second;
        CHECK(second.acquire(dir.string(), error) == FolderLock::Result::Busy);
        CHECK(!second.held());
        CHECK(!error.empty());
        CHECK(try_in_child(dir.string()) == 1);

        // Nothing written into the session folder.
        CHECK(fs::is_empty(dir));
    }
    // Released with its holder.
    CHECK(try_in_child(dir.string()) == 0);

    // A holder that dies without releasing it, as kill -9 would leave it.
    {
        const pid_t pid = ::fork();
        if (pid == 0) {
            FolderLock lock;
            std::string e;
            lock.acquire(dir.string(), e);
            ::_exit(0);
        }
        int status = 0;
        CHECK(pid > 0 && ::waitpid(pid, &status, 0) == pid);
        FolderLock after;
        CHECK(after.acquire(dir.string(), error) == FolderLock::Result::Held);
    }

    // A folder that is not there is a failure, not someone else's lock.
    {
        FolderLock lock;
        error.clear();
        CHECK(lock.acquire((dir / "missing").string(), error) == FolderLock::Result::Failed);
        CHECK(!error.empty());
    }

    fs::remove_all(dir);
    std::printf("test_folder_lock: ok\n");
    return 0;
}
