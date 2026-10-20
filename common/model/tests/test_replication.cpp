// SPDX-License-Identifier: GPL-3.0-or-later
//
// The replication format (ADR-0009 §2): every operation survives the codec
// unchanged, the codec refuses what is not an operation, and a replica fed
// a Document's updates stays equal to it through edits, undo, redo and
// eviction, and says so when it falls out of step.
#include <cmath>
#include <limits>
#include <optional>

#include "model_test.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"

using namespace model_test;
namespace midi = sonora::midi;

namespace {

// One of each, with every field away from its default, so a field the codec
// drops shows up as an inequality.
std::vector<Operation> one_of_each() {
    Source src;
    src.id = 7;
    src.path = "Audio Files/Kick_03.wav";
    src.channels = 2;
    src.length = 480000;
    src.format = AudioFormat::Float32;
    src.time_reference = 172800000;
    src.umid = std::string(64, 'a');
    src.kind = SourceKind::Recorded;
    src.take = 3;
    src.recorded_on_track = 2;
    src.latency_compensation = 64;

    TrackMix mix;
    mix.track = 2;
    mix.input = {4, 5};
    mix.monitor = MonitorMode::Input;
    mix.gain_db = -3.5f;
    mix.pan = 0.25f;
    mix.mute = true;
    mix.solo = true;
    mix.solo_safe = true;
    mix.midi = MidiRouting{"Keystation 49", 3, "UMC1820 MIDI 1", 10};

    MonitorBus bus;
    bus.outputs = {{2, 3}};
    bus.gain_db = -1.0f;
    bus.pan_law_db = -4.5f;

    SessionSettings st;
    st.name = "Session \"quoted\" \xC3\xA9";
    st.sample_rate = 96000;
    st.audio_format = AudioFormat::Float32;
    st.timecode.origin = 345600000;
    st.timecode.fps = Fps::F29_97df;
    st.tempo = {TempoEntry{0, 98.5f, {{6, 8}}}, TempoEntry{96000, 120.0f, {{4, 4}}}};
    st.transport.loop = {{100, 200}};
    st.transport.punch = {{300, 400}};
    st.transport.pre_roll = 1;
    st.transport.post_roll = 2;
    st.transport.punch_crossfade = 3;

    Region r = region(11, 7, 1000, 2000, 50, 2);
    r.gain_db = 2.5f;
    r.mute = true;
    r.fade_in = Fade{10, FadeShape::SCurve};
    r.fade_out = Fade{20, FadeShape::EqualPower};

    const Crossfade x{11, 12, 64, Align::Post, FadeShape::Linear};

    AddTrack at;
    at.track = 2;
    at.name = "Kick";
    at.channels = 2;
    at.playlist = 3;
    at.playlist_name = "Kick.01";
    at.mix = mix;

    RecordTake take;
    take.track = 2;
    take.playlist = 3;
    take.region = r;
    take.split_right_id = 13;
    take.crossfades = {x};

    Marker m;
    m.id = 20;
    m.position = 48000;
    m.name = "Chorus";
    m.length = 9600;

    const PluginRef lv2{PluginFormat::Lv2, "urn:sonora:test:gain", "Gain \xC3\xA9", "1.0"};
    const PluginRef vst3{PluginFormat::Vst3, "565354436F6D703A6C73702D636F6D70", "Comp", "2"};
    const std::string digest(64, 'b');
    const StateRef state{plugin_state_file(digest), digest, 99};

    MidiSource msrc;
    msrc.id = 40;
    msrc.path = "MIDI Files/Bass_02.mid";
    msrc.length = 96000;
    msrc.events = 17;
    msrc.time_reference = 172800000;
    msrc.kind = SourceKind::Recorded;
    msrc.take = 4;
    msrc.recorded_on_track = 2;
    msrc.latency_compensation = 32;

    auto seq = std::make_shared<MidiSequence>();
    seq->notes = {midi::Note{0, 480000, 1, 36, 90, 10}, midi::Note{960000, 1, 0, 127, 127, 0}};
    seq->events = {MidiEvent{0, 0xB3, 7, 100}, MidiEvent{5, 0xC3, 9, 0}};
    seq->sysex = {MidiSysEx{7, {0xF0, 0x43, 0x10, 0x7F, 0xF7}}};
    MidiRegion mr;
    mr.id = 41;
    mr.name = "Bass \xC3\xA9";
    mr.position = 3840000;
    mr.length = 7680000;
    mr.offset = 960000;
    mr.mute = true;
    mr.source = 40;
    mr.data = seq;

    AddTrack midi_track = at;
    midi_track.kind = TrackKind::Instrument;

    EditMidi em;
    em.playlist = 3;
    em.region = 41;
    em.remove_notes = {NoteRemoval{0, seq->notes[0]}, NoteRemoval{4, seq->notes[1]}};
    em.add_notes = {midi::Note{10, 20, 2, 60, 1, 127}};
    em.remove_events = {EventRemoval{1, seq->events[1]}};
    em.add_events = {MidiEvent{30, 0xE0, 0, 64}};
    em.remove_sysex = {SysExRemoval{0, seq->sysex[0]}};
    em.add_sysex = {MidiSysEx{40, {0xF0, 0xF7}}};

    return {AddSource{src},
            SetMix{mix},
            SetMonitorBus{bus},
            SetSettings{st},
            midi_track,
            PlaceRegion{3, r, 14},
            take,
            MoveRegion{3, 11, 5000, 15, Timebase::Ticks},
            TrimRegion{3, 11, Edge::Start, 1200, Timebase::Ticks},
            SplitRegion{3, 11, 1500, 16, Timebase::Ticks},
            RemoveRegion{3, 11},
            SetFade{3, 11, FadeEnd::Out, Fade{30, FadeShape::Linear}},
            SetCrossfade{3, x},
            RemoveCrossfade{3, 11, 12},
            AddPlaylist{2, 17, "Kick.02"},
            SetActivePlaylist{2, 17},
            AddMarker{m},
            MoveMarker{20, 96000},
            RemoveMarker{20},
            SetTrackName{2, "Kick In"},
            ReorderTracks{{2, 5, 9}},
            AddInsert{2, 30, 4, lv2, {{"gain", -3.5f}}, state},
            RemoveInsert{30},
            MoveInsert{30, 5, 9},
            SetInsertActive{30, false},
            ReplaceInsert{30, 31, vst3, {{"7", 0.5f}}, std::nullopt},
            SetInsertBypass{30, true},
            SetInsertParams{30, {{"gain", 1.5f}, {"mix", 0.25f}}},
            SetInsertState{30, state},
            AddMidiSource{msrc},
            PlaceMidiRegion{3, mr, 42, MidiDataRef{5, 9}},
            em,
            SetMidiRouting{2, MidiRouting{"all", 0, "none", 16}},
            Compound{{AddMarker{m}, take, SetMix{mix}}, "Paste \xC3\xA9"}};
}

template <typename T>
std::size_t variant_index() {
    return Operation(T{}).index();
}

const EditMidi& em_of(const std::vector<Operation>& ops) {
    return std::get<EditMidi>(ops[variant_index<EditMidi>()]);
}

bool same_op(const Operation& a, const Operation& b) {
    // Re-encoding both is the equality: canonical bytes, every field.
    std::string ea, eb, e;
    return encode_operation(a, ea, e) && encode_operation(b, eb, e) && ea == eb &&
           a.index() == b.index();
}

int test_every_operation_round_trips() {
    const std::vector<Operation> ops = one_of_each();
    CHECK(ops.size() == std::variant_size_v<Operation>);
    for (std::size_t i = 0; i < ops.size(); ++i) {
        CHECK(ops[i].index() == i);   // one of each, in order
        std::string bytes, error;
        CHECK(encode_operation(ops[i], bytes, error));
        Operation back;
        if (!decode_operation(bytes, back, error)) {
            std::fprintf(stderr, "  %s: %s\n", operation_name(ops[i]), error.c_str());
            return 1;
        }
        CHECK(same_op(ops[i], back));
    }
    // Spot checks that the comparison above would see a dropped field.
    std::string bytes, error;
    CHECK(encode_operation(ops[6], bytes, error));
    Operation back;
    CHECK(decode_operation(bytes, back, error));
    const auto& rt = std::get<RecordTake>(back);
    CHECK(rt.region == std::get<RecordTake>(ops[6]).region);
    CHECK(rt.crossfades.size() == 1 && rt.crossfades[0] == std::get<RecordTake>(ops[6]).crossfades[0]);
    CHECK(encode_operation(ops[3], bytes, error));
    CHECK(decode_operation(bytes, back, error));
    CHECK(std::get<SetSettings>(back).settings == std::get<SetSettings>(ops[3]).settings);
    CHECK(encode_operation(ops[1], bytes, error));
    CHECK(decode_operation(bytes, back, error));
    CHECK(std::get<SetMix>(back).mix.solo_safe);
    CHECK(std::get<SetMix>(back).mix.midi == std::get<SetMix>(ops[1]).mix.midi);
    CHECK(encode_operation(ops[4], bytes, error));
    CHECK(decode_operation(bytes, back, error));
    CHECK(std::get<AddTrack>(back).kind == TrackKind::Instrument);
    CHECK(encode_operation(ops[7], bytes, error));
    CHECK(decode_operation(bytes, back, error));
    CHECK(std::get<MoveRegion>(back).timebase == Timebase::Ticks);
    const std::size_t place_midi = variant_index<PlaceMidiRegion>();
    CHECK(encode_operation(ops[place_midi], bytes, error));
    CHECK(decode_operation(bytes, back, error));
    CHECK(std::get<PlaceMidiRegion>(back).region == std::get<PlaceMidiRegion>(ops[place_midi]).region);
    CHECK(mentions(bytes, "[7, \"f043107ff7\"]"));
    const std::size_t edit_midi = variant_index<EditMidi>();
    CHECK(encode_operation(ops[edit_midi], bytes, error));
    CHECK(decode_operation(bytes, back, error));
    const EditMidi& e2 = std::get<EditMidi>(back);
    CHECK(e2.remove_notes == em_of(ops).remove_notes && e2.add_notes == em_of(ops).add_notes);
    CHECK(e2.add_events == em_of(ops).add_events && e2.add_sysex == em_of(ops).add_sysex);
    return 0;
}

int test_codec_refuses_what_is_not_an_operation() {
    Operation out = AddMarker{};
    std::string error;
    struct Bad {
        const char* bytes;
        const char* says;
    };
    const Bad bad[] = {
        {"", "line"},
        {"[]", "\"op\""},
        {R"({"op": "Explode"})", "op: \"Explode\" is not an operation this version knows"},
        {R"({"op": "RemoveMarker"})", "missing required key \"marker\""},
        {R"({"op": "RemoveMarker", "marker": 1, "extra": 2})", "unknown key \"extra\""},
        {R"({"op": "RemoveMarker", "marker": -1})", "marker"},
        {R"({"op": "RemoveMarker", "marker": 1.5})", "marker"},
        {R"({"op": "MoveMarker", "marker": 1, "position": 9007199254740992})", "position"},
        {R"({"op": "TrimRegion", "playlist": 1, "region": 2, "edge": "middle", "to": 3,
             "timebase": "frames"})",
         "edge"},
        {R"({"op": "SetTrackName", "track": 1, "name": 7})", "must be a string"},
        {R"({"op": "SetInsertBypass", "insert": 1})", "missing required key \"bypass\""},
        {R"({"op": "SetInsertState", "insert": 1, "state": {"file": "x", "sha256": "y"}})",
         "missing required key \"bytes\""},
        {R"({"op": "SetInsertState", "insert": 1, "state": 3})", "state: must be an object"},
        {R"({"op": "MoveInsert", "insert": 1, "to_track": 2, "to_slot": 256})", "to_slot"},
        {R"({"op": "RemoveInsert", "insert": 1, "plugin": null})", "unknown key \"plugin\""},
        // A version-5 peer's SetMix, without solo safe.
        {R"({"op": "SetMix", "mix": {"track": 1, "input": [0], "monitor": "auto",
             "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false}})",
         "missing required key \"solo_safe\""},
        // A version-6 peer's MoveRegion, without the timebase it now needs
        // (A26): a position whose unit is not said is not guessed.
        {R"({"op": "MoveRegion", "playlist": 1, "region": 2, "position": 3,
             "split_right_id": 0})",
         "missing required key \"timebase\""},
        {R"({"op": "TrimRegion", "playlist": 1, "region": 2, "edge": "end", "to": 3,
             "timebase": "beats"})",
         "timebase"},
        {R"({"op": "EditMidi", "playlist": 1, "region": 2, "remove_notes": [-1],
             "add_notes": [], "remove_events": [], "add_events": [], "remove_sysex": [],
             "add_sysex": []})",
         "remove_notes"},
        // A protocol-7 draft's bare index: a removal carries its value.
        {R"({"op": "EditMidi", "playlist": 1, "region": 2, "remove_notes": [3],
             "add_notes": [], "remove_events": [], "add_events": [], "remove_sysex": [],
             "add_sysex": []})",
         "remove_notes[0]: must be [index, start, length"},
        {R"({"op": "PlaceMidiRegion", "playlist": 1, "split_right_id": 0,
             "region": {"id": 2, "name": "", "position_ticks": 0, "length_ticks": 1,
                        "offset_ticks": 0, "mute": false, "source": 0, "notes": [],
                        "events": [], "sysex": []}})",
         "missing required key \"data_from\""},
        {R"({"op": "EditMidi", "playlist": 1, "region": 2, "remove_notes": [],
             "add_notes": [[0, 1, 2]], "remove_events": [], "add_events": [],
             "remove_sysex": [], "add_sysex": []})",
         "add_notes[0]: must be [start, length"},
        {R"({"op": "SetMidiRouting", "track": 1, "midi_input": {"port": "all"},
             "midi_output": {"port": "none", "channel": 0}})",
         "midi_input: missing required key \"channel\""},
    };
    for (const Bad& b : bad) {
        error.clear();
        CHECK(!decode_operation(b.bytes, out, error));
        CHECK(mentions(error, b.says));
        // A refused decode leaves the output alone.
        CHECK(std::holds_alternative<AddMarker>(out));
    }
    // A value JSON cannot carry is refused at encode, not sent as garbage.
    SetMix nan_mix;
    nan_mix.mix.gain_db = std::numeric_limits<float>::quiet_NaN();
    std::string bytes;
    CHECK(!encode_operation(nan_mix, bytes, error));
    return 0;
}

