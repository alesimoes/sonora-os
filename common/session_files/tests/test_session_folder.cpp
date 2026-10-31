// SPDX-License-Identifier: GPL-3.0-or-later
//
// A session folder on disk (ADR-0015 §3, §6; session-format.md, "Saving"):
// sequence files before the session file, through tmp and rename; a file
// already there with the bytes its name hashes is not written again, and
// one that is not is; a save naming MIDI the document does not hold is
// refused with nothing written; cleanup deletes what no kept file names and
// nothing while a kept file cannot be read; an interrupted write is deleted
// when the folder is entered; the pre-migration copy and the newer-version
// refusal; plugin state files written once each and never cleaned, copied
// into the session file's own folder, synced until durable, ".tmp" swept.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <unistd.h>

#include "model_test.h"
#include "sonora/model/session_file.h"
#include "sonora/session_files/session_folder.h"
#include "sonora/sha256.h"

using namespace model_test;
using sonora::session_files::SessionFolder;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
        path = fs::temp_directory_path() /
               ("sonora-session-folder-" + std::to_string(::getpid()) + "-" + std::to_string(n++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void write(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << bytes;
}

// A session with one MIDI region of `events` controller events.
struct Midi {
    History h{new_session(settings())};
    Id playlist = 0;
    Id region = 0;

    explicit Midi(int events) {
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
        for (int i = 0; i < events; ++i) seq.events.push_back(MidiEvent{i * 1000, 0xB0, 1, 2});
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
    std::string bytes(std::uint64_t generation = 1) const {
        WriteInfo info{"test", generation};
        std::string out, error;
        serialize(h.current(), info, out, error);
        return out;
    }
};

fs::path msq(const fs::path& folder, const MidiSequence& s) {
    return folder / "MIDI Sequences" / (sequence_ref(s).sha256 + ".msq");
}

std::size_t entries(const fs::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) ++n;
    return n;
}

int test_sequences_before_the_session_file() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Bass";
    const fs::path file = folder / "Bass.sonora";
    Midi m(20);
    SessionFolder f;
    f.set_backups(0);   // a rotated backup would be a kept file naming the first
    std::string why, caution;
    CHECK(f.save(file.string(), m.bytes(), m.h.current(), why, caution));
    CHECK(caution.empty());
    CHECK(read(file) == m.bytes());
    const fs::path a = msq(folder, m.sequence());
    CHECK(read(a) == encode_sequence(m.sequence()));
    CHECK(entries(folder / "MIDI Sequences") == 1);   // no ".tmp" left behind
    CHECK(entries(folder) == 2);                      // the file and the folder, no ".tmp"
    CHECK(f.sequences_written() == 1);

    // The same sequence again is on disk already: not written twice.
    CHECK(f.save(file.string(), m.bytes(2), m.h.current(), why, caution));
    CHECK(f.sequences_written() == 1);

    // An edit makes another; once saved, nothing names the first.
    CHECK(m.edit(1));
    CHECK(f.save(file.string(), m.bytes(3), m.h.current(), why, caution));
    const fs::path b = msq(folder, m.sequence());
    CHECK(fs::exists(a) && fs::exists(b));
    CHECK(f.clean(caution) == 1 && caution.empty());
    CHECK(!fs::exists(a) && fs::exists(b));
    return 0;
}

int test_unheld_sequence_refused() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Keys";
    Midi named(20);
    Midi held(30);
    SessionFolder f;
    std::string why, caution;
    // What the bytes name is not in the document: an edit the engine refused.
    CHECK(!f.save((folder / "Keys.sonora").string(), named.bytes(), held.h.current(), why, caution));
    CHECK(mentions(why, "does not hold"));
    CHECK(!fs::exists(folder));

    CHECK(!f.save("Keys.sonora", held.bytes(), held.h.current(), why, caution));
    CHECK(mentions(why, "absolute path"));
    CHECK(!f.save((folder / "Keys.txt").string(), held.bytes(), held.h.current(), why, caution));
    CHECK(!f.save((folder / "Keys.sonora").string(), "{not json", held.h.current(), why, caution));
    CHECK(mentions(why, "not a session file"));
    CHECK(!fs::exists(folder));
    return 0;
}

