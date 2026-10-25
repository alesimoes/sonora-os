// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI sequences as spines of canonical chunks (ADR-0015 §1, §2, §4, §5):
// the chunking is a function of the contents alone, an edit copies only the
// chunks it touches, undo and the budget count per chunk, and the undo
// window carries each chunk once, in parts a replica decodes one at a time.
#include <optional>
#include <random>

#include "model_test.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"
#include "sonora/model/sequence_file.h"
#include "sonora/model/session_file.h"

using namespace model_test;
namespace midi = sonora::midi;

namespace {

constexpr Ticks kBeat = kTicksPerQuarter;

midi::Note note(Ticks start, Ticks length, std::uint8_t pitch) {
    midi::Note n;
    n.start = start;
    n.length = length;
    n.pitch = pitch;
    n.velocity = 100;
    return n;
}

// `seconds` at 120 bpm: a note each half beat and a controller each
// twentieth, so every chunk holds both.
MidiLists dense(int seconds) {
    MidiLists l;
    const Ticks span = static_cast<Ticks>(seconds) * 2 * kBeat;
    for (Ticks t = 0; t < span; t += kBeat / 2) {
        l.notes.push_back(note(t, kBeat / 4, static_cast<std::uint8_t>(36 + (t / kBeat) % 48)));
    }
    std::uint8_t v = 0;
    for (Ticks t = 0; t < span; t += kBeat / 20) {
        l.events.push_back(MidiEvent{t, 0xB0, 1, static_cast<std::uint8_t>(v++ & 0x7F)});
    }
    return l;
}

AddTrack midi_track(Ids& ids, const std::string& name) {
    AddTrack op;
    op.track = ids();
    op.playlist = ids();
    op.name = name;
    op.playlist_name = name + ".01";
    op.kind = TrackKind::Midi;
    op.channels = 0;
    return op;
}

MidiRegion midi_region(Id id, Ticks position, Ticks length,
                       std::shared_ptr<const MidiSequence> data) {
    MidiRegion r;
    r.id = id;
    r.name = "m" + std::to_string(id);
    r.position = position;
    r.length = length;
    r.data = std::move(data);
    return r;
}

// One MIDI track holding one region of `lists`.
struct Session {
    History h;
    Id playlist = 0, region = 0;
    explicit Session(std::uint64_t max_entries = History::kDefaultMidiMaxEntries)
        : h(new_session(settings()), History::kDefaultBudget, max_entries) {}

