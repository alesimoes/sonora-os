// SPDX-License-Identifier: GPL-3.0-or-later
// Every fixture here is built in the test, byte by byte: no third-party
// MIDI file is committed.
#include "sonora/midi/smf.h"

#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "sonora/midi/tempo_map.h"

using namespace sonora::midi;
using Bytes = std::vector<std::uint8_t>;

namespace {

void put32(Bytes& b, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>(v >> s));
}

void put16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}

void chunk(Bytes& b, const char* type, const Bytes& body) {
    b.insert(b.end(), type, type + 4);
    put32(b, static_cast<std::uint32_t>(body.size()));
    b.insert(b.end(), body.begin(), body.end());
}

Bytes header(std::uint16_t format, std::uint16_t ntracks, std::uint16_t division) {
    Bytes b;
    Bytes body;
    put16(body, format);
    put16(body, ntracks);
    put16(body, division);
    chunk(b, "MThd", body);
    return b;
}

Bytes file(std::uint16_t format, std::uint16_t division, const std::vector<Bytes>& tracks) {
    Bytes b = header(format, static_cast<std::uint16_t>(tracks.size()), division);
    for (const Bytes& t : tracks) chunk(b, "MTrk", t);
    return b;
}

// One format 0 track at 96 ppq, using running status twice.
// Offsets: header 0..13, MTrk 14..21, events from 22.
const Bytes fixture_track = {
    0x00, 0xFF, 0x03, 0x04, 'T', 'e', 's', 't',  // 22: track name
    0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,    // 30: tempo 500000
    0x00, 0xC0, 0x05,                            // 37: program change
    0x00, 0x90, 0x3C, 0x64,                      // 40: note on 60
    0x60, 0x3E, 0x64,                            // 44: running status, on 62 at 96
    0x81, 0x40, 0x3C, 0x00,                      // 47: running status, on 60 vel 0 at 288
    0x00, 0x80, 0x3E, 0x40,                      // 51: note off 62, release 64
    0x00, 0xF0, 0x03, 0x7E, 0x09, 0xF7,          // 55: SysEx
    0x00, 0xFF, 0x2F, 0x00,                      // 61: end of track
};

SmfReadResult read(const Bytes& b, const SmfReadLimits& limits = {}) {
    return read_smf(b.data(), b.size(), limits);
}

int expect_error(const Bytes& b, SmfError code, std::size_t offset,
                 const SmfReadLimits& limits = {}) {
    const SmfReadResult r = read(b, limits);
    CHECK(!r.ok());
    if (r.error.code != code || r.error.offset != offset) {
        std::fprintf(stderr, "  got code %d at %zu: %s\n", static_cast<int>(r.error.code),
                     r.error.offset, r.error.message.c_str());
    }
    CHECK(r.error.code == code);
    CHECK(r.error.offset == offset);
    CHECK(!r.error.message.empty());
    CHECK(r.error.message.find(std::to_string(offset)) != std::string::npos);
    return 0;
}

Bytes track_with(const Bytes& events) { return file(0, 96, {events}); }

int vlq_matches_the_specification_table() {
    const struct {
        std::uint32_t value;
        Bytes bytes;
    } table[] = {
        {0x00000000, {0x00}},
        {0x00000040, {0x40}},
        {0x0000007F, {0x7F}},
        {0x00000080, {0x81, 0x00}},
        {0x00002000, {0xC0, 0x00}},
        {0x00003FFF, {0xFF, 0x7F}},
        {0x00004000, {0x81, 0x80, 0x00}},
        {0x00100000, {0xC0, 0x80, 0x00}},
        {0x001FFFFF, {0xFF, 0xFF, 0x7F}},
        {0x00200000, {0x81, 0x80, 0x80, 0x00}},
        {0x08000000, {0xC0, 0x80, 0x80, 0x00}},
        {0x0FFFFFFF, {0xFF, 0xFF, 0xFF, 0x7F}},
    };
    for (const auto& row : table) {
        Bytes out;
        append_vlq(out, row.value);
        CHECK(out == row.bytes);
        std::size_t pos = 0;
        const auto back = read_vlq(row.bytes.data(), row.bytes.size(), pos);
        CHECK(back && *back == row.value && pos == row.bytes.size());
    }
    const Bytes five = {0x81, 0x80, 0x80, 0x80, 0x00};
    std::size_t pos = 0;
    CHECK(!read_vlq(five.data(), five.size(), pos));
    const Bytes cut = {0x81, 0x80};
    pos = 0;
    CHECK(!read_vlq(cut.data(), cut.size(), pos));
    return 0;
}

