// SPDX-License-Identifier: GPL-3.0-or-later
//
// The disk thread checkpoints every open take on a schedule (ADR-0009
// Consequences, "Crash recovery has a stated loss window"): the file's
// header is patched to what has been written and the data is synced, so a
// reader sees the take grow while it is still recording. Before this, a
// take read as empty until it was closed.
//
// The kill -9 half of the story is tests/posix/test_take_kill.cpp.
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "sonora/session.h"
#include "sonora/wav_reader.h"

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

int64_t frames_on_disk(const std::string& path) {
    sonora::WavReader r;
    std::string error;
    return r.open(path, error) ? r.frames() : -1;
}

// What the injected syncs below see. Plain globals: WavWriter::SyncData is
// a function pointer, and these tests drive the disk thread's work from one
// thread.
sonora::Session* g_session = nullptr;
const sonora::backend::AudioBlock* g_block = nullptr;
int g_syncs = 0;
std::size_t g_unread_at_second_sync = 0;

// A device that reports the data could not be kept, as fdatasync does
// with EIO.
bool failing_sync(std::FILE*) {
    errno = EIO;
    return false;
}

// The first sync is slow: the audio thread delivers a block meanwhile. The
// second records what that block left in the rings by then.
bool slow_sync(std::FILE* f) {
    ++g_syncs;
    if (g_syncs == 1) {
        static_cast<sonora::backend::ProcessCallback&>(*g_session).process(*g_block);
    } else if (g_syncs == 2) {
        g_unread_at_second_sync = 0;
        for (auto& t : g_session->tracks()) g_unread_at_second_sync += t->capture_ring().read_space();
    }
    return std::fflush(f) == 0;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_take_checkpoint";
    fs::remove_all(dir);
    sonora::SessionConfig cfg;
    cfg.dir = dir.string();
    cfg.track_count = 2;
    cfg.format = sonora::WavWriter::Format::Int24;
    cfg.ring_seconds = 1.0;
    cfg.sync_seconds = 0.02;
    cfg.playback.chunk_frames = 256;
    cfg.playback.window_seconds = 0.02;
    cfg.playback.pool_chunks = 64;
    cfg.playback.max_block_frames = 256;
    int64_t seen_mid_take = 0;
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        for (auto& t : s.tracks()) t->set_rec_armed(true);
        s.disk().start();
        std::string error;
        CHECK(s.start_recording(1, &error));

        std::vector<float> in(kBlock, 0.25f);
        const float* ptrs[2] = {in.data(), in.data()};
        sonora::backend::AudioBlock b{};
        b.inputs = ptrs;
        b.input_count = 2;
        b.nframes = kBlock;
        const std::string take = (dir / "Audio Files" / "Audio 2_01.wav").string();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < until) {
            static_cast<sonora::backend::ProcessCallback&>(s).process(b);
            std::this_thread::sleep_for(std::chrono::microseconds(1300));
            seen_mid_take = frames_on_disk(take);
            if (seen_mid_take >= 4800) break;
        }
        // Still recording: only a checkpoint can have written that header.
        CHECK(s.recording_take() == 1);
        CHECK(seen_mid_take >= 4800);
        CHECK(s.disk().checkpoints() > 0);
        std::printf("longest checkpoint: %.3f ms over %u passes\n",
                    static_cast<double>(s.disk().longest_checkpoint_ns()) / 1e6,
                    static_cast<unsigned>(s.disk().checkpoints()));
        CHECK(s.stop(&error));
        CHECK(s.tracks()[1]->overruns() == 0);
        CHECK(frames_on_disk(take) >= seen_mid_take);
    }
    // --- every drain pass hands its audio to the OS ---
    //
    // A kill -9 loses what the process holds, and stdio held up to a
    // buffer of each take (a write smaller than it stays in the process),
    // so a killed take lost up to that much past the last checkpoint. A
    // pass now leaves nothing of its audio in the process.
    fs::remove_all(dir);
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        s.tracks()[0]->set_rec_armed(true);
        std::string error;
        CHECK(s.start_recording(1, &error));
        std::vector<float> in(kBlock, 0.25f);
        const float* ptrs[2] = {in.data(), in.data()};
        sonora::backend::AudioBlock b{};
        b.inputs = ptrs;
        b.input_count = 2;
        b.nframes = kBlock;
        for (int i = 0; i < 3; ++i) static_cast<sonora::backend::ProcessCallback&>(s).process(b);
        s.disk().drain_once();
        const std::string take = (dir / "Audio Files" / "Audio 1_01.wav").string();
        // 24-bit mono: the header, then 3 bytes a frame. No checkpoint ran.
        const uint64_t header = 12 + (8 + 602) + (8 + 16) + 8;
        CHECK(s.disk().checkpoints() == 0);
        CHECK(fs::file_size(take) == header + 3 * kBlock * 3);
        CHECK(s.stop(&error));
    }

    // --- a checkpoint pass drains between the files it syncs ---
    //
    // Syncing every open take back to back kept the rings from draining for
    // the sum of the syncs: 18 tracks on a slow stick come close to the
    // ring's depth. A pass drains after each file, so the rings wait for one
    // sync at a time.
    fs::remove_all(dir);
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        for (auto& t : s.tracks()) t->set_rec_armed(true);
        std::string error;
        CHECK(s.start_recording(1, &error));
        std::vector<float> in(kBlock, 0.25f);
        const float* ptrs[2] = {in.data(), in.data()};
        sonora::backend::AudioBlock b{};
        b.inputs = ptrs;
        b.input_count = 2;
        b.nframes = kBlock;
        static_cast<sonora::backend::ProcessCallback&>(s).process(b);
        s.disk().drain_once();
        g_session = &s;
        g_block = &b;
        g_syncs = 0;
        for (auto& t : s.tracks()) {
            for (int slot = 0; slot < sonora::Track::kWriterSlots; ++slot) {
                t->writer(slot).set_sync(&slow_sync);
            }
        }
        s.disk().checkpoint_all();
        CHECK(g_syncs == 2);
        CHECK(g_unread_at_second_sync == 0);
        CHECK(s.take_failure().empty());
        CHECK(s.stop(&error));
        CHECK(frames_on_disk((dir / "Audio Files" / "Audio 2_01.wav").string()) == 2 * kBlock);
        g_session = nullptr;
        g_block = nullptr;
    }

    // --- a failed sync fails the take, and the session says so ---
    //
    // Linux clears the error a failed fdatasync reports, so the take is lost
    // from there; the operator must hear it while still recording, not at
    // the stop.
    fs::remove_all(dir);
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        s.tracks()[1]->set_rec_armed(true);
        std::string error;
        CHECK(s.start_recording(1, &error));
        std::vector<float> in(kBlock, 0.25f);
        const float* ptrs[2] = {in.data(), in.data()};
        sonora::backend::AudioBlock b{};
        b.inputs = ptrs;
        b.input_count = 2;
        b.nframes = kBlock;
        static_cast<sonora::backend::ProcessCallback&>(s).process(b);
        s.disk().drain_once();
        for (int slot = 0; slot < sonora::Track::kWriterSlots; ++slot) {
            s.tracks()[1]->writer(slot).set_sync(&failing_sync);
        }
        CHECK(s.take_failure().empty());
        s.disk().checkpoint_all();
        const std::string failure = s.take_failure();
        CHECK(failure.find("Audio 2_01.wav") != std::string::npos);
        CHECK(failure.find(std::strerror(EIO)) != std::string::npos);
        CHECK(s.recording_take() == 1);
        CHECK(!s.stop(&error));
        CHECK(error.find(std::strerror(EIO)) != std::string::npos);
    }

    fs::remove_all(dir);
    std::printf("test_take_checkpoint: ok\n");
    return 0;
}