    bool build(MidiLists lists) {
        Ids ids(h.current());
        const AddTrack t = midi_track(ids, "Keys");
        std::string error;
        playlist = t.playlist;
        region = ids();
        auto data = std::make_shared<const MidiSequence>(std::move(lists));
        const Ticks length = std::max<Ticks>(kBeat, data->empty() ? kBeat : 4 * kBeat);
        if (!h.apply(t, error) ||
            !h.apply(PlaceMidiRegion{playlist, midi_region(region, 0, length, data), 0}, error)) {
            std::fprintf(stderr, "  %s\n", error.c_str());
            return false;
        }
        return true;
    }
    const MidiSequence& data() const {
        return *h.current().edit->tracks[0]->midi_playlists[0]->regions[0].data;
    }
    std::shared_ptr<const MidiSequence> data_ptr() const {
        return h.current().edit->tracks[0]->midi_playlists[0]->regions[0].data;
    }
};

// Note `i` moved by `by`: what a drag in the piano roll sends.
EditMidi drag(const Session& s, std::size_t i, Ticks by) {
    EditMidi e;
    e.playlist = s.playlist;
    e.region = s.region;
    midi::Note n = s.data().notes()[i];
    e.remove_notes = {NoteRemoval{static_cast<std::uint32_t>(i), n}};
    n.start += by;
    e.add_notes = {n};
    return e;
}

std::size_t chunk_count(const MidiSequence& s) {
    std::size_t n = 0;
    for (const MidiSpineNode& l : s.spine()) n += l.leaf->chunks.size();
    return n;
}

int test_chunks_are_canonical() {
    const MidiLists l = dense(30);
    const MidiSequence s(l);
    CHECK(s.note_count() == l.notes.size() && s.event_count() == l.events.size());
    CHECK(s.lists() == l);
    CHECK(s.notes() == l.notes && s.events() == l.events && s.sysex().empty());
    // 30 s at 120 bpm is 60 beats, 15 chunks of four, all in one leaf.
    CHECK(chunk_count(s) == 15 && s.spine().size() == 1);
    std::int64_t k = 0;
    for (const MidiSpineEntry& e : s.spine()[0].leaf->chunks) {
        const MidiChunk& c = *e.chunk;
        CHECK(c.index == k++);
        for (const midi::Note& n : c.data.notes) CHECK(midi_chunk_of(n.start) == c.index);
        for (const MidiEvent& ev : c.data.events) CHECK(midi_chunk_of(ev.tick) == c.index);
    }
    // Random access agrees with the flat lists at every index.
    for (std::size_t i = 0; i < l.notes.size(); ++i) CHECK(s.notes()[i] == l.notes[i]);
    for (std::size_t i = 0; i < l.events.size(); i += 7) CHECK(s.events()[i] == l.events[i]);
    // Empty chunks are absent, and a leaf spans 64 chunks.
    MidiLists sparse;
    sparse.notes = {note(0, 1, 60), note(200 * kMidiChunkTicks, 1, 61)};
    const MidiSequence sp(sparse);
    CHECK(chunk_count(sp) == 2 && sp.spine().size() == 2);
    CHECK(sp.spine()[1].leaf->index == 200 / kMidiLeafChunks);
    CHECK(sp.chunk_at(1) == nullptr && sp.chunk_at(200) != nullptr);
    CHECK(sp.notes()[1].pitch == 61);
    return 0;
}

// Two paths to the same notes give equal spines: a sequence built whole, and
// one reached by edits.
int test_edits_reach_the_canonical_chunks() {
    Session s;
    MidiLists start = dense(20);
    CHECK(s.build(start));
    // Drag notes across chunk edges, both ways.
    for (const std::size_t i : {std::size_t{7}, std::size_t{20}, std::size_t{55}}) {
        CHECK_APPLY(s.h, drag(s, i, (i % 2 == 0 ? 3 : -3) * kBeat));
    }
    const MidiSequence rebuilt(s.data().lists());
    CHECK(rebuilt == s.data());
    CHECK(chunk_count(rebuilt) == chunk_count(s.data()));
    CHECK(invariants_hold(s.h.current()));
    return 0;
}

// ADR-0015 §2: a drag copies the chunks it touches and the spine above them,
// and shares every other chunk and leaf. Its charge comes from counts and
// fixed sizes, and stays far under 64 KiB for an hour-long region.
int test_a_drag_copies_only_what_it_touches() {
    Session s;
    CHECK(s.build(dense(3600)));
    const MidiSequence& before = s.data();
    const auto before_ptr = s.data_ptr();
    CHECK(chunk_count(before) == 1800 && before.spine().size() == 29);
    const std::size_t charged = s.h.charged_bytes();
    const std::uint64_t entries = s.h.midi_entries();
    // A note in chunk 900 moved within it.
    const std::size_t i = 900 * 8 + 1;
    CHECK(midi_chunk_of(before.notes()[i].start) == 900);
    CHECK_APPLY(s.h, drag(s, i, kBeat / 8));
    const MidiSequence& after = s.data();
    std::size_t shared_chunks = 0, shared_leaves = 0;
    for (std::size_t l = 0; l < after.spine().size(); ++l) {
        const MidiSpineLeaf& a = *after.spine()[l].leaf;
        const MidiSpineLeaf& b = *before_ptr->spine()[l].leaf;
        shared_leaves += &a == &b ? 1 : 0;
        for (std::size_t c = 0; c < a.chunks.size(); ++c) {
            shared_chunks += a.chunks[c].chunk == b.chunks[c].chunk ? 1 : 0;
        }
    }
    CHECK(shared_chunks == 1799 && shared_leaves == 28);
    const std::size_t drag_charge = s.h.charged_bytes() - charged;
    CHECK(drag_charge < 16 * 1024);
    // The budget counts the one chunk the step made, not the sequence.
    const MidiChunk& touched = *after.chunk_at(900);
    CHECK(s.h.midi_entries() - entries == touched.data.entries());
    CHECK(s.h.current_midi_entries() == entries);
    // A sequence of 29 leaves crosses the window and comes back the same.
    WindowParts w;
    std::string error;
    CHECK(encode_window(s.h, w, error));
    std::optional<History> back;
    CHECK(decode_window(w, back, error));
    CHECK(back.has_value() && back->current() == s.h.current());
    // Across a chunk edge: two chunks.
    CHECK_APPLY(s.h, drag(s, 1000 * 8, -kBeat));
    CHECK(s.h.midi_entries() - entries ==
          touched.data.entries() + s.data().chunk_at(999)->data.entries() +
              s.data().chunk_at(1000)->data.entries());
    return 0;
}

// The flat-list EditMidi this replaces, as the reference: removal by
// index, additions appended, a stable sort.
MidiLists flat_edit(MidiLists l, const EditMidi& e) {
    const auto drop = [](auto& v, const auto& rm) {
        for (std::size_t k = rm.size(); k-- > 0;) {
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(rm[k].index));
        }
    };
    drop(l.notes, e.remove_notes);
    drop(l.events, e.remove_events);
    l.notes.insert(l.notes.end(), e.add_notes.begin(), e.add_notes.end());
    midi::sort_notes(l.notes);
    l.events.insert(l.events.end(), e.add_events.begin(), e.add_events.end());
    std::stable_sort(l.events.begin(), l.events.end(),
                     [](const MidiEvent& a, const MidiEvent& b) { return a.tick < b.tick; });
    return l;
}