int reads_the_fixture() {
    const SmfReadResult r = read(track_with(fixture_track));
    CHECK(r.ok());
    const Smf& smf = *r.smf;
    CHECK(smf.format == 0 && smf.division.ppq() == 96 && smf.tracks.size() == 1);
    const EventList& e = smf.tracks[0].events;
    CHECK(e.size() == 9);
    CHECK(text_of(e[0]) == std::string("Test"));
    CHECK(tempo_of(e[1]) && *tempo_of(e[1]) == 500000);
    CHECK(e[2] == program_change(0, 0, 5));
    CHECK(e[3] == note_on(0, 0, 60, 100));
    CHECK(e[4] == note_on(96, 0, 62, 100));
    CHECK(e[5] == note_on(288, 0, 60, 0) && e[5].is_note_off());
    CHECK(e[6] == note_off(288, 0, 62, 64));
    CHECK(e[7] == sysex(288, {0x7E, 0x09}));
    CHECK(e[8].is_end_of_track() && e[8].tick == 288);
    return 0;
}

int writes_the_fixture_byte_for_byte() {
    const Bytes original = track_with(fixture_track);
    const SmfReadResult r = read(original);
    CHECK(r.ok());
    const SmfWriteResult w = write_smf(*r.smf);
    CHECK(w.ok());
    CHECK(w.bytes == original);

    // Without running status: bigger, and reads back the same.
    SmfWriteOptions plain;
    plain.running_status = false;
    const SmfWriteResult w2 = write_smf(*r.smf, plain);
    CHECK(w2.ok() && w2.bytes.size() == original.size() + 2);
    const SmfReadResult r2 = read(w2.bytes);
    CHECK(r2.ok() && *r2.smf == *r.smf);
    return 0;
}

Smf rich_song() {
    Smf smf;
    smf.format = 1;
    smf.division = Division::ticks_per_quarter(960);

    SmfTrack conductor;
    conductor.events = {
        track_name(0, "Conductor"),
        tempo(0, 500000),
        time_signature(0, 7, 8),
        key_signature(0, -2, false),
        meta_event(0, meta::smpte_offset, {0x21, 0, 0, 0, 0}),
        marker(3840, "Verse"),
        tempo(3840, 428571),
        time_signature(7680, 4, 4),
        marker(7680, "Chorus"),
        end_of_track(15360),
    };

    SmfTrack keys;
    keys.events = {track_name(0, "Keys"), program_change(0, 1, 4)};
    const auto cc = control_change_14bit(0, 1, 7, 12000);
    keys.events.push_back(cc[0]);
    keys.events.push_back(cc[1]);
    keys.events.push_back(note_on(0, 1, 60, 90));
    keys.events.push_back(note_on(0, 1, 64, 91));
    keys.events.push_back(poly_aftertouch(120, 1, 60, 70));
    keys.events.push_back(channel_aftertouch(240, 1, 50));
    keys.events.push_back(pitch_bend(360, 1, -4000));
    keys.events.push_back(note_off(960, 1, 60, 12));
    keys.events.push_back(note_on(960, 1, 64, 0));
    keys.events.push_back(sysex(1000, {0x41, 0x10, 0x42, 0x12}));
    // A SysEx split into packets: F0 without its F7, then an F7 escape.
    Event first;
    first.tick = 1100;
    first.status = status::sysex;
    first.payload = {0x43, 0x12};
    keys.events.push_back(first);
    Event cont;
    cont.tick = 1110;
    cont.status = status::sysex_escape;
    cont.payload = {0x00, 0x01, 0xF7};
    keys.events.push_back(cont);
    keys.events.push_back(meta_event(1200, meta::sequencer_specific, {0x00, 0x00, 0x41}));
    keys.events.push_back(text_event(1200, meta::lyric, "la"));
    // The longest gap a delta time can hold.
    keys.events.push_back(note_on(1200 + max_vlq, 2, 70, 1));
    keys.events.push_back(end_of_track(1200 + Tick{max_vlq}));

    smf.tracks = {conductor, keys};
    return smf;
}

int format_1_round_trip() {
    const Smf song = rich_song();
    for (const bool rs : {true, false}) {
        SmfWriteOptions options;
        options.running_status = rs;
        const SmfWriteResult w = write_smf(song, options);
        CHECK(w.ok());
        const SmfReadResult r = read(w.bytes);
        CHECK(r.ok());
        CHECK(*r.smf == song);
        // And again from what was read: the encoding is stable.
        const SmfWriteResult w2 = write_smf(*r.smf, options);
        CHECK(w2.ok() && w2.bytes == w.bytes);
    }

    // The tempo map built from the file sees both tempos.
    const SmfReadResult r = read(write_smf(song).bytes);
    const TempoMap map = TempoMap::from_events(r.smf->division.ppq(), merge_tracks(*r.smf));
    CHECK(map.points().size() == 2 && map.usec_per_quarter_at(4000) == 428571);
    return 0;
}