int test_replica_follows_the_document() {
    Document doc(new_session(settings()), 64 * 1024);
    Replica replica;
    std::string error;
    ReplicationUpdate u;

    // Nothing is applied before the first snapshot.
    CHECK(!replica.synced());
    CHECK(doc.snapshot(u, error));
    CHECK(replica.receive(u, error));
    CHECK(replica.synced());
    CHECK(replica.current() == doc.current());

    Ids ids(doc.current());
    const AddTrack t = add_track(ids, "Vox");
    CHECK(doc.apply(t, u, error));
    CHECK(u.kind == ReplicationUpdate::Kind::Op);
    CHECK(replica.receive(u, error));
    const AddSource src = add_source(ids, 100'000'000);
    CHECK(doc.apply(src, u, error));
    CHECK(replica.receive(u, error));

    // Enough edits that the 64 KiB budget evicts: the floor moves, and the
    // replica, whose own budget is unlimited, follows it exactly.
    for (int i = 0; i < 60; ++i) {
        Operation op = PlaceRegion{t.playlist,
                                   region(doc.current().next_id, src.source.id, i * 2000, 1000)};
        std::string bytes;
        CHECK(encode_operation(op, bytes, error));
        CHECK(doc.apply(bytes, u, error));   // the bytes path, as the engine gets it
        CHECK(replica.receive(u, error));
        CHECK(replica.current() == doc.current());
    }
    CHECK(doc.history().floor() > 0);
    CHECK(replica.history()->floor() == doc.history().floor());
    CHECK(replica.history()->undo_depth() == doc.history().undo_depth());

    // Undo and redo move the replica's own pointer.
    for (int i = 0; i < 3; ++i) {
        CHECK(doc.undo(u, error));
        CHECK(u.kind == ReplicationUpdate::Kind::Undo);
        CHECK(replica.receive(u, error));
        CHECK(replica.current() == doc.current());
    }
    CHECK(doc.redo(u, error));
    CHECK(replica.receive(u, error));
    CHECK(replica.current() == doc.current());
    CHECK(replica.can_redo() == doc.history().can_redo());

    // A refused operation changes neither side and sends nothing.
    const std::uint64_t rev = doc.current().revision;
    CHECK(!doc.apply(RemoveRegion{t.playlist, 999999}, u, error));
    CHECK(!error.empty());
    CHECK(doc.current().revision == rev);
    return 0;
}

