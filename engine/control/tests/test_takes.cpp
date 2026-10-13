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

#include <cstdint>
#include <cstdio>
#include <cstring>
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
    const sonora::SessionConfig cfg = config("test_takes_scan", 3);
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

// Only names this session writes count. A stray file that merely ends in
// "_<digits>.wav" used to set the counter: Guitar_20261012.wav made the next
// take 20261013, and x_999999999.wav made it 1000000000 -- which the scan, at
// ten digits, then ignored, so after a restart it offered the same number
// again and O_EXCL refused it for ever. The session could not record.
int test_stray_files_do_not_set_the_take() {
    const sonora::SessionConfig cfg = config("test_takes_stray", 2);
    const std::string audio = cfg.dir + "/Audio Files/";
    wipe(cfg.dir);
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        for (const char* name :
             {"Guitar_20261012.wav", "x_999999999.wav", "Audio 1_1.wav",
              "Audio 1_0000000000000000000000007.wav",
              "Audio 1_99999999999999999999999999.wav", "Audio 1_20261012.wav",
              "Audio 1_00.wav", "Audio 0_04.wav", "Audio 03_04.wav",
              "Audio _04.wav", "Audio x_04.wav", "audio 3_04.wav",
              "Guitar 3_04.wav", "Audio 3 _04.wav", "Audio -3_04.wav",
              "Audio 99999999999999999999_04.wav"}) {
            CHECK(write_text(audio + name, "not ours"));
        }
        // "1" and the long runs are not how this session spells a take,
        // 20261012 is past the last take number it would ever write, and
        // none of the others is a track name the session gives out.
        CHECK(s.next_free_take() == 1);
        CHECK(write_text(audio + "Audio 2_05.wav", "ours"));
        CHECK(s.next_free_take() == 6);
    }
    {
        // And it can record, now and after a restart.
        for (int restart = 0; restart < 2; ++restart) {
            sonora::Session s(cfg);
            s.tracks()[0]->set_rec_armed(true);
            const int take = s.next_free_take();
            CHECK(take == 6 + restart);
            std::string why;
            CHECK(s.start_recording(take, &why));
            s.stop();
        }
    }
    wipe(cfg.dir);
    return 0;
}

// A take number belongs to the session, not to the tracks open right now.
// Recorded with 18 tracks and only Audio 12 armed, then reopened with
// --tracks 8, the scan looked only for Audio 1..8 and offered take 01 again.
// O_EXCL kept the old files safe, but two different takes shared a number.
int test_takes_of_tracks_not_open_still_count() {
    const sonora::SessionConfig wide = config("test_takes_narrow", 18);
    const std::string audio = wide.dir + "/Audio Files/";
    wipe(wide.dir);
    {
        sonora::Session s(wide);
        CHECK(s.create_dirs());
        s.tracks()[11]->set_rec_armed(true);
        for (int take = 1; take <= 5; ++take) {
            CHECK(s.next_free_take() == take);
            CHECK(s.start_recording(take));
            s.stop();
        }
        CHECK(exists(audio + "Audio 12_05.wav"));
    }
    {
        sonora::Session s(config("test_takes_narrow", 8));
        CHECK(s.next_free_take() == 6);
        s.tracks()[0]->set_rec_armed(true);
        CHECK(s.start_recording(s.next_free_take()));
        s.stop();
        CHECK(exists(audio + "Audio 1_06.wav"));
        CHECK(!exists(audio + "Audio 1_01.wav"));
    }
    wipe(wide.dir);
    return 0;
}