int format_0_round_trip_and_missing_end() {
    Smf smf;
    smf.format = 0;
    smf.division = Division::ticks_per_quarter(480);
    smf.tracks.push_back(SmfTrack{{note_on(0, 9, 36, 127), note_off(240, 9, 36)}});
    const SmfWriteResult w = write_smf(smf);
    CHECK(w.ok());
    const SmfReadResult r = read(w.bytes);
    CHECK(r.ok());
    // The writer appended the end of track at the last tick.
    const EventList& e = r.smf->tracks[0].events;
    CHECK(e.size() == 3 && e[2].is_end_of_track() && e[2].tick == 240);
    CHECK(e[0] == smf.tracks[0].events[0] && e[1] == smf.tracks[0].events[1]);

    // An empty track is a lone end of track.
    Smf empty;
    empty.format = 1;
    empty.tracks.push_back(SmfTrack{});
    const SmfReadResult re = read(write_smf(empty).bytes);
    CHECK(re.ok() && re.smf->tracks[0].events.size() == 1);
    return 0;
}

int smpte_division_round_trips() {
    for (const int fps : {24, 25, 29, 30}) {
        Smf smf;
        smf.format = 0;
        smf.division = Division::smpte(fps, 80);
        CHECK(smf.division.is_smpte() && smf.division.smpte_fps() == fps);
        CHECK(smf.division.ticks_per_frame() == 80 && smf.division.ppq() == 0);
        smf.tracks.push_back(SmfTrack{{note_on(0, 0, 60, 1), end_of_track(80)}});
        const SmfReadResult r = read(write_smf(smf).bytes);
        CHECK(r.ok() && *r.smf == smf);
    }
    return 0;
}

int format_2_is_read_only() {
    const Bytes b = file(2, 96, {{0x00, 0xFF, 0x2F, 0x00},
                                 {0x00, 0x90, 0x3C, 0x40, 0x00, 0xFF, 0x2F, 0x00}});
    const SmfReadResult r = read(b);
    CHECK(r.ok() && r.smf->format == 2 && r.smf->tracks.size() == 2);
    const SmfWriteResult w = write_smf(*r.smf);
    CHECK(!w.ok() && w.error.code == SmfError::UnsupportedFormat && w.bytes.empty());
    CHECK(w.error.message.find("format 2") != std::string::npos);
    return 0;
}

int tolerated_structure() {
    // A longer MThd: the extra bytes are skipped.
    Bytes b = {'M', 'T', 'h', 'd'};
    put32(b, 8);
    put16(b, 0);
    put16(b, 1);
    put16(b, 96);
    put16(b, 0xBEEF);
    chunk(b, "MTrk", {0x00, 0xFF, 0x2F, 0x00});
    CHECK(read(b).ok());

    // An alien chunk between tracks is skipped, and trailing bytes too.
    Bytes c = header(1, 2, 96);
    chunk(c, "MTrk", {0x00, 0xFF, 0x2F, 0x00});
    chunk(c, "XFIH", {1, 2, 3});
    chunk(c, "MTrk", {0x00, 0xFF, 0x2F, 0x00});
    c.push_back(0x00);
    c.push_back(0x42);
    const SmfReadResult r = read(c);
    CHECK(r.ok() && r.smf->tracks.size() == 2);
    return 0;
}

int header_errors() {
    CHECK(expect_error({'R', 'I', 'F', 'F', 0, 0, 0, 6}, SmfError::NotMidiFile, 0) == 0);
    CHECK(expect_error({'M', 'T', 'h'}, SmfError::NotMidiFile, 0) == 0);
    CHECK(expect_error({'M', 'T', 'h', 'd', 0, 0}, SmfError::Truncated, 4) == 0);

    Bytes short_len = {'M', 'T', 'h', 'd'};
    put32(short_len, 5);
    short_len.insert(short_len.end(), {0, 0, 0, 1, 0});
    CHECK(expect_error(short_len, SmfError::BadHeaderLength, 4) == 0);

    Bytes cut = header(0, 1, 96);
    cut.resize(12);
    CHECK(expect_error(cut, SmfError::Truncated, 4) == 0);

    CHECK(expect_error(file(3, 96, {{0x00, 0xFF, 0x2F, 0x00}}), SmfError::BadFormat, 8) == 0);
    const Bytes eot = {0x00, 0xFF, 0x2F, 0x00};
    CHECK(expect_error(file(0, 96, {eot, eot}), SmfError::BadFormat, 10) == 0);
    CHECK(expect_error(file(0, 0, {eot}), SmfError::BadDivision, 12) == 0);
    // -23 fps is not an SMPTE rate; 25 fps with 0 ticks per frame is empty.
    CHECK(expect_error(file(0, 0xE928, {eot}), SmfError::BadDivision, 12) == 0);
    CHECK(expect_error(file(0, 0xE700, {eot}), SmfError::BadDivision, 12) == 0);
    return 0;
}

