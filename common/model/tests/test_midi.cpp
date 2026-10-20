// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI in the model (ADR-0014 M-1, brain/specs/midi.md §3): track kinds,
// MIDI playlists of windows onto shared sequences, the region operations'
// timebase tag, the new operations, undo, and the undo window carrying it
// all to a replica with its sharing intact.
#include <optional>

#include "model_test.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"

using namespace model_test;
namespace midi = sonora::midi;

namespace {

constexpr Ticks kBeat = kTicksPerQuarter;

AddTrack midi_track(Ids& ids, const std::string& name, TrackKind kind = TrackKind::Midi) {
    AddTrack op;
    op.track = ids();
    op.playlist = ids();
    op.name = name;
    op.playlist_name = name + ".01";
    op.kind = kind;
    op.channels = kind == TrackKind::Midi ? 0 : 2;
    return op;
}

std::shared_ptr<const MidiSequence> sequence(std::vector<midi::Note> notes,
                                             std::vector<MidiEvent> events = {}) {
    auto s = std::make_shared<MidiSequence>();
    s->notes = std::move(notes);
    s->events = std::move(events);
    return s;
}

MidiRegion midi_region(Id id, Ticks position, Ticks length, Ticks offset,
                       std::shared_ptr<const MidiSequence> data) {
    MidiRegion r;
    r.id = id;
    r.name = "m" + std::to_string(id);
    r.position = position;
    r.length = length;
    r.offset = offset;
    r.data = std::move(data);
    return r;
}

midi::Note note(Ticks start, Ticks length, std::uint8_t pitch) {
    midi::Note n;
    n.start = start;
    n.length = length;
    n.pitch = pitch;
    n.velocity = 100;
    return n;
}

const MidiPlaylist& midi_playlist_of(const Snapshot& s, std::size_t track) {
    return *s.edit->tracks[track]->midi_playlists.front();
}

const MidiRegion* find_midi(const MidiPlaylist& p, Id id) {
    for (const MidiRegion& r : p.regions) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

bool refused(History& h, const Operation& op, const char* why) {
    std::string error;
    const std::uint64_t revision = h.current().revision;
    if (h.apply(op, error)) {
        std::fprintf(stderr, "  applied, expected \"%s\"\n", why);
        return false;
    }
    return mentions(error, why) && h.current().revision == revision;
}

// One audio, one MIDI and one instrument track, and a region of four beats
// with a note on each beat at bar 2 of the MIDI track.
struct Fixture {
    History h{new_session(settings())};
    Id audio = 0, audio_pl = 0, midi = 0, midi_pl = 0, inst = 0, inst_pl = 0, region = 0;
    std::shared_ptr<const MidiSequence> data;

    bool build() {
        Ids ids(h.current());
        const AddTrack a = add_track(ids, "Vox");
        const AddTrack m = midi_track(ids, "Bass");
        const AddTrack i = midi_track(ids, "Piano", TrackKind::Instrument);
        std::string error;
        if (!h.apply(a, error) || !h.apply(m, error) || !h.apply(i, error)) {
            std::fprintf(stderr, "  %s\n", error.c_str());
            return false;
        }
        audio = a.track;
        audio_pl = a.playlist;
        midi = m.track;
        midi_pl = m.playlist;
        inst = i.track;
        inst_pl = i.playlist;
        data = sequence({note(0, kBeat / 2, 36), note(kBeat, kBeat / 2, 38),
                         note(2 * kBeat, kBeat / 2, 40), note(3 * kBeat, kBeat / 2, 41)});
        region = ids();
        if (!h.apply(PlaceMidiRegion{midi_pl, midi_region(region, 4 * kBeat, 4 * kBeat, 0, data), 0},
                     error)) {
            std::fprintf(stderr, "  %s\n", error.c_str());
            return false;
        }
        return true;
    }
};

int test_track_kinds() {
    Fixture f;
    CHECK(f.build());
    const Snapshot& s = f.h.current();
    CHECK(invariants_hold(s));
    const TrackEdit& m = *s.edit->tracks[1];
    CHECK(m.kind == TrackKind::Midi && m.channels == 0);
    CHECK(m.playlists.empty() && m.midi_playlists.size() == 1);
    CHECK(m.active_playlist == f.midi_pl);
    const TrackEdit& i = *s.edit->tracks[2];
    CHECK(i.kind == TrackKind::Instrument && i.channels == 2 && i.midi_playlists.size() == 1);
    CHECK(s.mix.find(f.midi)->midi.in_port == "all");
    CHECK(s.mix.find(f.midi)->midi.out_port == "none");

    Ids ids(s);
    AddTrack bad = midi_track(ids, "X");
    bad.channels = 1;
    CHECK(refused(f.h, bad, "a MIDI track has no audio channels"));
    bad = midi_track(ids, "X", TrackKind::Instrument);
    bad.channels = 0;
    CHECK(refused(f.h, bad, "1 or 2 channels"));
    bad = midi_track(ids, "X");
    bad.mix.input = {0};
    CHECK(refused(f.h, bad, "mix.input: a MIDI track records MIDI and has no audio input"));
    bad = midi_track(ids, "X");
    bad.mix.gain_db = -3.0f;
    CHECK(refused(f.h, bad, "mix.gain_db: a MIDI track has no audio fader"));
    bad = midi_track(ids, "X", TrackKind::Instrument);
    bad.mix.midi.out_port = "UMC1820 MIDI 1";
    CHECK(refused(f.h, bad, "an instrument track plays its own instrument"));
    AddTrack audio = add_track(ids, "Y");
    audio.mix.midi.in_port = "Keys";
    CHECK(refused(f.h, audio, "mix.midi: an audio track has no MIDI input or output"));

    // A playlist added to a MIDI track is a MIDI one, and can be made active.
    const Id pl = ids();
    CHECK_APPLY(f.h, (AddPlaylist{f.midi, pl, "Bass.02"}));
    CHECK(f.h.current().edit->tracks[1]->midi_playlists.size() == 2);
    CHECK_APPLY(f.h, (SetActivePlaylist{f.midi, pl}));
    CHECK(f.h.current().edit->tracks[1]->active_playlist == pl);
    CHECK(refused(f.h, SetActivePlaylist{f.midi, f.audio_pl}, "is not on track"));
    return 0;
}

// A26: frames sent to a tick playlist, or ticks to a frame one, are
// refused by name, never taken for the other unit.
int test_timebase_tag_refused_on_mismatch() {
    Fixture f;
    CHECK(f.build());
    // A frames MoveRegion is what a sender that forgot MIDI would send.
    CHECK(refused(f.h, MoveRegion{f.midi_pl, f.region, 96000, 0},
                  "is a MIDI playlist, in ticks, and this operation's position is in frames"));
    CHECK(refused(f.h, TrimRegion{f.midi_pl, f.region, Edge::End, 96000},
                  "in ticks, and this operation's position is in frames"));
    CHECK(refused(f.h, SplitRegion{f.midi_pl, f.region, 96000, 99},
                  "in ticks, and this operation's position is in frames"));
    // And the reverse.
    Ids ids(f.h.current());
    const AddSource src = add_source(ids, 480000);
    CHECK_APPLY(f.h, src);
    const Id ar = ids();
    CHECK_APPLY(f.h, (PlaceRegion{f.audio_pl, region(ar, src.source.id, 0, 48000), 0}));
    CHECK(refused(f.h, MoveRegion{f.audio_pl, ar, 10, 0, Timebase::Ticks},
                  "is an audio playlist, in frames, and this operation's position is in ticks"));
    CHECK(refused(f.h, TrimRegion{f.audio_pl, ar, Edge::End, 10, Timebase::Ticks},
                  "in frames, and this operation's position is in ticks"));
    CHECK(refused(f.h, SplitRegion{f.audio_pl, ar, 10, ids(), Timebase::Ticks},
                  "in frames, and this operation's position is in ticks"));
    // The audio-only operations name what a MIDI playlist is instead.
    CHECK(refused(f.h, SetFade{f.midi_pl, f.region, FadeEnd::In, Fade{10, FadeShape::Linear}},
                  "is a MIDI playlist; a MIDI region has no fades"));
    CHECK(refused(f.h, SetCrossfade{f.midi_pl, Crossfade{f.region, 1, 10}},
                  "MIDI regions have no crossfades"));
    CHECK(refused(f.h, PlaceRegion{f.midi_pl, region(ids(), src.source.id, 0, 48000), 0},
                  "is a MIDI playlist; place a MIDI region on it instead"));
    CHECK(refused(f.h,
                  PlaceMidiRegion{f.audio_pl, midi_region(ids(), 0, kBeat, 0, f.data), 0},
                  "is an audio playlist, not a MIDI one"));
    return 0;
}

int test_move_trim_split_in_ticks() {
    Fixture f;
    CHECK(f.build());
    // Trimming the start moves the window, not the notes: the second
    // beat's note is still at timeline beat 5.
    CHECK_APPLY(f.h, (TrimRegion{f.midi_pl, f.region, Edge::Start, 5 * kBeat, Timebase::Ticks}));
    const MidiRegion* r = find_midi(midi_playlist_of(f.h.current(), 1), f.region);
    CHECK(r != nullptr && r->position == 5 * kBeat && r->offset == kBeat);
    CHECK(r->length == 3 * kBeat && r->data == f.data);
    // Back past data tick 0 is refused; to it is allowed.
    CHECK(refused(f.h, TrimRegion{f.midi_pl, f.region, Edge::Start, 3 * kBeat, Timebase::Ticks},
                  "its start cannot move back past tick 0"));
    CHECK_APPLY(f.h, (TrimRegion{f.midi_pl, f.region, Edge::Start, 4 * kBeat, Timebase::Ticks}));
    r = find_midi(midi_playlist_of(f.h.current(), 1), f.region);
    CHECK(r->offset == 0 && r->position == 4 * kBeat);

    // A split makes two windows onto the one sequence.
    Ids ids(f.h.current());
    const Id right = ids();
    CHECK_APPLY(f.h, (SplitRegion{f.midi_pl, f.region, 6 * kBeat, right, Timebase::Ticks}));
    const MidiPlaylist& p = midi_playlist_of(f.h.current(), 1);
    CHECK(p.regions.size() == 2);
    CHECK(p.regions[0].id == f.region && p.regions[0].length == 2 * kBeat);
    CHECK(p.regions[1].id == right && p.regions[1].position == 6 * kBeat);
    CHECK(p.regions[1].offset == 2 * kBeat && p.regions[1].length == 2 * kBeat);
    CHECK(p.regions[0].data == p.regions[1].data);

    // A move overwrites what is beneath, splitting it with the id given.
    const Id other = ids();
    CHECK_APPLY(f.h, (PlaceMidiRegion{f.midi_pl, midi_region(other, 20 * kBeat, kBeat, 0, f.data), 0}));
    const Id spare = ids();
    CHECK(refused(f.h, MoveRegion{f.midi_pl, other, 4 * kBeat + kBeat / 2, 0, Timebase::Ticks},
                  "must be split, but the operation carries no split id"));
    CHECK_APPLY(f.h, (MoveRegion{f.midi_pl, other, 4 * kBeat + kBeat / 2, spare, Timebase::Ticks}));
    const MidiPlaylist& q = midi_playlist_of(f.h.current(), 1);
    CHECK(q.regions.size() == 4);
    CHECK(q.regions[0].id == f.region && q.regions[0].length == kBeat / 2);
    CHECK(q.regions[1].id == other);
    CHECK(q.regions[2].id == spare && q.regions[2].position == 5 * kBeat + kBeat / 2);
    CHECK(q.regions[2].offset == kBeat + kBeat / 2);
    CHECK(f.h.current().next_id == spare + 1);
    CHECK(invariants_hold(f.h.current()));

    // Removal by id, as for audio; undo brings it all back.
    CHECK_APPLY(f.h, (RemoveRegion{f.midi_pl, other}));
    CHECK(find_midi(midi_playlist_of(f.h.current(), 1), other) == nullptr);
    CHECK(f.h.undo() && f.h.undo());
    CHECK(midi_playlist_of(f.h.current(), 1).regions.size() == 3);
    return 0;
}

// Editing through one of two regions that share a sequence gives that one
// a sequence of its own; the other keeps the old (midi.md §3).
int test_edit_midi_copies_on_write() {
    Fixture f;
    CHECK(f.build());
    Ids ids(f.h.current());
    const Id right = ids();
    CHECK_APPLY(f.h, (SplitRegion{f.midi_pl, f.region, 6 * kBeat, right, Timebase::Ticks}));
    EditMidi e;
    e.playlist = f.midi_pl;
    e.region = f.region;
    e.remove_notes = {1};
    e.add_notes = {note(kBeat / 4, kBeat / 4, 50), note(0, kBeat / 2, 30)};
    e.add_events = {MidiEvent{0, 0xB0, 64, 127}, MidiEvent{0, 0xB0, 1, 10}};
    CHECK_APPLY(f.h, e);
    const MidiPlaylist& p = midi_playlist_of(f.h.current(), 1);
    const MidiSequence& left = *p.regions[0].data;
    CHECK(p.regions[1].data == f.data);
    CHECK(p.regions[0].data != f.data);
    // Sorted in: at start 0 the channel and pitch decide.
    CHECK(left.notes.size() == 5);
    CHECK(left.notes[0].pitch == 30 && left.notes[1].pitch == 36 && left.notes[2].pitch == 50);
    CHECK(left.notes[3].pitch == 40 && left.notes[4].pitch == 41);
    // Two events at one tick keep the order they were sent in.
    CHECK(left.events.size() == 2 && left.events[0].data1 == 64 && left.events[1].data1 == 1);
    CHECK(invariants_hold(f.h.current()));

    // Added after what is already at the tick.
    EditMidi more;
    more.playlist = f.midi_pl;
    more.region = f.region;
    more.add_events = {MidiEvent{0, 0xC0, 3, 0}};
    CHECK_APPLY(f.h, more);
    CHECK(midi_playlist_of(f.h.current(), 1).regions[0].data->events[2].status == 0xC0);

    // Undo is a pointer: the sequence before the edit comes back as it was.
    CHECK(f.h.undo() && f.h.undo());
    CHECK(midi_playlist_of(f.h.current(), 1).regions[0].data == f.data);
    CHECK(f.h.redo());
    CHECK(midi_playlist_of(f.h.current(), 1).regions[0].data->notes.size() == 5);

    // Refusals name the value by where it is in the operation.
    EditMidi bad;
    bad.playlist = f.midi_pl;
    bad.region = f.region;
    CHECK(refused(f.h, bad, "removes and adds nothing"));
    bad.add_notes = {note(0, 1, 60), note(0, 1, 200)};
    CHECK(refused(f.h, bad, "add_notes[1]: pitch must be from 0 to 127"));
    bad.add_notes = {note(0, 1, 60)};
    bad.add_notes[0].velocity = 0;
    CHECK(refused(f.h, bad, "add_notes[0]: velocity must be from 1 to 127"));
    bad.add_notes.clear();
    bad.add_events = {MidiEvent{0, 0x90, 60, 100}};
    CHECK(refused(f.h, bad, "add_events[0]: status must be a channel message other than a note"));
    bad.add_events.clear();
    bad.add_sysex = {MidiSysEx{0, {0xF0, 0x80, 0xF7}}};
    CHECK(refused(f.h, bad, "add_sysex[0]: a SysEx message's data bytes are below 0x80"));
    bad.add_sysex.clear();
    bad.remove_notes = {2, 2};
    CHECK(refused(f.h, bad, "remove_notes[1]: indices must be increasing"));
    bad.remove_notes = {5};
    CHECK(refused(f.h, bad, "remove_notes[0]: indices must be increasing and below 5"));
    bad.remove_notes = {0};
    bad.region = 9999;
    CHECK(refused(f.h, bad, "region 9999 is not on playlist"));
    return 0;
}

int test_place_midi_region_refusals() {
    Fixture f;
    CHECK(f.build());
    Ids ids(f.h.current());
    MidiRegion r = midi_region(ids(), 20 * kBeat, kBeat, 0, nullptr);
    CHECK(refused(f.h, PlaceMidiRegion{f.midi_pl, r, 0}, "has no sequence"));
    r.data = sequence({note(kBeat, 0, 60), note(0, 1, 60)});
    CHECK(refused(f.h, PlaceMidiRegion{f.midi_pl, r, 0}, "notes[1]: notes are not in order"));
    r.data = f.data;
    r.offset = -1;
    CHECK(refused(f.h, PlaceMidiRegion{f.midi_pl, r, 0}, "offset must not be negative"));
    r.offset = 0;
    r.source = 777;
    CHECK(refused(f.h, PlaceMidiRegion{f.midi_pl, r, 0}, "MIDI source 777 does not exist"));
    r.source = 0;
    r.position = kMaxInteger;
    CHECK(refused(f.h, PlaceMidiRegion{f.midi_pl, r, 0}, "past the end of the timeline"));
    // Overwrite, as audio: a region beneath spanning both ends needs an id.
    r.position = 5 * kBeat;
    CHECK(refused(f.h, PlaceMidiRegion{f.midi_pl, r, 0}, "must be split"));
    CHECK(refused(f.h, PlaceMidiRegion{f.midi_pl, r, r.id}, "the split id must differ"));
    const Id split = ids();
    CHECK_APPLY(f.h, (PlaceMidiRegion{f.midi_pl, r, split}));
    CHECK(midi_playlist_of(f.h.current(), 1).regions.size() == 3);
    CHECK(f.h.current().next_id == split + 1);
    return 0;
}

int test_midi_sources_and_routing() {
    Fixture f;
    CHECK(f.build());
    Ids ids(f.h.current());
    MidiSource s;
    s.id = ids();
    s.path = "MIDI Files/Bass_01.mid";
    s.length = 96000;
    s.events = 8;
    s.kind = SourceKind::Recorded;
    s.take = 1;
    s.recorded_on_track = f.midi;
    // A take is one compound: the source, then its region (midi.md §3.1).
    const Id rid = ids();
    MidiRegion r = midi_region(rid, 40 * kBeat, kBeat, 0, f.data);
    r.source = s.id;
    CHECK_APPLY(f.h, (Compound{{AddMidiSource{s}, PlaceMidiRegion{f.midi_pl, r, 0}}, "Record"}));
    CHECK(f.h.current().sources->find_midi(s.id) != nullptr);
    CHECK(f.h.can_undo());
    CHECK(f.h.undo());
    // Undo takes the region and leaves the source: the pool is outside undo.
    CHECK(f.h.current().sources->find_midi(s.id) != nullptr);
    CHECK(find_midi(midi_playlist_of(f.h.current(), 1), rid) == nullptr);

    MidiSource dup = s;
    dup.id = ids();
    CHECK(refused(f.h, AddMidiSource{dup}, "is already MIDI source"));
    dup.path = "MIDI Files/../x.mid";
    CHECK(refused(f.h, AddMidiSource{dup}, "has an empty, \".\" or \"..\" component"));
    dup.path = "MIDI Files/Other.mid";
    dup.kind = SourceKind::Imported;
    CHECK(refused(f.h, AddMidiSource{dup}, "an imported source has no take"));

    // Routing is mix state: saved, never undone.
    const std::uint64_t steps = f.h.step_count();
    CHECK_APPLY(f.h, (SetMidiRouting{f.midi, MidiRouting{"Keys", 2, "UMC1820 MIDI 1", 10}}));
    CHECK(f.h.step_count() == steps);
    const TrackMix* m = f.h.current().mix.find(f.midi);
    CHECK(m->midi.in_port == "Keys" && m->midi.out_channel == 10);
    CHECK(refused(f.h, SetMidiRouting{f.audio, MidiRouting{}}, "is an audio track"));
    CHECK(refused(f.h, SetMidiRouting{f.inst, MidiRouting{"all", 0, "Synth", 0}},
                  "has no MIDI output"));
    CHECK(refused(f.h, SetMidiRouting{f.midi, MidiRouting{"none", 0, "none", 0}},
                  "routing.midi_input.port: must be \"all\" or a port name"));
    CHECK(refused(f.h, SetMidiRouting{f.midi, MidiRouting{"all", 17, "none", 0}},
                  "routing.midi_input.channel"));
    CHECK(refused(f.h, SetMidiRouting{f.midi, MidiRouting{"all", 0, "", 0}},
                  "routing.midi_output.port"));
    // SetMix carries the routing too, held to the same rule.
    TrackMix mix = *m;
    mix.midi.out_port = "all";
    CHECK(refused(f.h, SetMix{mix}, "mix.midi_output.port"));
    return 0;
}

int test_inserts_on_midi_tracks() {
    Fixture f;
    CHECK(f.build());
    Ids ids(f.h.current());
    const PluginRef lv2{PluginFormat::Lv2, "urn:sonora:test:gain", "Gain", "1"};
    CHECK(refused(f.h, AddInsert{f.midi, ids(), 0, lv2, {}, std::nullopt},
                  "is a MIDI track; it has no audio"));
    // An instrument track takes inserts after its instrument.
    const Id ins = ids();
    CHECK_APPLY(f.h, (AddInsert{f.inst, ins, 0, lv2, {}, std::nullopt}));
    CHECK(refused(f.h, MoveInsert{ins, f.midi, 1}, "is a MIDI track"));
    return 0;
}

// What a replica rebuilt from the undo window holds: every kind, every
// sequence, and two regions sharing one sequence still sharing it.
int test_window_carries_midi() {
    Document doc(new_session(settings()));
    std::string error;
    ReplicationUpdate u;
    Ids ids(doc.current());
    const AddTrack m = midi_track(ids, "Bass");
    CHECK(doc.apply(m, u, error));
    auto data = sequence({note(0, kBeat, 36)}, {MidiEvent{0, 0xB0, 7, 90}});
    const Id r = ids();
    CHECK(doc.apply(PlaceMidiRegion{m.playlist, midi_region(r, 0, 4 * kBeat, 0, data), 0}, u,
                    error));
    const Id right = ids();
    CHECK(doc.apply(SplitRegion{m.playlist, r, 2 * kBeat, right, Timebase::Ticks}, u, error));
    CHECK(doc.apply(SetMidiRouting{m.track, MidiRouting{"Keys", 1, "Out", 2}}, u, error));
    CHECK(doc.snapshot(u, error));
    std::optional<History> h;
    if (!decode_window(u.bytes, h, error)) std::fprintf(stderr, "  %s\n", error.c_str());
    CHECK(h.has_value());
    CHECK(h->current() == doc.current());
    const MidiPlaylist& p = *h->current().edit->tracks[0]->midi_playlists[0];
    CHECK(p.regions.size() == 2 && p.regions[0].data == p.regions[1].data);
    CHECK(*p.regions[0].data == *data);
    // The step before the split shares the sequence too.
    const MidiPlaylist& before = *h->step_edit(2)->tracks[0]->midi_playlists[0];
    CHECK(before.regions.size() == 1 && before.regions[0].data == p.regions[0].data);
    CHECK(h->current().mix.find(m.track)->midi.out_port == "Out");

    // A replica fed the same updates stays equal, through undo.
    Replica rep;
    CHECK(rep.receive(u, error));
    CHECK(doc.undo(u, error));
    CHECK(rep.receive(u, error));
    CHECK(rep.current() == doc.current());
    return 0;
}

// A note edit copies the sequence it touches, and the undo budget is
// charged for it (ADR-0014 Unverified 6); a split shares it and is not.
int test_undo_charge_counts_sequences() {
    Fixture f;
    CHECK(f.build());
    std::vector<MidiEvent> controllers;
    for (int i = 0; i < 1000; ++i) {
        controllers.push_back(MidiEvent{i * 100, 0xB0, 1, static_cast<std::uint8_t>(i % 128)});
    }
    Ids ids(f.h.current());
    const Id big = ids();
    CHECK_APPLY(f.h, (PlaceMidiRegion{f.midi_pl,
                                      midi_region(big, 40 * kBeat, 4 * kBeat, 0,
                                                  sequence({note(0, 1, 60)}, controllers)),
                                      0}));
    const std::size_t placed = f.h.charged_bytes();
    const Id right = ids();
    CHECK_APPLY(f.h, (SplitRegion{f.midi_pl, big, 42 * kBeat, right, Timebase::Ticks}));
    const std::size_t split = f.h.charged_bytes() - placed;
    EditMidi e;
    e.playlist = f.midi_pl;
    e.region = big;
    e.add_notes = {note(1, 1, 61)};
    CHECK_APPLY(f.h, e);
    const std::size_t edited = f.h.charged_bytes() - placed - split;
    CHECK(split < 1000 * sizeof(MidiEvent));
    CHECK(edited > 1000 * sizeof(MidiEvent));
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
    t(test_track_kinds);
    t(test_timebase_tag_refused_on_mismatch);
    t(test_move_trim_split_in_ticks);
    t(test_edit_midi_copies_on_write);
    t(test_place_midi_region_refusals);
    t(test_midi_sources_and_routing);
    t(test_inserts_on_midi_tracks);
    t(test_window_carries_midi);
    t(test_undo_charge_counts_sequences);
    if (failed != 0) {
        std::fprintf(stderr, "model_midi: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_midi: %d passed\n", run);
    return 0;
}