int test_replica_says_when_it_lost_track() {
    Document doc(new_session(settings()));
    Replica replica;
    std::string error;
    ReplicationUpdate u;
    CHECK(doc.snapshot(u, error));
    CHECK(replica.receive(u, error));
    Ids ids(doc.current());
    CHECK(doc.apply(add_track(ids, "A"), u, error));
    CHECK(replica.receive(u, error));
    // One update missed.
    CHECK(doc.apply(add_track(ids, "B"), u, error));
    CHECK(doc.apply(add_track(ids, "C"), u, error));
    CHECK(!replica.receive(u, error));
    CHECK(mentions(error, "expected revision"));
    CHECK(!replica.synced());
    // Everything after is refused until the snapshot.
    CHECK(doc.undo(u, error));
    CHECK(!replica.receive(u, error));
    CHECK(doc.snapshot(u, error));
    CHECK(replica.receive(u, error));
    CHECK(replica.synced());
    CHECK(same_document(replica.current(), doc.current()));
    CHECK(replica.current().revision == doc.current().revision);
    CHECK(replica.current().next_id == doc.current().next_id);
    // And it follows on from there.
    CHECK(doc.apply(SetTrackName{doc.current().edit->tracks[0]->id, "A2"}, u, error));
    CHECK(replica.receive(u, error));
    CHECK(replica.current().edit->tracks[0]->name == "A2");
    return 0;
}

