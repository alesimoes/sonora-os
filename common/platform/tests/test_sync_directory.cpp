// SPDX-License-Identifier: GPL-3.0-or-later
//
// sync_directory on every platform that has a body for it (POSIX and
// Windows): a real folder with a file just created in it syncs, and a
// folder that is not there, or a path that is a file, fails with errno
// saying why. On Windows the first case is the one an ordinary user hits
// with access denied, which must still count as synced (ADR-0008,
// 2026-11-13); session open and template save depend on it.
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sonora_test_sync_directory";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream take(dir / "take.wav", std::ios::binary);
        take << "sonora";
    }

    errno = 0;
    CHECK(sonora::platform::sync_directory(dir.string()));

    errno = 0;
    CHECK(!sonora::platform::sync_directory((dir / "missing").string()));
    CHECK(errno == ENOENT);

    errno = 0;
    CHECK(!sonora::platform::sync_directory((dir / "missing" / "deeper").string()));
    CHECK(errno == ENOENT);

    errno = 0;
    CHECK(!sonora::platform::sync_directory((dir / "take.wav").string()));
    CHECK(errno == ENOTDIR);

    fs::remove_all(dir);
    std::printf("test_sync_directory: ok\n");
    return 0;
}