int chunk_errors() {
    const Bytes eot = {0x00, 0xFF, 0x2F, 0x00};
    Bytes missing = header(1, 2, 96);
    chunk(missing, "MTrk", eot);
    CHECK(expect_error(missing, SmfError::TrackCountMismatch, 26) == 0);

    Bytes past_end = header(0, 1, 96);
    past_end.insert(past_end.end(), {'M', 'T', 'r', 'k'});
    put32(past_end, 100);
    past_end.insert(past_end.end(), eot.begin(), eot.end());
    CHECK(expect_error(past_end, SmfError::Truncated, 18) == 0);

    Bytes stub = header(0, 1, 96);
    stub.insert(stub.end(), {'M', 'T', 'r'});
    CHECK(expect_error(stub, SmfError::Truncated, 14) == 0);

    Bytes binary = header(0, 1, 96);
    chunk(binary, "\x01\x02\x03\x04", eot);
    CHECK(expect_error(binary, SmfError::BadChunk, 14) == 0);
    return 0;
}

int event_errors() {
    // Offsets: the track's first byte is at 22.
    CHECK(expect_error(track_with({0x00, 0x3C, 0x64, 0x00, 0xFF, 0x2F, 0x00}),
                       SmfError::MissingRunningStatus, 23) == 0);
    // Running status does not survive a meta event.
    CHECK(expect_error(track_with({0x00, 0x90, 0x3C, 0x64, 0x00, 0xFF, 0x01, 0x00,
                                   0x00, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00}),
                       SmfError::MissingRunningStatus, 31) == 0);
    CHECK(expect_error(track_with({0x00, 0xF8, 0x00, 0xFF, 0x2F, 0x00}),
                       SmfError::BadStatus, 23) == 0);
    CHECK(expect_error(track_with({0x00, 0x90, 0xBC, 0x64, 0x00, 0xFF, 0x2F, 0x00}),
                       SmfError::BadDataByte, 24) == 0);
    CHECK(expect_error(track_with({0x00, 0x90, 0x3C, 0xE4, 0x00, 0xFF, 0x2F, 0x00}),
                       SmfError::BadDataByte, 25) == 0);
    CHECK(expect_error(track_with({0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x2F, 0x00}),
                       SmfError::BadVlq, 22) == 0);
    CHECK(expect_error(track_with({0x00, 0xFF, 0x51, 0x02, 0x07, 0xA1, 0x00, 0xFF, 0x2F, 0x00}),
                       SmfError::BadMetaLength, 23) == 0);
    CHECK(expect_error(track_with({0x00, 0xFF, 0x2F, 0x01, 0x00}),
                       SmfError::BadMetaLength, 23) == 0);
    CHECK(expect_error(track_with({0x00, 0x90, 0x3C, 0x64}),
                       SmfError::MissingEndOfTrack, 26) == 0);
    CHECK(expect_error(track_with({0x00, 0xFF, 0x2F, 0x00, 0x00, 0x90, 0x3C, 0x64}),
                       SmfError::DataAfterEndOfTrack, 26) == 0);
    CHECK(expect_error(track_with({0x00, 0x90, 0x3C}), SmfError::Truncated, 24) == 0);
    CHECK(expect_error(track_with({0x00, 0xF0, 0x7F, 0x01}), SmfError::Truncated, 24) == 0);
    CHECK(expect_error(track_with({0x00, 0xFF}), SmfError::Truncated, 24) == 0);
    CHECK(expect_error(track_with({0x00}), SmfError::Truncated, 23) == 0);
    CHECK(expect_error(track_with({0x00, 0xFF, 0x80, 0x00}), SmfError::BadDataByte, 24) == 0);
    return 0;
}

