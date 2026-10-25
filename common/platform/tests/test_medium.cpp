// SPDX-License-Identifier: GPL-3.0-or-later
//
// free_bytes() and read_only(), which the engine reads when it enters a
// session folder (ADR-0018 §8): a full or read-only medium has to be seen
// before a take starts, not partway through it.
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cstdint>
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

int test_free_bytes() {
    const std::string dir = std::filesystem::temp_directory_path().string() + "/sonora_test_medium";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    uint64_t bytes = 0;
    CHECK(sonora::platform::free_bytes(dir, bytes));
    struct statvfs vfs {};
    CHECK(::statvfs(dir.c_str(), &vfs) == 0);
    // Not f_bfree: root's reserve is not ours. Equal unless another
    // process writes between the two calls, so within a megabyte.
    const uint64_t expected = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    CHECK(bytes + (1u << 20) >= expected && bytes <= expected + (1u << 20));

    uint64_t untouched = 42;
    CHECK(!sonora::platform::free_bytes(dir + "/missing/deeper", untouched));
    CHECK(untouched == 42);
    std::filesystem::remove_all(dir);
    return 0;
}

int test_read_only() {
    const std::string dir = std::filesystem::temp_directory_path().string() + "/sonora_test_medium_ro";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    CHECK(!sonora::platform::read_only(dir));
    CHECK(::chmod(dir.c_str(), 0555) == 0);
    // Root writes through any permission bit, so only a mount can make a
    // folder read-only to it; the container build runs as root.
    if (::geteuid() != 0) CHECK(sonora::platform::read_only(dir));
    CHECK(::chmod(dir.c_str(), 0755) == 0);
    CHECK(!sonora::platform::read_only(dir));
    // A folder that is not there cannot be written either.
    CHECK(sonora::platform::read_only(dir + "/missing"));
    std::filesystem::remove_all(dir);
    return 0;
}

}  // namespace

int main() {
    if (test_free_bytes() != 0) return 1;
    if (test_read_only() != 0) return 1;
    std::printf("platform medium: ok\n");
    return 0;
}
