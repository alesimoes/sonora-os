// SPDX-License-Identifier: GPL-3.0-or-later
//
// The undo window, written (op_codec.h, encode_window).
#include "op_codec_detail.h"

namespace sonora::model {

namespace codec {
namespace {

// Each table entry is written once and referred to by index, so a track or
// playlist that several steps share crosses once and is shared again on
// the replica, as it is on the engine.
template <typename T>
class Table {
public:
    // The index of `p`, and whether it is new to the table.
    std::size_t index(const T* p, bool& added) {
        const auto it = seen_.find(p);
        if (it != seen_.end()) {
            added = false;
            return it->second;
        }
        const std::size_t ix = seen_.size();
        seen_.emplace(p, ix);
        added = true;
        return ix;
    }

private:
    std::unordered_map<const T*, std::size_t> seen_;
};

Value index_value(std::size_t ix) { return Value::integer(static_cast<std::int64_t>(ix)); }

Value playlist_value(const Playlist& p) {
    Value v = Value::object();
    v.set("id", id_value(p.id));
    v.set("name", Value::string(p.name));
    Value& rs = v.set("regions", Value::array());
    for (const Region& r : p.regions) rs.push(region_value(r));
    Value& xs = v.set("crossfades", Value::array());
    for (const Crossfade& x : p.crossfades) xs.push(crossfade_value(x));
    if (p.freeze) v.set("freeze", freeze_value(*p.freeze));
    return v;
}

class WindowWriter {
public:
    std::size_t edit(const EditState& e) {
        bool added = false;
        const std::size_t ix = edit_ix_.index(&e, added);
        if (!added) return ix;
        Value ev = Value::object();
        Value& ts = ev.set("tracks", Value::array());
        for (const auto& t : e.tracks) ts.push(index_value(track(*t)));
        if (e.markers) {
            ev.set("markers", index_value(markers(*e.markers)));
        } else {
            ev.set("markers", Value::null());
        }
        Value& bs = ev.set("buses", Value::array());
        for (const Bus& b : e.buses) {
            Value bv = aux_bus_value(b);
            bv.set("automation", lane_refs(b.automation));
            bs.push(std::move(bv));
        }
        ev.set("master_automation", lane_refs(e.master_automation));
        Value& gs = ev.set("groups", Value::array());
        for (const Group& g : e.groups) gs.push(group_value(g));
        Value& vs = ev.set("vcas", Value::array());
        for (const Vca& v : e.vcas) vs.push(named_set_value(v.id, v.name, v.members));
        Value& fs = ev.set("folders", Value::array());
        for (const Folder& f : e.folders) fs.push(named_set_value(f.id, f.name, f.members));
        edits_.push(std::move(ev));
        return ix;
    }

    Value playlists_ = Value::array();
    Value leaves_ = Value::array();
    Value sequences_ = Value::array();
    // The chunk table, in first-reference order: what the parts carry.
    std::vector<const MidiChunk*> chunks_;
    Value midi_playlists_ = Value::array();
    Value lanes_ = Value::array();
    Value tracks_ = Value::array();
    Value markers_ = Value::array();
    Value edits_ = Value::array();

private:
    // Each lane once, however many steps and strips share it.
    Value lane_refs(const AutomationLanes& lanes) {
        Value v = Value::array();
        for (const auto& l : lanes) {
            bool added = false;
            const std::size_t ix = lane_ix_.index(l.get(), added);
            if (added) lanes_.push(lane_value(*l));
            v.push(index_value(ix));
        }
        return v;
    }
    std::size_t track(const TrackEdit& t) {
        bool added = false;
        const std::size_t ix = track_ix_.index(&t, added);
        if (!added) return ix;
        Value tv = Value::object();
        tv.set("id", id_value(t.id));
        tv.set("kind", name_of(kTrackKinds, t.kind));
        tv.set("name", Value::string(t.name));
        tv.set("colour", integer(t.colour));
        tv.set("channels", integer(t.channels));
        tv.set("active_playlist", id_value(t.active_playlist));
        Value& ins = tv.set("inserts", Value::array());
        for (const Insert& in : t.inserts) ins.push(insert_value(in));
        Value& sends = tv.set("sends", Value::array());
        for (const Send& sd : t.sends) sends.push(send_value(sd));
        tv.set("output", id_value(t.output));
        tv.set("automation", lane_refs(t.automation));
        Value& ps = tv.set("playlists", Value::array());
        for (const auto& p : t.playlists) ps.push(index_value(playlist(*p)));
        Value& ms = tv.set("midi_playlists", Value::array());
        for (const auto& p : t.midi_playlists) ms.push(index_value(midi_playlist(*p)));
        tracks_.push(std::move(tv));
        return ix;
    }
    std::size_t midi_playlist(const MidiPlaylist& p) {
        bool added = false;
        const std::size_t ix = midi_ix_.index(&p, added);
        if (!added) return ix;
        Value v = Value::object();
        v.set("id", id_value(p.id));
        v.set("name", Value::string(p.name));
        Value& rs = v.set("regions", Value::array());
        for (const MidiRegion& r : p.regions) {
            Value rv = midi_region_fields(r);
            rv.set("sequence", index_value(sequence(*r.data)));
            rs.push(std::move(rv));
        }
        midi_playlists_.push(std::move(v));
        return ix;
    }
    std::size_t chunk(const MidiChunk& c) {
        bool added = false;
        const std::size_t ix = chunk_ix_.index(&c, added);
        if (added) chunks_.push_back(&c);
        return ix;
    }
    std::size_t leaf(const MidiSpineLeaf& l) {
        bool added = false;
        const std::size_t ix = leaf_ix_.index(&l, added);
        if (!added) return ix;
        Value v = Value::array();
        for (const MidiSpineEntry& e : l.chunks) v.push(index_value(chunk(*e.chunk)));
        leaves_.push(std::move(v));
        return ix;
    }
    std::size_t sequence(const MidiSequence& d) {
        bool added = false;
        const std::size_t ix = sequence_ix_.index(&d, added);
        if (!added) return ix;
        Value v = Value::object();
        Value& ls = v.set("leaves", Value::array());
        for (const MidiSpineNode& n : d.spine()) ls.push(index_value(leaf(*n.leaf)));
        if (d.missing) {
            const SequenceFileRef& r = *d.missing;
            Value& m = v.set("missing", Value::object());
            m.set("file", Value::string(r.file));
            m.set("sha256", Value::string(r.sha256));
            m.set("bytes", integer(static_cast<std::int64_t>(r.bytes)));
            m.set("notes", integer(static_cast<std::int64_t>(r.notes)));
            m.set("events", integer(static_cast<std::int64_t>(r.events)));
            m.set("sysex", integer(static_cast<std::int64_t>(r.sysex)));
        }
        sequences_.push(std::move(v));
        return ix;
    }
    std::size_t playlist(const Playlist& p) {
        bool added = false;
        const std::size_t ix = playlist_ix_.index(&p, added);
        if (added) playlists_.push(playlist_value(p));
        return ix;
    }
    std::size_t markers(const MarkerList& m) {
        bool added = false;
        const std::size_t ix = marker_ix_.index(&m, added);
        if (added) {
            Value& ml = markers_.push(Value::array());
            for (const Marker& k : m.markers) ml.push(marker_value(k));
        }
        return ix;
    }