int test_on_disk_is_verified() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Bass";
    const fs::path file = folder / "Bass.sonora";
    Midi m(20);
    {
        SessionFolder first;
        std::string why, caution;
        CHECK(first.save(file.string(), m.bytes(), m.h.current(), why, caution));
    }
    // Another saver (an engine restarted): the file there hashes to its
    // name, so it is not written again.
    SessionFolder again;
    std::string why, caution;
    CHECK(again.save(file.string(), m.bytes(2), m.h.current(), why, caution));
    CHECK(again.sequences_written() == 0);

    // One that does not is written again, whole.
    const fs::path a = msq(folder, m.sequence());
    write(a, "damaged");
    SessionFolder third;
    CHECK(third.save(file.string(), m.bytes(3), m.h.current(), why, caution));
    CHECK(third.sequences_written() == 1);
    CHECK(read(a) == encode_sequence(m.sequence()));
    return 0;
}

int test_interrupted_write_deleted_at_entry() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Bass";
    const fs::path file = folder / "Bass.sonora";
    Midi m(20);
    const fs::path torn = folder / "MIDI Sequences" / (std::string(64, 'a') + ".msq.tmp");
    write(torn, "torn");
    SessionFolder f;
    std::string why, caution;
    CHECK(f.save(file.string(), m.bytes(), m.h.current(), why, caution));
    CHECK(!fs::exists(torn));
    return 0;
}

int test_unreadable_kept_file_blocks_cleanup() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Keys";
    const fs::path file = folder / "Keys.sonora";
    Midi m(20);
    SessionFolder f;
    f.set_backups(0);   // a rotated backup would be a kept file naming the first
    std::string why, caution;
    CHECK(f.save(file.string(), m.bytes(), m.h.current(), why, caution));
    const fs::path a = msq(folder, m.sequence());
    const fs::path junk = folder / "Session File Backups" / "Keys.v9.sonora";
    write(junk, "{not json");
    CHECK(m.edit(1));
    CHECK(f.save(file.string(), m.bytes(2), m.h.current(), why, caution));
    CHECK(f.clean(caution) == 0 && mentions(caution, "cannot be read"));
    CHECK(fs::exists(a));
    const std::size_t reads = f.kept_reads();
    // Parsed once, not at every save.
    CHECK(f.clean(caution) == 0);
    CHECK(f.kept_reads() == reads);
    fs::remove(junk);
    caution.clear();
    CHECK(f.clean(caution) == 1 && caution.empty());
    CHECK(!fs::exists(a) && fs::exists(msq(folder, m.sequence())));

    // An autosave or backup names the session folder's files.
    const fs::path autosave = folder / "Session File Backups" / "Keys.autosave.sonora";
    write(autosave, m.bytes(9));
    CHECK(sonora::session_files::session_folder_of(autosave.string()) == folder.string());
    const fs::path kept = msq(folder, m.sequence());
    CHECK(m.edit(2));
    CHECK(f.save(file.string(), m.bytes(3), m.h.current(), why, caution));
    CHECK(f.clean(caution) == 0);
    CHECK(fs::exists(kept));
    return 0;
}

