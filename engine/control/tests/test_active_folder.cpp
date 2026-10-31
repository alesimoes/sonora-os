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
#include <poll.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/inotify.h>
#endif

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

// A take is placed only into the folder it was recorded in (ADR-0018
// checklist item 2, N2). Its files are collected by closed_takes(), and
// from then the folder may change, so placing it after a switch must not
// lose it silently: add_closed_take() used to skip every file outside the
// new folder with no word, and the take was in no session at all.
int test_a_take_collected_before_a_switch_is_not_lost_silently() {
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
    std::vector<sonora::Session::ClosedTake> closed = s.closed_takes();
    CHECK(closed.size() == 1);

    CHECK(folder.enter(s, b, ActiveFolder::Kind::Named, why, notices));
    sonora::model::Document document(sonora::engine_document(s));
    const sonora::TakeAdded added = sonora::add_closed_take(document, s, closed[0]);
    CHECK(added.updates.empty());
    CHECK(document.current().sources->sources.empty());
    CHECK(added.track_notices.size() == 1);
    CHECK(added.track_notices[0].find("not in the open session's folder") != std::string::npos);
    CHECK(added.track_notices[0].find(a + "/Audio Files/Audio 1_01.wav") != std::string::npos);
    CHECK(exists(a + "/Audio Files/Audio 1_01.wav"));
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

bool touch(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    return std::fclose(f) == 0;
}

// ADR-0018 §3: each new session gets its own folder, named by the
// interface's unused_session_name rule, so no two sessions share one.
int test_untitled_names_follow_the_interface() {
    const std::string r = root() + "/Sessions";
    fs::remove_all(root());
    std::string why;
    CHECK(ActiveFolder::make_untitled(r, why) == r + "/Untitled");
    CHECK(ActiveFolder::make_untitled(r, why) == r + "/Untitled 2");
    CHECK(ActiveFolder::make_untitled(r, why) == r + "/Untitled 3");
    fs::remove(r + "/Untitled");
    CHECK(ActiveFolder::make_untitled(r, why) == r + "/Untitled");
    CHECK(fs::is_directory(r + "/Untitled"));
    CHECK(touch(root() + "/file.txt"));
    CHECK(ActiveFolder::make_untitled(root() + "/file.txt", why).empty());
    CHECK(!why.empty());
    fs::remove_all(root());
    return 0;
}

// Untitled folders pile up after crashes and declined saves; the empty
// ones are removed (ADR-0018, Consequences): at exit, and at start for
// those an engine that died left behind.
int test_empty_untitled_folders_are_removed() {
    const std::string r = root() + "/Sessions";
    fs::remove_all(root());
    std::string why;
    std::vector<std::string> notices;
    {
        const std::string u = ActiveFolder::make_untitled(r, why);
        sonora::Session s(config(u));
        ActiveFolder folder;
        CHECK(folder.enter(s, u, ActiveFolder::Kind::Untitled, why, notices));
        CHECK(folder.fresh());
    }
    CHECK(!exists(r + "/Untitled"));
    {
        const std::string u = ActiveFolder::make_untitled(r, why);
        sonora::Session s(config(u));
        ActiveFolder folder;
        CHECK(folder.enter(s, u, ActiveFolder::Kind::Untitled, why, notices));
        CHECK(touch(u + "/Audio Files/Audio 1_01.wav"));
        CHECK(!folder.fresh());
    }
    CHECK(exists(r + "/Untitled/Audio Files/Audio 1_01.wav"));

    fs::create_directories(r + "/Untitled 2/Audio Files");
    fs::create_directories(r + "/Untitled 3/Audio Files");
    // Only the states of flushes no session file names: not data.
    fs::create_directories(r + "/Untitled 4/Plugin State");
    CHECK(touch(r + "/Untitled 4/Plugin State/0123.state"));
    CHECK(touch(r + "/Untitled 4/Plugin State/4567.state.tmp"));
    fs::create_directories(r + "/Untitled 5/Plugin State");
    CHECK(touch(r + "/Untitled 5/Plugin State/notes.txt"));
    fs::create_directories(r + "/Mix/Audio Files");
    fs::create_directories(r + "/Untitled Song/Audio Files");
    sonora::platform::FolderLock theirs;
    CHECK(theirs.acquire(r + "/Untitled 3", why) == sonora::platform::FolderLock::Result::Held);
    ActiveFolder::remove_empty_untitled(r);
    CHECK(!exists(r + "/Untitled 2"));
    CHECK(exists(r + "/Untitled 3/Audio Files"));
    CHECK(!exists(r + "/Untitled 4"));
    CHECK(exists(r + "/Untitled 5/Plugin State/notes.txt"));
    CHECK(exists(r + "/Untitled/Audio Files/Audio 1_01.wav"));
    CHECK(exists(r + "/Mix/Audio Files"));
    CHECK(exists(r + "/Untitled Song/Audio Files"));
    ActiveFolder::remove_empty_untitled(root() + "/nowhere");
    fs::remove_all(root());
    return 0;
}

// ADR-0018 §3, first Save: the untitled folder is renamed to the folder the
// session is saved in, takes and lock with it, so the takes are beside the
// session file and the session keeps the name the operator gave it.
int test_the_first_save_renames_the_folder() {
    const std::string r = root() + "/Sessions";
    fs::remove_all(root());
    std::string why;
    std::vector<std::string> notices;
    const std::string u = ActiveFolder::make_untitled(r, why);
    sonora::Session s(config(u));
    ActiveFolder folder;
    CHECK(folder.enter(s, u, ActiveFolder::Kind::Untitled, why, notices));
    s.tracks()[0]->set_rec_armed(true);
    s.disk().start();
    CHECK(s.start_recording(s.next_free_take()));
    feed(s, 64);
    CHECK(s.stop());
    CHECK(s.closed_takes().size() == 1);
    CHECK(exists(u + "/Audio Files/Audio 1_01.wav"));

    // Its own folder, or a folder that is there already, is not a rename.
    CHECK(!folder.names_on_save(u));
    fs::create_directories(r + "/Taken");
    CHECK(!folder.names_on_save(r + "/Taken"));
    // Nor is a folder inside it, which rename(2) refuses: the Save writes
    // there. A sibling whose name only starts with its name is not inside.
    CHECK(!folder.names_on_save(u + "/Sub"));
    CHECK(!folder.names_on_save(u + "/Sub/Deeper"));
    CHECK(!folder.names_on_save(r + "/./Untitled/Sub"));
    CHECK(folder.names_on_save(u + " 9"));
    CHECK(folder.names_on_save(r + "/Bass"));

    CHECK(folder.rename(s, r + "/Bass", why));
    CHECK(*s.folder() == r + "/Bass");
    CHECK(!folder.untitled());
    CHECK(!exists(u));
    CHECK(exists(r + "/Bass/Audio Files/Audio 1_01.wav"));
    sonora::platform::FolderLock probe;
    CHECK(probe.acquire(r + "/Bass", why) == sonora::platform::FolderLock::Result::Busy);
    // Named now: a second Save elsewhere is not a rename.
    CHECK(!folder.names_on_save(r + "/Other"));
    // The next take is numbered on from the folder's.
    CHECK(s.next_free_take() == 2);
    s.stop();
    fs::remove_all(root());
    return 0;
}

// A folder that has held a session file is that session's: renaming it
// would take its file along.
int test_a_folder_with_a_session_file_is_not_renamed() {
    const std::string r = root() + "/Sessions";
    fs::remove_all(root());
    std::string why;
    std::vector<std::string> notices;
    const std::string u = ActiveFolder::make_untitled(r, why);
    sonora::Session s(config(u));
    ActiveFolder folder;
    CHECK(folder.enter(s, u, ActiveFolder::Kind::Untitled, why, notices));
    CHECK(touch(u + "/Untitled.sonora"));
    CHECK(!folder.names_on_save(r + "/Bass"));
    CHECK(!folder.fresh());
    sonora::Session named_session(config(r + "/Mix"));
    ActiveFolder named;
    CHECK(named.enter(named_session, r + "/Mix", ActiveFolder::Kind::Named, why, notices));
    CHECK(!named.names_on_save(r + "/Bass"));
    CHECK(!named.fresh());
    fs::remove_all(root());
    return 0;
}

// Naming the session moves the folder a take is being written to: refused
// until it closes, saying where the take is (ADR-0018 §3).
int test_naming_is_refused_during_a_take() {
    const std::string r = root() + "/Sessions";
    fs::remove_all(root());
    std::string why;
    std::vector<std::string> notices;
    const std::string u = ActiveFolder::make_untitled(r, why);
    sonora::Session s(config(u));
    ActiveFolder folder;
    CHECK(folder.enter(s, u, ActiveFolder::Kind::Untitled, why, notices));
    s.tracks()[0]->set_rec_armed(true);
    s.disk().start();
    CHECK(s.start_recording(s.next_free_take()));
    feed(s, 64);
    CHECK(folder.names_on_save(r + "/Bass"));
    CHECK(!folder.rename(s, r + "/Bass", why));
    CHECK(why.find("stop recording to name the session") != std::string::npos);
    CHECK(why.find("the take is being written to Untitled") != std::string::npos);
    CHECK(*s.folder() == u);
    CHECK(folder.untitled());
    CHECK(!exists(r + "/Bass"));
    CHECK(s.stop());
    s.closed_takes();
    CHECK(folder.rename(s, r + "/Bass", why));
    CHECK(exists(r + "/Bass/Audio Files/Audio 1_01.wav"));
    s.stop();
    fs::remove_all(root());
    return 0;
}

// #150 review, item 1: a flush with no take and no Save, or a first Save
// that renamed the folder and then failed to write its file, left a folder
// that was neither removed nor listed, and pushed later sessions on to
// "Untitled <n+1>". With no take and no session file, nothing names those
// states: the folder goes when the engine leaves it. Anything else stays.
int test_a_folder_holding_only_unreferenced_states_goes() {
    const std::string r = root() + "/Sessions";
    fs::remove_all(root());
    std::string why;
    std::vector<std::string> notices;
    const std::string u = ActiveFolder::make_untitled(r, why);
    const std::string mix = r + "/Mix";
    sonora::Session s(config(u));
    std::string exiting;
    {
        ActiveFolder folder;
        CHECK(folder.enter(s, u, ActiveFolder::Kind::Untitled, why, notices));
        fs::create_directories(u + "/Plugin State");
        CHECK(touch(u + "/Plugin State/0123.state"));
        CHECK(!folder.fresh());
        CHECK(folder.enter(s, mix, ActiveFolder::Kind::Named, why, notices));
        CHECK(!exists(u));
        CHECK(ActiveFolder::make_untitled(r, why) == u);
        // A take beside the states, or a file of the operator's: kept.
        CHECK(folder.enter(s, u, ActiveFolder::Kind::Untitled, why, notices));
        fs::create_directories(u + "/Plugin State");
        CHECK(touch(u + "/Plugin State/0123.state"));
        CHECK(touch(u + "/Audio Files/Audio 1_01.wav"));
        CHECK(folder.enter(s, mix, ActiveFolder::Kind::Named, why, notices));
        CHECK(exists(u + "/Plugin State/0123.state"));
        fs::remove(u + "/Audio Files/Audio 1_01.wav");
        CHECK(touch(u + "/notes.txt"));
        CHECK(folder.enter(s, u, ActiveFolder::Kind::Untitled, why, notices));
        CHECK(folder.enter(s, mix, ActiveFolder::Kind::Named, why, notices));
        CHECK(exists(u + "/notes.txt"));
        fs::remove_all(u);

        // Renamed by a first Save whose file write then failed.
        const std::string v = ActiveFolder::make_untitled(r, why);
        CHECK(folder.enter(s, v, ActiveFolder::Kind::Untitled, why, notices));
        CHECK(folder.rename(s, r + "/Bass", why));
        fs::create_directories(r + "/Bass/Plugin State");
        CHECK(touch(r + "/Bass/Plugin State/0123.state"));
        CHECK(folder.enter(s, mix, ActiveFolder::Kind::Named, why, notices));
        CHECK(!exists(r + "/Bass"));
        // Renamed, and the file written: the session's own.
        const std::string w = ActiveFolder::make_untitled(r, why);
        CHECK(folder.enter(s, w, ActiveFolder::Kind::Untitled, why, notices));
        CHECK(folder.rename(s, r + "/Keys", why));
        CHECK(touch(r + "/Keys/Keys.sonora"));
        CHECK(folder.enter(s, mix, ActiveFolder::Kind::Named, why, notices));
        CHECK(exists(r + "/Keys/Keys.sonora"));
        // A folder entered by name is never removed, whatever it holds.
        CHECK(exists(mix + "/Audio Files"));

        // And as the engine exits.
        const std::string x = ActiveFolder::make_untitled(r, why);
        CHECK(folder.enter(s, x, ActiveFolder::Kind::Untitled, why, notices));
        fs::create_directories(x + "/Plugin State");
        CHECK(touch(x + "/Plugin State/89ab.state"));
        exiting = x;
    }
    CHECK(!exists(exiting));
    fs::remove_all(root());
    return 0;
}

// #151 review, B1: the checks followed symlinks, so a "Plugin State" that
// links to another session's had that session's states deleted, and a
// symlinked "Untitled <n>" had the folder it points to emptied. A symlink
// at any level is data the operator made: the folder stays as it is.
int test_a_symlink_keeps_the_folder() {
    const std::string r = root() + "/Sessions";
    fs::remove_all(root());
    fs::create_directories(r + "/Mix/Plugin State");
    CHECK(touch(r + "/Mix/Plugin State/0123.state"));
    fs::create_directories(r + "/Untitled 3/Audio Files");
    fs::create_directory_symlink(r + "/Mix/Plugin State", r + "/Untitled 3/Plugin State");

    fs::create_directories(root() + "/Elsewhere/Audio Files");
    fs::create_directory_symlink(root() + "/Elsewhere", r + "/Untitled 4");

    fs::create_directories(r + "/Untitled 5/Plugin State");
    fs::create_symlink(r + "/Mix/Plugin State/0123.state", r + "/Untitled 5/Plugin State/0123.state");

    fs::create_directories(root() + "/Empty");
    fs::create_directory_symlink(root() + "/Empty", r + "/Untitled 6");
    fs::create_directories(r + "/Untitled 7");
    fs::create_directory_symlink(root() + "/Empty", r + "/Untitled 7/Audio Files");

    ActiveFolder::remove_empty_untitled(r);
    CHECK(exists(r + "/Mix/Plugin State/0123.state"));
    CHECK(fs::is_symlink(r + "/Untitled 3/Plugin State"));
    CHECK(fs::is_symlink(r + "/Untitled 4"));
    CHECK(exists(root() + "/Elsewhere/Audio Files"));
    CHECK(fs::is_symlink(r + "/Untitled 5/Plugin State/0123.state"));
    CHECK(fs::is_symlink(r + "/Untitled 6"));
    CHECK(fs::is_symlink(r + "/Untitled 7/Audio Files"));
    CHECK(exists(root() + "/Empty"));

    // Leaving an untitled folder the same way.
    std::string why;
    std::vector<std::string> notices;
    const std::string u = ActiveFolder::make_untitled(r, why);
    sonora::Session s(config(u));
    {
        ActiveFolder folder;
        CHECK(folder.enter(s, u, ActiveFolder::Kind::Untitled, why, notices));
        fs::create_directory_symlink(r + "/Mix/Plugin State", u + "/Plugin State");
        CHECK(folder.enter(s, r + "/Mix", ActiveFolder::Kind::Named, why, notices));
    }
    CHECK(fs::is_symlink(u + "/Plugin State"));
    CHECK(exists(r + "/Mix/Plugin State/0123.state"));
    fs::remove_all(root());
    return 0;
}

#ifdef __linux__
// #151 review, N1: enter() released the old folder's lock before removing
// it, so another engine could enter the folder between the check and the
// unlinks and have its files deleted under it. The probe tries the lock as
// the first state file goes; it must find the folder still held (or gone).
int test_the_old_folder_stays_locked_while_it_is_removed() {
    const std::string r = root() + "/Sessions";
    fs::remove_all(root());
    std::string why;
    std::vector<std::string> notices;
    const std::string u = ActiveFolder::make_untitled(r, why);
    sonora::Session s(config(u));
    ActiveFolder folder;
    CHECK(folder.enter(s, u, ActiveFolder::Kind::Untitled, why, notices));
    const std::string states = u + "/Plugin State";
    fs::create_directories(states);
    // Enough unlinks that the probe runs while the removal is still going.
    for (int i = 0; i < 2000; ++i) CHECK(touch(states + "/" + std::to_string(i) + ".state"));

    const int fd = ::inotify_init1(IN_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(::inotify_add_watch(fd, states.c_str(), IN_DELETE) >= 0);
    int seen = -1;   // -1: no delete seen within the timeout
    std::thread probe([&] {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 5000) != 1) return;
        sonora::platform::FolderLock lock;
        std::string err;
        seen = static_cast<int>(lock.acquire(u, err));
    });
    const bool entered = folder.enter(s, r + "/Mix", ActiveFolder::Kind::Named, why, notices);
    probe.join();
    ::close(fd);
    CHECK(entered);
    CHECK(seen != -1);
    CHECK(seen != static_cast<int>(sonora::platform::FolderLock::Result::Held));
    CHECK(!exists(u));
    fs::remove_all(root());
    return 0;
}
#endif

}  // namespace

int main() {
    int failed = 0;
    failed += test_refused_while_a_take_is_open();
    failed += test_refused_while_a_take_is_closing();
    failed += test_busy_folder_is_refused_and_the_old_one_stays();
    failed += test_recovery_runs_on_entry();
    failed += test_a_take_after_a_switch_lands_in_the_new_folder();
    failed += test_a_take_collected_before_a_switch_is_not_lost_silently();
    failed += test_only_an_empty_untitled_folder_is_removed();
    failed += test_untitled_names_follow_the_interface();
    failed += test_empty_untitled_folders_are_removed();
    failed += test_the_first_save_renames_the_folder();
    failed += test_a_folder_with_a_session_file_is_not_renamed();
    failed += test_naming_is_refused_during_a_take();
    failed += test_a_folder_holding_only_unreferenced_states_goes();
    failed += test_a_symlink_keeps_the_folder();
#ifdef __linux__
    failed += test_the_old_folder_stays_locked_while_it_is_removed();
#endif
    if (failed) return 1;
    std::printf("active folder: ok\n");
    return 0;
}
