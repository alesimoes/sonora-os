// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session folder on disk, through std::filesystem (ADR-0008 P1): the
// folder and "Audio Files" are made when absent and accepted when present,
// a path that is a file is refused rather than recorded into, and the next
// take number counts the files already there.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#include "sonora/session.h"

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

std::unique_ptr<sonora::Session> open_session(const fs::path& dir) {
    sonora::SessionConfig cfg;
    cfg.dir = dir.string();
    cfg.track_count = 1;
    cfg.format = sonora::WavWriter::Format::Float32;
    cfg.ring_seconds = 1.0;
    cfg.playback.chunk_frames = 256;
    cfg.playback.window_seconds = 0.02;
    cfg.playback.pool_chunks = 64;
    cfg.playback.max_block_frames = 256;
    return std::make_unique<sonora::Session>(cfg);
}

void touch(const fs::path& p) { std::ofstream(p).put('x'); }

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "sonora_test_session_dirs";
    std::error_code ec;
    fs::remove_all(root, ec);
    CHECK(fs::create_directories(root, ec));

    // A new folder, nested one level below one that exists.
    {
        const fs::path dir = root / "fresh";
        auto s = open_session(dir);
        CHECK(s->create_dirs());
        CHECK(fs::is_directory(dir / "Audio Files", ec));
        // Again, with both there: accepted.
        CHECK(s->create_dirs());
        CHECK(s->next_free_take() == 1);
    }

    // Takes already on disk count, whatever the track; strays do not.
    {
        const fs::path dir = root / "fresh";
        touch(dir / "Audio Files" / "Audio 1_03.wav");
        touch(dir / "Audio Files" / "Audio 2_05.wav");
        touch(dir / "Audio Files" / "notes_99.wav");
        auto s = open_session(dir);
        CHECK(s->next_free_take() == 6);
    }

    // The session path is a file: no folder, and the take is refused.
    {
        const fs::path file = root / "not_a_folder";
        touch(file);
        auto s = open_session(file);
        CHECK(!s->create_dirs());
        CHECK(fs::is_regular_file(file, ec));
        s->tracks()[0]->set_rec_armed(true);
        std::string error;
        CHECK(!s->start_recording(1, &error));
        CHECK(!error.empty());
        CHECK(s->next_free_take() == 1);
    }

    // "Audio Files" is a file inside a real folder: also refused.
    {
        const fs::path dir = root / "audio_is_file";
        CHECK(fs::create_directories(dir, ec));
        touch(dir / "Audio Files");
        auto s = open_session(dir);
        CHECK(!s->create_dirs());
    }

    fs::remove_all(root, ec);
    std::printf("test_session_dirs: ok\n");
    return 0;
}