int test_versions() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Old";
    const fs::path file = folder / "Old.sonora";
    Midi m(5);
    std::string v99 = m.bytes();
    const std::string key = "\"sonora_session\": " + std::to_string(kFormatVersion);
    const std::size_t at = v99.find(key);
    CHECK(at != std::string::npos);
    v99.replace(at, key.size(), "\"sonora_session\": 99");
    write(file, v99);
    SessionFolder f;
    std::string why, caution;
    CHECK(!f.save(file.string(), m.bytes(), m.h.current(), why, caution));
    CHECK(mentions(why, "version 99") && mentions(why, "newer"));
    CHECK(read(file) == v99);
    CHECK(!fs::exists(folder / "MIDI Sequences"));

    std::string v5 = v99;
    v5.replace(at, std::string("\"sonora_session\": 99").size(), "\"sonora_session\": 5");
    write(file, v5);
    CHECK(f.save(file.string(), m.bytes(), m.h.current(), why, caution));
    CHECK(read(folder / "Session File Backups" / "Old.v5.sonora") == v5);
    CHECK(read(file) == m.bytes());
    return 0;
}

// A file opened from "Session File Backups/" is saved in place, but its
// sequence files go to the session folder above, where every loader
// (session_folder_of) looks for them.
int test_backup_file_saves_sequences_in_the_session_folder() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Keys";
    const fs::path file = folder / "Keys.sonora";
    const fs::path backup = folder / "Session File Backups" / "Keys.v5.sonora";
    Midi m(20);
    SessionFolder f;
    std::string why, caution;
    CHECK(f.save(file.string(), m.bytes(), m.h.current(), why, caution));
    const fs::path a = msq(folder, m.sequence());
    write(backup, m.bytes(2));
    CHECK(m.edit(1));
    CHECK(f.save(backup.string(), m.bytes(3), m.h.current(), why, caution));
    CHECK(caution.empty());
    CHECK(read(backup) == m.bytes(3));
    CHECK(f.folder() == folder.string());
    const fs::path b = msq(folder, m.sequence());
    CHECK(fs::exists(b));
    CHECK(!fs::exists(folder / "Session File Backups" / "MIDI Sequences"));
    // Keys.sonora still names the first, the backup the second.
    CHECK(f.clean(caution) == 0 && caution.empty());
    CHECK(fs::exists(a) && fs::exists(b));
    return 0;
}

bool no_sync(const std::string&) { return false; }

// ADR-0012 §6.1: plugin states are files named by their digest, written
// durably before a session file names them, never twice, never cleaned.
int test_plugin_states() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Mix";
    const fs::path file = folder / "Mix.sonora";
    Midi m(4);
    SessionFolder f;
    std::string why, caution;
    const std::string bytes = "a plugin's state blob";
    const sonora::session_files::StateFile s{sonora::sha256_hex(bytes), bytes};
    CHECK(f.write_states(folder.string(), {s}, why));
    const fs::path state = folder / "Plugin State" / (s.sha256 + ".state");
    CHECK(read(state) == bytes);
    CHECK(entries(folder / "Plugin State") == 1);   // no ".tmp" left behind
    CHECK(f.states_written() == 1);
    CHECK(f.write_states(folder.string(), {s}, why));
    CHECK(f.states_written() == 1);                 // already there with its bytes

    // A file torn by a crash under the name is written again.
    write(state, "torn");
    CHECK(f.write_states(folder.string(), {s}, why));
    CHECK(read(state) == bytes && f.states_written() == 2);

    // Cleanup after a save that names no state leaves it: states are
    // removed only by an explicit cleanup (§6.1).
    CHECK(f.save(file.string(), m.bytes(), m.h.current(), why, caution));
    f.clean(caution);
    CHECK(fs::exists(state));

    // Bytes that are not the digest's never get its name.
    const sonora::session_files::StateFile wrong{std::string(64, 'a'), bytes};
    CHECK(!f.write_states(folder.string(), {wrong}, why));
    CHECK(mentions(why, "does not match"));
    CHECK(!fs::exists(folder / "Plugin State" / (wrong.sha256 + ".state")));

    // A folder that will not sync is a failure: a session saved after it
    // could name a state a power cut takes away.
    SessionFolder unsynced(no_sync);
    const std::string other = "another state";
    CHECK(!unsynced.write_states(folder.string(), {{sonora::sha256_hex(other), other}}, why));
    CHECK(mentions(why, "sync"));
    return 0;
}

