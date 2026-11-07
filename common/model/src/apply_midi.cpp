// SPDX-License-Identifier: GPL-3.0-or-later
//
// apply() for the family MidiOp (ADR-0059):
// MIDI sources, regions, edits and routing.
#include "apply_detail.h"

namespace sonora::model::apply_detail {

bool Applier::operator()(const AddMidiSource& op) {
    const MidiSource& s = op.source;
    const std::string why = detail::midi_source_problem(s);
    if (!why.empty()) return fail("MIDI source " + str(s.id) + ": " + why);
    if (!fresh(s.id)) return false;
    const std::string taken = path_taken(s.path);
    if (!taken.empty()) return fail("MIDI source " + str(s.id) + ": " + taken);
    auto pool = std::make_shared<SourcePool>(*before_.sources);
    pool->midi_sources.push_back(s);
    after_ = before_;
    after_.sources = std::move(pool);
    after_.next_id = s.id + 1;
    return true;
}

bool Applier::operator()(const PlaceMidiRegion& op) {
    MidiRegion region = op.region;
    if (!fresh(region.id)) return false;
    if (op.data_from) {
        const MidiSequence* own = region.data.get();
        if (own != nullptr && !own->empty()) {
            return fail("region " + str(region.id) +
                        ": carries a sequence of its own and data_from; send one or the"
                        " other");
        }
        const MidiDataRef& ref = *op.data_from;
        Located at;
        const MidiRegion* from = nullptr;
        if (locate_midi_playlist(*before_.edit, ref.playlist, at)) {
            const MidiPlaylist& p =
                *before_.edit->tracks[at.track]->midi_playlists[at.playlist];
            const std::size_t i = midi_region_index(p, ref.region);
            if (i != kNone) from = &p.regions[i];
        }
        if (from == nullptr) {
            return fail("data_from: region " + str(ref.region) +
                        " is not on MIDI playlist " + str(ref.playlist));
        }
        // Shared, already checked when it was first placed.
        region.data = from->data;
    } else if (region.data && region.data->missing) {
        // The operation codec carries the lists, not the reference, so a
        // copy of a missing sequence would arrive as an empty one and
        // its file could never be restored to it.
        return fail("region " + str(region.id) +
                    ": its MIDI data is missing; place it with data_from instead");
    }
    std::string why = detail::midi_region_problem(region, *before_.sources);
    if (why.empty() && !op.data_from) {
        std::string where;
        why = detail::sequence_problem(*region.data, where);
        if (!why.empty()) why = where + ": " + why;
    }
    if (!why.empty()) return fail("region " + str(region.id) + ": " + why);
    Id top = region.id;
    const bool ok = edit_midi_playlist(op.playlist, [&](MidiPlaylist& p) {
        bool used = false;
        if (!midi_overwrite(p, region.position, region.end(), op.split_right_id, used)) {
            return false;
        }
        if (used) {
            if (op.split_right_id == region.id) {
                return fail("the split id must differ from the region's id");
            }
            if (!fresh(op.split_right_id)) return false;
            top = std::max(top, op.split_right_id);
        }
        insert_midi_region(p, region);
        return true;
    });
    if (ok) after_.next_id = top + 1;
    return ok;
}

bool Applier::operator()(const EditMidi& op) {
    if (op.remove_notes.empty() && op.add_notes.empty() && op.remove_events.empty() &&
        op.add_events.empty() && op.remove_sysex.empty() && op.add_sysex.empty()) {
        return fail("the edit removes and adds nothing");
    }
    // Each value on its own first, so a bad one is named by where it is
    // in the operation rather than where it would have landed.
    for (std::size_t i = 0; i < op.add_notes.size(); ++i) {
        MidiLists one;
        one.notes.push_back(op.add_notes[i]);
        if (!value_ok(one, "add_notes", i)) return false;
    }
    for (std::size_t i = 0; i < op.add_events.size(); ++i) {
        MidiLists one;
        one.events.push_back(op.add_events[i]);
        if (!value_ok(one, "add_events", i)) return false;
    }
    for (std::size_t i = 0; i < op.add_sysex.size(); ++i) {
        MidiLists one;
        one.sysex.push_back(op.add_sysex[i]);
        if (!value_ok(one, "add_sysex", i)) return false;
    }
    return edit_midi_playlist(op.playlist, [&](MidiPlaylist& p) {
        const std::size_t i = midi_region_index(p, op.region);
        if (i == kNone) return no_region(op.region, op.playlist);
        const MidiSequence& old = *p.regions[i].data;
        if (old.missing) {
            return fail("region " + str(op.region) + ": its MIDI data is missing (" +
                        old.missing->file +
                        "); it cannot be edited until the file is restored or it is"
                        " rebuilt from its take");
        }
        // Only the chunks the edit touches are copied (ADR-0015 §2); every
        // other one, and every leaf none of them is in, stays shared.
        std::map<std::int64_t, Touched> touched;
        if (!removals(old, old.notes(), op.remove_notes, "remove_notes", "note", touched,
                      &Touched::notes) ||
            !removals(old, old.events(), op.remove_events, "remove_events", "event",
                      touched, &Touched::events) ||
            !removals(old, old.sysex(), op.remove_sysex, "remove_sysex", "SysEx message",
                      touched, &Touched::sysex)) {
            return false;
        }
        for (const midi::Note& n : op.add_notes) {
            touched[midi_chunk_of(n.start)].add.notes.push_back(n);
        }
        for (const MidiEvent& e : op.add_events) {
            touched[midi_chunk_of(e.tick)].add.events.push_back(e);
        }
        for (const MidiSysEx& x : op.add_sysex) {
            touched[midi_chunk_of(x.tick)].add.sysex.push_back(x);
        }
        static const MidiLists kNothing;
        std::vector<std::shared_ptr<const MidiChunk>> replace;
        replace.reserve(touched.size());
        for (auto& [k, t] : touched) {
            auto c = std::make_shared<MidiChunk>();
            c->index = k;
            const MidiChunk* before = old.chunk_at(k);
            const MidiLists& was = before != nullptr ? before->data : kNothing;
            // Stable throughout: what was there stays before what is
            // added at the same place, and the additions keep their own
            // order. A chunk is a tick range, so sorting within it is
            // sorting the whole list.
            rebuilt(was.notes, t.notes, t.add.notes, c->data.notes);
            midi::sort_notes(c->data.notes);
            rebuilt(was.events, t.events, t.add.events, c->data.events);
            std::stable_sort(
                c->data.events.begin(), c->data.events.end(),
                [](const MidiEvent& a, const MidiEvent& b) { return a.tick < b.tick; });
            rebuilt(was.sysex, t.sysex, t.add.sysex, c->data.sysex);
            std::stable_sort(
                c->data.sysex.begin(), c->data.sysex.end(),
                [](const MidiSysEx& a, const MidiSysEx& b) { return a.tick < b.tick; });
            replace.push_back(std::move(c));
        }
        p.regions[i].data =
            std::make_shared<const MidiSequence>(MidiSequence::replaced(old, replace));
        return true;
    });
}

bool Applier::operator()(const SetMidiRouting& op) {
    const std::size_t t = locate_track(*before_.edit, op.track);
    if (t == kNone) return fail("track " + str(op.track) + " does not exist");
    const TrackEdit& track = *before_.edit->tracks[t];
    if (track.kind == TrackKind::Audio) {
        return fail("track " + str(op.track) + " is an audio track; it has no MIDI routing");
    }
    std::string where;
    const std::string why = detail::midi_routing_problem(op.routing, track.kind, where);
    if (!why.empty()) return fail("routing." + where + ": " + why);
    const TrackMix* existing = before_.mix.find(op.track);
    TrackMix m = existing != nullptr ? *existing : TrackMix{};
    m.track = op.track;
    m.midi = op.routing;
    after_ = before_;
    set_mix(after_.mix, m);
    return true;
}

}  // namespace sonora::model::apply_detail
