// SPDX-License-Identifier: GPL-3.0-or-later
//
// The undo window, read (op_codec.h, WindowDecoder).
#include "op_codec_detail.h"

namespace sonora::model {

namespace codec {
namespace {

// An index into a table already read: only backwards, so nothing refers
// to itself or to what is not there yet.
bool table_index(Reader& rd, const Value& e, const std::string& p, std::size_t size,
                 std::size_t& out) {
    std::int64_t n = 0;
    if (!e.is_number() || !e.is_integer_lexeme() || !e.to_int64(n) || n < 0 ||
        static_cast<std::uint64_t>(n) >= size) {
        return rd.fail(p, "must be an index below " + std::to_string(size));
    }
    out = static_cast<std::size_t>(n);
    return true;
}

bool read_lane_table(Reader& rd, const Value& root,
                     std::vector<std::shared_ptr<const AutomationLane>>& out) {
    const Value* list = nullptr;
    if (!rd.array(root, "", "lanes", list)) return false;
    for (const Value& e : list->items()) {
        auto lane = std::make_shared<AutomationLane>();
        if (!read_lane(rd, e, "lanes", *lane)) return false;
        out.push_back(std::move(lane));
    }
    return true;
}

// `obj`'s `key`: indices into the lane table.
bool read_lane_refs(Reader& rd, const Value& obj, const std::string& p, const char* key,
                    const std::vector<std::shared_ptr<const AutomationLane>>& table,
                    AutomationLanes& out) {
    const Value* list = nullptr;
    if (!rd.array(obj, p, key, list)) return false;
    const std::string lp = Reader::at(p, key);
    for (const Value& i : list->items()) {
        std::size_t ix = 0;
        if (!table_index(rd, i, lp, table.size(), ix)) return false;
        out.push_back(table[ix]);
    }
    return true;
}

bool read_playlists(Reader& rd, const Value& root,
                    std::vector<std::shared_ptr<const Playlist>>& out) {
    const Value* list = nullptr;
    if (!rd.array(root, "", "playlists", list)) return false;
    for (const Value& e : list->items()) {
        auto p = std::make_shared<Playlist>();
        const Value* rs = nullptr;
        const Value* xs = nullptr;
        const bool frozen = e.is_object() && e.find("freeze") != nullptr;
        if (!(frozen ? rd.keys(e, "playlists", {"id", "name", "regions", "crossfades", "freeze"})
                     : rd.keys(e, "playlists", {"id", "name", "regions", "crossfades"})) ||
            !rd.id(e, "playlists", "id", p->id) || !rd.text(e, "playlists", "name", p->name) ||
            !rd.array(e, "playlists", "regions", rs) ||
            !rd.array(e, "playlists", "crossfades", xs) ||
            (frozen && !read_freeze(rd, e, "playlists", p->freeze))) {
            return false;
        }
        for (const Value& r : rs->items()) {
            Region region;
            if (!read_region_value(rd, r, "playlists.regions", region)) return false;
            p->regions.push_back(std::move(region));
        }
        for (const Value& x : xs->items()) {
            Crossfade c;
            if (!read_crossfade(rd, x, "playlists.crossfades", c)) return false;
            p->crossfades.push_back(c);
        }
        out.push_back(std::move(p));
    }
    return true;
}

bool read_leaves(Reader& rd, const Value& root,
                 const std::vector<std::shared_ptr<const MidiChunk>>& chunks,
                 std::vector<std::shared_ptr<const MidiSpineLeaf>>& out) {
    const Value* list = nullptr;
    if (!rd.array(root, "", "leaves", list)) return false;
    for (const Value& e : list->items()) {
        if (!e.is_array()) return rd.fail("leaves", "must hold arrays of chunk indices");
        std::vector<std::shared_ptr<const MidiChunk>> cs;
        cs.reserve(e.items().size());
        for (const Value& i : e.items()) {
            std::size_t ix = 0;
            if (!table_index(rd, i, "leaves", chunks.size(), ix)) return false;
            cs.push_back(chunks[ix]);
        }
        std::shared_ptr<const MidiSpineLeaf> leaf;
        std::string why;
        if (!MidiSequence::make_leaf(std::move(cs), leaf, why)) return rd.fail("leaves", why);
        out.push_back(std::move(leaf));
    }
    return true;
}

bool read_sequences(Reader& rd, const Value& root,
                    const std::vector<std::shared_ptr<const MidiSpineLeaf>>& leaves,
                    std::vector<std::shared_ptr<const MidiSequence>>& out) {
    const Value* list = nullptr;
    if (!rd.array(root, "", "sequences", list)) return false;
    for (const Value& e : list->items()) {
        const bool missing = e.is_object() && e.find("missing") != nullptr;
        const Value* ls = nullptr;
        if (!rd.keys(e, "sequences",
                     missing ? std::initializer_list<const char*>{"leaves", "missing"}
                             : std::initializer_list<const char*>{"leaves"}) ||
            !rd.array(e, "sequences", "leaves", ls)) {
            return false;
        }
        std::vector<std::shared_ptr<const MidiSpineLeaf>> spine;
        spine.reserve(ls->items().size());
        for (const Value& i : ls->items()) {
            std::size_t ix = 0;
            if (!table_index(rd, i, "sequences.leaves", leaves.size(), ix)) return false;
            spine.push_back(leaves[ix]);
        }
        MidiSequence d;
        std::string why;
        if (!MidiSequence::from_leaves(std::move(spine), d, why)) {
            return rd.fail("sequences.leaves", why);
        }
        if (missing) {
            const Value& m = *e.find("missing");
            const std::string mp = "sequences.missing";
            SequenceFileRef r;
            std::int64_t bytes = 0, notes = 0, events = 0, sysex = 0;
            if (!rd.keys(m, mp, {"file", "sha256", "bytes", "notes", "events", "sysex"}) ||
                !rd.text(m, mp, "file", r.file) || !rd.text(m, mp, "sha256", r.sha256) ||
                !rd.integer(m, mp, "bytes", kMaxInteger, bytes) ||
                !rd.integer(m, mp, "notes", kMaxInteger, notes) ||
                !rd.integer(m, mp, "events", kMaxInteger, events) ||
                !rd.integer(m, mp, "sysex", kMaxInteger, sysex)) {
                return false;
            }
            r.bytes = static_cast<std::uint64_t>(bytes);
            r.notes = static_cast<std::uint64_t>(notes);
            r.events = static_cast<std::uint64_t>(events);
            r.sysex = static_cast<std::uint64_t>(sysex);
            d.missing = std::move(r);
            // Its reference, here: the invariant check of the current state
            // below does not see the sequences only older steps hold. The
            // chunks were checked as their parts arrived.
            std::string where;
            const std::string problem = detail::sequence_problem(d, where);
            if (!problem.empty()) return rd.fail("sequences." + where, problem);
        }
        out.push_back(std::make_shared<const MidiSequence>(std::move(d)));
    }
    return true;
}

bool read_midi_playlists(Reader& rd, const Value& root,
                         const std::vector<std::shared_ptr<const MidiSequence>>& sequences,
                         std::vector<std::shared_ptr<const MidiPlaylist>>& out) {
    const Value* list = nullptr;
    if (!rd.array(root, "", "midi_playlists", list)) return false;
    for (const Value& e : list->items()) {
        auto p = std::make_shared<MidiPlaylist>();
        const Value* rs = nullptr;
        if (!rd.keys(e, "midi_playlists", {"id", "name", "regions"}) ||
            !rd.id(e, "midi_playlists", "id", p->id) ||
            !rd.text(e, "midi_playlists", "name", p->name) ||
            !rd.array(e, "midi_playlists", "regions", rs)) {
            return false;
        }
        for (const Value& r : rs->items()) {
            const std::string rp = "midi_playlists.regions";
            MidiRegion region;
            std::size_t ix = 0;
            if (!rd.keys(r, rp, {"id", "name", "colour", "position_ticks", "length_ticks",
                                 "offset_ticks", "mute", "source", "sequence"}) ||
                !read_midi_region_fields(rd, r, rp, region) ||
                !table_index(rd, *r.find("sequence"), rp + ".sequence", sequences.size(), ix)) {
                return false;
            }
            region.data = sequences[ix];
            p->regions.push_back(std::move(region));
        }
        out.push_back(std::move(p));
    }
    return true;
}

bool read_tracks(Reader& rd, const Value& root,
                 const std::vector<std::shared_ptr<const Playlist>>& playlists,
                 const std::vector<std::shared_ptr<const MidiPlaylist>>& midi_playlists,
                 const std::vector<std::shared_ptr<const AutomationLane>>& lanes,
                 std::vector<std::shared_ptr<const TrackEdit>>& out) {
    const Value* list = nullptr;
    if (!rd.array(root, "", "tracks", list)) return false;
    for (const Value& e : list->items()) {
        auto t = std::make_shared<TrackEdit>();
        const Value* ps = nullptr;
        const Value* ms = nullptr;
        const Value* ins = nullptr;
        const Value* sends = nullptr;
        if (!rd.keys(e, "tracks",
                     {"id", "kind", "name", "colour", "channels", "active_playlist", "inserts",
                      "sends", "output", "automation", "playlists", "midi_playlists"}) ||
            !rd.id(e, "tracks", "id", t->id) || !rd.id(e, "tracks", "output", t->output) ||
            !rd.choice(e, "tracks", "kind", kTrackKinds, t->kind) ||
            !rd.text(e, "tracks", "name", t->name) ||
            !read_colour(rd, e, "tracks", false, t->colour) ||
            !rd.small(e, "tracks", "channels", t->channels) ||
            !rd.id(e, "tracks", "active_playlist", t->active_playlist) ||
            !rd.array(e, "tracks", "inserts", ins) || !rd.array(e, "tracks", "sends", sends) ||
            !rd.array(e, "tracks", "playlists", ps) ||
            !rd.array(e, "tracks", "midi_playlists", ms) ||
            !read_lane_refs(rd, e, "tracks", "automation", lanes, t->automation)) {
            return false;
        }
        for (const Value& i : ms->items()) {
            std::size_t ix = 0;
            if (!table_index(rd, i, "tracks.midi_playlists", midi_playlists.size(), ix)) {
                return false;
            }
            t->midi_playlists.push_back(midi_playlists[ix]);
        }
        for (const Value& i : ins->items()) {
            Insert in;
            if (!read_insert_value(rd, i, "tracks.inserts", in)) return false;
            t->inserts.push_back(std::move(in));
        }
        for (const Value& i : sends->items()) {
            Send sd;
            if (!read_send_value(rd, i, "tracks.sends", sd)) return false;
            t->sends.push_back(sd);
        }
        for (const Value& i : ps->items()) {
            std::size_t ix = 0;
            if (!table_index(rd, i, "tracks.playlists", playlists.size(), ix)) return false;
            t->playlists.push_back(playlists[ix]);
        }
        out.push_back(std::move(t));
    }
    return true;
}

bool read_marker_lists(Reader& rd, const Value& root,
                       std::vector<std::shared_ptr<const MarkerList>>& out) {
    const Value* list = nullptr;
    if (!rd.array(root, "", "markers", list)) return false;
    for (const Value& e : list->items()) {
        if (!e.is_array()) return rd.fail("markers", "must hold arrays of markers");
        auto ml = std::make_shared<MarkerList>();
        for (const Value& m : e.items()) {
            Marker marker;
            if (!read_marker_value(rd, m, "markers", marker)) return false;
            ml->markers.push_back(std::move(marker));
        }
        out.push_back(std::move(ml));
    }
    return true;
}

bool read_edits(Reader& rd, const Value& root,
                const std::vector<std::shared_ptr<const TrackEdit>>& tracks,
                const std::vector<std::shared_ptr<const MarkerList>>& markers,
                const std::vector<std::shared_ptr<const AutomationLane>>& lanes,
                std::vector<std::shared_ptr<const EditState>>& out) {
    const Value* list = nullptr;
    if (!rd.array(root, "", "edits", list)) return false;
    for (const Value& e : list->items()) {
        auto es = std::make_shared<EditState>();
        const Value* ts = nullptr;
        const Value* bs = nullptr;
        const Value* gs = nullptr;
        const Value* vs = nullptr;
        const Value* fs = nullptr;
        if (!rd.keys(e, "edits", {"tracks", "markers", "buses", "master_automation", "groups",
                                  "vcas", "folders"}) ||
            !rd.array(e, "edits", "tracks", ts) || !rd.array(e, "edits", "buses", bs) ||
            !rd.array(e, "edits", "groups", gs) || !rd.array(e, "edits", "vcas", vs) ||
            !rd.array(e, "edits", "folders", fs) ||
            !read_lane_refs(rd, e, "edits", "master_automation", lanes, es->master_automation)) {
            return false;
        }
        for (const Value& g : gs->items()) {
            Group grp;
            if (!read_group_value(rd, g, "edits.groups", false, grp)) return false;
            es->groups.push_back(std::move(grp));
        }
        for (const Value& x : vs->items()) {
            Vca vca;
            if (!read_named_set(rd, x, "edits.vcas", false, vca.id, vca.name, vca.members)) {
                return false;
            }
            es->vcas.push_back(std::move(vca));
        }
        for (const Value& x : fs->items()) {
            Folder folder;
            if (!read_named_set(rd, x, "edits.folders", false, folder.id, folder.name,
                                folder.members)) {
                return false;
            }
            es->folders.push_back(std::move(folder));
        }
        for (const Value& b : bs->items()) {
            Bus bus;
            if (!read_aux_bus_value(rd, b, "edits.buses", bus) ||
                !read_lane_refs(rd, b, "edits.buses", "automation", lanes, bus.automation)) {
                return false;
            }
            es->buses.push_back(std::move(bus));
        }
        for (const Value& i : ts->items()) {
            std::size_t ix = 0;
            if (!table_index(rd, i, "edits.tracks", tracks.size(), ix)) return false;
            es->tracks.push_back(tracks[ix]);
        }
        const Value& mv = *e.find("markers");
        if (!mv.is_null()) {
            std::size_t ix = 0;
            if (!table_index(rd, mv, "edits.markers", markers.size(), ix)) return false;
            es->markers = markers[ix];
        }
        out.push_back(std::move(es));
    }
    return true;
}

bool read_window(Reader& rd, const Value& root,
                 const std::vector<std::shared_ptr<const MidiChunk>>& chunks,
                 std::optional<History>& out) {
    // The format first: another version's keys are not this one's to judge.
    if (!root.is_object() || root.find("window_format") == nullptr) {
        return rd.fail("", "missing required key \"window_format\"");
    }
    std::int64_t format = 0;
    if (!rd.integer(root, "", "window_format", kMaxInteger, format)) return false;
    if (format != kWindowFormat) {
        return rd.fail("window_format", "is " + std::to_string(format) +
                                            " where this version reads " +
                                            std::to_string(kWindowFormat));
    }
    if (!rd.keys(root, "", {"window_format", "revision", "next_id", "position", "settings",
                            "bus", "mix", "insert_mix", "bus_mix", "vca_mix", "send_mix", "sources",
                            "midi_sources", "playlists",
                            "chunks", "leaves", "sequences", "midi_playlists", "lanes",
                            "tracks", "markers", "edits", "steps"})) {
        return false;
    }
    std::int64_t chunk_count = 0;
    if (!rd.integer(root, "", "chunks", kMaxInteger, chunk_count)) return false;
    if (static_cast<std::uint64_t>(chunk_count) != chunks.size()) {
        return rd.fail("chunks", "is " + std::to_string(chunk_count) + " and the parts brought " +
                                     std::to_string(chunks.size()));
    }
    Snapshot cur;
    std::int64_t revision = 0, position = 0;
    if (!rd.integer(root, "", "revision", kMaxInteger, revision) ||
        !rd.id(root, "", "next_id", cur.next_id) ||
        !rd.integer(root, "", "position", kMaxInteger, position) ||
        !read_settings(rd, root, cur.settings) || !read_bus(rd, root, cur.mix.bus)) {
        return false;
    }
    cur.revision = static_cast<std::uint64_t>(revision);

    const Value* list = nullptr;
    if (!rd.array(root, "", "mix", list)) return false;
    for (const Value& e : list->items()) {
        TrackMix m;
        if (!read_mix_value(rd, e, "mix", m)) return false;
        cur.mix.tracks.push_back(std::move(m));
    }
    if (!rd.array(root, "", "insert_mix", list)) return false;
    for (const Value& e : list->items()) {
        InsertMix m;
        if (!read_insert_mix_value(rd, e, "insert_mix", m)) return false;
        cur.mix.inserts.push_back(std::move(m));
    }
    if (!rd.array(root, "", "bus_mix", list)) return false;
    for (const Value& e : list->items()) {
        BusMix m;
        if (!read_bus_mix_value(rd, e, "bus_mix", m)) return false;
        cur.mix.buses.push_back(m);
    }
    if (!rd.array(root, "", "vca_mix", list)) return false;
    for (const Value& e : list->items()) {
        VcaMix m;
        if (!read_vca_mix_value(rd, e, "vca_mix", m)) return false;
        cur.mix.vcas.push_back(m);
    }
    if (!rd.array(root, "", "send_mix", list)) return false;
    for (const Value& e : list->items()) {
        SendMix m;
        if (!read_send_mix_value(rd, e, "send_mix", m)) return false;
        cur.mix.sends.push_back(m);
    }
    auto pool = std::make_shared<SourcePool>();
    if (!rd.array(root, "", "sources", list)) return false;
    for (const Value& e : list->items()) {
        Source src;
        if (!read_source_value(rd, e, "sources", src)) return false;
        pool->sources.push_back(std::move(src));
    }
    if (!rd.array(root, "", "midi_sources", list)) return false;
    for (const Value& e : list->items()) {
        MidiSource src;
        if (!read_midi_source_value(rd, e, "midi_sources", src)) return false;
        pool->midi_sources.push_back(std::move(src));
    }
    cur.sources = std::move(pool);

    std::vector<std::shared_ptr<const Playlist>> playlists;
    std::vector<std::shared_ptr<const MidiSpineLeaf>> leaves;
    std::vector<std::shared_ptr<const MidiSequence>> sequences;
    std::vector<std::shared_ptr<const MidiPlaylist>> midi_playlists;
    std::vector<std::shared_ptr<const AutomationLane>> lanes;
    std::vector<std::shared_ptr<const TrackEdit>> tracks;
    std::vector<std::shared_ptr<const MarkerList>> markers;
    std::vector<std::shared_ptr<const EditState>> edits;
    if (!read_playlists(rd, root, playlists) || !read_leaves(rd, root, chunks, leaves) ||
        !read_sequences(rd, root, leaves, sequences) ||
        !read_midi_playlists(rd, root, sequences, midi_playlists) ||
        !read_lane_table(rd, root, lanes) ||
        !read_tracks(rd, root, playlists, midi_playlists, lanes, tracks) ||
        !read_marker_lists(rd, root, markers) ||
        !read_edits(rd, root, tracks, markers, lanes, edits)) {
        return false;
    }

    std::vector<History::WindowStep> steps;
    if (!rd.array(root, "", "steps", list)) return false;
    for (const Value& e : list->items()) {
        History::WindowStep st;
        std::int64_t rev = 0;
        std::size_t ix = 0;
        if (!rd.keys(e, "steps", {"revision", "edit", "label"}) ||
            !rd.integer(e, "steps", "revision", kMaxInteger, rev) ||
            !rd.text(e, "steps", "label", st.label) ||
            !table_index(rd, *e.find("edit"), "steps.edit", edits.size(), ix)) {
            return false;
        }
        st.revision = static_cast<std::uint64_t>(rev);
        st.edit = edits[ix];
        steps.push_back(std::move(st));
    }
    if (static_cast<std::uint64_t>(position) >= steps.size()) {
        return rd.fail("position", "is past the last undo step");
    }
    cur.edit = steps[static_cast<std::size_t>(position)].edit;

    std::string why;
    if (!check_invariants(cur, why)) return rd.fail("", "the current state: " + why);
    if (!History::restore(std::move(cur), std::move(steps), static_cast<std::size_t>(position),
                          out, why)) {
        return rd.fail("steps", why);
    }
    return true;
}

}  // namespace
}  // namespace codec

using namespace codec;

bool WindowDecoder::begin(std::uint64_t parts, std::uint64_t bytes, std::uint64_t max_bytes,
                          std::string& error) {
    *this = WindowDecoder();
    if (bytes > max_bytes) {
        error = "the engine's undo window has " + std::to_string(bytes) +
                " bytes of MIDI chunks, over this interface's limit of " +
                std::to_string(max_bytes) + "; raise the limit to attach";
        return false;
    }
    if (parts > bytes) {
        error = "the engine announced " + std::to_string(parts) + " parts in " +
                std::to_string(bytes) + " bytes";
        return false;
    }
    parts_ = parts;
    bytes_ = bytes;
    started_ = true;
    return true;
}

bool WindowDecoder::part(std::string_view bytes, std::string& error) {
    if (!started_ || got_parts_ == parts_) {
        error = "a part of the undo window that was not announced";
        return false;
    }
    if (bytes.size() > kMaxChunkPartBytes || bytes.size() > bytes_ - got_bytes_) {
        error = "part " + std::to_string(got_parts_) + " is " + std::to_string(bytes.size()) +
                " bytes, more than was announced";
        return false;
    }
    std::string why;
    if (!decode_chunk_part(bytes, table_, why)) {
        error = "undo window part " + std::to_string(got_parts_) + ": " + why;
        return false;
    }
    ++got_parts_;
    got_bytes_ += bytes.size();
    return true;
}

bool WindowDecoder::finish(std::string_view window, std::optional<History>& out,
                           std::string& error) {
    if (started_ && (got_parts_ != parts_ || got_bytes_ != bytes_)) {
        error = "the undo window arrived after " + std::to_string(got_parts_) + " of its " +
                std::to_string(parts_) + " parts";
        return false;
    }
    // The engine's bytes, already held to the wire's 64 MiB cap. A window
    // has more, smaller values per byte than a session file, so the value
    // and DOM caps are raised to what 64 MiB of it can hold.
    json::Limits limits;
    limits.max_values = std::size_t{16} << 20;
    limits.max_dom_bytes = std::size_t{1} << 30;
    Value root;
    json::Error perr;
    if (!json::parse(window, root, perr, limits)) {
        error = "undo window: line " + std::to_string(perr.line) + ", column " +
                std::to_string(perr.column) + ": " + perr.message;
        return false;
    }
    Reader rd(error);
    std::optional<History> decoded;
    if (!read_window(rd, root, table_, decoded)) {
        error = "undo window: " + error;
        return false;
    }
    table_.clear();
    out = std::move(decoded);
    return true;
}

bool decode_window(const WindowParts& in, std::optional<History>& out, std::string& error) {
    WindowDecoder d;
    if (!d.begin(in.parts.size(), in.part_bytes(), UINT64_MAX, error)) return false;
    for (const std::string& p : in.parts) {
        if (!d.part(p, error)) return false;
    }
    return d.finish(in.window, out, error);
}

}  // namespace sonora::model