// An audio track with one insert whose state is `state`: what a session
// saved after a FlushPluginState names.
struct Plugged {
    History h{new_session(settings())};
    explicit Plugged(const std::string& state) {
        Ids ids(h.current());
        const AddTrack t = add_track(ids, "Gtr");
        std::string why;
        h.apply(t, why);
        AddInsert in;
        in.track = t.track;
        in.insert = ids();
        in.slot = 0;
        in.plugin = PluginRef{PluginFormat::Lv2, "urn:test#state", "urn:test#state", "test"};
        h.apply(in, why);
        const std::string sha = sonora::sha256_hex(state);
        h.apply(SetInsertState{in.insert, StateRef{plugin_state_file(sha), sha, state.size()}}, why);
    }
    std::string bytes() const {
        std::string out, error;
        serialize(h.current(), WriteInfo{"test", 1}, out, error);
        return out;
    }
};

// #131 B1: a flush writes states into the engine's folder, which need not
// be the session file's. The save copies each state the file names into
// the file's own folder first, so a copy of that folder reopens with them.
int test_states_follow_the_session_file() {
    TempDir tmp;
    const fs::path engine_dir = tmp.path / "Untitled";
    const std::string blob = "a plugin's state blob";
    const std::string sha = sonora::sha256_hex(blob);
    const Plugged p(blob);
    CHECK(mentions(p.bytes(), sha.c_str()));
    SessionFolder f;
    std::string why, caution;
    CHECK(f.write_states(engine_dir.string(), {{sha, blob}}, why));

    const fs::path file = tmp.path / "Mix" / "Mix.sonora";
    CHECK(f.save(file.string(), p.bytes(), p.h.current(), why, caution, engine_dir.string()));
    CHECK(caution.empty());
    CHECK(read(tmp.path / "Mix" / "Plugin State" / (sha + ".state")) == blob);
    CHECK(entries(tmp.path / "Mix" / "Plugin State") == 1);
    CHECK(f.states_written() == 2);
    // Saved again: already there, not copied again.
    CHECK(f.save(file.string(), p.bytes(), p.h.current(), why, caution, engine_dir.string()));
    CHECK(f.states_written() == 2);

    // Reopened from a copy, nowhere near the engine's folder.
    const fs::path copy = tmp.path / "Stick" / "Mix";
    fs::create_directories(copy.parent_path());
    fs::copy(tmp.path / "Mix", copy, fs::copy_options::recursive);
    fs::remove_all(engine_dir);
    Loaded loaded;
    LoadError error;
    CHECK(deserialize(read(copy / "Mix.sonora"), loaded, error));
    const Snapshot& s = loaded.snapshot;
    CHECK(s.edit->tracks.size() == 1 && s.edit->tracks[0]->inserts.size() == 1);
    const InsertMix* m = s.mix.find_insert(s.edit->tracks[0]->inserts[0].id);
    CHECK(m != nullptr && m->state);
    CHECK(read(copy / m->state->file) == blob);
    CHECK(sonora::sha256_hex(read(copy / m->state->file)) == m->state->sha256);
    return 0;
}

