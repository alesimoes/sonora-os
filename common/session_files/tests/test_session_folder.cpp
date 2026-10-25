// SPDX-License-Identifier: GPL-3.0-or-later
//
// A session folder on disk (ADR-0015 §3, §6; session-format.md, "Saving"):
// sequence files before the session file, through tmp and rename; a file
// already there with the bytes its name hashes is not written again, and
// one that is not is; a save naming MIDI the document does not hold is
// refused with nothing written; cleanup deletes what no kept file names and
// nothing while a kept file cannot be read; an interrupted write is deleted
// when the folder is entered; the pre-migration copy and the newer-version
// refusal.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <unistd.h>

#include "model_test.h"
#include "sonora/model/session_file.h"
#include "sonora/session_files/session_folder.h"

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
    if (failed != 0) {
        std::fprintf(stderr, "session_folder: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("session_folder: %d passed\n", run);
    return 0;
}