int test_random_edits_match_the_flat_lists() {
    Session s;
    CHECK(s.build(dense(40)));
    MidiLists reference = s.data().lists();
    std::mt19937 rng(7);
    for (int round = 0; round < 300; ++round) {
        EditMidi e;
        e.playlist = s.playlist;
        e.region = s.region;
        const MidiSequence& now = s.data();
        // Up to three removals of each kind, increasing, and as many adds.
        for (std::size_t i = rng() % 5; i < now.note_count() && e.remove_notes.size() < 3;
             i += 1 + rng() % 40) {
            e.remove_notes.push_back(NoteRemoval{static_cast<std::uint32_t>(i), now.notes()[i]});
        }
        for (std::size_t i = rng() % 9; i < now.event_count() && e.remove_events.size() < 3;
             i += 1 + rng() % 200) {
            e.remove_events.push_back(
                EventRemoval{static_cast<std::uint32_t>(i), now.events()[i]});
        }
        const int adds = static_cast<int>(rng() % 4);
        for (int a = 0; a < adds; ++a) {
            const Ticks t = static_cast<Ticks>(rng() % (90 * kBeat));
            e.add_notes.push_back(note(t, kBeat / 3, static_cast<std::uint8_t>(30 + rng() % 60)));
            e.add_events.push_back(MidiEvent{t, 0xB0, 7, static_cast<std::uint8_t>(rng() % 128)});
        }
        if (e.remove_notes.empty() && e.add_notes.empty() && e.remove_events.empty()) continue;
        reference = flat_edit(std::move(reference), e);
        CHECK_APPLY(s.h, e);
        if (!(s.data().lists() == reference)) {
            std::fprintf(stderr, "  round %d differs from the flat lists\n", round);
            return 1;
        }
        CHECK(MidiSequence(reference) == s.data());
    }
    CHECK(invariants_hold(s.h.current()));
    return 0;
}