// A named state found in neither folder refuses the save, and the folder
// the save made is taken back; one the replaced file already named (lost
// before, said at open) does not block saving.
int test_missing_state_refused() {
    TempDir tmp;
    const std::string blob = "state";
    const std::string sha = sonora::sha256_hex(blob);
    const Plugged p(blob);
    SessionFolder f;
    std::string why, caution;
    const fs::path file = tmp.path / "Mix" / "Mix.sonora";
    CHECK(!f.save(file.string(), p.bytes(), p.h.current(), why, caution, (tmp.path / "Untitled").string()));
    CHECK(mentions(why, "plugin state") && mentions(why, sha.substr(0, 12).c_str()));
    CHECK(!fs::exists(tmp.path / "Mix"));

    // A source whose file does not hash to its name is no source.
    write(tmp.path / "Untitled" / "Plugin State" / (sha + ".state"), "torn");
    CHECK(!f.save(file.string(), p.bytes(), p.h.current(), why, caution, (tmp.path / "Untitled").string()));
    CHECK(!fs::exists(tmp.path / "Mix"));

    write(tmp.path / "Untitled" / "Plugin State" / (sha + ".state"), blob);
    CHECK(f.save(file.string(), p.bytes(), p.h.current(), why, caution, (tmp.path / "Untitled").string()));
    fs::remove(tmp.path / "Mix" / "Plugin State" / (sha + ".state"));
    fs::remove_all(tmp.path / "Untitled");
    CHECK(f.save(file.string(), p.bytes(), p.h.current(), why, caution, (tmp.path / "Untitled").string()));
    return 0;
}

int g_state_dir_syncs = 0;
bool g_fail_state_dir_sync = false;
bool counting_sync(const std::string& path) {
    if (fs::path(path).filename() != "Plugin State") return true;
    ++g_state_dir_syncs;
    return !g_fail_state_dir_sync;
}

// #131 N1: a state renamed into place before a folder sync that failed is
// not a durable entry. The next flush finds its bytes there and must still
// sync the folder before answering Ok; once a sync has covered it, not again.
int test_state_folder_synced_until_durable() {
    TempDir tmp;
    const fs::path folder = tmp.path / "Mix";
    const std::string blob = "state";
    SessionFolder f(counting_sync);
    std::string why;
    g_state_dir_syncs = 0;
    g_fail_state_dir_sync = true;
    CHECK(!f.write_states(folder.string(), {{sonora::sha256_hex(blob), blob}}, why));
    g_fail_state_dir_sync = false;
    CHECK(f.write_states(folder.string(), {{sonora::sha256_hex(blob), blob}}, why));
    CHECK(g_state_dir_syncs == 2);
    CHECK(f.write_states(folder.string(), {{sonora::sha256_hex(blob), blob}}, why));
    CHECK(g_state_dir_syncs == 2);
    return 0;
}

// #131 N2: a crash inside a state write leaves "<sha>.state.tmp"; it goes
// the first time the folder is seen, by a flush or by a save.
int test_interrupted_state_write_swept() {
    TempDir tmp;
    const std::string blob = "state";
    const fs::path flushed = tmp.path / "Untitled" / "Plugin State" / (std::string(64, 'a') + ".state.tmp");
    const fs::path saved = tmp.path / "Mix" / "Plugin State" / (std::string(64, 'b') + ".state.tmp");
    write(flushed, "half");
    write(saved, "half");
    write(tmp.path / "Mix" / "Plugin State" / "notes.txt", "not ours");
    SessionFolder f;
    std::string why, caution;
    CHECK(f.write_states((tmp.path / "Untitled").string(), {{sonora::sha256_hex(blob), blob}}, why));
    CHECK(!fs::exists(flushed));
    Midi m(1);
    CHECK(f.save((tmp.path / "Mix" / "Mix.sonora").string(), m.bytes(), m.h.current(), why, caution));
    CHECK(!fs::exists(saved));
    CHECK(fs::exists(tmp.path / "Mix" / "Plugin State" / "notes.txt"));
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
    t(test_sequences_before_the_session_file);
    t(test_unheld_sequence_refused);
    t(test_on_disk_is_verified);
    t(test_interrupted_write_deleted_at_entry);
    t(test_unreadable_kept_file_blocks_cleanup);
    t(test_versions);
    t(test_backup_file_saves_sequences_in_the_session_folder);
    t(test_plugin_states);
    t(test_states_follow_the_session_file);
    t(test_missing_state_refused);
    t(test_state_folder_synced_until_durable);
    t(test_interrupted_state_write_swept);
    if (failed != 0) {
        std::fprintf(stderr, "session_folder: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("session_folder: %d passed\n", run);
    return 0;
}
