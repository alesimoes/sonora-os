// SPDX-License-Identifier: GPL-3.0-or-later
//
// A loop take's wrap list (ADR-0016 ยง2): written append-only next to the
// take, read back whole, a torn last record dropped, and never created over
// a file that exists.
#include "sonora/wrap_sidecar.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

namespace fs = std::filesystem;
using sonora::WrapList;
using sonora::WrapSidecar;

fs::path scratch(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_wrap_sidecar";
    fs::create_directories(dir);
    const fs::path p = dir / name;
    fs::remove(p);
    return p;
}

int test_round_trip() {
    const std::string path = scratch("Audio 1_03.wav.wraps").string();
    WrapSidecar w;
    CHECK(w.open(path, 48000, 96000, 37));
    CHECK(w.append(144000));
    CHECK(w.append(WrapSidecar::kLostWrap));
    CHECK(w.append(240000));
    CHECK(w.checkpoint());
    CHECK(w.close());
    CHECK(fs::file_size(path) == WrapSidecar::kHeaderBytes + 3 * 8);

    WrapList list;
    std::string error;
    CHECK(read_wrap_sidecar(path, list, error));
    CHECK(list.loop_from == 48000 && list.loop_to == 96000);
    // ADR-0016 ง2: the list says which L its wraps carry, so recovery of an
    // orphan, whose bext has none, does not need it from its caller.
    CHECK(list.latency == 37);
    CHECK((list.wraps == std::vector<uint64_t>{144000, WrapSidecar::kLostWrap, 240000}));
    return 0;
}

// What a kill -9 between the two halves of a write leaves: the whole
// records still read, the torn one does not.
int test_torn_record_is_dropped() {
    const std::string path = scratch("torn.wraps").string();
    WrapSidecar w;
    CHECK(w.open(path, 0, 4800, 0));
    CHECK(w.append(4800));
    CHECK(w.close());
    std::FILE* f = std::fopen(path.c_str(), "ab");
    CHECK(f != nullptr);
    const unsigned char half[3] = {1, 2, 3};
    CHECK(std::fwrite(half, 1, sizeof half, f) == sizeof half);
    std::fclose(f);
    WrapList list;
    std::string error;
    CHECK(read_wrap_sidecar(path, list, error));
    CHECK((list.wraps == std::vector<uint64_t>{4800}));
    return 0;
}

int test_never_over_a_file() {
    const std::string path = scratch("taken.wraps").string();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    std::fputs("keep", f);
    std::fclose(f);
    WrapSidecar w;
    CHECK(!w.open(path, 0, 4800, 0));
    CHECK(errno == EEXIST);
    CHECK(fs::file_size(path) == 4);
    return 0;
}

int test_not_a_wrap_list() {
    WrapList list;
    std::string error;
    CHECK(!read_wrap_sidecar(scratch("missing.wraps").string(), list, error));
    const std::string path = scratch("other.wraps").string();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    std::fputs("RIFF and then some more bytes", f);
    std::fclose(f);
    error.clear();
    CHECK(!read_wrap_sidecar(path, list, error));
    CHECK(error.find("not a wrap list") != std::string::npos);
    return 0;
}

// Version 2 adds kBeforeFile, a late track's wraps before its file. A
// version 1 list, which never held one, reads as before; a later version
// is refused.
int test_versions() {
    const std::string path = scratch("v1.wraps").string();
    WrapSidecar w;
    CHECK(w.open(path, 0, 4800, 0));
    CHECK(w.append(WrapSidecar::kBeforeFile));
    CHECK(w.append(4000));
    CHECK(w.close());
    WrapList list;
    std::string error;
    CHECK(read_wrap_sidecar(path, list, error));
    CHECK((list.wraps == std::vector<uint64_t>{WrapSidecar::kBeforeFile, 4000}));
    const auto set_version = [&](unsigned char v) {
        std::FILE* f = std::fopen(path.c_str(), "r+b");
        if (f == nullptr) return false;
        const unsigned char le[4] = {v, 0, 0, 0};
        const bool ok = std::fseek(f, 4, SEEK_SET) == 0 && std::fwrite(le, 1, 4, f) == 4;
        std::fclose(f);
        return ok;
    };
    CHECK(set_version(1));
    CHECK(read_wrap_sidecar(path, list, error));
    CHECK(list.wraps.size() == 2);
    CHECK(set_version(3));
    CHECK(!read_wrap_sidecar(path, list, error));
    CHECK(error.find("version 3") != std::string::npos);
    return 0;
}

}  // namespace

int main() {
    if (test_round_trip()) return 1;
    if (test_torn_record_is_dropped()) return 1;
    if (test_never_over_a_file()) return 1;
    if (test_not_a_wrap_list()) return 1;
    if (test_versions()) return 1;
    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / "sonora_test_wrap_sidecar", ec);
    std::printf("wrap_sidecar: ok\n");
    return 0;
}