// A region pasted elsewhere shares the sequence; editing the copy shares
// all but the touched chunk, so the budget grows by that chunk alone.
int test_budget_counts_shared_chunks_once() {
    Session s;
    CHECK(s.build(dense(60)));
    const std::uint64_t one = s.data().entries();
    CHECK(s.h.current_midi_entries() == one);
    Ids ids(s.h.current());
    const Id copy = ids();
    PlaceMidiRegion paste{s.playlist, midi_region(copy, 1000 * kBeat, 4 * kBeat, nullptr), 0};
    paste.data_from = MidiDataRef{s.playlist, s.region};
    CHECK_APPLY(s.h, paste);
    CHECK(s.h.current_midi_entries() == one);
    EditMidi e;
    e.playlist = s.playlist;
    e.region = copy;
    e.add_events = {MidiEvent{kBeat / 3, 0xB0, 64, 127}};
    CHECK_APPLY(s.h, e);
    const MidiChunk* c0 = nullptr;
    for (const MidiRegion& r : s.h.current().edit->tracks[0]->midi_playlists[0]->regions) {
        if (r.id == copy) c0 = r.data->chunk_at(0);
    }
    CHECK(c0 != nullptr);
    // The original's 30 chunks and the copy's new chunk 0.
    CHECK(s.h.current_midi_entries() == one + c0->data.entries());
    // Counted per sequence, the edited copy would be a second `one`: a budget
    // of one and its new chunk would refuse the edit.
    Session tight(one + c0->data.entries());
    CHECK(tight.build(dense(60)));
    Ids tids(tight.h.current());
    const Id tcopy = tids();
    PlaceMidiRegion again{tight.playlist, midi_region(tcopy, 1000 * kBeat, 4 * kBeat, nullptr),
                          0};
    again.data_from = MidiDataRef{tight.playlist, tight.region};
    CHECK_APPLY(tight.h, again);
    e.playlist = tight.playlist;
    e.region = tcopy;
    CHECK_APPLY(tight.h, e);
    CHECK(tight.h.current_midi_entries() == one + c0->data.entries());
    // One more entry is over it, even with no history kept.
    e.add_events = {MidiEvent{kBeat / 2, 0xB0, 65, 127}};
    std::string why;
    CHECK(!tight.h.apply(e, why) && mentions(why, "midi_max_entries"));
    return 0;
}

// What a test of the wire would send: the window in parts, small enough
// that this session needs several.
constexpr std::size_t kSmallPart = 4096;

ReplicationUpdate snapshot_of(const History& h, std::size_t part_bytes = kSmallPart) {
    WindowParts w;
    std::string error;
    if (!encode_window(h, w, error, part_bytes)) std::fprintf(stderr, "  %s\n", error.c_str());
    ReplicationUpdate u;
    u.kind = ReplicationUpdate::Kind::Snapshot;
    u.revision = h.current().revision;
    u.undo_floor = h.floor();
    u.next_id = h.current().next_id;
    u.bytes = std::move(w.window);
    u.parts = std::move(w.parts);
    return u;
}

// The wire's order: header, parts, window.
std::vector<ReplicationUpdate> in_parts(const ReplicationUpdate& whole) {
    std::vector<ReplicationUpdate> out;
    ReplicationUpdate h = whole;
    h.kind = ReplicationUpdate::Kind::SnapshotHeader;
    h.bytes.clear();
    h.parts.clear();
    h.part_count = whole.parts.size();
    for (const std::string& p : whole.parts) h.part_bytes += p.size();
    out.push_back(h);
    for (const std::string& p : whole.parts) {
        ReplicationUpdate u;
        u.kind = ReplicationUpdate::Kind::SnapshotPart;
        u.revision = whole.revision;
        u.bytes = p;
        out.push_back(u);
    }
    ReplicationUpdate w = whole;
    w.parts.clear();
    out.push_back(w);
    return out;
}

// ADR-0015 §4: a chunk 200 undo steps share crosses once, and the table
// travels in parts of whole chunks, each at most the part size.
int test_window_carries_each_chunk_once_in_parts() {
    Session s;
    CHECK(s.build(dense(120)));
    for (std::size_t i = 0; i < 200; ++i) CHECK_APPLY(s.h, drag(s, i, kBeat / 16));
    WindowParts w;
    std::string error;
    CHECK(encode_window(s.h, w, error, kSmallPart));
    // Every chunk of the placed region, and each drag's one new chunk.
    const std::size_t distinct =
        chunk_count(*s.h.step_edit(2)->tracks[0]->midi_playlists[0]->regions[0].data) + 200;
    std::vector<std::shared_ptr<const MidiChunk>> table;
    for (const std::string& p : w.parts) {
        CHECK(p.size() <= kSmallPart);
        CHECK(decode_chunk_part(p, table, error));
    }
    CHECK(w.parts.size() > 10);
    CHECK(table.size() == distinct);
    std::optional<History> back;
    CHECK(decode_window(w, back, error));
    CHECK(back.has_value() && back->current() == s.h.current());
    CHECK(back->step_count() == s.h.step_count());
    // Shared on the replica as on the engine: the floor's chunk 10 is the
    // current state's.
    // Shared on the replica as on the engine: no drag touched chunk 59.
    const auto chunk59 = [](const History& h, std::size_t step) {
        return h.step_edit(step)->tracks[0]->midi_playlists[0]->regions[0].data->chunk_at(59);
    };
    CHECK(chunk59(*back, 2) != nullptr && chunk59(*back, 2) == chunk59(*back, 202));
    CHECK(back->midi_entries() == s.h.midi_entries());
    CHECK(back->charged_bytes() == s.h.charged_bytes());
    return 0;
}