bool same_history(const Replica& r, const Document& d) {
    return r.synced() && r.current() == d.current() &&
           r.history()->floor() == d.history().floor() &&
           r.history()->undo_depth() == d.history().undo_depth() &&
           r.history()->step_count() == d.history().step_count() &&
           r.can_undo() == d.history().can_undo() && r.can_redo() == d.history().can_redo();
}

// A replica that joins, or resyncs, after edits offers the engine's undo
// steps, not one. The snapshot used to carry only the current state, so
// the first Undo after a resync found nothing to undo on the replica and
// every Undo and Redo after that desynced it.
int test_resync_keeps_the_undo_window() {
    Document doc(new_session(settings()), 64 * 1024);
    std::string error;
    ReplicationUpdate u;
    Ids ids(doc.current());
    const AddTrack t = add_track(ids, "Vox");
    CHECK(doc.apply(t, u, error));
    const AddSource src = add_source(ids, 100'000'000);
    CHECK(doc.apply(src, u, error));
    for (int i = 0; i < 60; ++i) {
        CHECK(doc.apply(PlaceRegion{t.playlist, region(doc.current().next_id, src.source.id,
                                                       i * 2000, 1000)},
                        u, error));
    }
    CHECK(doc.history().floor() > 0);
    // A track added, its fader moved, then undone: its mix entry outlives
    // it, and a redo on the replica must bring it back with that fader.
    Ids later(doc.current());
    AddTrack gtr = add_track(later, "Gtr");
    CHECK(doc.apply(gtr, u, error));
    TrackMix m = gtr.mix;
    m.track = gtr.track;
    m.gain_db = -6.0f;
    CHECK(doc.apply(SetMix{m}, u, error));
    CHECK(doc.undo(u, error));
    CHECK(doc.undo(u, error));
    CHECK(doc.history().can_redo());

    Replica replica;
    CHECK(doc.snapshot(u, error));
    CHECK(replica.receive(u, error));
    CHECK(same_history(replica, doc));

    // Undo and redo several times with no further snapshot.
    const auto step = [&](bool undo) {
        if (!(undo ? doc.undo(u, error) : doc.redo(u, error))) return false;
        if (!replica.receive(u, error)) {
            std::fprintf(stderr, "  replica: %s\n", error.c_str());
            return false;
        }
        return same_history(replica, doc);
    };
    for (int i = 0; i < 5; ++i) CHECK(step(true));
    for (int i = 0; i < 7; ++i) CHECK(step(false));
    CHECK(replica.current().edit->tracks.size() == 2);
    const TrackMix* back = replica.current().mix.find(gtr.track);
    CHECK(back != nullptr && back->gain_db == -6.0f);
    for (int i = 0; i < 3; ++i) CHECK(step(true));
    // An edit after undo drops the redo branch on both sides.
    CHECK(doc.apply(SetTrackName{t.track, "Lead"}, u, error));
    CHECK(replica.receive(u, error));
    CHECK(same_history(replica, doc));
    while (doc.history().can_undo()) CHECK(step(true));
    CHECK(!replica.can_undo());
    CHECK(step(false));
    return 0;
}

