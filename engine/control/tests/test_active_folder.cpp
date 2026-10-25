// SPDX-License-Identifier: GPL-3.0-or-later
//
// The active session folder (ADR-0018 §1). Until it existed the engine
// recorded into --dir whatever session was open, so a session saved
// elsewhere named takes that were not in its folder. Entering a folder is
// refused while a take is open or closing, since a take must close into
// the folder it was opened in; a folder another engine holds is refused
// and the old one stays; recovery runs on every entry; and the next take
// lands in the folder entered.
#include <dirent.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "sonora/closed_take.h"
#include "sonora/model/replication.h"
#include "sonora/platform/file.h"
#include "sonora/session.h"
#include "sonora/active_folder.h"
#include "sonora/wav_reader.h"
#include "sonora/wav_writer.h"

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
using sonora::ActiveFolder;

bool exists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

std::string root() { return fs::temp_directory_path().string() + "/sonora_test_active_folder"; }

void feed(sonora::Session& s, uint32_t nframes) {
    static const float block[256] = {0.25f};
    const float* inputs[2] = {block, block};
    sonora::backend::AudioBlock b{};
    b.inputs = inputs;
    b.input_count = static_cast<uint32_t>(s.tracks().size());
    b.nframes = nframes;
    s.process(b);
}

sonora::SessionConfig config(const std::string& dir) {
    sonora::SessionConfig cfg;
    cfg.dir = dir;
    cfg.track_count = 1;
    cfg.ring_seconds = 0.05;
    return cfg;
}

int test_refused_while_a_take_is_open() {
    const std::string a = root() + "/A", b = root() + "/B";
    fs::remove_all(root());
    fs::create_directories(root());
    sonora::Session s(config(a));
    ActiveFolder folder;
    std::string why;
    std::vector<std::string> notices;
    CHECK(folder.enter(s, a, ActiveFolder::Kind::Named, why, notices));
    s.tracks()[0]->set_rec_armed(true);
    s.disk().start();
    CHECK(s.start_recording(1));
    feed(s, 64);
    CHECK(!folder.enter(s, b, ActiveFolder::Kind::Named, why, notices));
    CHECK(why.find("take 1 is open") != std::string::npos);
    CHECK(*s.folder() == a);
    CHECK(!exists(b));
    s.stop();
    CHECK(exists(a + "/Audio Files/Audio 1_01.wav"));
    fs::remove_all(root());
    return 0;
}

