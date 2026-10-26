// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session file: canonical bytes, round trips, versions, and every kind
// of malformed file refused with its key path and line.
#include <algorithm>
#include <clocale>
#include <cstring>
#include <fstream>
#include <sstream>

#include "model_test.h"
#include "sonora/model/session_file.h"

#ifndef SONORA_MODEL_FIXTURES
#define SONORA_MODEL_FIXTURES "fixtures"
#endif

using namespace model_test;

namespace {

// Test-side file reading only; the model itself never touches a file.
std::string read_fixture(const char* name) {
    std::ifstream in(std::string(SONORA_MODEL_FIXTURES) + "/" + name, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// The fixtures folder as a session folder: "MIDI Sequences/<sha>.msq"
// beside the fixtures. A test hides or damages one file to see a session
// open without it.
struct FixtureFolder : SequenceSource {
    std::string hidden;     // a file read as absent
    std::string damaged;    // a file read with its last byte changed
    std::string truncated;  // a file read one byte short
    int reads = 0;
    bool read(const std::string& file, std::string& bytes, std::string& why) override {
        ++reads;
        if (file == hidden) {
            why = "the file is not there";
            return false;
        }
        bytes = read_fixture(file.c_str());
        if (bytes.empty()) {
            why = "the file is not there";
            return false;
        }
        if (file == damaged) bytes.back() = static_cast<char>(bytes.back() ^ 1);
        if (file == truncated) bytes.pop_back();
        return true;
    }
};

FixtureFolder g_folder;

bool load(const std::string& text, Loaded& out, LoadError& err) {
    return deserialize(text, out, err, &g_folder);
}

bool save(const Loaded& l, std::string& out) {
    std::string err;
    if (serialize(l.snapshot, WriteInfo{l.written_by, l.save_generation}, out, err)) {
        return true;
    }
    std::fprintf(stderr, "  serialize: %s\n", err.c_str());
    return false;
}

int line_of(const std::string& text, const std::string& needle) {
    const std::size_t at = text.find(needle);
    return static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n')) + 1;
}

// `text` with the one occurrence of `from` replaced; the test fails loudly
// if the fixture drifted and `from` is not there exactly once.
std::string edit(const std::string& text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    if (at == std::string::npos || text.find(from, at + 1) != std::string::npos) {
        std::fprintf(stderr, "  fixture edit: \"%s\" is not unique\n", from.c_str());
        return "<bad edit>";
    }
    std::string s = text;
    s.replace(at, from.size(), to);
    return s;
}

// The edited file is refused, the message has `needle`, the key path is
// `path`, and the line is `line` (0: not checked).
bool refused(const std::string& text, const char* needle, const char* path, int line = 0) {
    Loaded l;
    LoadError e;
    if (load(text, l, e)) {
        std::fprintf(stderr, "  loaded, expected \"%s\"\n", needle);
        return false;
    }
    const std::string full = e.text();
    if (full.find(needle) == std::string::npos) {
        std::fprintf(stderr, "  expected \"%s\" in: %s\n", needle, full.c_str());
        return false;
    }
    if (path != nullptr && e.path != path) {
        std::fprintf(stderr, "  path was \"%s\", expected \"%s\" (%s)\n", e.path.c_str(), path,
                     full.c_str());
        return false;
    }
    if (line != 0 && e.line != line) {
        std::fprintf(stderr, "  line was %d, expected %d (%s)\n", e.line, line, full.c_str());
        return false;
    }
    return true;
}

// What the v4 -> v5 migration adds to a version 4 file's text, as this
// build writes it: "kind": "audio" second in every track, and an empty
// midi_sources after the sources.
std::string as_v5(const std::string& v4) {
    std::string s = edit(v4, "\"sonora_session\": 4,", "\"sonora_session\": 5,");
    const std::string id = "\n      \"id\": ";
    const std::string name = "\n      \"name\":";
    for (std::size_t at = s.find(id); at != std::string::npos; at = s.find(id, at + 1)) {
        const std::size_t eol = s.find('\n', at + 1);
        if (s.compare(eol, name.size(), name) == 0) {
            s.insert(eol, "\n      \"kind\": \"audio\",");
        }
    }
    const std::size_t tracks = s.find("\n  \"tracks\": [");
    s.insert(tracks, "\n  \"midi_sources\": [],");
    return s;
}

// What v5 -> v6 makes of a file with no MIDI region: the version alone.
std::string as_v6(const std::string& v5) {
    return edit(v5, "\"sonora_session\": 5,", "\"sonora_session\": 6,");
}

// What v6 -> v7 makes of a file: the version, no session id and no take
// left, last.
std::string as_v7(const std::string& v6) {
    std::string s = edit(v6, "\"sonora_session\": 6,", "\"sonora_session\": 7,");
    const std::string end = "\n  ]\n}\n";
    if (s.size() < end.size() || s.compare(s.size() - end.size(), end.size(), end) != 0) {
        return "<bad edit>";
    }
    s.resize(s.size() - end.size());
    return s + "\n  ],\n  \"session_id\": \"\",\n  \"left_takes\": []\n}\n";
}

// What v7 -> v8 makes of a file: the version, and the count-off's bars
// after the punch crossfade.
std::string as_v8(const std::string& v7) {
    std::string s = edit(v7, "\"sonora_session\": 7,", "\"sonora_session\": 8,");
    const std::size_t at = s.find("\"punch_crossfade\": ");
    if (at == std::string::npos) return "<bad edit>";
    s.insert(s.find('\n', at), ",\n    \"count_off_bars\": 2");
    return s;
}

// What v8 -> v9 makes of a file with no loop range: the version, and loop
// playback off after the count-off's bars.
std::string as_v9(const std::string& v8) {
    std::string s = edit(v8, "\"sonora_session\": 8,", "\"sonora_session\": 9,");
    const std::size_t at = s.find("\"count_off_bars\": ");
    if (at == std::string::npos) return "<bad edit>";
    s.insert(s.find('\n', at), ",\n    \"loop_playback\": false");
    return s;
}

// A version 4 file with no MIDI region, as this build saves it.
std::string as_current(const std::string& v4) {
    return as_v9(as_v8(as_v7(as_v6(as_v5(v4)))));
}

// The current version's frozen fixture: what this build writes. Its
// sequence files are in fixtures/MIDI Sequences/.
constexpr const char* kCurrent = "v9-loop-playback.sonora";
// The version 8 fixture: the same session before loop playback's switch.
constexpr const char* kNoLoopPlayback = "v8-count-off.sonora";
// The version 7 fixture: the same session before the count-off's bars.
constexpr const char* kNoCountOff = "v7-session-id.sonora";
// The version 6 fixture: the same session with no id and no take left.
constexpr const char* kNoSessionId = "v6-midi.sonora";
// The version 5 fixture, the same session with its MIDI inline: what the
// inline reader still reads, and the source of the v5 -> v6 migration.
constexpr const char* kInlineMidi = "v5-midi.sonora";
const std::string kBassFile =
    "MIDI Sequences/dc6e2eac578d3696f120d40fc46c6eb1f4e14483c8db63382bb1e08ab9fba9e9.msq";

int test_fixture_is_canonical() {
    const std::string text = read_fixture(kCurrent);
    CHECK(!text.empty());
    Loaded l;
    LoadError e;
    if (!load(text, l, e)) std::fprintf(stderr, "  %s\n", e.text().c_str());
    g_folder.reads = 0;
    CHECK(load(text, l, e));
    CHECK(l.file_version == 9);
    CHECK(l.repairs.empty());
    CHECK(l.missing_sequences.empty());
    // Three regions' sequences, each file read once.
    CHECK(l.verified_sequences.size() == 3 && g_folder.reads == 3);
    CHECK(l.save_generation == 42);
    CHECK(l.written_by == "sonora-engine 0.2.0");
    // Load, save, compare bytes: the writer is canonical.
    std::string out;
    CHECK(save(l, out));
    if (out != text) {
        std::ofstream("v9-loop-playback.out.sonora", std::ios::binary) << out;
        std::fprintf(stderr, "  re-saved bytes differ; wrote v9-loop-playback.out.sonora\n");
    }
    CHECK(out == text);
    // And each sequence encodes to the bytes of the file that names it.
    SequenceDigests digests;
    for (const NamedSequence& n : named_sequences(l.snapshot, digests)) {
        CHECK(encode_sequence(*n.data) == read_fixture(n.ref.file.c_str()));
    }

    const Snapshot& s = l.snapshot;
    CHECK(invariants_hold(s));
    CHECK(s.next_id == 53);   // max(id) + 1; the counter is not stored
    CHECK(!s.mix.find(3)->solo_safe);
    CHECK(s.mix.find(4)->solo_safe);

    // The inserts: structure on the track, values in the mix.
    const TrackEdit& vox = *s.edit->tracks[0];
    CHECK(vox.inserts.size() == 2);
    CHECK(vox.inserts[0].id == 42 && vox.inserts[0].slot == 0 && vox.inserts[0].active);
    CHECK(vox.inserts[0].plugin.format == PluginFormat::Lv2);
    CHECK(vox.inserts[0].plugin.id == "http://gareus.org/oss/lv2/dpl#mono");
    CHECK(vox.inserts[1].slot == 3 && !vox.inserts[1].active);
    CHECK(vox.inserts[1].plugin.format == PluginFormat::Vst3);
    CHECK(s.edit->tracks[1]->inserts.empty());
    const InsertMix* lim = s.mix.find_insert(42);
    CHECK(lim != nullptr && !lim->bypass && lim->params.size() == 2);
    CHECK(lim->params[0].key == "threshold" && lim->params[0].value == -1.0f);
    CHECK(lim->params[1].value == 0.3f);
    CHECK(lim->state && lim->state->bytes == 812);
    CHECK(lim->state->file == plugin_state_file(lim->state->sha256));
    const InsertMix* comp = s.mix.find_insert(43);
    CHECK(comp != nullptr && comp->bypass && !comp->state);
    CHECK(comp->params[1].key == "4294967295" && comp->params[1].value == 1.0f);
    CHECK(s.revision == 0);
    CHECK(s.mix.find(3)->gain_db == -2.5f);
    CHECK(s.mix.find(4)->pan == -0.5f);
    CHECK(s.sources->find(9)->path == "Audio Files/Rhodes \xC3\xA9t\xC3\xA9.wav");
    CHECK(s.sources->find(9)->kind == SourceKind::Imported);
    CHECK(s.settings.tempo.size() == 2 && s.settings.tempo[1].bpm == 96.5f);
    const Playlist& p = playlist_of(s, 0);
    CHECK(p.regions.size() == 3 && p.crossfades.size() == 2);
    CHECK(p.crossfades[1].length == 241);
    CHECK(s.edit->tracks[0]->playlists.size() == 2);

    // One track of each kind (midi.md §4.5).
    CHECK(s.edit->tracks.size() == 4);
    CHECK(s.edit->tracks[0]->kind == TrackKind::Audio);
    const TrackEdit& bass = *s.edit->tracks[2];
    CHECK(bass.kind == TrackKind::Midi && bass.channels == 0 && bass.playlists.empty());
    CHECK(bass.midi_playlists.size() == 1 && bass.active_playlist == 46);
    const MidiPlaylist& bp = *bass.midi_playlists[0];
    CHECK(bp.regions.size() == 2);
    const MidiRegion& b1 = bp.regions[0];
    CHECK(b1.id == 47 && b1.position == 15360000 && b1.length == 30720000 && b1.offset == 0);
    CHECK(b1.source == 44 && !b1.mute);
    CHECK(b1.data->notes().size() == 3);
    CHECK(b1.data->notes()[1].start == 960000 && b1.data->notes()[1].pitch == 43);
    CHECK(b1.data->notes()[2].channel == 1 && b1.data->notes()[2].release_velocity == 0);
    CHECK(b1.data->events().size() == 4);
    // At one tick, in stored order: the bank select before the program change.
    CHECK(b1.data->events()[0].status == 0xB0 && b1.data->events()[1].status == 0xC0);
    CHECK(b1.data->events()[2].status == 0xE0 && b1.data->events()[2].data2 == 72);
    CHECK(bp.regions[1].source == 0 && bp.regions[1].mute && bp.regions[1].data->notes().empty());
    const TrackMix* bm = s.mix.find(45);
    CHECK(bm != nullptr && bm->input.empty() && bm->gain_db == 0.0f);
    CHECK(bm->midi.in_port == "all" && bm->midi.in_channel == 0);
    CHECK(bm->midi.out_port == "UMC1820 MIDI 1" && bm->midi.out_channel == 2);

    const TrackEdit& piano = *s.edit->tracks[3];
    CHECK(piano.kind == TrackKind::Instrument && piano.channels == 2);
    CHECK(piano.midi_playlists.size() == 2 && piano.midi_playlists[1]->regions.empty());
    const MidiRegion& pr = piano.midi_playlists[0]->regions[0];
    CHECK(pr.offset == 480000 && pr.data->sysex().size() == 1);
    CHECK(pr.data->sysex()[0].bytes ==
          (std::vector<std::uint8_t>{0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}));
    const TrackMix* pm = s.mix.find(49);
    CHECK(pm != nullptr && pm->gain_db == -6.0f && pm->pan == 0.25f && !pm->solo_safe);
    CHECK(pm->midi.in_port == "Keystation 49 MIDI 1" && pm->midi.in_channel == 1);
    CHECK(pm->midi.out_port == "none");

    const MidiSource* ms = s.sources->find_midi(44);
    CHECK(ms != nullptr && ms->path == "MIDI Files/Synth Bass_01.mid" && ms->events == 412);
    CHECK(ms->kind == SourceKind::Recorded && ms->recorded_on_track == 45);
    CHECK(s.sources->find(44) == nullptr);
    return 0;
}

// The key set is the kind's, and every MIDI value is checked at load with
// its key path (midi.md §4.3, §4.4). On the version 5 fixture, whose
// inline lists the reader still reads and checks row by row, with the key
// paths of that file.
int test_malformed_midi() {
    const std::string text = read_fixture(kInlineMidi);
    const std::string bass = "      \"kind\": \"midi\",\n";
    CHECK(refused(edit(text, bass, bass + "      \"input\": [0],\n"), "unknown key \"input\"",
                  "tracks[2].input"));
    CHECK(refused(edit(text, bass, bass + "      \"channels\": 1,\n"),
                  "unknown key \"channels\"", "tracks[2].channels"));
    CHECK(refused(edit(text, bass, bass + "      \"inserts\": [],\n"),
                  "unknown key \"inserts\"", "tracks[2].inserts"));
    CHECK(refused(edit(text, "\"kind\": \"midi\",", "\"kind\": \"drums\","), "must be one of",
                  "tracks[2].kind"));
    // An instrument track has no MIDI output; an audio track no MIDI input.
    CHECK(refused(edit(text, "      \"monitor\": \"input\",\n      \"gain_db\": -6.0,",
                       "      \"midi_output\": {\"port\": \"none\", \"channel\": 0},\n"
                       "      \"monitor\": \"input\",\n      \"gain_db\": -6.0,"),
                  "unknown key \"midi_output\"", "tracks[3].midi_output"));
    CHECK(refused(edit(text, "\"port\": \"all\"", "\"port\": \"none\""),
                  "must be \"all\" or a port name", "tracks[2].midi_input.port"));
    CHECK(refused(edit(text, "\"port\": \"UMC1820 MIDI 1\"", "\"port\": \"all\""),
                  "must be \"none\" or a port name", "tracks[2].midi_output.port"));
    CHECK(refused(edit(text, "\"channel\": 2", "\"channel\": 17"), "1 to 16",
                  "tracks[2].midi_output.channel"));
    // Regions: the window, the notes, the events, the SysEx.
    CHECK(refused(edit(text, "\"length_ticks\": 3840000", "\"length_ticks\": 0"),
                  "length must be greater than 0", "tracks[2].playlists[0].regions[1]"));
    CHECK(refused(edit(text, "\"position_ticks\": 46080000", "\"position_ticks\": 46079999"),
                  "overlaps the region before it", "tracks[2].playlists[0].regions[1]"));
    CHECK(refused(edit(text, "\"position_ticks\": 0,", "\"position_ticks\": -1,"),
                  "must be an integer from 0", "tracks[3].playlists[0].regions[0].position_ticks"));
    CHECK(refused(edit(text, "[0, 480000, 0, 36, 96, 64]", "[0, 480000, 0, 36, 0, 64]"),
                  "velocity must be from 1 to 127",
                  "tracks[2].playlists[0].regions[0].notes[0]"));
    CHECK(refused(edit(text, "[0, 480000, 0, 36, 96, 64]", "[0, 480000, 16, 36, 96, 64]"),
                  "channel must be from 0 to 15", "tracks[2].playlists[0].regions[0].notes[0]"));
    CHECK(refused(edit(text, "[0, 480000, 0, 36, 96, 64]", "[0, 480000, 0, 36, 96]"),
                  "must be [start, length", "tracks[2].playlists[0].regions[0].notes[0]"));
    CHECK(refused(edit(text, "[0, 480000, 0, 36, 96, 64]", "[1000000, 480000, 0, 36, 96, 64]"),
                  "notes are not in order", "tracks[2].playlists[0].regions[0].notes[1]"));
    CHECK(refused(edit(text, "[0, 192, 5, 0]", "[0, 192, 5, 9]"), "data2 must be 0",
                  "tracks[2].playlists[0].regions[0].events[1]"));
    CHECK(refused(edit(text, "[0, 192, 5, 0]", "[0, 144, 5, 9]"), "other than a note",
                  "tracks[2].playlists[0].regions[0].events[1]"));
    CHECK(refused(edit(text, "[96000, 224, 0, 72]", "[2000000, 224, 0, 72]"),
                  "events are not sorted by tick", "tracks[2].playlists[0].regions[0].events[3]"));
    CHECK(refused(edit(text, "\"f07e7f0901f7\"", "\"F07E7F0901F7\""), "lower-case hex",
                  "tracks[3].playlists[0].regions[0].sysex[0][1]"));
    CHECK(refused(edit(text, "\"f07e7f0901f7\"", "\"f07e7f0901\""), "starts with F0 and ends with F7",
                  "tracks[3].playlists[0].regions[0].sysex[0]"));
    CHECK(refused(edit(text, "\"f07e7f0901f7\"", "\"f07e8f0901f7\""), "below 0x80",
                  "tracks[3].playlists[0].regions[0].sysex[0]"));
    CHECK(refused(edit(text, "\"f07e7f0901f7\"", "\"f0" + std::string(2 * kMaxSysexBytes, '0') + "f7\""),
                  "at most 4096 bytes", "tracks[3].playlists[0].regions[0].sysex[0][1]"));
    CHECK(refused(edit(text, "\"source\": 44,", "\"source\": 9,"), "MIDI source 9 does not exist",
                  "tracks[2].playlists[0].regions[0]"));
    CHECK(refused(edit(text, "\"source\": 44,", "\"source\": 0,"), "leave it out for none",
                  "tracks[2].playlists[0].regions[0].source"));
    CHECK(refused(edit(text, "\"active_playlist\": 46,", "\"active_playlist\": 11,"),
                  "names no playlist of this track", "tracks[2].active_playlist"));
    // MIDI sources share the one id space and the folder's paths.
    CHECK(refused(edit(text, "      \"id\": 44,\n      \"path\"", "      \"id\": 9,\n      \"path\""),
                  "id 9 is already used", "midi_sources[0].id"));
    CHECK(refused(edit(text, "\"events\": 412,", "\"events\": -1,"), "events",
                  "midi_sources[0].events"));
    return 0;
}

// A version 6 region's "data" is a reference, checked by key path like
// every other value before any file is read (ADR-0015 §3).
int test_malformed_sequence_refs() {
    const std::string text = read_fixture(kCurrent);
    const char* data = "tracks[2].playlists[0].regions[0].data";
    const std::string sha = "dc6e2eac578d3696f120d40fc46c6eb1f4e14483c8db63382bb1e08ab9fba9e9";
    CHECK(refused(edit(text, "\"sha256\": \"" + sha, "\"sha256\": \"DC6E" + sha.substr(4)),
                  "sha256 must be 64 lower-case hex digits", data));
    CHECK(refused(edit(text, "\"file\": \"MIDI Sequences/dc6e", "\"file\": \"MIDI Files/dc6e"),
                  "file must be \"MIDI Sequences/dc6e", data));
    CHECK(refused(edit(text, "\"bytes\": 54,", "\"bytes\": -54,"), "must be an integer from 0",
                  "tracks[2].playlists[0].regions[0].data.bytes"));
    CHECK(refused(edit(text, "\"bytes\": 54,", ""), "missing required key \"bytes\"", data));
    CHECK(refused(edit(text, "\"bytes\": 54,", "\"bytes\": 54, \"chunks\": 1,"),
                  "unknown key \"chunks\"", "tracks[2].playlists[0].regions[0].data.chunks"));
    // The inline lists are version 5's; a version 6 file cannot hold them.
    const std::string inline_data = edit(
        text,
        "\"data\": {\n                \"file\": \"MIDI Sequences/1c9b",
        "\"notes\": [],\n              \"data\": {\n                \"file\": \"MIDI Sequences/1c9b");
    CHECK(refused(inline_data, "unknown key \"notes\"", "tracks[2].playlists[0].regions[1].notes"));
    return 0;
}

// O5: PR #100's format 5 opens through v5 -> v6 with its inline sequences,
// says once that they move beside the file, and its first save is exactly
// the version 6 fixture and its files.
int test_v5_migrates() {
    const std::string v5 = read_fixture(kInlineMidi);
    CHECK(!v5.empty());
    Loaded l;
    LoadError e;
    g_folder.reads = 0;
    if (!load(v5, l, e)) std::fprintf(stderr, "  %s\n", e.text().c_str());
    CHECK(load(v5, l, e));
    CHECK(g_folder.reads == 0);   // nothing to read: the lists are in the file
    CHECK(l.file_version == 5);
    CHECK(l.missing_sequences.empty() && l.verified_sequences.empty());
    CHECK(l.repairs.size() == 1);
    CHECK(l.repairs[0].path == "MIDI regions" && l.repairs[0].was == "3 with their events inline");
    CHECK(mentions(l.repairs[0].now, "MIDI Sequences/"));
    CHECK(mentions(l.repairs[0].why, "version 5 file is kept as a backup"));
    Loaded current;
    const std::string v9 = as_v9(as_v8(as_v7(read_fixture(kNoSessionId))));
    CHECK(load(v9, current, e));
    CHECK(same_document(l.snapshot, current.snapshot));
    std::string out;
    CHECK(save(l, out));
    CHECK(out == v9);
    SequenceDigests digests;
    const std::vector<NamedSequence> named = named_sequences(l.snapshot, digests);
    CHECK(named.size() == 3);
    for (const NamedSequence& n : named) {
        CHECK(n.encoded == read_fixture(n.ref.file.c_str()));
    }
    std::int64_t version = 0;
    CHECK(file_format_version(out, version) && version == 9);
    // A version 5 file cannot carry what version 5 does not define.
    CHECK(refused(edit(v5, "\"offset_ticks\": 480000,",
                       "\"offset_ticks\": 480000,\n              \"data\": {},"),
                  "migrating from format version 5: tracks[3].playlists[0].regions[0] has \"data\"",
                  ""));
    return 0;
}

// O2: a sequence file missing or damaged at open does not stop the session
// opening or drop a region. Its regions keep their windows, silent, and the
// next save writes their reference back as it was.
int test_missing_sequence_opens() {
    const std::string text = read_fixture(kCurrent);
    struct Case {
        const char* what;
        std::string FixtureFolder::*how;
        const char* why;
    };
    const Case cases[] = {
        {"hidden", &FixtureFolder::hidden, "not there"},
        {"damaged", &FixtureFolder::damaged, "checksum does not match"},
        {"truncated", &FixtureFolder::truncated, "53 bytes and the session says 54"},
    };
    for (const Case& c : cases) {
        g_folder.*(c.how) = kBassFile;
        Loaded l;
        LoadError e;
        const bool ok = load(text, l, e);
        (g_folder.*(c.how)).clear();
        if (!ok) std::fprintf(stderr, "  %s: %s\n", c.what, e.text().c_str());
        CHECK(ok);
        CHECK(l.missing_sequences.size() == 1 && l.verified_sequences.size() == 2);
        const MissingSequence& m = l.missing_sequences[0];
        CHECK(m.ref.file == kBassFile && m.ref.notes == 3 && m.ref.events == 4);
        CHECK(mentions(m.why, c.why));
        CHECK(m.regions == std::vector<std::string>{"tracks[2].playlists[0].regions[0]"});
        // It came from take 44, which the opener can offer to rebuild it from.
        CHECK(m.sources == std::vector<Id>{44});
        const MidiRegion& r = l.snapshot.edit->tracks[2]->midi_playlists[0]->regions[0];
        CHECK(r.data->missing && r.data->missing->file == kBassFile);
        CHECK(r.data->notes().empty() && r.data->events().empty());
        CHECK(r.position == 15360000 && r.length == 30720000);
        CHECK(invariants_hold(l.snapshot));
        std::string out;
        CHECK(save(l, out));
        CHECK(out == text);
        // Not editable until it is restored or rebuilt; still movable.
        History h(l.snapshot);
        EditMidi em;
        em.playlist = 46;
        em.region = r.id;
        em.add_notes = {sonora::midi::Note{}};
        em.add_notes[0].velocity = 100;
        std::string why;
        CHECK(!h.apply(em, why) && mentions(why, "MIDI data is missing"));
        CHECK_APPLY(h, (MoveRegion{46, r.id, 0, 0, Timebase::Ticks}));
    }
    // Opened without its folder, every sequence is missing, and still opens.
    Loaded l;
    LoadError e;
    CHECK(deserialize(text, l, e));
    CHECK(l.missing_sequences.size() == 3);
    CHECK(mentions(l.missing_sequences[0].why, "without its folder"));
    return 0;
}

// What ModelAction::Load carries (protocol 8): the session file and the
// sequence files it names, in one blob the engine decodes without a folder.
int test_session_bundle() {
    Loaded l;
    LoadError e;
    CHECK(load(read_fixture(kCurrent), l, e));
    std::string bundle, err;
    CHECK(encode_session_bundle(l.snapshot, WriteInfo{"sonora-ui", 1}, bundle, err));
    Loaded back;
    if (!decode_session_bundle(bundle, back, e)) std::fprintf(stderr, "  %s\n", e.text().c_str());
    CHECK(decode_session_bundle(bundle, back, e));
    CHECK(back.missing_sequences.empty() && back.verified_sequences.size() == 3);
    CHECK(same_document(back.snapshot, l.snapshot));
    // A bare session file is a bundle with no sequence files.
    Loaded no_id;
    CHECK(load(read_fixture(kNoSessionId), no_id, e));
    CHECK(decode_session_bundle(read_fixture(kInlineMidi), back, e));
    CHECK(same_document(back.snapshot, no_id.snapshot));
    // Cut anywhere, it is refused, or opens with a sequence missing; it is
    // never read past its end.
    for (std::size_t n : {std::size_t{4}, std::size_t{12}, bundle.size() / 2, bundle.size() - 1}) {
        Loaded cut;
        if (decode_session_bundle(bundle.substr(0, n), cut, e)) {
            CHECK(!cut.missing_sequences.empty());
        }
    }
    CHECK(!decode_session_bundle(bundle + "x", back, e) && mentions(e.message, "bytes after"));
    // A missing sequence crosses as its reference and arrives missing.
    g_folder.hidden = kBassFile;
    Loaded gap;
    CHECK(load(read_fixture(kCurrent), gap, e));
    g_folder.hidden.clear();
    CHECK(encode_session_bundle(gap.snapshot, WriteInfo{"sonora-ui", 1}, bundle, err));
    CHECK(decode_session_bundle(bundle, back, e));
    CHECK(back.missing_sequences.size() == 1 && back.missing_sequences[0].ref.file == kBassFile);
    CHECK(mentions(back.missing_sequences[0].why, "did not send it"));
    CHECK(same_document(back.snapshot, gap.snapshot));
    return 0;
}

// The frozen version-1 fixture loads forever, through v1 -> v2, which adds
// an empty insert list to every track and changes nothing else.
int test_v1_migrates() {
    const std::string v1 = read_fixture("v1-punch.sonora");
    CHECK(!v1.empty());
    Loaded l;
    LoadError e;
    if (!load(v1, l, e)) std::fprintf(stderr, "  %s\n", e.text().c_str());
    CHECK(load(v1, l, e));
    CHECK(l.file_version == 1);
    CHECK(invariants_hold(l.snapshot));
    CHECK(l.snapshot.next_id == 42);
    for (const auto& t : l.snapshot.edit->tracks) CHECK(t->inserts.empty());
    CHECK(l.snapshot.mix.inserts.empty());

    for (const TrackMix& m : l.snapshot.mix.tracks) CHECK(!m.solo_safe);

    // Saved, it is the same session at version 6: the version, one empty
    // list, one solo safe off and one audio kind per track, and an empty
    // midi_sources, are the whole difference.
    std::string out;
    CHECK(save(l, out));
    const std::string expected = as_current(
        edit(edit(edit(v1, "\"sonora_session\": 1,", "\"sonora_session\": 4,"),
                  "      \"active_playlist\": 11,\n",
                  "      \"solo_safe\": false,\n      \"active_playlist\": 11,\n"
                  "      \"inserts\": [],\n"),
             "      \"active_playlist\": 13,\n",
             "      \"solo_safe\": false,\n      \"active_playlist\": 13,\n"
             "      \"inserts\": [],\n"));
    if (out != expected) {
        std::ofstream("v1-migrated.out.sonora", std::ios::binary) << out;
        std::fprintf(stderr, "  migrated bytes differ; wrote v1-migrated.out.sonora\n");
    }
    CHECK(out == expected);
    std::int64_t version = 0;
    CHECK(file_format_version(v1, version) && version == 1);
    CHECK(file_format_version(out, version) && version == kFormatVersion);
    CHECK(!file_format_version("{\"keymap\": 1}", version));
    CHECK(!file_format_version("not json", version));

    // A version-1 file cannot carry what version 1 does not define.
    CHECK(refused(edit(v1, "      \"active_playlist\": 11,\n",
                       "      \"active_playlist\": 11,\n      \"inserts\": [],\n"),
                  "migrating from format version 1: tracks[0] has \"inserts\"", ""));
    return 0;
}

// The frozen version-2 fixture loads forever, through v2 -> v3, which turns
// solo safe off on every track and changes nothing else.
int test_v2_migrates() {
    const std::string v2 = read_fixture("v2-inserts.sonora");
    CHECK(!v2.empty());
    Loaded l;
    LoadError e;
    if (!load(v2, l, e)) std::fprintf(stderr, "  %s\n", e.text().c_str());
    CHECK(load(v2, l, e));
    CHECK(l.file_version == 2);
    CHECK(invariants_hold(l.snapshot));
    CHECK(l.snapshot.mix.tracks.size() == 2);
    for (const TrackMix& m : l.snapshot.mix.tracks) CHECK(!m.solo_safe);

    std::string out;
    CHECK(save(l, out));
    const std::string expected = as_current(
        edit(edit(edit(v2, "\"sonora_session\": 2,", "\"sonora_session\": 4,"),
                  "      \"solo\": false,\n      \"active_playlist\": 11,\n",
                  "      \"solo\": false,\n      \"solo_safe\": false,\n"
                  "      \"active_playlist\": 11,\n"),
             "      \"solo\": false,\n      \"active_playlist\": 13,\n",
             "      \"solo\": false,\n      \"solo_safe\": false,\n"
             "      \"active_playlist\": 13,\n"));
    if (out != expected) {
        std::ofstream("v2-migrated.out.sonora", std::ios::binary) << out;
        std::fprintf(stderr, "  migrated bytes differ; wrote v2-migrated.out.sonora\n");
    }
    CHECK(out == expected);

    // A version-2 file cannot carry what version 2 does not define.
    CHECK(refused(edit(v2, "      \"solo\": false,\n      \"active_playlist\": 13,\n",
                       "      \"solo\": false,\n      \"solo_safe\": true,\n"
                       "      \"active_playlist\": 13,\n"),
                  "migrating from format version 2: tracks[1] has \"solo_safe\"", ""));
    return 0;
}

// The frozen version-3 fixture loads forever, through v3 -> v4. Its tempo
// map is inside the bounds, so nothing but the version changes and nothing
// is reported.
int test_v3_migrates() {
    const std::string v3 = read_fixture("v3-solo-safe.sonora");
    CHECK(!v3.empty());
    Loaded l;
    LoadError e;
    if (!load(v3, l, e)) std::fprintf(stderr, "  %s\n", e.text().c_str());
    CHECK(load(v3, l, e));
    CHECK(l.file_version == 3);
    CHECK(l.repairs.empty());
    CHECK(invariants_hold(l.snapshot));
    std::string out;
    CHECK(save(l, out));
    CHECK(out == as_current(edit(v3, "\"sonora_session\": 3,", "\"sonora_session\": 4,")));
    return 0;
}

// The frozen version-4 fixture loads forever, through v4 -> v5: every track
// is an audio track, the session has no MIDI source, nothing is reported.
int test_v4_migrates() {
    const std::string v4 = read_fixture("v4-tempo-bounds.sonora");
    CHECK(!v4.empty());
    Loaded l;
    LoadError e;
    if (!load(v4, l, e)) std::fprintf(stderr, "  %s\n", e.text().c_str());
    CHECK(load(v4, l, e));
    CHECK(l.file_version == 4);
    CHECK(l.repairs.empty());
    CHECK(invariants_hold(l.snapshot));
    for (const auto& t : l.snapshot.edit->tracks) {
        CHECK(t->kind == TrackKind::Audio && t->midi_playlists.empty());
    }
    CHECK(l.snapshot.sources->midi_sources.empty());
    std::string out;
    CHECK(save(l, out));
    if (out != as_current(v4)) {
        std::ofstream("v4-migrated.out.sonora", std::ios::binary) << out;
        std::fprintf(stderr, "  migrated bytes differ; wrote v4-migrated.out.sonora\n");
    }
    CHECK(out == as_current(v4));
    // A version-4 file cannot carry what version 4 does not define.
    CHECK(refused(edit(v4, "      \"name\": \"Keys\",", "      \"kind\": \"audio\",\n      \"name\": \"Keys\","),
                  "migrating from format version 4: tracks[1] has \"kind\"", ""));
    CHECK(refused(edit(v4, "  \"tracks\": [", "  \"midi_sources\": [],\n  \"tracks\": ["),
                  "migrating from format version 4: the file has \"midi_sources\"", ""));
    return 0;
}

// O7: a version 3 tempo map outside the bounds opens inside them, with each
// change reported by key path, old value and new; nothing inside them moves.
int test_v3_tempo_brought_inside_bounds() {
    const std::string v3 = read_fixture("v3-tempo-out-of-bounds.sonora");
    CHECK(!v3.empty());
    Loaded l;
    LoadError e;
    if (!load(v3, l, e)) std::fprintf(stderr, "  %s\n", e.text().c_str());
    CHECK(load(v3, l, e));
    CHECK(l.file_version == 3);
    const std::vector<TempoEntry>& t = l.snapshot.settings.tempo;
    CHECK(t.size() == 4);
    CHECK(t[0].bpm == 1.0f && t[0].meter[0] == 4 && t[0].meter[1] == 4);
    CHECK(t[1].bpm == 1000.0f && t[1].meter[0] == 7 && t[1].meter[1] == 4);
    CHECK(t[2].bpm == 96.5f && t[2].meter[0] == 5 && t[2].meter[1] == 64);
    CHECK(t[3].bpm == 1000.0f && t[3].meter[0] == 3 && t[3].meter[1] == 16);
    if (l.repairs.size() != 5) {
        for (const Repair& r : l.repairs) std::fprintf(stderr, "  repair: %s\n", r.text().c_str());
    }
    CHECK(l.repairs.size() == 5);
    // Each by key path, the old and the new, so the opener can show them
    // one by one.
    CHECK(l.repairs[0].path == "tempo[0].bpm" && l.repairs[0].was == "0.5 bpm" &&
          l.repairs[0].now == "1 bpm");
    CHECK(l.repairs[1].path == "tempo[1].bpm" && l.repairs[1].was == "2400.0 bpm" &&
          l.repairs[1].now == "1000 bpm");
    CHECK(l.repairs[2].path == "tempo[1].meter" && l.repairs[2].was == "7/3" &&
          l.repairs[2].now == "7/4");
    CHECK(l.repairs[3].path == "tempo[2].meter" && l.repairs[3].was == "5/128" &&
          l.repairs[3].now == "5/64");
    CHECK(l.repairs[4].path == "tempo[3].meter" && l.repairs[4].was == "3/12" &&
          l.repairs[4].now == "3/16");
    CHECK(mentions(l.repairs[0].why, "1 to 1000 bpm"));
    CHECK(mentions(l.repairs[2].why, "power of two"));
    for (const Repair& r : l.repairs) CHECK(mentions(r.why, "version 3 file"));
    CHECK(l.repairs[1].text() ==
          "tempo[1].bpm: 2400.0 bpm -> 1000 bpm (" + l.repairs[1].why + ")");
    // Saved, it is a version 4 file that opens with nothing to report.
    std::string out;
    CHECK(save(l, out));
    Loaded again;
    CHECK(load(out, again, e));
    CHECK(again.file_version == kFormatVersion && again.repairs.empty());
    CHECK(again.snapshot.settings.tempo == t);
    return 0;
}

// A version 4 file outside the bounds is refused by key path, not changed:
// only a migration changes what a file says.
int test_v4_tempo_bounds_refused() {
    const std::string text = read_fixture(kCurrent);
    CHECK(refused(edit(text, "\"bpm\": 96.5,", "\"bpm\": 1000.5,"),
                  "bpm must be from 1 to 1000", "tempo[1]"));
    CHECK(refused(edit(text, "\"bpm\": 120.0,", "\"bpm\": 0.99,"),
                  "bpm must be from 1 to 1000", "tempo[0]"));
    CHECK(refused(edit(text, "\"meter\": [6, 8]", "\"meter\": [6, 6]"),
                  "power of two from 1 to 64", "tempo[1]"));
    CHECK(refused(edit(text, "\"meter\": [6, 8]", "\"meter\": [6, 128]"),
                  "power of two from 1 to 64", "tempo[1]"));
    CHECK(refused(edit(text, "\"sample_rate\": 48000,", "\"sample_rate\": 768000,"),
                  "must be at most 384000", "sample_rate"));
    // The edges are inside.
    Loaded l;
    LoadError e;
    CHECK(load(edit(edit(text, "\"bpm\": 96.5,", "\"bpm\": 1000.0,"), "\"meter\": [6, 8]",
                    "\"meter\": [6, 64]"),
               l, e));
    CHECK(load(edit(text, "\"bpm\": 120.0,", "\"bpm\": 1.0,"), l, e));
    return 0;
}

int test_round_trip_from_edits() {
    // A session built by operations, including a punch, survives
    // serialize -> deserialize as the same document, and saves to the same
    // bytes again.
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack t = add_track(ids, "Vox");
    CHECK_APPLY(h, t);
    const AddTrack t2 = add_track(ids, "Stereo \"Keys\"", 2);
    CHECK_APPLY(h, t2);
    const AddSource s = add_source(ids, 1'000'000, 1, t.track);
    CHECK_APPLY(h, s);
    const AddSource s2 = add_source(ids, 500'000, 2);
    CHECK_APPLY(h, s2);
    const Id r = ids();
    CHECK_APPLY(h, (PlaceRegion{t.playlist, region(r, s.source.id, 100000, 400000, 200000)}));
    CHECK_APPLY(h, (PlaceRegion{t2.playlist, region(ids(), s2.source.id, 0, 100000, 0, 2)}));
    const AddSource take = add_source(ids, 200000, 1, t.track);
    CHECK_APPLY(h, take);
    RecordTake op;
    op.track = t.track;
    op.playlist = t.playlist;
    op.region = region(ids(), take.source.id, 200000, 100000, 50000);
    op.region.gain_db = 0.1f;
    op.split_right_id = ids();
    op.crossfades = {Crossfade{r, op.region.id, 240, Align::Centre, FadeShape::EqualPower},
                     Crossfade{op.region.id, op.split_right_id, 241, Align::Pre,
                               FadeShape::SCurve}};
    CHECK_APPLY(h, op);
    CHECK_APPLY(h, (AddMarker{Marker{h.current().next_id, 48000, "Intro", 0}}));
    TrackMix m = *h.current().mix.find(t.track);
    m.gain_db = -0.3f;   // not exact in binary: the case that round-trips badly
    m.pan = 0.7f;
    m.solo_safe = true;
    CHECK_APPLY(h, SetMix{m});

    std::string bytes, err;
    CHECK(serialize(h.current(), WriteInfo{"test", 7}, bytes, err));
    Loaded l;
    LoadError e;
    CHECK(load(bytes, l, e));
    CHECK(same_document(l.snapshot, h.current()));
    CHECK(l.snapshot.mix.find(t.track)->gain_db == -0.3f);
    CHECK(l.snapshot.mix.find(t.track)->solo_safe);
    CHECK(!l.snapshot.mix.find(t2.track)->solo_safe);
    CHECK(l.save_generation == 7);
    std::string again;
    CHECK(save(l, again));
    CHECK(again == bytes);
    // Serialising the same snapshot twice gives the same bytes.
    std::string twice;
    CHECK(serialize(h.current(), WriteInfo{"test", 7}, twice, err));
    CHECK(twice == bytes);

    // After an undo, the undone take's region is gone from the file and its
    // source is not.
    CHECK(h.undo());
    CHECK(h.undo());   // the marker, then the take
    CHECK(serialize(h.current(), WriteInfo{"test", 8}, bytes, err));
    CHECK(load(bytes, l, e));
    CHECK(l.snapshot.sources->find(take.source.id) != nullptr);
    CHECK(playlist_of(l.snapshot, 0).regions.size() == 1);
    return 0;
}

int test_versions() {
    const std::string text = read_fixture(kCurrent);
    const int line = line_of(text, "\"sonora_session\"");
    CHECK(refused(edit(text, "\"sonora_session\": 9,", "\"sonora_session\": 10,"),
                  "format version 10, newer than this program, which reads up to version 9",
                  "sonora_session", line));
    // A newer file is refused even when it carries keys this version does
    // not know: the version is read before anything else.
    CHECK(refused(edit(edit(text, "\"sonora_session\": 9,", "\"sonora_session\": 10,"),
                       "\"name\": \"Vocals Friday\",", "\"mixer\": {},"),
                  "newer than this program", "sonora_session"));
    // Too large for int64 is still a newer version, not a malformed one.
    CHECK(refused(edit(text, "\"sonora_session\": 9,",
                       "\"sonora_session\": 99999999999999999999,"),
                  "format version 99999999999999999999, newer than this program",
                  "sonora_session"));
    CHECK(refused(edit(text, "\"sonora_session\": 9,", "\"sonora_session\": 0,"),
                  "does not exist", "sonora_session"));
    CHECK(refused(edit(text, "\"sonora_session\": 9,", "\"sonora_session\": 9.0,"),
                  "integer format version", "sonora_session"));
    // A current file must have the keys versions 2 and 3 added; only the
    // migrations fill them in for an older file.
    CHECK(refused(edit(text, "      \"active_playlist\": 13,\n      \"inserts\": [],\n",
                       "      \"active_playlist\": 13,\n"),
                  "missing required key \"inserts\"", "tracks[1]"));
    CHECK(refused(edit(text, "      \"solo_safe\": true,\n", ""),
                  "missing required key \"solo_safe\"", "tracks[1]"));
    CHECK(refused(edit(text, "\"solo_safe\": true,", "\"solo_safe\": 1,"),
                  "must be true or false", "tracks[1].solo_safe",
                  line_of(text, "\"solo_safe\": true,")));
    // The magic must come first, unlike every other key.
    CHECK(refused(edit(text, "  \"sonora_session\": 9,\n  \"written_by\": \"sonora-engine 0.2.0\",",
                       "  \"written_by\": \"sonora-engine 0.2.0\",\n  \"sonora_session\": 9,"),
                  "\"sonora_session\" must be the first key", ""));
    CHECK(refused("[]", "not a Sonora session", ""));
    CHECK(refused("{\"keymap\": 1}", "not a Sonora session", ""));
    return 0;
}

int test_key_order_is_lenient() {
    const std::string text = read_fixture(kCurrent);
    // As after a pass through jq: other keys in another order still load,
    // and the next save restores the canonical order.
    const std::string swapped =
        edit(edit(text, "  \"name\": \"Vocals Friday\",\n  \"sample_rate\": 48000,",
                  "  \"sample_rate\": 48000,\n  \"name\": \"Vocals Friday\","),
             "          \"id\": 12,\n          \"name\": \"Lead Vox.02\",",
             "          \"name\": \"Lead Vox.02\",\n          \"id\": 12,");
    CHECK(swapped != text);
    Loaded l;
    LoadError e;
    CHECK(load(swapped, l, e));
    std::string out;
    CHECK(save(l, out));
    CHECK(out == text);
    return 0;
}

int test_malformed() {
    const std::string text = read_fixture(kCurrent);
    const char* region0 = "tracks[0].playlists[0].regions[0]";

    // Syntax: JSON's own errors, with the line.
    CHECK(refused(edit(text, "\"pre_roll\": 96000,", "\"pre_roll\": 96000,,"),
                  "expected a quoted member name", nullptr, line_of(text, "\"pre_roll\"")));
    CHECK(refused(edit(text, "\"pre_roll\": 96000,", "\"pre_roll\": /* two s */ 96000,"),
                  "unexpected character '/'", nullptr));
    CHECK(refused(edit(text, "\"pre_roll\": 96000,", "\"pre_roll\": 1, \"pre_roll\": 2,"),
                  "duplicate member \"pre_roll\"", nullptr, line_of(text, "\"pre_roll\"")));

    // Keys: unknown, missing.
    CHECK(refused(edit(text, "\"source_offset\": 24000,", "\"source_offset\": 24000, \"colour\": 3,"),
                  "unknown key \"colour\"", "tracks[0].playlists[0].regions[0].colour",
                  line_of(text, "\"source_offset\": 24000,")));
    CHECK(refused(edit(text, "              \"source_offset\": 24000,\n", ""),
                  "missing required key \"source_offset\"", region0));
    CHECK(refused(edit(text, "      \"time_reference\": 0,\n      \"kind\": \"imported\"",
                       "      \"time_reference\": 0,\n      \"kind\": \"imported\",\n"
                       "      \"take\": 3"),
                  "unknown key \"take\"", "sources[2].take"));

    // Types and integers.
    CHECK(refused(edit(text, "\"name\": \"Lead Vox\",", "\"name\": 7,"), "must be a string",
                  "tracks[0].name", line_of(text, "\"name\": \"Lead Vox\",")));
    CHECK(refused(edit(text, "\"source_offset\": 24000,", "\"source_offset\": 24000.0,"),
                  "without a fraction or an exponent", "tracks[0].playlists[0].regions[0].source_offset"));
    CHECK(refused(edit(text, "\"source_offset\": 24000,", "\"source_offset\": 24e3,"),
                  "without a fraction or an exponent", nullptr));
    CHECK(refused(edit(text, "\"source_offset\": 24000,", "\"source_offset\": -24000,"),
                  "from 0 to 9007199254740991", nullptr));
    CHECK(refused(edit(text, "\"source_offset\": 24000,", "\"source_offset\": 9007199254740992,"),
                  "from 0 to 9007199254740991", nullptr));
    CHECK(refused(edit(text, "\"gain_db\": -2.5,", "\"gain_db\": 1e39,"), "too large for a float",
                  "tracks[0].gain_db"));
    CHECK(refused(edit(text, "\"gain_db\": -2.5,", "\"gain_db\": \"-2.5\","), "must be a number",
                  "tracks[0].gain_db"));
    CHECK(refused(edit(text, "\"monitor\": \"auto\",", "\"monitor\": \"Auto\","),
                  "must be one of \"auto\", \"input\", \"off\"", "tracks[0].monitor"));

    // Invariants, each with the object's path.
    CHECK(refused(edit(text, "\"position\": 3696000,", "\"position\": 3695000,"),
                  "overlaps the region before it", "tracks[0].playlists[0].regions[1]",
                  line_of(text, "\"id\": 22,") - 1));
    CHECK(refused(edit(text, "\"length\": 96000,\n              \"source\": [\n                {\n                  \"id\": 7,",
                       "\"length\": 2860000,\n              \"source\": [\n                {\n                  \"id\": 7,"),
                  "past the end of source 7", region0));
    CHECK(refused(edit(text, "\"source_offset\": 48000,", "\"source_offset\": 0,"),
                  "frames of handle before its start; the crossfade needs 120",
                  "tracks[0].playlists[0].crossfades[0]"));
    CHECK(refused(edit(text, "\"path\": \"Audio Files/Audio 3_01.wav\"", "\"path\": \"../Audio 3_01.wav\""),
                  "\"..\" component", "sources[0]"));
    CHECK(refused(edit(text, "\"active_playlist\": 11,", "\"active_playlist\": 99,"),
                  "names no playlist", "tracks[0].active_playlist"));
    CHECK(refused(edit(text, "\"id\": 41,", "\"id\": 21,"), "id 21 is already used by",
                  "markers[1].id"));
    CHECK(refused(edit(text, "\"position\": 9600000,\n      \"name\": \"Chorus 2\"",
                       "\"position\": 0,\n      \"name\": \"Chorus 2\""),
                  "sorted by position", "markers[1].position"));
    CHECK(refused(edit(text, "\"input\": [2],", "\"input\": [2, 3],"),
                  "one capture channel per track channel", "tracks[0].input"));
    CHECK(refused(edit(text, "\"channels\": 2,\n      \"input\"", "\"channels\": 3,\n      \"input\""),
                  "must be 1 or 2", "tracks[1].channels"));
    CHECK(refused(edit(text, "\"length\": 480,\n                \"shape\": \"s_curve\"",
                       "\"length\": 96001,\n                \"shape\": \"s_curve\""),
                  "longer than the region", "tracks[1].playlists[0].regions[0]"));
    CHECK(refused(edit(text, "\"bpm\": 96.5,", "\"bpm\": 0.0,"), "bpm must be a positive",
                  "tempo[1]"));
    return 0;
}

// Every rule of session-format.md's "Inserts", each refused with the
// insert's key path and line.
int test_malformed_inserts() {
    const std::string text = read_fixture(kCurrent);

    CHECK(refused(edit(text, "\"format\": \"lv2\",", "\"format\": \"clap\","),
                  "must be one of \"lv2\", \"vst3\"", "tracks[0].inserts[0].plugin.format",
                  line_of(text, "\"format\": \"lv2\",")));
    CHECK(refused(edit(text, "\"slot\": 3,", "\"slot\": 10,"), "from 0 to 9",
                  "tracks[0].inserts[1].slot", line_of(text, "\"slot\": 3,")));
    CHECK(refused(edit(text, "\"slot\": 3,", "\"slot\": 0,"), "slot 0 is used twice",
                  "tracks[0].inserts[1].slot"));
    CHECK(refused(edit(text, "\"slot\": 0,", "\"slot\": 5,"), "inserts must be ordered by slot",
                  "tracks[0].inserts[1].slot"));
    CHECK(refused(edit(text, "\"id\": 43,", "\"id\": 7,"), "id 7 is already used by sources[0]",
                  "tracks[0].inserts[1].id"));
    CHECK(refused(edit(text, "\"id\": \"565354436F6D703A6C73702D636F6D70\"",
                       "\"id\": \"565354436f6d703a6c73702d636f6d70\""),
                  "32 upper-case hex digits", "tracks[0].inserts[1].plugin.id"));
    CHECK(refused(edit(text, "\"id\": \"http://gareus.org/oss/lv2/dpl#mono\"",
                       "\"id\": \"x42 dpl\""),
                  "no space or control character", "tracks[0].inserts[0].plugin.id"));
    CHECK(refused(edit(text, "\"key\": \"threshold\"", "\"key\": \"thresh-old\""),
                  "is not an LV2 port symbol", "tracks[0].inserts[0].params[0]"));
    CHECK(refused(edit(text, "\"key\": \"gain\"", "\"key\": \"threshold\""),
                  "key \"threshold\" appears twice", "tracks[0].inserts[0].params[1]"));
    CHECK(refused(edit(text, "\"key\": \"0\"", "\"key\": \"00\""),
                  "is not a VST3 ParamID in decimal", "tracks[0].inserts[1].params[0]"));
    CHECK(refused(edit(text, "\"key\": \"4294967295\"", "\"key\": \"4294967296\""),
                  "is not a VST3 ParamID in decimal", "tracks[0].inserts[1].params[1]"));
    CHECK(refused(edit(text, "\"value\": 0.25", "\"value\": 1.25"),
                  "a VST3 value is normalised, from 0 to 1", "tracks[0].inserts[1].params[0]"));
    CHECK(refused(edit(text, "\"bytes\": 812", "\"bytes\": 812.5"),
                  "without a fraction or an exponent", "tracks[0].inserts[0].state.bytes"));
    CHECK(refused(edit(text, "\"file\": \"Plugin State/3f1c", "\"file\": \"Plugin State/3f1d"),
                  "file must be \"Plugin State/3f1c", "tracks[0].inserts[0].state"));
    CHECK(refused(edit(text, "\"sha256\": \"3f1c", "\"sha256\": \"3F1C"),
                  "64 lower-case hex digits", "tracks[0].inserts[0].state"));
    CHECK(refused(edit(text, "          \"bypass\": false,\n", ""),
                  "missing required key \"bypass\"", "tracks[0].inserts[0]"));
    CHECK(refused(edit(text, "          \"active\": false,\n",
                       "          \"active\": false,\n          \"wet\": 1,\n"),
                  "unknown key \"wet\"", "tracks[0].inserts[1].wet"));
    CHECK(refused(edit(text, "\"version\": \"1.2.14\"",
                       "\"version\": \"1.2.14\", \"vendor\": \"LSP\""),
                  "unknown key \"vendor\"", "tracks[0].inserts[1].plugin.vendor"));

    // Too many values is refused by the count, before any is read: the
    // malformed entry past the cap is never reached.
    std::string many = "\"params\": [\n";
    for (std::size_t k = 0; k < kMaxInsertParams; ++k) {
        many += "            {\"key\": \"p" + std::to_string(k) + "\", \"value\": 0},\n";
    }
    const std::string crowded =
        edit(edit(text, "\"params\": [\n            {\n              \"key\": \"threshold\"",
                  many + "            {\n              \"key\": \"threshold\""),
             "\"key\": \"gain\",\n              \"value\": 0.3", "\"key\": \"gain\"");
    CHECK(refused(crowded, "holds 8194 values; an insert holds at most 8192",
                  "tracks[0].inserts[0].params"));
    return 0;
}

// The spec's own example: a machine set to de_DE reads "-2.5" as -2.5, and
// saves the same bytes it loaded.
int test_comma_locale() {
    const char* names[] = {"de_DE.UTF-8", "de_DE.utf8", "de_DE"};
    const char* got = nullptr;
    for (const char* n : names) {
        if (std::setlocale(LC_ALL, n) != nullptr) {
            got = n;
            break;
        }
    }
    if (got == nullptr) {
        std::printf("SKIP model comma locale: no de_DE locale installed\n");
        return 0;
    }
    const std::string text = read_fixture(kCurrent);
    Loaded l;
    LoadError e;
    const bool loaded = load(text, l, e);
    std::string out;
    const bool saved = loaded && save(l, out);
    const bool comma = std::strcmp(std::localeconv()->decimal_point, ",") == 0;
    std::setlocale(LC_ALL, "C");
    CHECK(comma);
    CHECK(loaded && saved);
    CHECK(l.snapshot.mix.find(3)->gain_db == -2.5f);
    CHECK(l.snapshot.mix.find_insert(42)->params[1].value == 0.3f);
    CHECK(out == text);
    return 0;
}

// Before PR #69 the model accepted a fade running into the crossfade at its
// region's other end whenever an edge edit or a crossfade clear produced it,
// and the interface saved it. Such a v2 file must still open: the fade is
// shortened to meet the crossfade, and the load says so.
int test_fade_into_crossfade_is_repaired_at_load() {
    // R [100000, 500000) butts S [500000, 600000); a centred 960 crossfade
    // reaches 480 into each. S's fade-out met it, then S's end was trimmed
    // to 550000: the old model kept a 50000 fade-out over 49520 frames.
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddTrack t = add_track(ids, "Vox");
    CHECK_APPLY(h, t);
    const AddSource src = add_source(ids, 1'000'000, 1, t.track);
    CHECK_APPLY(h, src);
    const Id r = ids();
    const Id s = ids();
    CHECK_APPLY(h, (PlaceRegion{t.playlist, region(r, src.source.id, 100000, 400000, 200000)}));
    CHECK_APPLY(h, (PlaceRegion{t.playlist, region(s, src.source.id, 500000, 100000, 500)}));
    CHECK_APPLY(h, (SetFade{t.playlist, s, FadeEnd::Out, Fade{100000 - 480, FadeShape::Linear}}));
    CHECK_APPLY(h, (SetCrossfade{t.playlist,
                                 Crossfade{r, s, 960, Align::Centre, FadeShape::EqualPower}}));
    CHECK_APPLY(h, (TrimRegion{t.playlist, s, Edge::End, 550000}));
    std::string bytes, err;
    CHECK(serialize(h.current(), WriteInfo{"sonora-ui 0.1.0", 3}, bytes, err));
    const std::string old_file = edit(bytes, "\"length\": 49520", "\"length\": 50000");

    Loaded l;
    LoadError e;
    CHECK(load(old_file, l, e));
    const Region* back = find(playlist_of(l.snapshot, 0), s);
    CHECK(back != nullptr && back->fade_out.length == 50000 - 480);
    CHECK(l.repairs.size() == 1);
    CHECK(l.repairs[0].path == "tracks[0].playlists[0].regions[1].fade_out.length");
    CHECK(l.repairs[0].was == "50000 frames" && l.repairs[0].now == "49520 frames");
    CHECK(mentions(l.repairs[0].why, "crossfade at the region's start"));
    // What loads is what the current model would have made, and it saves.
    std::string again;
    CHECK(save(l, again));
    CHECK(again == bytes);
    // A file without the overlap loads with nothing to report.
    CHECK(load(bytes, l, e));
    CHECK(l.repairs.empty());

    // The mirror, as a crossfade clear left it: S's fade-in, kept under a
    // crossfade at its start, runs into the crossfade at its end.
    std::string mirror;
    {
        // Swap the crossfade to S's end: S|T, with T butting at 550000.
        History m(h.current());
        const Id tt = m.current().next_id;
        CHECK_APPLY(m, (RemoveCrossfade{t.playlist, r, s}));
        CHECK_APPLY(m, (SetFade{t.playlist, s, FadeEnd::Out, Fade{0, FadeShape::Linear}}));
        CHECK_APPLY(m, (SetFade{t.playlist, s, FadeEnd::In, Fade{50000 - 480, FadeShape::Linear}}));
        CHECK_APPLY(m, (PlaceRegion{t.playlist, region(tt, src.source.id, 550000, 10000, 700000)}));
        CHECK_APPLY(m, (SetCrossfade{t.playlist,
                                     Crossfade{s, tt, 960, Align::Centre, FadeShape::EqualPower}}));
        CHECK(serialize(m.current(), WriteInfo{"sonora-ui 0.1.0", 4}, mirror, err));
    }
    CHECK(load(edit(mirror, "\"length\": 49520", "\"length\": 50000"), l, e));
    back = find(playlist_of(l.snapshot, 0), s);
    CHECK(back != nullptr && back->fade_in.length == 50000 - 480);
    CHECK(l.repairs.size() == 1);
    CHECK(mentions(l.repairs[0].path, ".fade_in.length"));
    CHECK(l.repairs[0].was == "50000 frames" && l.repairs[0].now == "49520 frames");
    CHECK(mentions(l.repairs[0].why, "crossfade at the region's end"));
    return 0;
}

// Version 7: the session's id and the takes the operator left. A version
// 6 session opens with neither and its first save writes both keys, empty;
// a version 6 file cannot carry them. The fixture's id and left takes come
// back as they are, and a malformed id or list is refused by key path.
int test_v6_migrates_and_session_id() {
    const std::string v6 = read_fixture(kNoSessionId);
    CHECK(!v6.empty());
    Loaded l;
    LoadError e;
    CHECK(load(v6, l, e));
    CHECK(l.file_version == 6 && l.repairs.empty());
    CHECK(l.snapshot.settings.session_id.empty() && l.snapshot.settings.left_takes.empty());
    std::string out;
    CHECK(save(l, out));
    CHECK(out == as_v9(as_v8(as_v7(v6))));
    const std::string last = "\n  ]\n}\n";
    CHECK(refused(v6.substr(0, v6.size() - last.size()) + "\n  ],\n  \"session_id\": \"\"\n}\n",
                  "migrating from format version 6: the file has \"session_id\"", nullptr));

    const std::string text = read_fixture(kCurrent);
    Loaded c;
    CHECK(load(text, c, e));
    CHECK(c.snapshot.settings.session_id == "6f1c2a9e-3b4d-4e5f-8a7b-9c0d1e2f3a4b");
    CHECK(c.snapshot.settings.left_takes ==
          (std::vector<std::string>{"Audio Files/Audio 1_07.wav", "Audio Files/Audio 2_03.wav"}));
    const std::string id = "\"6f1c2a9e-3b4d-4e5f-8a7b-9c0d1e2f3a4b\"";
    CHECK(refused(edit(text, id, "\"Not An Id\""), "must be empty or up to 64", "session_id",
                  line_of(text, "\"session_id\"")));
    CHECK(refused(edit(text, id, "7"), "must be a string", "session_id"));
    const std::string left = "[\"Audio Files/Audio 1_07.wav\", \"Audio Files/Audio 2_03.wav\"]";
    CHECK(refused(edit(text, left, "[\"Audio Files/Audio 2_03.wav\", \"Audio Files/Audio 1_07.wav\"]"),
                  "must be sorted", "left_takes[1]"));
    CHECK(refused(edit(text, left, "[\"Audio Files/Audio 1_07.wav\", \"Audio Files/Audio 1_07.wav\"]"),
                  "must be sorted", "left_takes[1]"));
    CHECK(refused(edit(text, left, "[\"/tmp/Audio 1_07.wav\"]"), "is absolute", "left_takes[0]"));
    CHECK(refused(edit(text, left, "[7]"), "must be a string", "left_takes[0]"));
    Loaded none_left;
    CHECK(load(edit(text, left, "[]"), none_left, e));
    CHECK(none_left.snapshot.settings.left_takes.empty());

    // Fresh ids: version 4 UUIDs, and two are never the same.
    const std::string a = new_session_id();
    const std::string b = new_session_id();
    CHECK(a.size() == 36 && a[8] == '-' && a[13] == '-' && a[14] == '4' && a[18] == '-' &&
          a[23] == '-');
    CHECK(valid_session_id(a) && a != b);
    CHECK(valid_session_id("") && !valid_session_id("A") && !valid_session_id("a b") &&
          !valid_session_id(std::string(65, 'a')));
    return 0;
}

// Version 8 (ADR-0020 §3): the count-off's length in bars. A version 7
// session opens with Pro Tools' 2 and its first save writes the key; a
// version 7 file cannot carry it; a length outside 1 to 99 is refused by
// key path.
int test_v7_migrates_and_count_off() {
    const std::string v7 = read_fixture(kNoCountOff);
    CHECK(!v7.empty());
    Loaded l;
    LoadError e;
    CHECK(load(v7, l, e));
    CHECK(l.file_version == 7 && l.repairs.empty());
    CHECK(l.snapshot.settings.transport.count_off_bars == 2);
    std::string out;
    CHECK(save(l, out));
    CHECK(out == as_v9(as_v8(v7)));
    CHECK(out == read_fixture(kCurrent));
    CHECK(refused(edit(v7, "\"punch_crossfade\": 240", "\"punch_crossfade\": 240,\n    \"count_off_bars\": 2"),
                  "migrating from format version 7: the file has \"transport.count_off_bars\"",
                  nullptr));

    const std::string text = read_fixture(kCurrent);
    const std::string bars = "\"count_off_bars\": 2";
    Loaded four;
    CHECK(load(edit(text, bars, "\"count_off_bars\": 4"), four, e));
    CHECK(four.snapshot.settings.transport.count_off_bars == 4);
    CHECK(refused(edit(text, bars, "\"count_off_bars\": 0"), "must be from 1 to 99",
                  "transport.count_off_bars", line_of(text, "\"count_off_bars\"")));
    CHECK(refused(edit(text, bars, "\"count_off_bars\": 100"), "must be an integer from 0 to 99",
                  "transport.count_off_bars"));
    CHECK(refused(edit(text, ",\n    " + bars, ""), "missing required key \"count_off_bars\"",
                  "transport"));
    return 0;
}

// Version 9 (ADR-0016, update "the loop on switch"): loop playback apart
// from its range. A version 8 file played its loop range whenever one was
// set, so one with a range opens with loop playback on and one without
// opens with it off; a version 8 file cannot carry the key; the key is
// required and must be true or false; the switch survives a save.
int test_v8_migrates_and_loop_playback() {
    const std::string v8 = read_fixture(kNoLoopPlayback);
    CHECK(!v8.empty());
    Loaded l;
    LoadError e;
    CHECK(load(v8, l, e));
    CHECK(l.file_version == 8 && l.repairs.empty());
    CHECK(!l.snapshot.settings.transport.loop_playback);
    std::string out;
    CHECK(save(l, out));
    CHECK(out == as_v9(v8));
    CHECK(out == read_fixture(kCurrent));

    const std::string ranged = edit(v8, "\"loop\": [0, 0]", "\"loop\": [480000, 960000]");
    Loaded looped;
    CHECK(load(ranged, looped, e));
    CHECK(looped.snapshot.settings.transport.loop_playback);
    CHECK(looped.snapshot.settings.transport.loop[0] == 480000 &&
          looped.snapshot.settings.transport.loop[1] == 960000);
    CHECK(save(looped, out));
    CHECK(mentions(out, "\"loop_playback\": true"));
    CHECK(refused(edit(v8, "\"count_off_bars\": 2",
                       "\"count_off_bars\": 2,\n    \"loop_playback\": true"),
                  "migrating from format version 8: the file has \"transport.loop_playback\"",
                  nullptr));

    const std::string text = read_fixture(kCurrent);
    const std::string flag = "\"loop_playback\": false";
    // On with no range is kept: the mode waits for a range, as Pro Tools'
    // Loop Playback does with no selection.
    const std::string on_text = edit(text, flag, "\"loop_playback\": true");
    Loaded on;
    CHECK(load(on_text, on, e));
    CHECK(on.snapshot.settings.transport.loop_playback);
    CHECK(save(on, out));
    CHECK(out == on_text);
    CHECK(refused(edit(text, flag, "\"loop_playback\": 1"), "must be true or false",
                  "transport.loop_playback", line_of(text, "\"loop_playback\"")));
    CHECK(refused(edit(text, ",\n    " + flag, ""), "missing required key \"loop_playback\"",
                  "transport"));
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
    t(test_fixture_is_canonical);
    t(test_v1_migrates);
    t(test_v2_migrates);
    t(test_v3_migrates);
    t(test_v3_tempo_brought_inside_bounds);
    t(test_v4_migrates);
    t(test_v4_tempo_bounds_refused);
    t(test_round_trip_from_edits);
    t(test_versions);
    t(test_key_order_is_lenient);
    t(test_malformed);
    t(test_malformed_inserts);
    t(test_malformed_midi);
    t(test_malformed_sequence_refs);
    t(test_v5_migrates);
    t(test_v6_migrates_and_session_id);
    t(test_v7_migrates_and_count_off);
    t(test_v8_migrates_and_loop_playback);
    t(test_missing_sequence_opens);
    t(test_session_bundle);
    t(test_comma_locale);
    t(test_fade_into_crossfade_is_repaired_at_load);
    if (failed != 0) {
        std::fprintf(stderr, "model_session_file: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_session_file: %d passed\n", run);
    return 0;
}