int limits_are_enforced() {
    const Bytes good = track_with(fixture_track);
    SmfReadLimits limits;

    limits.max_file_bytes = good.size() - 1;
    CHECK(expect_error(good, SmfError::TooLarge, 0, limits) == 0);

    limits = {};
    limits.max_events = 3;
    CHECK(expect_error(good, SmfError::TooManyEvents, 40, limits) == 0);

    limits = {};
    limits.max_event_bytes = 2;
    CHECK(expect_error(good, SmfError::EventTooLarge, 25, limits) == 0);

    limits = {};
    limits.max_tracks = 1;
    const Bytes eot = {0x00, 0xFF, 0x2F, 0x00};
    CHECK(expect_error(file(1, 96, {eot, eot}), SmfError::TooManyTracks, 10, limits) == 0);
    return 0;
}

int every_truncation_is_an_error() {
    const Bytes good = file(1, 96, {fixture_track, fixture_track});
    for (std::size_t n = 0; n < good.size(); ++n) {
        const Bytes cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
        const SmfReadResult r = read(cut);
        CHECK(!r.ok());
        CHECK(r.error.offset <= n);
    }
    return 0;
}

int corrupted_files_never_misbehave() {
    // Each byte replaced by each of a few values. Under ASan this is the
    // check that matters; here, whatever reads must also write and read
    // back to the same value.
    const Bytes good = file(1, 96, {fixture_track, fixture_track});
    const std::uint8_t values[] = {0x00, 0x01, 0x2F, 0x7F, 0x80, 0x90, 0xF0, 0xF7, 0xFF};
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < good.size(); ++i) {
        for (const std::uint8_t v : values) {
            Bytes bad = good;
            bad[i] = v;
            const SmfReadResult r = read(bad);
            if (!r.ok()) {
                CHECK(r.error.code != SmfError::None && r.error.offset <= bad.size());
                continue;
            }
            ++accepted;
            if (r.smf->format == 2) continue;
            const SmfWriteResult w = write_smf(*r.smf);
            CHECK(w.ok());
            const SmfReadResult again = read(w.bytes);
            CHECK(again.ok() && *again.smf == *r.smf);
        }
    }
    CHECK(accepted > 0);
    return 0;
}

int writer_errors() {
    Smf smf;
    smf.format = 1;

    smf.tracks = {SmfTrack{{note_on(10, 0, 60, 1), note_on(5, 0, 60, 1)}}};
    SmfWriteResult w = write_smf(smf);
    CHECK(w.error.code == SmfError::UnsortedEvents && w.error.event_index == 1);

    smf.tracks = {SmfTrack{{note_on(-1, 0, 60, 1)}}};
    CHECK(write_smf(smf).error.code == SmfError::UnsortedEvents);

    smf.tracks = {SmfTrack{{end_of_track(0)}}, SmfTrack{{end_of_track(0), note_on(0, 0, 60, 1)}}};
    w = write_smf(smf);
    CHECK(w.error.code == SmfError::InvalidEvent && w.error.track == 1 &&
          w.error.event_index == 1);

    Event bad_data = note_on(0, 0, 60, 1);
    bad_data.data2 = 0x80;
    smf.tracks = {SmfTrack{{bad_data}}};
    CHECK(write_smf(smf).error.code == SmfError::InvalidEvent);

    Event no_status;
    smf.tracks = {SmfTrack{{no_status}}};
    CHECK(write_smf(smf).error.code == SmfError::InvalidEvent);

    smf.tracks = {SmfTrack{{meta_event(0, meta::tempo, {1, 2})}}};
    CHECK(write_smf(smf).error.code == SmfError::InvalidEvent);

    smf.tracks = {SmfTrack{{note_on(Tick{max_vlq} + 1, 0, 60, 1)}}};
    CHECK(write_smf(smf).error.code == SmfError::DeltaTooLarge);

    smf.format = 0;
    smf.tracks = {SmfTrack{}, SmfTrack{}};
    CHECK(write_smf(smf).error.code == SmfError::BadFormat);
    smf.format = 7;
    CHECK(write_smf(smf).error.code == SmfError::BadFormat);

    smf.format = 1;
    smf.division.raw = 0;
    CHECK(write_smf(smf).error.code == SmfError::BadDivision);
    return 0;
}

}  // namespace

int main() {
    RUN(vlq_matches_the_specification_table);
    RUN(reads_the_fixture);
    RUN(writes_the_fixture_byte_for_byte);
    RUN(format_1_round_trip);
    RUN(format_0_round_trip_and_missing_end);
    RUN(smpte_division_round_trips);
    RUN(format_2_is_read_only);
    RUN(tolerated_structure);
    RUN(header_errors);
    RUN(chunk_errors);
    RUN(event_errors);
    RUN(limits_are_enforced);
    RUN(every_truncation_is_an_error);
    RUN(corrupted_files_never_misbehave);
    RUN(writer_errors);
    std::printf("midi_smf: all tests passed\n");
    return 0;
}
