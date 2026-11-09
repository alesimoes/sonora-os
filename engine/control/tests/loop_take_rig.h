// SPDX-License-Identifier: GPL-3.0-or-later
//
// The loop-take test rig shared by test_loop_take.cpp and
// test_loop_take_late.cpp: a real Session over a temporary directory, fed
// blocks of 0.25 on every armed track, with the disk thread drained by hand
// so a test decides when an Open or a Wrap record finds the ring.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "sonora/closed_take.h"
#include "sonora/session.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace loop_take_test {

namespace fs = std::filesystem;
using namespace sonora;

inline bool mentions(const std::string& haystack, const std::string& needle) {
    if (haystack.find(needle) != std::string::npos) return true;
    std::fprintf(stderr, "  expected \"%s\" in: %s\n", needle.c_str(), haystack.c_str());
    return false;
}

inline bool any_mentions(const std::vector<std::string>& lines, const std::string& needle) {
    for (const std::string& l : lines) {
        if (l.find(needle) != std::string::npos) return true;
    }
    std::fprintf(stderr, "  expected \"%s\" in one of %zu notices:\n", needle.c_str(), lines.size());
    for (const std::string& l : lines) std::fprintf(stderr, "    %s\n", l.c_str());
    return false;
}

// A = 2000, B = 4000: two blocks of max_block_frames (1024) and more.
constexpr uint64_t kA = 2000, kB = 4000, kLen = kB - kA;

struct Rig {
    fs::path dir;
    SessionConfig cfg;
    std::unique_ptr<Session> session;
    std::vector<float> in;
    std::vector<const float*> inputs;

    Rig(const char* name, int tracks, uint32_t boundary_records = 0, uint32_t cap = 99) {
        dir = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(dir, ec);
        cfg.dir = dir.string();
        cfg.track_count = tracks;
        cfg.ring_seconds = 1.0;
        cfg.boundary_records = boundary_records;
        cfg.loop_pass_cap = cap;
        session = std::make_unique<Session>(cfg);
        session->create_dirs();
        for (auto& t : session->tracks()) t->set_rec_armed(true);
        in.assign(1024, 0.25f);
        inputs.assign(static_cast<std::size_t>(tracks), in.data());
    }
    ~Rig() {
        session.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    void block(uint32_t frames, bool drain = true) {
        backend::AudioBlock b{};
        b.inputs = inputs.data();
        b.input_count = static_cast<uint32_t>(inputs.size());
        b.nframes = frames;
        session->process(b);
        if (drain) session->disk().drain_once();
    }

    // A loop take of `frames` frames from A - pre_roll, in blocks of `n`.
    bool loop_take(uint64_t pre_roll, uint64_t frames, uint32_t n = 250) {
        LoopRange l;
        l.from = kA;
        l.to = kB;
        l.start = kA - pre_roll;
        std::string why;
        if (!session->start_loop(session->next_free_take(), l, &why)) {
            std::fprintf(stderr, "  %s\n", why.c_str());
            return false;
        }
        for (uint64_t done = 0; done < frames; done += n) block(n);
        return session->stop();
    }

    Session::ClosedTake take() {
        std::vector<Session::ClosedTake> closed = session->closed_takes();
        return closed.size() == 1 ? closed[0] : Session::ClosedTake{};
    }
};

inline const model::Playlist* active(const model::Snapshot& s, std::size_t track) {
    const model::TrackEdit& t = *s.edit->tracks[track];
    for (const auto& p : t.playlists) {
        if (p->id == t.active_playlist) return p.get();
    }
    return nullptr;
}

}  // namespace loop_take_test
