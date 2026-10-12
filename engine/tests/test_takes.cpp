// SPDX-License-Identifier: GPL-3.0-or-later
//
// A take on disk is never overwritten.
//
// main.cpp started counting takes at 1 on every launch and the writer opened
// with fopen("wb"). sonora-session.service restarts the engine whenever it
// exits -- the operator pressing q, the audio server going away -- so the
// first take after any restart truncated "Audio N_01.wav" from the session
// before, silently, on every armed track.
#include <dirent.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "sonora/command.h"
#include "sonora/session.h"
#include "sonora/session_commands.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

bool exists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

bool write_text(const std::string& path, const char* text) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fputs(text, f);
    return std::fclose(f) == 0;
}

std::string slurp(const std::string& path) {
    std::string out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// A failed earlier run leaves its files behind, and a leftover take is
// exactly what these tests are about, so every test starts from nothing.
void wipe(const std::string& dir) {
    const std::string audio = dir + "/Audio Files";
    if (DIR* d = ::opendir(audio.c_str())) {
        std::vector<std::string> names;
        while (const dirent* e = ::readdir(d)) names.push_back(e->d_name);
        ::closedir(d);
        for (const std::string& n : names) {
            if (n != "." && n != "..") std::remove((audio + "/" + n).c_str());
        }
    }
    ::rmdir(audio.c_str());
    ::rmdir(dir.c_str());
}

sonora::SessionConfig config(const char* dir, int tracks) {
    sonora::SessionConfig cfg;
    cfg.dir = dir;
    cfg.track_count = tracks;
    cfg.ring_seconds = 0.01;
    return cfg;
}

int test_next_free_take() {
    const sonora::SessionConfig cfg = config("test_takes_scan", 2);
    wipe(cfg.dir);
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        const std::string audio = cfg.dir + "/Audio Files/";
        CHECK(s.next_free_take() == 1);

        CHECK(write_text(audio + "Audio 1_01.wav", "x"));
        CHECK(s.next_free_take() == 2);

        // Any track's take counts: a take number is shared by every track,
        // and track 3 may have been armed when track 1 was not.
        CHECK(write_text(audio + "Audio 3_07.wav", "x"));
        CHECK(s.next_free_take() == 8);

        // Names that are not ours do not move the counter.
        for (const char* name : {"notes_99.txt", "Audio 1_x9.wav", "Audio 2_.wav",
                                 "Audio 1_12.wav.bak"}) {
            CHECK(write_text(audio + name, "x"));
        }
        CHECK(s.next_free_take() == 8);
    }
    wipe(cfg.dir);
    return 0;
}

int test_restart_never_overwrites() {
    const sonora::SessionConfig cfg = config("test_takes_restart", 2);
    const std::string first = cfg.dir + "/Audio Files/Audio 1_01.wav";
    const std::string second = cfg.dir + "/Audio Files/Audio 1_02.wav";
    wipe(cfg.dir);

    // The take an operator cares about.
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        s.tracks()[0]->set_rec_armed(true);
        CHECK(s.next_free_take() == 1);
        s.disk().start();
        CHECK(s.start_recording(1));
        const float block[64] = {0.25f};
        const float* inputs[2] = {block, block};
        s.process(inputs, 64);
        s.stop();
        CHECK(s.tracks()[0]->writer().frames_written() == 64);
    }
    const std::string before = slurp(first);
    CHECK(before.size() > 64 * 3);

    // The engine comes back up, as systemd brings it back.
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        s.tracks()[0]->set_rec_armed(true);
        CHECK(s.next_free_take() == 2);

        // Asked outright for the number that is taken, the session refuses
        // and says which file stood in the way.
        std::string why;
        CHECK(!s.start_recording(1, &why));
        CHECK(why.find("Audio 1_01.wav") != std::string::npos);
        CHECK(s.transport().state() == sonora::TransportState::Stopped);
        CHECK(slurp(first) == before);

        // A stale counter in the caller -- what main.cpp's "take = 1" was --
        // still lands on a free number rather than on the old take.
        sonora::CommandRegistry reg;
        int take = 1;
        bool quit = false;
        sonora::install_session_commands(reg, s, take, quit);
        s.disk().start();
        CHECK(reg.dispatch("transport.record") == sonora::DispatchResult::Ok);
        CHECK(take == 2);
        CHECK(reg.dispatch("transport.stop") == sonora::DispatchResult::Ok);
        CHECK(exists(second));
        CHECK(slurp(first) == before);

        // A refused take reaches the operator as a reason, not as "the
        // command failed".
        s.tracks()[0]->set_rec_armed(false);
        std::string why_not;
        CHECK(reg.dispatch("transport.record", {}, why_not) ==
              sonora::DispatchResult::Failed);
        CHECK(why_not == "no track is armed");
    }
    wipe(cfg.dir);
    return 0;
}

// One track's number being taken fails the whole take, and leaves nothing of
// it behind: a take that silently lacks a track is not what was armed.
int test_partial_take_is_refused() {
    const sonora::SessionConfig cfg = config("test_takes_partial", 2);
    const std::string audio = cfg.dir + "/Audio Files/";
    wipe(cfg.dir);
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        CHECK(write_text(audio + "Audio 2_01.wav", "someone else's take"));
        s.tracks()[0]->set_rec_armed(true);
        s.tracks()[1]->set_rec_armed(true);

        std::string why;
        CHECK(!s.start_recording(1, &why));
        CHECK(why.find("Audio 2_01.wav") != std::string::npos);
        CHECK(s.transport().state() == sonora::TransportState::Stopped);
        CHECK(!s.tracks()[0]->writer().is_open());
        CHECK(!exists(audio + "Audio 1_01.wav"));
        CHECK(slurp(audio + "Audio 2_01.wav") == "someone else's take");
    }
    wipe(cfg.dir);
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += test_next_free_take();
    failures += test_restart_never_overwrites();
    failures += test_partial_take_is_refused();
    if (failures != 0) {
        std::fprintf(stderr, "FAIL %d take tests\n", failures);
        return 1;
    }
    std::printf("test_takes: ok\n");
    return 0;
}
