// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's saver thread (ADR-0015 §6, update of 2026-10-25): with no
// take open a save writes at once and cleans up after; with one open it
// touches the disk only after the disk thread's checkpoint counter moves,
// one save per bump, and its cleanup waits for the transport to stop; a
// take closing ends the wait; stop() finishes what was handed over; a
// refused save is answered with its reason.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <thread>
#include <unistd.h>

#include "model_test.h"
#include "sonora/model/session_file.h"
#include "sonora/platform/file.h"
#include "sonora/session_saver.h"
#include "sonora/sha256.h"

using namespace model_test;
using sonora::SessionSaver;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
        path = fs::temp_directory_path() /
               ("sonora-saver-" + std::to_string(::getpid()) + "-" + std::to_string(n++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct Midi {
    History h{new_session(settings())};
    Id playlist = 0;
    Id region = 0;

    Midi() {
        Ids ids(h.current());
        AddTrack t;
        t.track = ids();
        t.playlist = ids();
        t.name = "Synth";
        t.playlist_name = "Synth.01";
        t.kind = TrackKind::Midi;
        t.channels = 0;
        std::string why;
        h.apply(t, why);
        MidiLists seq;
        for (int i = 0; i < 20; ++i) seq.events.push_back(MidiEvent{i * 1000, 0xB0, 1, 2});
        MidiRegion r;
        r.id = ids();
        r.name = "Synth_01";
        r.length = 4 * kTicksPerQuarter;
        r.data = std::make_shared<const MidiSequence>(seq);
        h.apply(PlaceMidiRegion{t.playlist, r, 0, {}}, why);
        playlist = t.playlist;
        region = r.id;
    }
    bool edit(int tick) {
        EditMidi e;
        e.playlist = playlist;
        e.region = region;
        e.add_events = {MidiEvent{100000 + tick, 0xB0, 1, 3}};
        std::string why;
        return h.apply(e, why);
    }
    const MidiSequence& sequence() const {
        return *h.current().edit->tracks.back()->midi_playlists[0]->regions[0].data;
    }
    SessionSaver::Job job(const fs::path& file, uint64_t seq) const {
        SessionSaver::Job j;
        j.client = 7;
        j.seq = seq;
        j.file = file.string();
        WriteInfo info{"test", seq};
        std::string error;
        serialize(h.current(), info, j.bytes, error);
        j.document = h.current();
        return j;
    }
};

fs::path msq(const fs::path& folder, const MidiSequence& s) {
    return folder / "MIDI Sequences" / (sequence_ref(s).sha256 + ".msq");
}

// Set, the session folder will not sync; "MIDI Sequences/" still does.
std::atomic<bool> folder_sync_fails{false};

bool sync_unless_failing(const std::string& path) {
    if (folder_sync_fails.load() && fs::path(path).filename() != "MIDI Sequences") return false;
    return sonora::platform::sync_directory(path);
}

struct Rig {
    std::atomic<uint32_t> checkpoints{0};
    std::atomic<int> ready{0};
    SessionSaver saver{[this]() { return checkpoints.load(); }, [this]() { ++ready; },
                       std::chrono::milliseconds(2), sync_unless_failing};
    Rig() { saver.start(); }

    // The results, once `n` have come or two seconds have passed.
    std::vector<SessionSaver::Done> wait(std::size_t n) {
        std::vector<SessionSaver::Done> out;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (out.size() < n && std::chrono::steady_clock::now() < until) {
            for (SessionSaver::Done& d : saver.take_done()) out.push_back(std::move(d));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return out;
    }
    // Nothing comes in `ms`.
    bool quiet(int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return saver.take_done().empty();
    }
};

int test_no_take_writes_at_once_and_cleans() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Bass";
    const fs::path file = folder / "Bass.sonora";
    Midi m;
    Rig rig;
    rig.saver.submit(m.job(file, 1));
    auto done = rig.wait(1);
    CHECK(done.size() == 1 && done[0].ok && done[0].seq == 1 && done[0].client == 7);
    CHECK(done[0].message.empty() && !done[0].paced);
    CHECK(fs::exists(file) && fs::exists(msq(folder, m.sequence())));
    CHECK(rig.ready.load() >= 1);
    CHECK(rig.saver.pending() == 0);
    const fs::path a = msq(folder, m.sequence());
    CHECK(m.edit(1));
    rig.saver.submit(m.job(file, 2));
    done = rig.wait(1);
    CHECK(done.size() == 1 && done[0].ok);
    CHECK(!fs::exists(a) && fs::exists(msq(folder, m.sequence())));
    CHECK(rig.saver.cleanups() == 2 && rig.saver.paced_saves() == 0);
    return 0;
}

// The regression the pacing exists for: a save that syncs while a take is
// open, in front of the take's own checkpoint.
int test_take_open_waits_for_a_checkpoint() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Keys";
    const fs::path file = folder / "Keys.sonora";
    Midi m;
    Rig rig;
    rig.saver.set_take_open(true);
    rig.saver.set_transport_stopped(false);
    rig.checkpoints = 5;
    rig.saver.submit(m.job(file, 1));
    CHECK(rig.quiet(60));
    // Not a byte on disk before the bump.
    CHECK(!fs::exists(folder));
    rig.checkpoints = 6;
    auto done = rig.wait(1);
    CHECK(done.size() == 1 && done[0].ok && done[0].paced);
    CHECK(fs::exists(file) && rig.saver.paced_saves() == 1);

    // One save per bump: the next waits for another, and its cleanup waits
    // for the transport to stop.
    const fs::path a = msq(folder, m.sequence());
    CHECK(m.edit(1));
    rig.saver.submit(m.job(file, 2));
    CHECK(rig.quiet(60));
    rig.checkpoints = 7;
    done = rig.wait(1);
    CHECK(done.size() == 1 && done[0].ok && done[0].paced);
    CHECK(fs::exists(a));
    CHECK(rig.saver.cleanups() == 0);
    rig.saver.set_take_open(false);   // a loop take between passes: still rolling
    CHECK(rig.quiet(30) && fs::exists(a));
    rig.saver.set_transport_stopped(true);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (rig.saver.cleanups() == 0 && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(rig.saver.cleanups() == 1 && !fs::exists(a));
    return 0;
}

// A FlushPluginState's states (ADR-0012 §6.1): written to "Plugin State/",
// paced behind a checkpoint as a save is, answered with the flush's id and
// no cleanup run.
int test_states_paced_and_answered() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Mix";
    Rig rig;
    rig.saver.set_take_open(true);
    rig.saver.set_transport_stopped(false);
    SessionSaver::Job j;
    j.kind = SessionSaver::Kind::States;
    j.seq = 42;
    j.file = folder.string();
    const std::string bytes = "state";
    j.states.push_back({sonora::sha256_hex(bytes), bytes});
    rig.saver.submit(std::move(j));
    CHECK(rig.quiet(40));
    CHECK(!fs::exists(folder / "Plugin State"));
    rig.checkpoints = 1;
    auto done = rig.wait(1);
    CHECK(done.size() == 1 && done[0].ok && done[0].paced);
    CHECK(done[0].kind == SessionSaver::Kind::States && done[0].seq == 42);
    CHECK(fs::exists(folder / "Plugin State" / (sonora::sha256_hex(bytes) + ".state")));
    CHECK(rig.saver.pending() == 0 && rig.saver.cleanups() == 0);

    rig.saver.set_take_open(false);
    SessionSaver::Job bad;
    bad.kind = SessionSaver::Kind::States;
    bad.seq = 43;
    bad.file = folder.string();
    bad.states.push_back({std::string(64, '0'), bytes});
    rig.saver.submit(std::move(bad));
    done = rig.wait(1);
    CHECK(done.size() == 1 && !done[0].ok && mentions(done[0].message, "digest"));
    return 0;
}

int test_take_closing_ends_the_wait() {
    TempDir tmp;
    const fs::path file = tmp.path / "Bass" / "Bass.sonora";
    Midi m;
    Rig rig;
    rig.saver.set_take_open(true);
    rig.saver.set_transport_stopped(false);
    rig.saver.submit(m.job(file, 1));
    CHECK(rig.quiet(30));
    rig.saver.set_take_open(false);
    auto done = rig.wait(1);
    CHECK(done.size() == 1 && done[0].ok && fs::exists(file));
    return 0;
}

int test_stop_finishes_what_was_handed_over() {
    TempDir tmp;
    const fs::path file = tmp.path / "Bass" / "Bass.sonora";
    Midi m;
    Rig rig;
    rig.saver.set_take_open(true);
    rig.saver.submit(m.job(file, 1));
    rig.saver.stop();
    CHECK(fs::exists(file));
    const auto done = rig.saver.take_done();
    CHECK(done.size() == 1 && done[0].ok);
    // Started again, it saves again.
    rig.saver.start();
    rig.saver.set_take_open(false);
    rig.saver.submit(m.job(file, 2));
    CHECK(rig.wait(1).size() == 1);
    return 0;
}

int test_refusal_answered() {
    TempDir tmp;
    Midi m;
    Rig rig;
    SessionSaver::Job j = m.job(tmp.path / "Bass" / "Bass.sonora", 3);
    j.document = new_session(settings());   // holds none of what the bytes name
    rig.saver.submit(std::move(j));
    const auto done = rig.wait(1);
    CHECK(done.size() == 1 && !done[0].ok && done[0].seq == 3);
    CHECK(mentions(done[0].message, "does not hold"));
    CHECK(!fs::exists(tmp.path / "Bass"));
    return 0;
}

// A cleanup deferred behind a take is dropped when a later save is not
// durable: the file on disk may still be the one the deferred cleanup's
// save replaced, and after a power cut it names what cleanup would delete.
int test_undurable_save_drops_the_deferred_cleanup() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Keys";
    const fs::path file = folder / "Keys.sonora";
    Midi m;
    Rig rig;
    rig.saver.submit(m.job(file, 1));
    CHECK(rig.wait(1).size() == 1);
    const fs::path a = msq(folder, m.sequence());

    rig.saver.set_take_open(true);
    rig.saver.set_transport_stopped(false);
    rig.checkpoints = 1;
    CHECK(m.edit(1));
    rig.saver.submit(m.job(file, 2));
    CHECK(rig.quiet(30));
    rig.checkpoints = 2;
    auto done = rig.wait(1);
    CHECK(done.size() == 1 && done[0].ok && done[0].message.empty());
    const fs::path b = msq(folder, m.sequence());

    folder_sync_fails = true;
    CHECK(m.edit(2));
    rig.saver.submit(m.job(file, 3));
    CHECK(rig.quiet(30));
    rig.checkpoints = 3;
    done = rig.wait(1);
    folder_sync_fails = false;
    CHECK(done.size() == 1 && done[0].ok && mentions(done[0].message, "power cut"));

    rig.saver.set_take_open(false);
    rig.saver.set_transport_stopped(true);
    CHECK(rig.quiet(60));
    CHECK(rig.saver.cleanups() == 1);   // the first save's, none since
    CHECK(fs::exists(a) && fs::exists(b));
    return 0;
}