// Punched out while rolling: the take has ended, and the disk thread has
// still to close its file or closed_takes() to collect it.
int test_refused_while_a_take_is_closing() {
    const std::string a = root() + "/A", b = root() + "/B";
    fs::remove_all(root());
    fs::create_directories(root());
    sonora::Session s(config(a));
    ActiveFolder folder;
    std::string why;
    std::vector<std::string> notices;
    CHECK(folder.enter(s, a, ActiveFolder::Kind::Named, why, notices));
    s.tracks()[0]->set_rec_armed(true);
    s.disk().start();
    CHECK(s.start_recording(1));
    feed(s, 64);
    CHECK(s.punch_out());
    feed(s, 64);
    CHECK(s.recording_take() == 0);
    CHECK(!folder.enter(s, b, ActiveFolder::Kind::Named, why, notices));
    CHECK(why.find("take 1 is closing") != std::string::npos);
    CHECK(*s.folder() == a);

    std::vector<sonora::Session::ClosedTake> closed;
    for (int i = 0; i < 2000 && closed.empty(); ++i) {
        closed = s.closed_takes();
        if (closed.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(closed.size() == 1);
    CHECK(folder.enter(s, b, ActiveFolder::Kind::Named, why, notices));
    CHECK(*s.folder() == b);
    s.stop();
    fs::remove_all(root());
    return 0;
}

int test_busy_folder_is_refused_and_the_old_one_stays() {
    const std::string a = root() + "/A", b = root() + "/B";
    fs::remove_all(root());
    fs::create_directories(b);
    sonora::Session s(config(a));
    ActiveFolder folder;
    std::string why;
    std::vector<std::string> notices;
    CHECK(folder.enter(s, a, ActiveFolder::Kind::Named, why, notices));
    {
        sonora::platform::FolderLock other;
        std::string err;
        CHECK(other.acquire(b, err) == sonora::platform::FolderLock::Result::Held);
        CHECK(!folder.enter(s, b, ActiveFolder::Kind::Named, why, notices));
        CHECK(why.find("one session takes one engine") != std::string::npos);
        CHECK(*s.folder() == a);
        CHECK(!exists(b + "/Audio Files"));
    }
    // Still locked by this engine: make before break kept the old lock.
    sonora::platform::FolderLock probe;
    std::string err;
    CHECK(probe.acquire(a, err) == sonora::platform::FolderLock::Result::Busy);
    // Re-entering the active folder keeps its own lock rather than finding
    // it busy.
    CHECK(folder.enter(s, a, ActiveFolder::Kind::Named, why, notices));
    // Once B is entered, A is released.
    CHECK(folder.enter(s, b, ActiveFolder::Kind::Named, why, notices));
    sonora::platform::FolderLock after;
    CHECK(after.acquire(a, err) == sonora::platform::FolderLock::Result::Held);
    fs::remove_all(root());
    return 0;
}

int test_recovery_runs_on_entry() {
    const std::string a = root() + "/A", b = root() + "/B";
    fs::remove_all(root());
    fs::create_directories(b + "/Audio Files");
    const std::string path = b + "/Audio Files/Audio 1_01.wav";
    {
        sonora::WavWriter w;
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        const std::vector<float> block(24000, 0.25f);
        CHECK(w.write(block.data(), block.size()));
        CHECK(w.close());
    }
    // What a dead engine leaves: the audio, both RIFF and data sizes zero.
    {
        std::FILE* f = std::fopen(path.c_str(), "r+b");
        CHECK(f != nullptr);
        const char zero[4] = {0, 0, 0, 0};
        std::fseek(f, 4, SEEK_SET);
        std::fwrite(zero, 1, 4, f);
        std::fseek(f, 12 + 8 + 602 + 8 + 16 + 4, SEEK_SET);
        std::fwrite(zero, 1, 4, f);
        std::fclose(f);
    }
    sonora::Session s(config(a));
    ActiveFolder folder;
    std::string why;
    std::vector<std::string> notices;
    CHECK(folder.enter(s, a, ActiveFolder::Kind::Named, why, notices));
    CHECK(notices.empty());
    CHECK(folder.enter(s, b, ActiveFolder::Kind::Named, why, notices));
    CHECK(notices.size() == 1);
    CHECK(notices[0].find("recovered Audio 1_01.wav") != std::string::npos);
    sonora::WavReader r;
    CHECK(r.open(path, why));
    CHECK(r.frames() == 24000);
    r.close();
    fs::remove_all(root());
    return 0;
}

int test_a_take_after_a_switch_lands_in_the_new_folder() {
    const std::string a = root() + "/A", b = root() + "/B";
    fs::remove_all(root());
    fs::create_directories(root());
    sonora::Session s(config(a));
    ActiveFolder folder;
    std::string why;
    std::vector<std::string> notices;
    CHECK(folder.enter(s, a, ActiveFolder::Kind::Named, why, notices));
    s.tracks()[0]->set_rec_armed(true);
    s.disk().start();
    CHECK(s.start_recording(s.next_free_take()));
    feed(s, 64);
    CHECK(s.stop());
    s.closed_takes();
    CHECK(exists(a + "/Audio Files/Audio 1_01.wav"));

    CHECK(folder.enter(s, b, ActiveFolder::Kind::Named, why, notices));
    CHECK(exists(b + "/Audio Files"));
    // The count is B's: take 1 there, not take 2 after A's.
    CHECK(s.next_free_take() == 1);
    s.disk().start();
    CHECK(s.start_recording(s.next_free_take()));
    feed(s, 64);
    CHECK(s.stop());
    CHECK(exists(b + "/Audio Files/Audio 1_01.wav"));
    CHECK(!exists(a + "/Audio Files/Audio 1_02.wav"));

    // Placed relative to B, as the document in B names it.
    std::vector<sonora::Session::ClosedTake> closed = s.closed_takes();
    CHECK(closed.size() == 1);
    sonora::model::Document document(sonora::engine_document(s));
    const sonora::TakeAdded added = sonora::add_closed_take(document, s, closed[0]);
    CHECK(added.track_notices.empty() && added.refused.empty());
    CHECK(!added.updates.empty());
    const auto& sources = document.current().sources->sources;
    CHECK(sources.size() == 1);
    CHECK(sources[0].path == "Audio Files/Audio 1_01.wav");
    fs::remove_all(root());
    return 0;
}

// An Untitled folder the engine made is removed on leaving only if nothing
// was put in it; a named one never is.
int test_only_an_empty_untitled_folder_is_removed() {
    const std::string u1 = root() + "/Untitled 1", u2 = root() + "/Untitled 2";
    const std::string named = root() + "/Mix";
    fs::remove_all(root());
    fs::create_directories(root());
    sonora::Session s(config(u1));
    ActiveFolder folder;
    std::string why;
    std::vector<std::string> notices;
    CHECK(folder.enter(s, u1, ActiveFolder::Kind::Untitled, why, notices));
    CHECK(folder.enter(s, u2, ActiveFolder::Kind::Untitled, why, notices));
    CHECK(!exists(u1));
    {
        std::FILE* f = std::fopen((u2 + "/Audio Files/keep.wav").c_str(), "wb");
        CHECK(f != nullptr);
        std::fclose(f);
    }
    CHECK(folder.enter(s, named, ActiveFolder::Kind::Named, why, notices));
    CHECK(exists(u2 + "/Audio Files/keep.wav"));
    CHECK(folder.enter(s, u1, ActiveFolder::Kind::Untitled, why, notices));
    CHECK(exists(named + "/Audio Files"));
    CHECK(!folder.read_only());
    CHECK(folder.free_bytes() > 0);
    fs::remove_all(root());
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_refused_while_a_take_is_open();
    failed += test_refused_while_a_take_is_closing();
    failed += test_busy_folder_is_refused_and_the_old_one_stays();
    failed += test_recovery_runs_on_entry();
    failed += test_a_take_after_a_switch_lands_in_the_new_folder();
    failed += test_only_an_empty_untitled_folder_is_removed();
    if (failed) return 1;
    std::printf("active folder: ok\n");
    return 0;
}