int test_a_bad_part_abandons_the_attach() {
    Session s;
    CHECK(s.build(dense(90)));
    Replica r;
    std::string error;
    const std::vector<ReplicationUpdate> first = in_parts(snapshot_of(s.h));
    CHECK(first.size() > 4);
    for (const ReplicationUpdate& u : first) CHECK(r.receive(u, error));
    CHECK(r.synced() && r.current() == s.h.current());
    const Snapshot held = r.current();

    CHECK_APPLY(s.h, drag(s, 3, kBeat / 8));
    std::vector<ReplicationUpdate> next = in_parts(snapshot_of(s.h));
    // Part 2 cut short by a byte: it no longer decodes on its own.
    next[3].bytes.pop_back();
    std::size_t i = 0;
    CHECK(r.receive(next[i++], error) && r.attaching() && !r.synced());
    CHECK(r.receive(next[i++], error) && r.receive(next[i++], error));
    CHECK(!r.receive(next[i++], error));
    CHECK(mentions(error, "part 2"));
    // The replica still holds what it had, untouched, and drops the rest of
    // this Snapshot without asking again.
    CHECK(!r.synced() && !r.attaching() && r.history() != nullptr);
    CHECK(r.history()->current() == held);
    for (; i < next.size(); ++i) CHECK(r.receive(next[i], error));
    CHECK(!r.synced());
    // The Snapshot asked for after it attaches.
    for (const ReplicationUpdate& u : in_parts(snapshot_of(s.h))) CHECK(r.receive(u, error));
    CHECK(r.synced() && r.current() == s.h.current());
    return 0;
}

int test_part_refusals() {
    Session s;
    CHECK(s.build(dense(30)));
    WindowParts w;
    std::string error;
    CHECK(encode_window(s.h, w, error, kSmallPart));
    CHECK(w.parts.size() >= 2);
    std::vector<std::shared_ptr<const MidiChunk>> table;
    // Out of order: the second part first.
    CHECK(!decode_chunk_part(w.parts[1], table, error) && mentions(error, "starts at chunk"));
    CHECK(table.empty());
    CHECK(!decode_chunk_part("SWCP\x02", table, error) && mentions(error, "version 2"));
    CHECK(!decode_chunk_part(w.parts[0] + "x", table, error));
    CHECK(table.empty());
    // A chunk too large for a part on its own is refused by name.
    CHECK(!encode_window(s.h, w, error, 64) && mentions(error, "a part of the chunk table"));
    // The replica's own cap on what the header announces.
    Replica r;
    r.set_max_snapshot_bytes(1000);
    const std::vector<ReplicationUpdate> parts = in_parts(snapshot_of(s.h));
    CHECK(!r.receive(parts[0], error) && mentions(error, "over this interface's limit of 1000"));
    // A window whose parts did not all arrive.
    Replica r2;
    CHECK(r2.receive(parts[0], error) && r2.receive(parts[1], error));
    CHECK(!r2.receive(parts.back(), error) && mentions(error, "after 1 of its"));
    // An update inside a Snapshot.
    Replica r3;
    CHECK(r3.receive(parts[0], error));
    ReplicationUpdate op;
    op.kind = ReplicationUpdate::Kind::Undo;
    CHECK(!r3.receive(op, error) && mentions(error, "inside a snapshot"));
    return 0;
}

// Lists from the wire are checked as lists: chunking places each note by
// its start, which would hide an order broken across chunks.
int test_an_order_broken_across_chunks_is_refused() {
    Session s;
    CHECK(s.build(dense(4)));
    Ids ids(s.h.current());
    PlaceMidiRegion p{s.playlist,
                      midi_region(ids(), 100 * kBeat, 4 * kBeat,
                                  std::make_shared<const MidiSequence>(MidiSequence())),
                      0};
    std::string bytes, error;
    CHECK(encode_operation(p, bytes, error));
    const std::string empty = "\"notes\": []";
    const std::size_t at = bytes.find(empty);
    CHECK(at != std::string::npos);
    bytes.replace(at, empty.size(),
                  "\"notes\": [[7680000, 1, 0, 60, 100, 64], [0, 1, 0, 61, 100, 64]]");
    Operation op;
    CHECK(!decode_operation(bytes, op, error) && mentions(error, "notes are not in order"));
    return 0;
}

