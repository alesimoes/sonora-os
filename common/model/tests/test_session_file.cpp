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

bool load(const std::string& text, Loaded& out, LoadError& err) {
    return deserialize(text, out, err);
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

// The current version's frozen fixture: what this build writes.
constexpr const char* kCurrent = "v3-solo-safe.sonora";

int test_fixture_is_canonical() {
    const std::string text = read_fixture(kCurrent);
    CHECK(!text.empty());
    Loaded l;
    LoadError e;
    if (!load(text, l, e)) std::fprintf(stderr, "  %s\n", e.text().c_str());
    CHECK(load(text, l, e));
    CHECK(l.file_version == 3);
    CHECK(l.save_generation == 42);
    CHECK(l.written_by == "sonora-engine 0.2.0");
    // Load, save, compare bytes: the writer is canonical.
    std::string out;
    CHECK(save(l, out));
    if (out != text) {
        std::ofstream("v3-solo-safe.out.sonora", std::ios::binary) << out;
        std::fprintf(stderr, "  re-saved bytes differ; wrote v3-solo-safe.out.sonora\n");
    }
    CHECK(out == text);

    const Snapshot& s = l.snapshot;
    CHECK(invariants_hold(s));
    CHECK(s.next_id == 44);   // max(id) + 1; the counter is not stored
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

    // Saved, it is the same session at version 3: the version, one empty
    // list and one solo safe off per track are the whole difference.
    std::string out;
    CHECK(save(l, out));
    const std::string expected =
        edit(edit(edit(v1, "\"sonora_session\": 1,", "\"sonora_session\": 3,"),
                  "      \"active_playlist\": 11,\n",
                  "      \"solo_safe\": false,\n      \"active_playlist\": 11,\n"
                  "      \"inserts\": [],\n"),
             "      \"active_playlist\": 13,\n",
             "      \"solo_safe\": false,\n      \"active_playlist\": 13,\n"
             "      \"inserts\": [],\n");
    if (out != expected) {
        std::ofstream("v1-migrated.out.sonora", std::ios::binary) << out;
        std::fprintf(stderr, "  migrated bytes differ; wrote v1-migrated.out.sonora\n");
    }
    CHECK(out == expected);
    std::int64_t version = 0;
    CHECK(file_format_version(v1, version) && version == 1);
    CHECK(file_format_version(out, version) && version == 3);
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
    const std::string expected =
        edit(edit(edit(v2, "\"sonora_session\": 2,", "\"sonora_session\": 3,"),
                  "      \"solo\": false,\n      \"active_playlist\": 11,\n",
                  "      \"solo\": false,\n      \"solo_safe\": false,\n"
                  "      \"active_playlist\": 11,\n"),
             "      \"solo\": false,\n      \"active_playlist\": 13,\n",
             "      \"solo\": false,\n      \"solo_safe\": false,\n"
             "      \"active_playlist\": 13,\n");
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
    CHECK(refused(edit(text, "\"sonora_session\": 3,", "\"sonora_session\": 4,"),
                  "format version 4, newer than this program, which reads up to version 3",
                  "sonora_session", line));
    // A newer file is refused even when it carries keys this version does
    // not know: the version is read before anything else.
    CHECK(refused(edit(edit(text, "\"sonora_session\": 3,", "\"sonora_session\": 5,"),
                       "\"name\": \"Vocals Friday\",", "\"mixer\": {},"),
                  "newer than this program", "sonora_session"));
    // Too large for int64 is still a newer version, not a malformed one.
    CHECK(refused(edit(text, "\"sonora_session\": 3,",
                       "\"sonora_session\": 99999999999999999999,"),
                  "format version 99999999999999999999, newer than this program",
                  "sonora_session"));
    CHECK(refused(edit(text, "\"sonora_session\": 3,", "\"sonora_session\": 0,"),
                  "does not exist", "sonora_session"));
    CHECK(refused(edit(text, "\"sonora_session\": 3,", "\"sonora_session\": 3.0,"),
                  "integer format version", "sonora_session"));
    // A current file must have the keys versions 2 and 3 added; only the
    // migrations fill them in for an older file.
    CHECK(refused(edit(text, "      \"inserts\": [],\n", ""),
                  "missing required key \"inserts\"", "tracks[1]"));
    CHECK(refused(edit(text, "      \"solo_safe\": true,\n", ""),
                  "missing required key \"solo_safe\"", "tracks[1]"));
    CHECK(refused(edit(text, "\"solo_safe\": true,", "\"solo_safe\": 1,"),
                  "must be true or false", "tracks[1].solo_safe",
                  line_of(text, "\"solo_safe\": true,")));
    // The magic must come first, unlike every other key.
    CHECK(refused(edit(text, "  \"sonora_session\": 3,\n  \"written_by\": \"sonora-engine 0.2.0\",",
                       "  \"written_by\": \"sonora-engine 0.2.0\",\n  \"sonora_session\": 3,"),
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
    CHECK(mentions(l.repairs[0], "tracks[0].playlists[0].regions[1]"));
    CHECK(mentions(l.repairs[0], "fade-out shortened from 50000 to 49520 frames"));
    CHECK(mentions(l.repairs[0], "crossfade at its start"));
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
    CHECK(mentions(l.repairs[0], "fade-in shortened from 50000 to 49520 frames"));
    CHECK(mentions(l.repairs[0], "crossfade at its end"));
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
    t(test_round_trip_from_edits);
    t(test_versions);
    t(test_key_order_is_lenient);
    t(test_malformed);
    t(test_malformed_inserts);
    t(test_comma_locale);
    t(test_fade_into_crossfade_is_repaired_at_load);
    if (failed != 0) {
        std::fprintf(stderr, "model_session_file: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_session_file: %d passed\n", run);
    return 0;
}