// Over the cap a snapshot carries, the engine drops its oldest undo steps
// until it fits. A replica that followed all along learns the raised floor
// with the next update and stays equal to the engine.
int test_snapshot_over_the_cap_raises_the_floor() {
    Document doc(new_session(settings()));
    Replica old_hand;
    std::string error;
    ReplicationUpdate u;
    CHECK(doc.snapshot(u, error));
    CHECK(old_hand.receive(u, error));
    Ids ids(doc.current());
    const AddTrack t = add_track(ids, "Vox");
    CHECK(doc.apply(t, u, error));
    CHECK(old_hand.receive(u, error));
    for (int i = 0; i < 40; ++i) {
        CHECK(doc.apply(AddMarker{Marker{ids(), i * 1000, "m" + std::to_string(i), 0}}, u, error));
        CHECK(old_hand.receive(u, error));
    }
    // The current state alone is too large for a tiny cap: refused, and
    // the engine's undo is left as it was.
    const std::size_t steps = doc.history().step_count();
    CHECK(!doc.snapshot(u, error, 100));
    CHECK(mentions(error, "over the 100"));
    CHECK(doc.history().step_count() == steps);

    Document doc2(new_session(settings()));
    Replica a;
    CHECK(doc2.snapshot(u, error));
    CHECK(a.receive(u, error));
    Ids ids2(doc2.current());
    for (int i = 0; i < 40; ++i) {
        CHECK(doc2.apply(AddMarker{Marker{ids2(), i * 1000, "m" + std::to_string(i), 0}}, u,
                         error));
        CHECK(a.receive(u, error));
    }
    CHECK(doc2.snapshot(u, error));
    const std::size_t full = u.bytes.size();
    CHECK(doc2.snapshot(u, error, full * 3 / 4));
    CHECK(u.bytes.size() <= full * 3 / 4);
    CHECK(doc2.history().floor() > 0);
    Replica b;
    CHECK(b.receive(u, error));
    CHECK(same_history(b, doc2));
    // `a` still has the old floor until the next update tells it.
    CHECK(a.history()->floor() < doc2.history().floor());
    while (doc2.history().can_undo()) {
        CHECK(doc2.undo(u, error));
        CHECK(a.receive(u, error));
        CHECK(b.receive(u, error));
        CHECK(same_history(a, doc2));
        CHECK(same_history(b, doc2));
    }
    CHECK(!doc2.undo(u, error));
    return 0;
}

