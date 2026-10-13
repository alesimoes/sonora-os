// SPDX-License-Identifier: GPL-3.0-or-later
//
// A take file open for writing must not leak into a child process: the
// engine starts sonora-tui while a take may be recording, and a child that
// held the descriptor would keep the file open after the engine closed it,
// and could write into it. The take is opened close-on-exec, still
// exclusively. POSIX only: it reads the child's /proc/self/fd.
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "sonora/platform/file.h"
#include "sonora/platform/process.h"
#include "sonora/wav_writer.h"

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

using namespace sonora;

namespace {

// What the descriptors of a shell started now point at, one per line.
std::string child_descriptors(const std::string& out) {
    platform::ChildProcess::Options o;
    o.stdout_path = out;
    std::string error;
    auto child = platform::ChildProcess::spawn(
        {"/bin/sh", "-c", "for f in /proc/$$/fd/*; do readlink \"$f\"; done"}, o, error);
    if (!child) {
        std::fprintf(stderr, "  spawn: %s\n", error.c_str());
        return {};
    }
    child->wait(-1);
    std::ifstream in(out);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

}  // namespace

int main() {
    const std::string base = "/tmp/sonora_test_cloexec_" + std::to_string(::getpid());
    const std::string take = base + "_take.wav";
    const std::string raw = base + "_raw.bin";
    const std::string out = base + "_fds.txt";
    std::remove(take.c_str());
    std::remove(raw.c_str());

    WavWriter w;
    CHECK(w.open(take, 48000, 2, WavWriter::Format::Int24, 0));
    std::FILE* f = platform::create_exclusive(raw);
    CHECK(f != nullptr);

    const std::string seen = child_descriptors(out);
    CHECK(!seen.empty());
    CHECK(seen.find(take) == std::string::npos);
    CHECK(seen.find(raw) == std::string::npos);

    // Still exclusive: an existing name is refused with EEXIST, untouched.
    errno = 0;
    CHECK(platform::create_exclusive(raw) == nullptr && errno == EEXIST);
    WavWriter again;
    errno = 0;
    CHECK(!again.open(take, 48000, 2, WavWriter::Format::Int24, 0) && errno == EEXIST);

    if (f != nullptr) std::fclose(f);
    w.close();
    std::remove(take.c_str());
    std::remove(raw.c_str());
    std::remove(out.c_str());
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("wav writer close-on-exec: ok\n");
    return 0;
}