// PR #142 review N3: a Load fences the saver before it leaves the folder.
// What was handed over is written when fence() returns, unpaced, and a
// cleanup deferred behind a take never runs in the folder left.
int test_fence_writes_what_was_handed_over() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Keys";
    const fs::path file = folder / "Keys.sonora";
    Midi m;
    Rig rig;
    rig.saver.submit(m.job(file, 1));
    CHECK(rig.wait(1).size() == 1);
    const fs::path a = msq(folder, m.sequence());

    rig.saver.set_take_open(true);
    rig.saver.set_transport_stopped(false);
    CHECK(m.edit(1));
    rig.saver.submit(m.job(file, 2));
    CHECK(rig.quiet(30));
    rig.checkpoints = 1;
    CHECK(rig.wait(1).size() == 1);   // its cleanup deferred
    CHECK(m.edit(2));
    rig.saver.submit(m.job(file, 3));
    SessionSaver::Job j;
    j.kind = SessionSaver::Kind::States;
    j.seq = 44;
    j.file = folder.string();
    const std::string bytes = "state";
    j.states.push_back({sonora::sha256_hex(bytes), bytes});
    rig.saver.submit(std::move(j));
    rig.saver.fence();
    CHECK(fs::exists(folder / "Plugin State" / (sonora::sha256_hex(bytes) + ".state")));
    const auto done = rig.saver.take_done();
    CHECK(done.size() == 2 && done[0].ok && done[0].seq == 3 && done[1].seq == 44);
    CHECK(rig.saver.pending() == 0);

    rig.saver.set_take_open(false);
    rig.saver.set_transport_stopped(true);
    CHECK(rig.quiet(60));
    CHECK(rig.saver.cleanups() == 1 && fs::exists(a));
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int run = 0;
    const auto t = [&](int (*f)()) {
        ++run;
        failed += f();
    };
    t(test_no_take_writes_at_once_and_cleans);
    t(test_take_open_waits_for_a_checkpoint);
    t(test_take_closing_ends_the_wait);
    t(test_states_paced_and_answered);
    t(test_stop_finishes_what_was_handed_over);
    t(test_refusal_answered);
    t(test_undurable_save_drops_the_deferred_cleanup);
    t(test_fence_writes_what_was_handed_over);
    if (failed != 0) {
        std::fprintf(stderr, "session_saver: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("session_saver: %d passed\n", run);
    return 0;
}