// A Load in parts: the sequence files ahead in parts of whole files, the
// session file last; the engine decodes each part as it arrives.
int test_load_in_parts() {
    History h(new_session(settings()));
    Ids ids(h.current());
    std::string error;
    for (int t = 0; t < 4; ++t) {
        const AddTrack a = midi_track(ids, "K" + std::to_string(t));
        CHECK_APPLY(h, a);
        MidiLists l = dense(20 + t);
        l.notes[0].pitch = static_cast<std::uint8_t>(10 + t);
        CHECK_APPLY(h, (PlaceMidiRegion{a.playlist,
                                        midi_region(ids(), 0, 8 * kBeat,
                                                    std::make_shared<const MidiSequence>(l)),
                                        0}));
    }
    WriteInfo info;
    info.written_by = "test";
    std::string session;
    std::vector<std::string> parts;
    CHECK(encode_load_parts(h.current(), info, session, parts, error, nullptr, 8000));
    CHECK(parts.size() == 4);
    LoadParts in(History::kDefaultMidiMaxEntries);
    for (const std::string& p : parts) {
        CHECK(p.size() <= 8000);
        CHECK(in.add(p, error));
    }
    CHECK(in.files() == 4);
    Document doc(new_session(settings()));
    CHECK(doc.load(session, 0, error, &in));
    Loaded whole;
    LoadError le;
    std::string bundle;
    CHECK(encode_session_bundle(h.current(), info, bundle, error));
    CHECK(decode_session_bundle(bundle, whole, le));
    const auto sequences = [](const Snapshot& s) {
        std::vector<MidiLists> out;
        for (const auto& t : s.edit->tracks) {
            for (const MidiRegion& r : t->midi_playlists[0]->regions) {
                out.push_back(r.data->lists());
            }
        }
        return out;
    };
    CHECK(sequences(doc.current()) == sequences(whole.snapshot));
    // A file too large for a part alone is refused by name.
    CHECK(!encode_load_parts(h.current(), info, session, parts, error, nullptr, 500) &&
          mentions(error, "one part of a Load may carry"));
    // A bad part keeps what the earlier ones brought; one past the budget
    // is refused by name.
    CHECK(encode_load_parts(h.current(), info, session, parts, error, nullptr, 8000));
    LoadParts some(History::kDefaultMidiMaxEntries);
    CHECK(some.add(parts[0], error));
    std::string bad = parts[1];
    bad.pop_back();
    CHECK(!some.add(bad, error) && some.files() == 1);
    LoadParts small(100);
    CHECK(!small.add(parts[0], error) && mentions(error, "midi_max_entries of 100"));
    // A file the parts did not bring opens missing, as with a bundle.
    LoadParts partial(History::kDefaultMidiMaxEntries);
    CHECK(partial.add(parts[0], error));
    Document doc2(new_session(settings()));
    CHECK(doc2.load(session, 0, error, &partial));
    std::size_t missing = 0;
    for (const auto& t : doc2.current().edit->tracks) {
        for (const MidiRegion& r : t->midi_playlists[0]->regions) missing += r.data->missing ? 1 : 0;
    }
    CHECK(missing == 3);
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
    t(test_chunks_are_canonical);
    t(test_edits_reach_the_canonical_chunks);
    t(test_a_drag_copies_only_what_it_touches);
    t(test_random_edits_match_the_flat_lists);
    t(test_budget_counts_shared_chunks_once);
    t(test_window_carries_each_chunk_once_in_parts);
    t(test_a_bad_part_abandons_the_attach);
    t(test_part_refusals);
    t(test_an_order_broken_across_chunks_is_refused);
    t(test_load_in_parts);
    if (failed != 0) {
        std::fprintf(stderr, "model_midi_chunks: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_midi_chunks: %d passed\n", run);
    return 0;
}