// The last take number is a stop with a reason, not a refusal with none.
int test_last_take_number() {
    const sonora::SessionConfig cfg = config("test_takes_last", 1);
    const std::string audio = cfg.dir + "/Audio Files/";
    wipe(cfg.dir);
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        s.tracks()[0]->set_rec_armed(true);
        char name[64];
        std::snprintf(name, sizeof name, "Audio 1_%02d.wav",
                      sonora::Session::kMaxTake);
        CHECK(write_text(audio + name, "ours"));
        CHECK(s.next_free_take() == sonora::Session::kMaxTake + 1);
        std::string why;
        CHECK(!s.start_recording(s.next_free_take(), &why));
        CHECK(why.find("new session") != std::string::npos);
        CHECK(s.transport().state() == sonora::TransportState::Stopped);
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

// A take begins with its own audio. Samples the previous take left in a ring
// -- here, a take stopped with no disk thread to drain it -- must never reach
// the head of the next file. Under TSan this is also where the disk thread
// and the interface thread meet over the writer and the ring.
int test_take_starts_with_its_own_samples() {
    const sonora::SessionConfig cfg = config("test_takes_head", 1);
    const std::string audio = cfg.dir + "/Audio Files/";
    wipe(cfg.dir);
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        s.tracks()[0]->set_rec_armed(true);

        float old_block[64];
        for (float& f : old_block) f = 0.25f;
        const float* old_inputs[1] = {old_block};
        CHECK(s.start_recording(1));
        s.process(old_inputs, 64);
        s.stop();  // no disk thread ran: 64 frames are still in the ring

        float new_block[64];
        for (float& f : new_block) f = -0.5f;
        const float* new_inputs[1] = {new_block};
        s.disk().start();
        CHECK(s.start_recording(2));
        s.process(new_inputs, 64);
        s.stop();
        CHECK(s.tracks()[0]->writer().frames_written() == 64);
    }
    // -0.5 in 24-bit little-endian is 0xC00000; 0.25 would be 0x200000.
    const std::string b = slurp(audio + "Audio 1_02.wav");
    const std::size_t data = b.find("data");
    CHECK(data != std::string::npos);
    CHECK(b.size() == data + 8 + 64 * 3);
    for (std::size_t i = data + 8; i < b.size(); i += 3) {
        CHECK(static_cast<unsigned char>(b[i]) == 0x00);
        CHECK(static_cast<unsigned char>(b[i + 1]) == 0x00);
        CHECK(static_cast<unsigned char>(b[i + 2]) == 0xC0);
    }
    wipe(cfg.dir);
    return 0;
}

// A take that hits the WAV size limit is reported as failed when it stops,
// with the file and the reason, and its file is still valid. The writer used
// to wrap the 32-bit sizes past 4 GiB and the disk thread ignored write()
// failures, so nobody heard of it. A small limit stands in for 2^32 - 1.
int test_take_at_the_size_limit_fails_with_a_reason() {
    const sonora::SessionConfig cfg = config("test_takes_limit", 2);
    const std::string audio = cfg.dir + "/Audio Files/";
    constexpr uint32_t kEmptyRiffSize = 4 + (8 + 602) + (8 + 16) + 8;
    wipe(cfg.dir);
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        s.tracks()[0]->set_rec_armed(true);
        s.tracks()[1]->set_rec_armed(true);
        // Only track 2 runs out of room: 10 mono 24-bit frames.
        s.tracks()[1]->writer().set_riff_size_limit(kEmptyRiffSize + 10 * 3);

        sonora::CommandRegistry reg;
        int take = 1;
        bool quit = false;
        sonora::install_session_commands(reg, s, take, quit);
        s.disk().start();
        CHECK(reg.dispatch("transport.record") == sonora::DispatchResult::Ok);
        float block[64];
        for (float& f : block) f = 0.25f;
        const float* inputs[2] = {block, block};
        s.process(inputs, 64);

        std::string why;
        CHECK(reg.dispatch("transport.stop", {}, why) ==
              sonora::DispatchResult::Failed);
        CHECK(why.find("Audio 2_01.wav") != std::string::npos);
        CHECK(why.find("take reached the 4 GB WAV limit") != std::string::npos);
        CHECK(why.find("Audio 1_01.wav") == std::string::npos);
        // Stopped all the same, and ready for the next take.
        CHECK(s.transport().state() == sonora::TransportState::Stopped);
        CHECK(take == 2);
        CHECK(s.tracks()[0]->writer().frames_written() == 64);
        CHECK(s.tracks()[1]->writer().frames_written() == 10);

        // The next take starts clean, and stop() reports nothing for it.
        s.tracks()[1]->writer().set_riff_size_limit(
            sonora::WavWriter::kRiffSizeLimit);
        CHECK(s.start_recording(2));
        s.process(inputs, 64);
        std::string none;
        CHECK(s.stop(&none));
        CHECK(none.empty());
    }
    const std::string b = slurp(audio + "Audio 2_01.wav");
    const std::size_t data = b.find("data");
    CHECK(data != std::string::npos);
    CHECK(b.size() == data + 8 + 10 * 3);
    uint32_t riff = 0;
    std::memcpy(&riff, b.data() + 4, 4);
    CHECK(riff == b.size() - 8);
    wipe(cfg.dir);
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += test_next_free_take();
    failures += test_stray_files_do_not_set_the_take();
    failures += test_takes_of_tracks_not_open_still_count();
    failures += test_last_take_number();
    failures += test_restart_never_overwrites();
    failures += test_partial_take_is_refused();
    failures += test_take_starts_with_its_own_samples();
    failures += test_take_at_the_size_limit_fails_with_a_reason();
    if (failures != 0) {
        std::fprintf(stderr, "FAIL %d take tests\n", failures);
        return 1;
    }
    std::printf("test_takes: ok\n");
    return 0;
}
