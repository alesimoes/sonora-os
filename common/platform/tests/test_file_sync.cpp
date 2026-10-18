// SPDX-License-Identifier: GPL-3.0-or-later
//
// sync_data and sync_directory, the disk thread's checkpoint (ADR-0009
// Consequences, "Crash recovery has a stated loss window"). What reaches the
// medium cannot be observed from here; what can is that a regular file and
// a directory sync, that a sync flushes what stdio was holding, and that a
// failure says why.
#include <cerrno>
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

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sonora_test_file_sync";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "take.wav").string();

    std::FILE* f = sonora::platform::create_exclusive(path);
    CHECK(f != nullptr);
    // Fully buffered and smaller than the buffer: only the sync's flush can
    // put it in the file.
    CHECK(std::fwrite("sonora", 1, 6, f) == 6);
    CHECK(sonora::platform::sync_data(f));
    CHECK(fs::file_size(path) == 6);
    CHECK(sonora::platform::sync_directory(dir.string()));
    std::fclose(f);

    errno = 0;
    CHECK(!sonora::platform::sync_directory((dir / "missing").string()));
    CHECK(errno == ENOENT);

    fs::remove_all(dir);
    std::printf("test_file_sync: ok\n");
    return 0;
}
