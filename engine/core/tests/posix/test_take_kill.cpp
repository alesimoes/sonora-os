// SPDX-License-Identifier: GPL-3.0-or-later
//
// A take survives kill -9 of the engine (BACKLOG DAW 1, take safety;
// session-format.md, "Crash recovery"). A child process records one track,
// its disk thread running as in the engine; the parent kills it with
// SIGKILL mid-take, with no chance to close anything.
//
// Before the periodic checkpoint the file's sizes were the zeros open()
// wrote, so the take read as empty however much of it was on disk. Now the
// header the child left is valid as it stands, and recover_take() extends
// it to everything that reached the file. Every sample is its own timeline
// frame, so a lost, repeated or misplaced block is a wrong value.
//
// POSIX only: fork and kill (ADR-0008 V3).
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "sonora/session.h"
#include "sonora/wav_reader.h"
#include "sonora/wav_recovery.h"

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
constexpr uint32_t kBlock = 64;

sonora::SessionConfig config(const fs::path& dir) {
    sonora::SessionConfig cfg;
    cfg.dir = dir.string();
    cfg.track_count = 1;
    cfg.format = sonora::WavWriter::Format::Float32;
    cfg.ring_seconds = 1.0;
    // Long enough that the kill lands well inside an interval, with audio
    // past the last checkpoint on disk for recovery to find.
    cfg.sync_seconds = 0.5;
    cfg.playback.chunk_frames = 256;
    cfg.playback.window_seconds = 0.02;
    cfg.playback.pool_chunks = 64;
    cfg.playback.max_block_frames = 256;
    return cfg;
}

// The child: records until it is killed. Never returns. `produced` is the
// playhead after each callback: every frame up to it is in a capture ring.
[[noreturn]] void record_forever(const fs::path& dir, std::atomic<uint64_t>* produced) {
    sonora::Session s(config(dir));
    s.create_dirs();
    s.tracks()[0]->set_rec_armed(true);
    s.disk().start();
    std::string error;
    if (!s.start_recording(1, &error)) {
        std::fprintf(stderr, "child: %s\n", error.c_str());
        _exit(2);
    }
    std::vector<float> in(kBlock);
    const float* ptrs[1] = {in.data()};
    for (;;) {
        const uint64_t at = s.transport().playhead();
        for (uint32_t i = 0; i < kBlock; ++i) in[i] = static_cast<float>(at + i);
        sonora::backend::AudioBlock b{};
        b.inputs = ptrs;
        b.input_count = 1;
        b.nframes = kBlock;
        static_cast<sonora::backend::ProcessCallback&>(s).process(b);
        produced->store(s.transport().playhead(), std::memory_order_release);
        // About real time at 48 kHz.
        std::this_thread::sleep_for(std::chrono::microseconds(1300));
    }
}

struct Seen {
    bool ok = false;
    int64_t frames = 0;
    uint64_t time_reference = 0;
};

// What a reader makes of the file as it is: each sample must be
// TimeReference + its index.
Seen read_take(const std::string& path) {
    Seen seen;
    sonora::WavReader r;
    std::string error;
    if (!r.open(path, error)) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return seen;
    }
    seen.frames = r.frames();
    seen.time_reference = r.time_reference();
    std::vector<float> s(static_cast<std::size_t>(seen.frames));
    if (seen.frames > 0 && r.read(0, seen.frames, s.data()) != seen.frames) return seen;
    for (int64_t i = 0; i < seen.frames; ++i) {
        if (s[static_cast<std::size_t>(i)] != static_cast<float>(r.time_reference() + i)) {
            std::fprintf(stderr, "  sample %lld is %.0f, TimeReference + index is %llu\n",
                         static_cast<long long>(i), static_cast<double>(s[i]),
                         static_cast<unsigned long long>(r.time_reference() + i));
            return seen;
        }
    }
    seen.ok = true;
    return seen;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_take_kill";
    fs::remove_all(dir);
    const std::string take = (dir / "Audio Files" / "Audio 1_01.wav").string();

    // Shared with the child, so what it produced is known after it dies.
    void* shared = ::mmap(nullptr, sizeof(std::atomic<uint64_t>), PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(shared != MAP_FAILED);
    auto* produced = new (shared) std::atomic<uint64_t>(0);

    // Forked before this process has any thread, so the child is whole.
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) record_forever(dir, produced);

    // Until the first checkpoint shows, then about half an interval: the
    // kill lands between checkpoints.
    Seen before;
    for (int i = 0; i < 1000; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        before = read_take(take);
        if (before.ok && before.frames > 0) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(::kill(child, SIGKILL) == 0);
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    const uint64_t end = produced->load(std::memory_order_acquire);

    // As the engine left it: valid already, up to its last checkpoint.
    const Seen killed = read_take(take);
    std::printf("header after kill -9: %lld frames\n", static_cast<long long>(killed.frames));
    CHECK(killed.ok);
    CHECK(killed.frames >= 9600);

    // The next open: everything that reached the file, in whole frames.
    const sonora::TakeRecovery r = sonora::recover_take(take);
    CHECK(r.outcome == sonora::TakeRecovery::Outcome::Repaired);
    CHECK(r.header_frames == static_cast<uint64_t>(killed.frames));
    const Seen recovered = read_take(take);
    CHECK(recovered.ok);
    CHECK(static_cast<uint64_t>(recovered.frames) == r.frames);
    CHECK(recovered.frames > killed.frames);
    // What died with the process: the rings' undrained part, at most a
    // pass (10 ms) and its jitter. Not a stdio buffer of the file, and not
    // the audio since the last checkpoint. A tenth of a second bounds it
    // with room for a loaded or sanitized run.
    const int64_t lost =
        static_cast<int64_t>(end) - static_cast<int64_t>(recovered.time_reference + recovered.frames);
    std::printf("recovered: %lld frames; lost to kill -9: %lld frames (%.1f ms)\n",
                static_cast<long long>(recovered.frames), static_cast<long long>(lost),
                static_cast<double>(lost) / 48.0);
    CHECK(lost >= 0);
    CHECK(lost <= 4800);
    // Nothing left to do a second time.
    CHECK(sonora::recover_take(take).outcome == sonora::TakeRecovery::Outcome::Intact);

    fs::remove_all(dir);
    std::printf("test_take_kill: ok\n");
    return 0;
}
