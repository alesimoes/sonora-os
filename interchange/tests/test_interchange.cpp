// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-convert (ADR-0007 M10): a golden native session read and written
// again, an SMF imported and exported, the report, and the command line.
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "sonora/interchange/aaf_import.h"
#include "sonora/interchange/interchange.h"
#include "sonora/model/session_file.h"
#include "sonora/midi/notes.h"
#include "sonora/midi/smf.h"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

// CHECK that also says why, for the calls that return a reason.
#define CHECK_OK(cond, why)                                                      \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s: %s\n", __FILE__, __LINE__, #cond, \
                         (why).c_str());                                         \
            return 1;                                                            \
        }                                                                        \
    } while (0)

using namespace sonora;
namespace fs = std::filesystem;

namespace {

fs::path scratch() {
    static const fs::path dir =
        fs::temp_directory_path() / ("sonora_interchange_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    return dir;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// A song as a DAW writes one: a conductor track at 140 bpm then 3/4 from the
// second bar, a named track of notes with a controller and a SysEx, a track
// with only a key signature, and a note that never ends.
std::string song_bytes() {
    midi::Smf smf;
    smf.format = 1;
    smf.division = midi::Division::ticks_per_quarter(480);
    midi::SmfTrack conductor;
    conductor.events.push_back(midi::tempo(0, 428571));   // 140 bpm
    conductor.events.push_back(midi::time_signature(0, 4, 4));
    conductor.events.push_back(midi::marker(480, "verse"));
    conductor.events.push_back(midi::time_signature(1920, 3, 4));
    smf.tracks.push_back(conductor);
    midi::SmfTrack bass;
    bass.events.push_back(midi::track_name(0, "Bass"));
    bass.events.push_back(midi::note_on(0, 1, 40, 90));
    bass.events.push_back(midi::note_off(480, 1, 40));
    bass.events.push_back(midi::control_change(240, 1, 7, 100));
    bass.events.push_back(midi::sysex(480, {0x7E, 0x7F, 0x09, 0x01}));
    bass.events.push_back(midi::note_on(960, 1, 43, 80));
    bass.events.push_back(midi::note_off(1440, 1, 43));
    midi::sort_by_tick(bass.events);
    smf.tracks.push_back(bass);
    midi::SmfTrack keys;
    keys.events.push_back(midi::key_signature(0, 2, false));
    smf.tracks.push_back(keys);
    const midi::SmfWriteResult w = midi::write_smf(smf);
    return std::string(w.bytes.begin(), w.bytes.end());
}

bool mentions(const std::vector<std::string>& lines, const char* needle) {
    for (const std::string& l : lines) {
        if (l.find(needle) != std::string::npos) return true;
    }
    return false;
}

int test_smf_import() {
    interchange::SmfImporter importer;
    interchange::ConversionReport report;
    model::Snapshot s;
    std::string error;
    CHECK_OK(importer.read_bytes(song_bytes(), "Song", s, report, error), error);
    CHECK(s.settings.name == "Song");
    CHECK(s.edit->tracks.size() == 1);
    const model::TrackEdit& t = *s.edit->tracks[0];
    CHECK(t.kind == model::TrackKind::Midi && t.name == "Bass");
    const model::MidiRegion& r = t.midi_playlists.front()->regions.front();
    // 480 ppq to 960000: a quarter is 960000 ticks here.
    CHECK(r.data->note_count() == 2);
    CHECK(r.data->notes()[0].length == model::kTicksPerQuarter);
    CHECK(r.data->notes()[1].start == 2 * model::kTicksPerQuarter);
    CHECK(r.data->event_count() == 1 && r.data->sysex_count() == 1);
    CHECK(s.settings.tempo.size() == 2);
    CHECK(s.settings.tempo[0].bpm > 139.9f && s.settings.tempo[0].bpm < 140.1f);
    CHECK(s.settings.tempo[1].meter[0] == 3);
    // 4 beats at 140 bpm, at 48 kHz.
    const model::Frames bar = 4 * 60 * 48000 / 140;
    CHECK(s.settings.tempo[1].at >= bar && s.settings.tempo[1].at <= bar + 1);
    // What did not survive is said.
    CHECK(mentions(report.dropped, "marker"));
    CHECK(mentions(report.dropped, "key signature") || mentions(report.dropped, "no notes"));
    return 0;
}

int test_smf_round_trip() {
    interchange::SmfImporter importer;
    interchange::SmfExporter exporter;
    interchange::ConversionReport report;
    model::Snapshot s;
    std::string error, bytes;
    CHECK_OK(importer.read_bytes(song_bytes(), "Song", s, report, error), error);
    CHECK(exporter.encode(s, bytes, report, error));
    const midi::SmfReadResult back = midi::read_smf(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    CHECK(back.ok());
    CHECK(back.smf->format == 1 && back.smf->division.ppq() == 960);
    CHECK(back.smf->tracks.size() == 2);
    const midi::NoteExtraction x = midi::extract_notes(back.smf->tracks[1].events);
    CHECK(x.notes.size() == 2);
    CHECK(x.notes[0].pitch == 40 && x.notes[0].start == 0 && x.notes[0].length == 960);
    CHECK(x.notes[1].pitch == 43 && x.notes[1].start == 1920 && x.notes[1].length == 960);
    CHECK(x.notes[0].channel == 1 && x.notes[0].velocity == 90);
    bool cc = false, sx = false;
    for (const midi::Event& e : back.smf->tracks[1].events) {
        cc = cc || (e.status == 0xB1 && e.data1 == 7 && e.data2 == 100 && e.tick == 480);
        sx = sx || (e.status == midi::status::sysex && e.tick == 960);
    }
    CHECK(cc && sx);
    bool tempo = false, three_four = false;
    for (const midi::Event& e : back.smf->tracks[0].events) {
        if (const auto u = midi::tempo_of(e)) tempo = tempo || *u == 428571;
        if (const auto ts = midi::time_signature_of(e)) {
            three_four = three_four || (ts->numerator == 3 && e.tick == 3840);
        }
    }
    CHECK(tempo && three_four);
    CHECK(mentions(report.dropped, "mix, routing"));
    return 0;
}

int test_hostile_smf() {
    interchange::SmfImporter importer;
    interchange::ConversionReport report;
    model::Snapshot s;
    std::string error;
    CHECK(!importer.read_bytes("MThd garbage", "x", s, report, error) && !error.empty());
    std::string cut = song_bytes();
    cut.resize(cut.size() / 2);
    CHECK(!importer.read_bytes(cut, "x", s, report, error));
    CHECK(!importer.read_bytes(std::string(), "x", s, report, error));
    return 0;
}

// The frozen v1 session in, out in the current version, in again: the same
// document, nothing lost.
int test_golden_session() {
    const fs::path golden = fs::path(SONORA_MODEL_FIXTURES) / "v1-punch.sonora";
    interchange::ConversionReport report;
    model::Snapshot before, after;
    std::string error;
    CHECK(interchange::read_session(golden.string(), before, report, error));
    CHECK(!before.edit->tracks.empty());
    const fs::path out = scratch() / "golden" / "golden.sonora";
    CHECK_OK(interchange::write_session(out.string(), before, report, error), error);
    CHECK(fs::exists(out));
    interchange::ConversionReport again;
    CHECK(interchange::read_session(out.string(), after, again, error));
    CHECK(model::same_document(before, after));
    CHECK(again.dropped.empty());
    std::int64_t version = 0;
    CHECK(model::file_format_version(slurp(out), version) && version == model::kFormatVersion);
    // Writing what was written changes nothing: the file is canonical.
    const fs::path twice = scratch() / "golden2" / "golden2.sonora";
    CHECK(interchange::write_session(twice.string(), after, again, error));
    interchange::ConversionReport third;
    model::Snapshot reread;
    CHECK(interchange::read_session(twice.string(), reread, third, error));
    CHECK(model::same_document(after, reread));
    return 0;
}

// A session that names plugin state is converted only with the state beside
// it, as the engine's saver demands; the fixtures have none, so it is refused
// by name and no half session is left behind.
int test_plugin_state_refused() {
    const fs::path golden = fs::path(SONORA_MODEL_FIXTURES) / "v6-midi.sonora";
    interchange::ConversionReport report;
    model::Snapshot s;
    std::string error;
    CHECK_OK(interchange::read_session(golden.string(), s, report, error), error);
    CHECK(s.edit->tracks.size() > 0);
    const fs::path out = scratch() / "state" / "state.sonora";
    CHECK(!interchange::write_session(out.string(), s, report, error,
                                      (fs::path(SONORA_MODEL_FIXTURES)).string()));
    CHECK(error.find("plugin state") != std::string::npos);
    CHECK(!fs::exists(out));
    return 0;
}

int test_session_errors() {
    interchange::ConversionReport report;
    model::Snapshot s;
    std::string error;
    CHECK(!interchange::read_session((scratch() / "nope.sonora").string(), s, report, error));
    CHECK(error.find("not there") != std::string::npos);
    const fs::path bad = scratch() / "bad.sonora";
    std::ofstream(bad) << "{ not a session";
    CHECK(!interchange::read_session(bad.string(), s, report, error));
    CHECK(!interchange::write_session((scratch() / "x.txt").string(), s, report, error));
    return 0;
}

int test_command_line() {
    std::string out, err;
    CHECK(interchange::run_convert({}, out, err) == 2 && !err.empty());
    out.clear();
    CHECK(interchange::run_convert({"--help"}, out, err) == 0 && !out.empty());
    err.clear();
    CHECK(interchange::run_convert({"frobnicate", "a", "b"}, out, err) == 2);
    err.clear();
#ifdef SONORA_PTX_READER_OFF
    CHECK(interchange::run_convert({"import", "x.ptx", (scratch() / "p.sonora").string()}, out, err) == 1);
    CHECK(err.find("no .ptx reader") != std::string::npos);
    err.clear();
#endif
    if (!interchange::make_libaaf_reader()) {  // option OFF: fail loudly, never an empty session
        CHECK(interchange::run_convert({"import", "x.aaf", (scratch() / "a.sonora").string()}, out, err) == 1);
        CHECK(err.find("built without AAF support") != std::string::npos);
    }
    err.clear();
    CHECK(interchange::run_convert({"smf-import", (scratch() / "missing.mid").string(),
                                    (scratch() / "o.sonora").string()},
                                   out, err) == 1);
    CHECK(err.find("sonora-convert:") == 0);

    const fs::path mid = scratch() / "song.mid";
    { std::ofstream(mid, std::ios::binary) << song_bytes(); }
    const fs::path session = scratch() / "cli" / "Song.sonora";
    out.clear();
    err.clear();
    CHECK_OK(interchange::run_convert({"smf-import", mid.string(), session.string()}, out, err) == 0, err);
    CHECK(out.find("dropped:") != std::string::npos && fs::exists(session));
    const fs::path exported = scratch() / "back.mid";
    CHECK(interchange::run_convert({"smf-export", session.string(), exported.string()}, out,
                                   err) == 0);
    const std::string bytes = slurp(exported);
    CHECK(midi::read_smf(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()).ok());
    const fs::path copy = scratch() / "cli2" / "Song.sonora";
    CHECK(interchange::run_convert({"session", session.string(), copy.string()}, out, err) == 0);
    CHECK(fs::exists(copy));
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_smf_import();
    failed += test_smf_round_trip();
    failed += test_hostile_smf();
    failed += test_golden_session();
    failed += test_plugin_state_refused();
    failed += test_session_errors();
    failed += test_command_line();
    std::error_code ec;
    fs::remove_all(scratch(), ec);
    if (failed == 0) std::puts("interchange: all passed");
    return failed == 0 ? 0 : 1;
}
