// SPDX-License-Identifier: GPL-3.0-or-later
//
// free_bytes() and read_only(), which the engine reads when it enters a
// session folder (ADR-0018 §8): a full or read-only medium has to be seen
// before a take starts, not partway through it. rename_dir_durably() and
// same_filesystem(), with which an untitled session's first Save names its
// folder (ADR-0018 §3).
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

// rename(2) replaces an empty folder that is in the way; the first Save
// must never put a session's takes in place of another folder.
int test_rename_dir() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "sonora_test_medium_rename";
    fs::remove_all(root);
    fs::create_directories(root / "Untitled" / "Audio Files");
    std::FILE* f = std::fopen((root / "Untitled" / "Audio Files" / "Audio 1_01.wav").c_str(), "wb");
    CHECK(f != nullptr);
    std::fclose(f);
    CHECK(sonora::platform::same_filesystem((root / "Untitled").string(), root.string()));
    CHECK(!sonora::platform::same_filesystem((root / "missing").string(), root.string()));

    std::string error;
    CHECK(sonora::platform::rename_dir_durably((root / "Untitled").string(), (root / "Mix").string(),
                                               error));
    CHECK(error.empty());
    CHECK(!fs::exists(root / "Untitled"));
    CHECK(fs::is_regular_file(root / "Mix" / "Audio Files" / "Audio 1_01.wav"));

    fs::create_directories(root / "Taken");
    CHECK(!sonora::platform::rename_dir_durably((root / "Mix").string(), (root / "Taken").string(),
                                                error));
    CHECK(!error.empty());
    CHECK(fs::is_regular_file(root / "Mix" / "Audio Files" / "Audio 1_01.wav"));
    CHECK(fs::is_empty(root / "Taken"));
    error.clear();
    CHECK(!sonora::platform::rename_dir_durably((root / "Gone").string(),
                                                (root / "Elsewhere").string(), error));
    CHECK(!error.empty());
    fs::remove_all(root);
    return 0;
}

// The caution names the folder whose sync failed, with that sync's errno.
// "<root>/Late/../Late" renames <root>/Late, and once it has moved, its own
// spelling of its parent no longer opens, while <root> still syncs.
int test_rename_dir_names_the_folder_that_did_not_sync() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "sonora_test_medium_rename_sync";
    fs::remove_all(root);
    fs::create_directories(root / "Late");
    const std::string from = (root / "Late").string() + "/../Late";
    std::string error;
    CHECK(sonora::platform::rename_dir_durably(from, (root / "Named").string(), error));
    CHECK(fs::is_directory(root / "Named"));
    const std::string expected = "renamed, but " + (root / "Late").string() +
                                 "/.. could not be synced: " + std::strerror(ENOENT);
    if (error != expected) std::fprintf(stderr, "caution: %s\n", error.c_str());
    CHECK(error == expected);
    fs::remove_all(root);
    return 0;
}

}  // namespace

int main() {
    if (test_free_bytes() != 0) return 1;
    if (test_read_only() != 0) return 1;
    if (test_rename_dir() != 0) return 1;
    if (test_rename_dir_names_the_folder_that_did_not_sync() != 0) return 1;
    std::printf("platform medium: ok\n");
    return 0;
}