// The window decoder refuses what would not make a consistent history,
// naming why, and leaves the replica waiting for a good snapshot.
int test_window_refuses_what_does_not_fit() {
    Document doc(new_session(settings()));
    std::string error;
    ReplicationUpdate u;
    Ids ids(doc.current());
    CHECK(doc.apply(add_track(ids, "A"), u, error));
    CHECK(doc.apply(add_track(ids, "B"), u, error));
    CHECK(doc.snapshot(u, error));
    std::optional<History> h;
    CHECK(decode_window(u.bytes, h, error));
    CHECK(h.has_value() && h->step_count() == 3 && h->undo_depth() == 2);
    // Steps share what they did not change, on the replica as on the engine.
    CHECK(h->step_edit(1)->tracks[0] == h->step_edit(2)->tracks[0]);

    const auto refused = [&](const std::string& from, const std::string& to,
                             const char* why) {
        std::string bytes = u.bytes;
        const std::size_t at = bytes.find(from);
        if (at == std::string::npos) {
            std::fprintf(stderr, "  no \"%s\" in the window\n", from.c_str());
            return false;
        }
        bytes.replace(at, from.size(), to);
        std::optional<History> out;
        std::string err;
        return !decode_window(bytes, out, err) && !out.has_value() && mentions(err, why);
    };
    CHECK(refused("\"window_format\": 5", "\"window_format\": 4", "window_format"));
    CHECK(refused("\"window_format\": 5", "\"window_format\": 6", "window_format"));
    CHECK(refused("\"position\": 2", "\"position\": 3", "position"));
    CHECK(refused("\"edit\": 2", "\"edit\": 9", "steps.edit"));
    CHECK(refused("\"revision\": 1,", "\"revision\": 0,", "do not rise"));
    CHECK(refused("{", "", "undo window"));

    Replica r;
    ReplicationUpdate bad = u;
    bad.bytes = "{}";
    CHECK(!r.receive(bad, error));
    CHECK(!r.synced());
    bad = u;
    bad.undo_floor = 7;
    CHECK(!r.receive(bad, error));
    CHECK(mentions(error, "floor"));
    CHECK(r.receive(u, error));
    CHECK(same_history(r, doc));
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += test_every_operation_round_trips();
    failures += test_codec_refuses_what_is_not_an_operation();
    failures += test_replica_follows_the_document();
    failures += test_replica_says_when_it_lost_track();
    failures += test_resync_keeps_the_undo_window();
    failures += test_snapshot_over_the_cap_raises_the_floor();
    failures += test_window_refuses_what_does_not_fit();
    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    std::printf("model replication: all tests passed\n");
    return 0;
}
