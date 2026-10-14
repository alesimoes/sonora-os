// SPDX-License-Identifier: GPL-3.0-or-later
//
// The disk thread checkpoints every open take on a schedule (ADR-0009
// Consequences, "Crash recovery has a stated loss window"): the file's
// header is patched to what has been written and the data is synced, so a
// reader sees the take grow while it is still recording. Before this, a
// take read as empty until it was closed.
//
// The kill -9 half of the story is tests/posix/test_take_kill.cpp.
#include <chrono>
#include <cstdio>
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
        std::printf("longest checkpoint pass: %.3f ms over %u passes\n",
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

    fs::remove_all(dir);
    std::printf("test_take_checkpoint: ok\n");
    return 0;
}