    Table<Playlist> playlist_ix_;
    Table<MidiChunk> chunk_ix_;
    Table<MidiSpineLeaf> leaf_ix_;
    Table<MidiSequence> sequence_ix_;
    Table<MidiPlaylist> midi_ix_;
    Table<TrackEdit> track_ix_;
    Table<AutomationLane> lane_ix_;
    Table<MarkerList> marker_ix_;
    Table<EditState> edit_ix_;
};

bool window_value(const History& h, WindowWriter& w, Value& root, std::string& error) {
    const Snapshot& cur = h.current();
    if (cur.edit != h.step_edit(h.undo_depth())) {
        error = "the current state is not the one at the undo pointer";
        return false;
    }
    Value steps = Value::array();
    for (std::size_t i = 0; i < h.step_count(); ++i) {
        Value sv = Value::object();
        sv.set("revision", integer(static_cast<std::int64_t>(h.step_revision(i))));
        sv.set("edit", index_value(w.edit(*h.step_edit(i))));
        sv.set("label", Value::string(h.step_label(i)));
        steps.push(std::move(sv));
    }

    root = Value::object();
    root.set("window_format", integer(kWindowFormat));
    root.set("revision", integer(static_cast<std::int64_t>(cur.revision)));
    root.set("next_id", id_value(cur.next_id));
    root.set("position", index_value(h.undo_depth()));
    root.set("settings", settings_value(cur.settings));
    root.set("bus", bus_value(cur.mix.bus));
    Value& mix = root.set("mix", Value::array());
    for (const TrackMix& m : cur.mix.tracks) mix.push(mix_value(m));
    Value& insert_mix = root.set("insert_mix", Value::array());
    for (const InsertMix& m : cur.mix.inserts) insert_mix.push(insert_mix_value(m));
    Value& bus_mix = root.set("bus_mix", Value::array());
    for (const BusMix& m : cur.mix.buses) bus_mix.push(bus_mix_value(m));
    Value& vca_mix = root.set("vca_mix", Value::array());
    for (const VcaMix& m : cur.mix.vcas) vca_mix.push(vca_mix_value(m));
    Value& send_mix = root.set("send_mix", Value::array());
    for (const SendMix& m : cur.mix.sends) send_mix.push(send_mix_value(m));
    Value& sources = root.set("sources", Value::array());
    if (cur.sources) {
        for (const Source& src : cur.sources->sources) sources.push(source_value(src));
    }
    Value& midi_sources = root.set("midi_sources", Value::array());
    if (cur.sources) {
        for (const MidiSource& src : cur.sources->midi_sources) {
            midi_sources.push(midi_source_value(src));
        }
    }
    root.set("playlists", std::move(w.playlists_));
    root.set("chunks", index_value(w.chunks_.size()));
    root.set("leaves", std::move(w.leaves_));
    root.set("sequences", std::move(w.sequences_));
    root.set("midi_playlists", std::move(w.midi_playlists_));
    root.set("lanes", std::move(w.lanes_));
    root.set("tracks", std::move(w.tracks_));
    root.set("markers", std::move(w.markers_));
    root.set("edits", std::move(w.edits_));
    root.set("steps", std::move(steps));
    return true;
}

}  // namespace
}  // namespace codec

using namespace codec;

bool encode_window(const History& history, WindowParts& out, std::string& error,
                   std::size_t max_part_bytes) {
    WindowWriter w;
    Value root;
    if (!window_value(history, w, root, error)) return false;
    WindowParts parts;
    if (!encode_chunk_parts(w.chunks_, max_part_bytes, parts.parts, error)) return false;
    if (!json::write(root, parts.window, error)) return false;
    out = std::move(parts);
    return true;
}

bool encode_window_alone(const History& history, std::string& window, std::string& error) {
    WindowWriter w;
    Value root;
    return window_value(history, w, root, error) && json::write(root, window, error);
}

std::uint64_t WindowParts::part_bytes() const {
    std::uint64_t n = 0;
    for (const std::string& p : parts) n += p.size();
    return n;
}

}  // namespace sonora::model
