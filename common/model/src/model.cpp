// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "checks.h"
#include "sonora/json/json.h"
#include "sonora/model/musical_map.h"

namespace sonora::model {

namespace {

std::string str(std::int64_t n) { return std::to_string(n); }
std::string str(std::uint64_t n) { return std::to_string(n); }

bool in_range(std::int64_t n) { return n >= 0 && n <= kMaxInteger; }

bool lower_hex(const std::string& s, std::size_t n) {
    if (s.size() != n) return false;
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// Written so that NaN fails it.
bool gain_in_range(float db) { return db >= kMinGainDb && db <= kMaxGainDb; }

std::string gain_range() {
    return "must be from " + str(static_cast<std::int64_t>(kMinGainDb)) + " to " +
           str(static_cast<std::int64_t>(kMaxGainDb)) + " dB";
}

// A crossfade or a region needs this much of every source it names; the
// shortest one decides. -1 when a source is missing.
Frames shortest_source(const Region& r, const SourcePool& pool) {
    Frames shortest = kMaxInteger;
    for (const SourceRef& c : r.channels) {
        const Source* s = pool.find(c.source);
        if (s == nullptr) return -1;
        shortest = std::min(shortest, s->length);
    }
    return shortest;
}

template <typename T>
bool same_ptr_or_value(const std::shared_ptr<const T>& a,
                       const std::shared_ptr<const T>& b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return *a == *b;
}

}  // namespace

void crossfade_extent(const Crossfade& x, Frames& before, Frames& after) {
    switch (x.align) {
        case Align::Pre:
            before = x.length;
            after = 0;
            return;
        case Align::Post:
            before = 0;
            after = x.length;
            return;
        case Align::Centre:
            break;
    }
    before = x.length / 2;
    after = x.length - before;
}

void crossfade_handles(const Region& left, const Region& right, const SourcePool& pool,
                       Frames& before, Frames& after) {
    before = std::max<Frames>(right.source_offset, 0);
    const Frames shortest = shortest_source(left, pool);
    after = shortest < 0 ? 0 : std::max<Frames>(shortest - left.source_offset - left.length, 0);
}

bool fit_crossfade(Crossfade& x, Frames before, Frames after) {
    if (x.length <= 0) return false;
    before = std::max<Frames>(before, 0);
    after = std::max<Frames>(after, 0);
    switch (x.align) {
        case Align::Pre:
            if (before == 0) return false;
            x.length = std::min(x.length, before);
            return true;
        case Align::Post:
            if (after == 0) return false;
            x.length = std::min(x.length, after);
            return true;
        case Align::Centre:
            break;
    }
    // The longest centred length: the extra frame of an odd one goes after
    // the cut (crossfade_extent).
    const Frames centred = after <= before ? 2 * after : 2 * before + 1;
    if (x.length <= centred) return true;
    if (before == 0 && after == 0) return false;
    if (after == 0 || before == 0) {
        x.align = after == 0 ? Align::Pre : Align::Post;
        x.length = std::min(x.length, after == 0 ? before : after);
        return true;
    }
    x.length = centred;
    return true;
}

bool Region::operator==(const Region& o) const {
    return id == o.id && name == o.name && position == o.position &&
           length == o.length && source_offset == o.source_offset &&
           channels == o.channels && gain_db == o.gain_db && mute == o.mute &&
           fade_in == o.fade_in && fade_out == o.fade_out;
}

bool Playlist::operator==(const Playlist& o) const {
    return id == o.id && name == o.name && regions == o.regions &&
           crossfades == o.crossfades;
}

bool MidiRegion::operator==(const MidiRegion& o) const {
    return id == o.id && name == o.name && position == o.position && length == o.length &&
           offset == o.offset && mute == o.mute && source == o.source &&
           same_ptr_or_value(data, o.data);
}

bool TrackEdit::operator==(const TrackEdit& o) const {
    if (id != o.id || kind != o.kind || name != o.name || channels != o.channels ||
        active_playlist != o.active_playlist || inserts != o.inserts || sends != o.sends ||
        output != o.output ||
        playlists.size() != o.playlists.size() ||
        midi_playlists.size() != o.midi_playlists.size()) {
        return false;
    }
    for (std::size_t i = 0; i < playlists.size(); ++i) {
        if (!same_ptr_or_value(playlists[i], o.playlists[i])) return false;
    }
    for (std::size_t i = 0; i < midi_playlists.size(); ++i) {
        if (!same_ptr_or_value(midi_playlists[i], o.midi_playlists[i])) return false;
    }
    return true;
}

bool EditState::operator==(const EditState& o) const {
    if (tracks.size() != o.tracks.size() || buses != o.buses) return false;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (!same_ptr_or_value(tracks[i], o.tracks[i])) return false;
    }
    if (markers == o.markers) return true;
    if (!markers || !o.markers) return false;
    return markers->markers == o.markers->markers;
}

bool Source::operator==(const Source& o) const {
    return id == o.id && path == o.path && channels == o.channels &&
           length == o.length && format == o.format &&
           time_reference == o.time_reference && umid == o.umid &&
           kind == o.kind && take == o.take &&
           recorded_on_track == o.recorded_on_track &&
           latency_compensation == o.latency_compensation;
}

bool MidiSource::operator==(const MidiSource& o) const {
    return id == o.id && path == o.path && length == o.length && events == o.events &&
           time_reference == o.time_reference && kind == o.kind && take == o.take &&
           recorded_on_track == o.recorded_on_track &&
           latency_compensation == o.latency_compensation;
}

bool TrackMix::operator==(const TrackMix& o) const {
    return track == o.track && input == o.input && monitor == o.monitor &&
           gain_db == o.gain_db && pan == o.pan && mute == o.mute &&
           solo == o.solo && solo_safe == o.solo_safe && midi == o.midi;
}

const Source* SourcePool::find(Id id) const {
    const auto it = std::lower_bound(
        sources.begin(), sources.end(), id,
        [](const Source& s, Id want) { return s.id < want; });
    if (it == sources.end() || it->id != id) return nullptr;
    return &*it;
}

const MidiSource* SourcePool::find_midi(Id id) const {
    const auto it = std::lower_bound(
        midi_sources.begin(), midi_sources.end(), id,
        [](const MidiSource& s, Id want) { return s.id < want; });
    if (it == midi_sources.end() || it->id != id) return nullptr;
    return &*it;
}

const TrackMix* MixState::find(Id track) const {
    for (const TrackMix& m : tracks) {
        if (m.track == track) return &m;
    }
    return nullptr;
}

const InsertMix* MixState::find_insert(Id insert) const {
    for (const InsertMix& m : inserts) {
        if (m.insert == insert) return &m;
    }
    return nullptr;
}

const BusMix* MixState::find_bus(Id bus) const {
    for (const BusMix& m : buses) {
        if (m.bus == bus) return &m;
    }
    return nullptr;
}

const SendMix* MixState::find_send(Id send) const {
    for (const SendMix& m : sends) {
        if (m.send == send) return &m;
    }
    return nullptr;
}

std::string plugin_state_file(const std::string& sha256) {
    return "Plugin State/" + sha256 + ".state";
}

std::string midi_sequence_file(const std::string& sha256) {
    return "MIDI Sequences/" + sha256 + ".msq";
}

// --- MidiSequence (ADR-0015 §1) ------------------------------------------------

namespace {

Ticks tick_of(const midi::Note& n) { return n.start; }
Ticks tick_of(const MidiEvent& e) { return e.tick; }
Ticks tick_of(const MidiSysEx& x) { return x.tick; }

std::int64_t leaf_of(std::int64_t chunk) { return chunk / kMidiLeafChunks; }

// Stable by chunk. A list already in order, as every valid one is, is left
// as it is without sorting.
template <typename T>
void by_chunk(std::vector<T>& v) {
    const auto chunk_less = [](const T& a, const T& b) {
        return midi_chunk_of(tick_of(a)) < midi_chunk_of(tick_of(b));
    };
    if (!std::is_sorted(v.begin(), v.end(), chunk_less)) {
        std::stable_sort(v.begin(), v.end(), chunk_less);
    }
}

// Moves the run of `from`, starting at `at`, that falls in chunk `k`.
template <typename T>
void take_run(std::vector<T>& from, std::size_t& at, std::int64_t k, std::vector<T>& to) {
    const std::size_t first = at;
    while (at < from.size() && midi_chunk_of(tick_of(from[at])) == k) ++at;
    to.assign(std::make_move_iterator(from.begin() + static_cast<std::ptrdiff_t>(first)),
              std::make_move_iterator(from.begin() + static_cast<std::ptrdiff_t>(at)));
}

template <typename T>
std::int64_t next_chunk(const std::vector<T>& v, std::size_t at) {
    return at < v.size() ? midi_chunk_of(tick_of(v[at])) : INT64_MAX;
}

template <typename T>
bool in_chunk(const std::vector<T>& v, std::int64_t k) {
    return v.empty() ||
           (midi_chunk_of(tick_of(v.front())) == k && midi_chunk_of(tick_of(v.back())) == k);
}

// Counts filled in; the chunks are known to be canonical.
std::shared_ptr<const MidiSpineLeaf> leaf_of_chunks(
    std::vector<std::shared_ptr<const MidiChunk>> chunks) {
    auto leaf = std::make_shared<MidiSpineLeaf>();
    leaf->index = leaf_of(chunks.front()->index);
    leaf->chunks.reserve(chunks.size());
    for (std::shared_ptr<const MidiChunk>& c : chunks) {
        MidiSpineEntry e;
        e.notes = leaf->notes;
        e.events = leaf->events;
        e.sysex = leaf->sysex;
        leaf->notes += c->data.notes.size();
        leaf->events += c->data.events.size();
        leaf->sysex += c->data.sysex.size();
        e.chunk = std::move(c);
        leaf->chunks.push_back(std::move(e));
    }
    return leaf;
}

}  // namespace

MidiSequence::MidiSequence(MidiLists lists) {
    by_chunk(lists.notes);
    by_chunk(lists.events);
    by_chunk(lists.sysex);
    std::vector<std::shared_ptr<const MidiChunk>> pending;
    std::size_t n = 0, e = 0, x = 0;
    for (;;) {
        const std::int64_t k = std::min({next_chunk(lists.notes, n), next_chunk(lists.events, e),
                                         next_chunk(lists.sysex, x)});
        if (k == INT64_MAX) break;
        if (!pending.empty() && leaf_of(pending.front()->index) != leaf_of(k)) {
            spine_.push_back(MidiSpineNode{leaf_of_chunks(std::move(pending)), 0, 0, 0});
            pending.clear();
        }
        auto c = std::make_shared<MidiChunk>();
        c->index = k;
        take_run(lists.notes, n, k, c->data.notes);
        take_run(lists.events, e, k, c->data.events);
        take_run(lists.sysex, x, k, c->data.sysex);
        pending.push_back(std::move(c));
    }
    if (!pending.empty()) {
        spine_.push_back(MidiSpineNode{leaf_of_chunks(std::move(pending)), 0, 0, 0});
    }
    count();
}

void MidiSequence::count() {
    notes_ = events_ = sysex_ = 0;
    for (MidiSpineNode& node : spine_) {
        node.notes = notes_;
        node.events = events_;
        node.sysex = sysex_;
        notes_ += node.leaf->notes;
        events_ += node.leaf->events;
        sysex_ += node.leaf->sysex;
    }
}

bool MidiSequence::make_leaf(std::vector<std::shared_ptr<const MidiChunk>> chunks,
                             std::shared_ptr<const MidiSpineLeaf>& out, std::string& why) {
    if (chunks.empty()) {
        why = "a spine leaf holds no chunk";
        return false;
    }
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const MidiChunk* c = chunks[i].get();
        if (c == nullptr || c->data.empty()) {
            why = "a spine leaf holds an empty chunk";
            return false;
        }
        if (c->index < 0 || leaf_of(c->index) != leaf_of(chunks.front()->index)) {
            why = "chunk " + std::to_string(c->index) + " is not in its leaf's span";
            return false;
        }
        if (i > 0 && c->index <= chunks[i - 1]->index) {
            why = "a spine leaf's chunks are not in order";
            return false;
        }
        if (!in_chunk(c->data.notes, c->index) || !in_chunk(c->data.events, c->index) ||
            !in_chunk(c->data.sysex, c->index)) {
            why = "chunk " + std::to_string(c->index) + " holds data outside its span";
            return false;
        }
    }
    out = leaf_of_chunks(std::move(chunks));
    return true;
}

bool MidiSequence::from_leaves(std::vector<std::shared_ptr<const MidiSpineLeaf>> leaves,
                               MidiSequence& out, std::string& why) {
    MidiSequence s;
    s.spine_.reserve(leaves.size());
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        if (!leaves[i] || leaves[i]->chunks.empty()) {
            why = "a sequence holds an empty spine leaf";
            return false;
        }
        if (!s.spine_.empty() && leaves[i]->index <= s.spine_.back().leaf->index) {
            why = "a sequence's spine leaves are not in order";
            return false;
        }
        s.spine_.push_back(MidiSpineNode{std::move(leaves[i]), 0, 0, 0});
    }
    s.count();
    out = std::move(s);
    return true;
}

MidiSequence MidiSequence::replaced(const MidiSequence& base,
                                    const std::vector<std::shared_ptr<const MidiChunk>>& replace) {
    MidiSequence s;
    s.spine_.reserve(base.spine_.size() + 1);
    std::size_t r = 0;
    std::size_t b = 0;
    while (b < base.spine_.size() || r < replace.size()) {
        const std::int64_t base_leaf =
            b < base.spine_.size() ? base.spine_[b].leaf->index : INT64_MAX;
        const std::int64_t new_leaf = r < replace.size() ? leaf_of(replace[r]->index) : INT64_MAX;
        if (base_leaf < new_leaf) {
            s.spine_.push_back(MidiSpineNode{base.spine_[b++].leaf, 0, 0, 0});
            continue;
        }
        // Merged by index, the replacements winning, empty ones dropped.
        const std::int64_t leaf = new_leaf;
        std::vector<std::shared_ptr<const MidiChunk>> chunks;
        const std::vector<MidiSpineEntry>* old =
            base_leaf == leaf ? &base.spine_[b].leaf->chunks : nullptr;
        std::size_t o = 0;
        while ((old != nullptr && o < old->size()) ||
               (r < replace.size() && leaf_of(replace[r]->index) == leaf)) {
            const bool have_new = r < replace.size() && leaf_of(replace[r]->index) == leaf;
            const bool have_old = old != nullptr && o < old->size();
            const std::int64_t ki = have_new ? replace[r]->index : INT64_MAX;
            const std::int64_t ko = have_old ? (*old)[o].chunk->index : INT64_MAX;
            if (ko < ki) {
                chunks.push_back((*old)[o++].chunk);
                continue;
            }
            if (ko == ki) ++o;
            if (!replace[r]->data.empty()) chunks.push_back(replace[r]);
            ++r;
        }
        if (old != nullptr) ++b;
        if (!chunks.empty()) {
            s.spine_.push_back(MidiSpineNode{leaf_of_chunks(std::move(chunks)), 0, 0, 0});
        }
    }
    s.count();
    return s;
}

const MidiChunk* MidiSequence::chunk_at(std::int64_t index) const {
    const std::int64_t leaf = leaf_of(index);
    const auto node = std::lower_bound(
        spine_.begin(), spine_.end(), leaf,
        [](const MidiSpineNode& n, std::int64_t l) { return n.leaf->index < l; });
    if (node == spine_.end() || node->leaf->index != leaf) return nullptr;
    const std::vector<MidiSpineEntry>& cs = node->leaf->chunks;
    const auto it = std::lower_bound(
        cs.begin(), cs.end(), index,
        [](const MidiSpineEntry& e, std::int64_t k) { return e.chunk->index < k; });
    return it != cs.end() && it->chunk->index == index ? it->chunk.get() : nullptr;
}

MidiLists MidiSequence::lists() const {
    MidiLists out;
    out.notes.reserve(note_count());
    out.events.reserve(event_count());
    out.sysex.reserve(sysex_count());
    for (const MidiSpineNode& node : spine_) {
        for (const MidiSpineEntry& e : node.leaf->chunks) {
            const MidiLists& d = e.chunk->data;
            out.notes.insert(out.notes.end(), d.notes.begin(), d.notes.end());
            out.events.insert(out.events.end(), d.events.begin(), d.events.end());
            out.sysex.insert(out.sysex.end(), d.sysex.begin(), d.sysex.end());
        }
    }
    return out;
}

bool MidiSequence::operator==(const MidiSequence& o) const {
    if (!(missing == o.missing) || notes_ != o.notes_ || events_ != o.events_ ||
        sysex_ != o.sysex_ || spine_.size() != o.spine_.size()) {
        return false;
    }
    // Canonical, so equal contents are equal spines.
    for (std::size_t i = 0; i < spine_.size(); ++i) {
        const MidiSpineLeaf& a = *spine_[i].leaf;
        const MidiSpineLeaf& b = *o.spine_[i].leaf;
        if (&a == &b) continue;
        if (a.index != b.index || a.chunks.size() != b.chunks.size()) return false;
        for (std::size_t k = 0; k < a.chunks.size(); ++k) {
            const MidiChunk& x = *a.chunks[k].chunk;
            const MidiChunk& y = *b.chunks[k].chunk;
            if (&x != &y && (x.index != y.index || !(x.data == y.data))) return false;
        }
    }
    return true;
}

std::string new_session_id() {
    std::random_device rd;
    std::array<std::uint8_t, 16> b{};
    for (std::size_t i = 0; i < b.size(); i += 4) {
        const std::uint32_t r = rd();
        for (std::size_t k = 0; k < 4; ++k) b[i + k] = static_cast<std::uint8_t>(r >> (8 * k));
    }
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);
    static const char kHex[] = "0123456789abcdef";
    std::string id;
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) id += '-';
        id += kHex[b[i] >> 4];
        id += kHex[b[i] & 0x0F];
    }
    return id;
}

bool valid_session_id(const std::string& id) {
    if (id.size() > 64) return false;
    for (const char c : id) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-')) return false;
    }
    return true;
}

Snapshot new_session(const SessionSettings& settings) {
    auto edit = std::make_shared<EditState>();
    edit->markers = std::make_shared<const MarkerList>();
    Snapshot s;
    s.edit = std::move(edit);
    s.sources = std::make_shared<const SourcePool>();
    s.settings = settings;
    return s;
}

bool operator==(const Snapshot& a, const Snapshot& b) {
    return same_ptr_or_value(a.edit, b.edit) && same_ptr_or_value(a.sources, b.sources) &&
           a.mix == b.mix && a.settings == b.settings &&
           a.revision == b.revision && a.next_id == b.next_id;
}

bool same_document(const Snapshot& a, const Snapshot& b) {
    if (!same_ptr_or_value(a.edit, b.edit)) return false;
    if (!same_ptr_or_value(a.sources, b.sources)) return false;
    if (!(a.settings == b.settings) || !(a.mix.bus == b.mix.bus)) return false;
    for (const auto& t : a.edit->tracks) {
        const TrackMix* ma = a.mix.find(t->id);
        const TrackMix* mb = b.mix.find(t->id);
        if (ma == nullptr || mb == nullptr || !(*ma == *mb)) return false;
        for (const Insert& i : t->inserts) {
            const InsertMix* ia = a.mix.find_insert(i.id);
            const InsertMix* ib = b.mix.find_insert(i.id);
            if (ia == nullptr || ib == nullptr || !(*ia == *ib)) return false;
        }
    }
    const auto same_sends = [&](const std::vector<Send>& sends) {
        for (const Send& sd : sends) {
            const SendMix* sa = a.mix.find_send(sd.id);
            const SendMix* sb = b.mix.find_send(sd.id);
            if (sa == nullptr || sb == nullptr || !(*sa == *sb)) return false;
        }
        return true;
    };
    for (const auto& t : a.edit->tracks) {
        if (!same_sends(t->sends)) return false;
    }
    for (const Bus& bus : a.edit->buses) {
        const BusMix* ba = a.mix.find_bus(bus.id);
        const BusMix* bb = b.mix.find_bus(bus.id);
        if (ba == nullptr || bb == nullptr || !(*ba == *bb)) return false;
        if (!same_sends(bus.sends)) return false;
    }
    return true;
}

namespace detail {

bool valid_text(const std::string& s) { return json::valid_utf8(s); }

namespace {

std::string path_problem(const std::string& path) {
    if (!valid_text(path)) return "path is not valid UTF-8";
    // The folder is the unit: a path that leaves it, or that only means
    // something on this machine, breaks the session when the folder moves.
    if (path.empty()) return "path is empty";
    if (path.front() == '/') return "path \"" + path + "\" is absolute";
    if (path.find('\\') != std::string::npos) {
        return "path \"" + path + "\" uses '\\'; paths are '/'-separated";
    }
    if (path.find(':') != std::string::npos) {
        return "path \"" + path + "\" contains ':'";
    }
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = path.find('/', start);
        const std::string part =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") {
            return "path \"" + path + "\" has an empty, \".\" or \"..\" component";
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return {};
}

std::string take_problem(SourceKind kind, std::int64_t take, Id recorded_on_track,
                         Frames latency_compensation) {
    if (kind == SourceKind::Imported) {
        if (take != 0 || recorded_on_track != 0 || latency_compensation != 0) {
            return "an imported source has no take, recorded_on_track or"
                   " latency_compensation";
        }
        return {};
    }
    if (!in_range(take)) return "take is out of range";
    if (recorded_on_track > static_cast<Id>(kMaxInteger)) {
        return "recorded_on_track is out of range";
    }
    if (!in_range(latency_compensation)) return "latency_compensation is out of range";
    return {};
}

}  // namespace

std::string source_problem(const Source& s) {
    if (s.id == 0) return "id must be at least 1";
    const std::string why = path_problem(s.path);
    if (!why.empty()) return why;
    if (s.channels == 0) return "channels must be at least 1";
    if (!in_range(s.length)) return "length is out of range";
    if (!in_range(s.time_reference)) return "time_reference is out of range";
    if (!s.umid.empty()) {
        bool hex = s.umid.size() == 64;
        for (const char c : s.umid) {
            hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
        if (!hex) return "umid must be 64 lower-case hex digits";
    }
    return take_problem(s.kind, s.take, s.recorded_on_track, s.latency_compensation);
}

std::string midi_source_problem(const MidiSource& s) {
    if (s.id == 0) return "id must be at least 1";
    const std::string why = path_problem(s.path);
    if (!why.empty()) return why;
    if (!in_range(s.length)) return "length is out of range";
    if (s.events > static_cast<std::uint64_t>(kMaxInteger)) return "events is out of range";
    if (!in_range(s.time_reference)) return "time_reference is out of range";
    return take_problem(s.kind, s.take, s.recorded_on_track, s.latency_compensation);
}

bool note_before(const midi::Note& a, const midi::Note& b) {
    if (a.start != b.start) return a.start < b.start;
    if (a.channel != b.channel) return a.channel < b.channel;
    if (a.pitch != b.pitch) return a.pitch < b.pitch;
    return a.end() < b.end();
}

std::string sequence_ref_problem(const SequenceFileRef& r) {
    if (!lower_hex(r.sha256, 64)) return "sha256 must be 64 lower-case hex digits";
    if (r.file != midi_sequence_file(r.sha256)) {
        return "file must be \"" + midi_sequence_file(r.sha256) + "\"";
    }
    for (const std::uint64_t n : {r.bytes, r.notes, r.events, r.sysex}) {
        if (n > static_cast<std::uint64_t>(kMaxInteger)) {
            return "bytes, notes, events and sysex must be at most " + str(kMaxInteger);
        }
    }
    return {};
}

namespace {

template <typename Notes, typename Events, typename Sysex>
std::string lists_problem(const Notes& notes, const Events& events, const Sysex& sysex,
                          std::string& where) {
    const auto at = [&](const char* list, std::size_t i) {
        where = std::string(list) + "[" + std::to_string(i) + "]";
    };
    std::size_t i = 0;
    const midi::Note* prev_note = nullptr;
    for (const midi::Note& n : notes) {
        at("notes", i++);
        if (!in_range(n.start) || !in_range(n.length) || n.start + n.length > kMaxInteger) {
            return "start and length must not be negative, and start + length at most " +
                   str(kMaxInteger);
        }
        if (n.channel > 15) return "channel must be from 0 to 15";
        if (n.pitch > 127) return "pitch must be from 0 to 127";
        // 0 is a note-off on the wire: a note stored with it never sounds.
        if (n.velocity < 1 || n.velocity > 127) return "velocity must be from 1 to 127";
        if (n.release_velocity > 127) return "release velocity must be from 0 to 127";
        if (prev_note != nullptr && note_before(n, *prev_note)) {
            return "notes are not in order (start, channel, pitch, end)";
        }
        prev_note = &n;
    }
    i = 0;
    const MidiEvent* prev_event = nullptr;
    for (const MidiEvent& e : events) {
        at("events", i++);
        if (!in_range(e.tick)) return "tick must be from 0 to " + str(kMaxInteger);
        if (e.status < 0xA0 || e.status > 0xEF) {
            return "status must be a channel message other than a note, 0xA0 to 0xEF";
        }
        if (e.data1 > 127 || e.data2 > 127) return "data bytes must be from 0 to 127";
        const std::uint8_t type = e.status & 0xF0;
        if ((type == 0xC0 || type == 0xD0) && e.data2 != 0) {
            return "a program change or channel pressure has one data byte; data2 must be 0";
        }
        if (prev_event != nullptr && e.tick < prev_event->tick) {
            return "events are not sorted by tick";
        }
        prev_event = &e;
    }
    i = 0;
    const MidiSysEx* prev_sysex = nullptr;
    for (const MidiSysEx& x : sysex) {
        at("sysex", i++);
        if (!in_range(x.tick)) return "tick must be from 0 to " + str(kMaxInteger);
        if (x.bytes.size() < 2 || x.bytes.size() > kMaxSysexBytes) {
            return "a SysEx message is from 2 to " +
                   str(static_cast<std::int64_t>(kMaxSysexBytes)) + " bytes, F0 to F7";
        }
        if (x.bytes.front() != 0xF0 || x.bytes.back() != 0xF7) {
            return "a SysEx message starts with F0 and ends with F7";
        }
        for (std::size_t k = 1; k + 1 < x.bytes.size(); ++k) {
            if (x.bytes[k] > 0x7F) return "a SysEx message's data bytes are below 0x80";
        }
        if (prev_sysex != nullptr && x.tick < prev_sysex->tick) {
            return "sysex is not sorted by tick";
        }
        prev_sysex = &x;
    }
    where.clear();
    return {};
}

}  // namespace

std::string sequence_problem(const MidiLists& s, std::string& where) {
    return lists_problem(s.notes, s.events, s.sysex, where);
}

std::string sequence_problem(const MidiSequence& s, std::string& where) {
    if (s.missing) {
        where = "data";
        if (!s.empty()) return "a sequence whose file is missing holds no notes or events";
        std::string why = sequence_ref_problem(*s.missing);
        if (why.empty()) where.clear();
        return why;
    }
    return lists_problem(s.notes(), s.events(), s.sysex(), where);
}

std::string midi_region_problem(const MidiRegion& r, const SourcePool& pool) {
    if (r.id == 0) return "id must be at least 1";
    if (!valid_text(r.name)) return "name is not valid UTF-8";
    if (r.length <= 0) return "length must be greater than 0";
    if (r.position < 0) return "position must not be negative";
    if (r.offset < 0) return "offset must not be negative";
    if (r.position > kMaxInteger || r.length > kMaxInteger ||
        r.position + r.length > kMaxInteger) {
        return "position + length is past the end of the timeline";
    }
    if (r.offset > kMaxInteger) return "offset is out of range";
    if (r.source != 0 && pool.find_midi(r.source) == nullptr) {
        return "MIDI source " + str(r.source) + " does not exist";
    }
    if (!r.data) return "has no sequence";
    return {};
}

std::string midi_order_problem(const MidiPlaylist& p, std::size_t i) {
    if (i == 0 || p.regions[i].position >= p.regions[i - 1].end()) return {};
    return p.regions[i - 1].position <= p.regions[i].position
               ? "overlaps the region before it (regions[" +
                     str(static_cast<std::int64_t>(i - 1)) + "])"
               : "is not sorted by position";
}

std::string midi_playlist_problem(const MidiPlaylist& p, const SourcePool& pool, bool check_data,
                                  std::string& where) {
    where.clear();
    if (p.id == 0) return "id must be at least 1";
    if (!valid_text(p.name)) return "name is not valid UTF-8";
    std::unordered_set<Id> ids;
    for (std::size_t i = 0; i < p.regions.size(); ++i) {
        const MidiRegion& r = p.regions[i];
        where = "regions[" + str(static_cast<std::int64_t>(i)) + "]";
        std::string why = midi_region_problem(r, pool);
        if (why.empty()) why = midi_order_problem(p, i);
        if (why.empty() && !ids.insert(r.id).second) {
            why = "region id " + str(r.id) + " appears twice";
        }
        if (why.empty() && check_data) {
            std::string inner;
            why = sequence_problem(*r.data, inner);
            if (!why.empty()) where += "." + inner;
        }
        if (!why.empty()) return why;
    }
    where.clear();
    return {};
}

const char* track_kind_name(TrackKind kind) {
    switch (kind) {
        case TrackKind::Midi: return "MIDI";
        case TrackKind::Instrument: return "instrument";
        case TrackKind::Audio: break;
    }
    return "audio";
}

std::string track_shape_problem(const TrackEdit& t) {
    if (t.kind == TrackKind::Midi) {
        if (t.channels != 0) return "a MIDI track has no audio channels; channels must be 0";
        if (!t.inserts.empty()) return "a MIDI track has no audio, and so no insert";
        if (!t.sends.empty()) return "a MIDI track has no audio, and so no send";
    } else if (t.channels < 1 || t.channels > 2) {
        return "needs 1 or 2 channels";
    }
    if (t.kind == TrackKind::Audio) {
        if (!t.midi_playlists.empty()) return "an audio track holds no MIDI playlist";
        if (t.playlists.empty()) return "a track needs at least one playlist";
    } else {
        if (!t.playlists.empty()) {
            return std::string(t.kind == TrackKind::Midi ? "a MIDI" : "an instrument") +
                   " track holds MIDI playlists, not audio ones";
        }
        if (t.midi_playlists.empty()) return "a track needs at least one playlist";
    }
    return {};
}

std::string region_problem(const Region& r, const SourcePool& pool,
                           std::uint16_t track_channels) {
    if (r.id == 0) return "id must be at least 1";
    if (!valid_text(r.name)) return "name is not valid UTF-8";
    if (r.length <= 0) return "length must be greater than 0";
    if (r.position < 0) return "position must not be negative";
    if (r.source_offset < 0) return "source_offset must not be negative";
    if (r.position > kMaxInteger || r.length > kMaxInteger ||
        r.position + r.length > kMaxInteger) {
        return "position + length is past the end of the timeline";
    }
    if (r.source_offset > kMaxInteger) return "source_offset is out of range";
    if (r.channels.size() != track_channels) {
        return "has " + str(static_cast<std::int64_t>(r.channels.size())) +
               " source entries for a " +
               str(static_cast<std::int64_t>(track_channels)) + "-channel track";
    }
    for (const SourceRef& c : r.channels) {
        const Source* s = pool.find(c.source);
        if (s == nullptr) return "source " + str(c.source) + " does not exist";
        if (c.channel >= s->channels) {
            return "source " + str(c.source) + " has no channel " +
                   str(static_cast<std::int64_t>(c.channel));
        }
        if (r.source_offset + r.length > s->length) {
            return "source_offset + length (" + str(r.source_offset + r.length) +
                   ") is past the end of source " + str(c.source) + " (length " +
                   str(s->length) + ")";
        }
    }
    if (r.fade_in.length < 0 || r.fade_out.length < 0) {
        return "a fade length must not be negative";
    }
    if (r.fade_in.length > r.length - r.fade_out.length) {
        return "fade_in.length + fade_out.length is longer than the region";
    }
    if (!gain_in_range(r.gain_db)) return "gain_db " + gain_range();
    return {};
}

const Region* find_region(const Playlist& p, Id id) {
    for (const Region& r : p.regions) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

std::string crossfade_problem(const Crossfade& x, const Playlist& p,
                              const SourcePool& pool) {
    return crossfade_problem(x, find_region(p, x.left), find_region(p, x.right), pool);
}

std::string crossfade_problem(const Crossfade& x, const Region* l, const Region* r,
                              const SourcePool& pool) {
    if (x.length <= 0 || x.length > kMaxInteger) {
        return "length must be greater than 0";
    }
    if (l == nullptr) return "region " + str(x.left) + " is not on this playlist";
    if (r == nullptr) return "region " + str(x.right) + " is not on this playlist";
    if (l->end() != r->position) {
        return "regions " + str(x.left) + " and " + str(x.right) +
               " do not butt (" + str(x.left) + " ends at " + str(l->end()) +
               ", " + str(x.right) + " starts at " + str(r->position) + ")";
    }
    Frames before = 0, after = 0;
    crossfade_extent(x, before, after);
    if (before > l->length || after > r->length) {
        return "longer than the regions it joins";
    }
    // Refused, not shortened: a crossfade quietly shorter than asked for is
    // a different edit from the one the operator made.
    const Frames left_handle = shortest_source(*l, pool) - l->source_offset - l->length;
    if (left_handle < after) {
        return "region " + str(x.left) + " has " + str(left_handle) +
               " frames of handle after its end; the crossfade needs " + str(after);
    }
    if (r->source_offset < before) {
        return "region " + str(x.right) + " has " + str(r->source_offset) +
               " frames of handle before its start; the crossfade needs " +
               str(before);
    }
    return {};
}

void sort_crossfades(Playlist& p) {
    // The map below is over every region; a playlist with nothing to sort
    // is most of them, and every member of a compound comes through here.
    if (p.crossfades.size() < 2) return;
    std::unordered_map<Id, Frames> pos;
    for (const Region& r : p.regions) pos[r.id] = r.position;
    std::stable_sort(p.crossfades.begin(), p.crossfades.end(),
                     [&](const Crossfade& a, const Crossfade& b) {
                         return pos[a.left] < pos[b.left];
                     });
}

void sort_markers(MarkerList& m) {
    std::stable_sort(m.markers.begin(), m.markers.end(),
                     [](const Marker& a, const Marker& b) {
                         if (a.position != b.position) return a.position < b.position;
                         return a.id < b.id;
                     });
}

std::vector<FadeFit> fit_fades_to_crossfades(Playlist& p) {
    struct Reach {
        Frames in = -1;    // -1: no crossfade at that end
        Frames out = -1;
    };
    std::unordered_map<Id, Reach> reach;
    for (const Crossfade& x : p.crossfades) {
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        reach[x.left].out = before;
        reach[x.right].in = after;
    }
    std::vector<FadeFit> fits;
    for (std::size_t i = 0; i < p.regions.size(); ++i) {
        Region& r = p.regions[i];
        const auto it = reach.find(r.id);
        if (it == reach.end()) continue;
        const Reach& x = it->second;
        // With crossfades at both ends both fades are unheard; only a fade
        // at an end without one can run into the other end's crossfade. A
        // crossfade longer than the region is left for playlist_problem to
        // refuse, not turned into a negative fade.
        if (x.in >= 0 && x.out < 0 && x.in <= r.length &&
            r.fade_out.length > r.length - x.in) {
            fits.push_back({i, false, r.fade_out.length, r.length - x.in});
            r.fade_out.length = r.length - x.in;
        } else if (x.out >= 0 && x.in < 0 && x.out <= r.length &&
                   r.fade_in.length > r.length - x.out) {
            fits.push_back({i, true, r.fade_in.length, r.length - x.out});
            r.fade_in.length = r.length - x.out;
        }
    }
    return fits;
}

std::string playlist_problem(const Playlist& p, const SourcePool& pool,
                             std::uint16_t track_channels, std::string& where) {
    where.clear();
    if (p.id == 0) return "id must be at least 1";
    if (!valid_text(p.name)) return "name is not valid UTF-8";
    std::unordered_map<Id, std::size_t> index;
    for (std::size_t i = 0; i < p.regions.size(); ++i) {
        const Region& r = p.regions[i];
        std::string why = region_problem(r, pool, track_channels);
        if (why.empty()) why = order_problem(p, i);
        if (why.empty() && !index.emplace(r.id, i).second) {
            why = "region id " + str(r.id) + " appears twice";
        }
        if (!why.empty()) {
            where = "regions[" + str(static_cast<std::int64_t>(i)) + "]";
            return why;
        }
    }
    // At most two regions sound at any instant: the crossfades at the two
    // ends of one region must not meet inside it.
    std::vector<Frames> in_after(p.regions.size(), 0);
    std::vector<Frames> out_before(p.regions.size(), 0);
    std::vector<bool> has_in(p.regions.size(), false);
    std::vector<bool> has_out(p.regions.size(), false);
    for (std::size_t k = 0; k < p.crossfades.size(); ++k) {
        const Crossfade& x = p.crossfades[k];
        where = "crossfades[" + str(static_cast<std::int64_t>(k)) + "]";
        std::string why = crossfade_problem(x, p, pool);
        if (!why.empty()) return why;
        const std::size_t li = index[x.left];
        const std::size_t ri = index[x.right];
        if (has_out[li] || has_in[ri]) return second_crossfade_problem(x);
        if (k > 0 && index[p.crossfades[k - 1].left] > li) return kCrossfadesUnsorted;
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        has_out[li] = true;
        out_before[li] = before;
        has_in[ri] = true;
        in_after[ri] = after;
    }
    // What is heard at an end is its crossfade if it has one, else its own
    // fade. A fade under a crossfade at the same end is kept but silent; one
    // at the other end that runs into a crossfade would be multiplied by it.
    for (std::size_t i = 0; i < p.regions.size(); ++i) {
        std::string why =
            heard_problem(p.regions[i], has_in[i], in_after[i], has_out[i], out_before[i]);
        if (why.empty()) continue;
        where = "regions[" + str(static_cast<std::int64_t>(i)) + "]";
        return why;
    }
    where.clear();
    return {};
}

std::string order_problem(const Playlist& p, std::size_t i) {
    if (i == 0 || p.regions[i].position >= p.regions[i - 1].end()) return {};
    return p.regions[i - 1].position <= p.regions[i].position
               ? "overlaps the region before it (regions[" +
                     str(static_cast<std::int64_t>(i - 1)) + "])"
               : "is not sorted by position";
}

std::string second_crossfade_problem(const Crossfade& x) {
    return "a second crossfade between regions " + str(x.left) + " and " + str(x.right);
}

const char* const kCrossfadesUnsorted = "crossfades are not sorted by position";

std::string heard_problem(const Region& r, bool has_in, Frames in_after, bool has_out,
                          Frames out_before) {
    const Frames heard_in = has_in ? in_after : r.fade_in.length;
    const Frames heard_out = has_out ? out_before : r.fade_out.length;
    if (heard_in + heard_out <= r.length) return {};
    if (has_in && has_out) {
        return "the crossfades at its two ends overlap; at most two"
               " regions may sound at once";
    }
    return has_in ? "its fade-out overlaps the crossfade at its start"
                  : "its fade-in overlaps the crossfade at its end";
}

std::string midi_routing_problem(const MidiRouting& r, TrackKind kind, std::string& where) {
    where.clear();
    if (kind == TrackKind::Audio) {
        if (r == MidiRouting{}) return {};
        where = "midi";
        return "an audio track has no MIDI input or output";
    }
    // A name is what the backend reports; the two words are the only ones
    // with a meaning of their own, each on its side only.
    const auto port = [](const std::string& name, const char* other) {
        return !name.empty() && valid_text(name) && name != other;
    };
    if (!port(r.in_port, kNoMidiOutput)) {
        where = "midi_input.port";
        return "must be \"all\" or a port name";
    }
    if (r.in_channel > 16) {
        where = "midi_input.channel";
        return "must be 0 (all) or a channel from 1 to 16";
    }
    if (kind == TrackKind::Instrument) {
        if (r.out_port == kNoMidiOutput && r.out_channel == 0) return {};
        where = "midi_output";
        return "an instrument track plays its own instrument and has no MIDI output";
    }
    if (!port(r.out_port, kAllMidiInputs)) {
        where = "midi_output.port";
        return "must be \"none\" or a port name";
    }
    if (r.out_channel > 16) {
        where = "midi_output.channel";
        return "must be 0 (as recorded) or a channel from 1 to 16";
    }
    return {};
}

std::string mix_problem(const TrackMix& m, TrackKind kind, std::uint16_t track_channels,
                        std::string& where) {
    where.clear();
    if (kind != TrackKind::Audio && !m.input.empty()) {
        where = "input";
        return std::string("a ") + (kind == TrackKind::Midi ? "MIDI" : "n instrument") +
               " track records MIDI and has no audio input";
    }
    if (kind == TrackKind::Audio && m.input.size() != track_channels) {
        where = "input";
        return "needs one capture channel per track channel (" +
               str(static_cast<std::int64_t>(track_channels)) + ")";
    }
    if (kind == TrackKind::Midi && (m.gain_db != 0.0f || m.pan != 0.0f)) {
        where = m.gain_db != 0.0f ? "gain_db" : "pan";
        return "a MIDI track has no audio fader or pan; it must be 0";
    }
    std::string why = midi_routing_problem(m.midi, kind, where);
    if (!why.empty()) return why;
    if (!gain_in_range(m.gain_db)) {
        where = "gain_db";
        return gain_range();
    }
    if (!(m.pan >= -1.0f && m.pan <= 1.0f)) {
        where = "pan";
        return "must be from -1 to 1";
    }
    return {};
}

std::string bus_problem(const MonitorBus& b, std::string& where) {
    where.clear();
    if (!gain_in_range(b.gain_db)) {
        where = "gain_db";
        return gain_range();
    }
    // Written so that NaN fails it.
    if (!(b.pan_law_db >= kMinPanLawDb && b.pan_law_db <= 0.0f)) {
        where = "pan_law_db";
        return "must be from " + str(static_cast<std::int64_t>(kMinPanLawDb)) + " to 0";
    }
    return {};
}

std::string settings_problem(const SessionSettings& s, std::string& where) {
    where.clear();
    if (!valid_text(s.name)) {
        where = "name";
        return "is not valid UTF-8";
    }
    if (s.sample_rate == 0) {
        where = "sample_rate";
        return "must be greater than 0";
    }
    if (!in_range(s.timecode.origin)) {
        where = "timecode.origin";
        return "is out of range";
    }
    // The map's shape and the bounds musical time needs (midi.md §2.4), in
    // the one place MusicalMap::build also answers to.
    std::string why = tempo_map_problem(s.sample_rate, s.tempo, where);
    if (!why.empty()) return why;
    const Transport& tr = s.transport;
    for (const auto& range : {std::make_pair("transport.loop", tr.loop),
                              std::make_pair("transport.punch", tr.punch)}) {
        if (!in_range(range.second[0]) || !in_range(range.second[1]) ||
            range.second[0] > range.second[1]) {
            where = range.first;
            return "must be [start, end] with start <= end";
        }
    }
    if (!in_range(tr.pre_roll)) {
        where = "transport.pre_roll";
        return "is out of range";
    }
    if (!in_range(tr.post_roll)) {
        where = "transport.post_roll";
        return "is out of range";
    }
    if (!in_range(tr.punch_crossfade)) {
        where = "transport.punch_crossfade";
        return "is out of range";
    }
    if (tr.count_off_bars < 1 || tr.count_off_bars > kMaxCountOffBars) {
        where = "transport.count_off_bars";
        return "must be from 1 to " + std::to_string(kMaxCountOffBars);
    }
    if (!valid_session_id(s.session_id)) {
        where = "session_id";
        return "must be empty or up to 64 of 0-9, a-f and \"-\"";
    }
    if (s.left_takes.size() > kMaxLeftTakes) {
        where = "left_takes";
        return "holds more than " + std::to_string(kMaxLeftTakes) + " files";
    }
    for (std::size_t i = 0; i < s.left_takes.size(); ++i) {
        const std::string why = path_problem(s.left_takes[i]);
        if (!why.empty()) {
            where = "left_takes[" + std::to_string(i) + "]";
            return why;
        }
        if (i > 0 && !(s.left_takes[i - 1] < s.left_takes[i])) {
            where = "left_takes[" + std::to_string(i) + "]";
            return "must be sorted, with no file twice";
        }
    }
    where.clear();
    return {};
}

std::string marker_problem(const Marker& m) {
    if (m.id == 0) return "id must be at least 1";
    if (!valid_text(m.name)) return "name is not valid UTF-8";
    if (!in_range(m.position) || !in_range(m.length) ||
        m.position + m.length > kMaxInteger) {
        return "position + length is out of range";
    }
    return {};
}

static bool hex_digits(const std::string& s, std::size_t n, char ten) {
    if (s.size() != n) return false;
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= ten && c <= ten + 5))) return false;
    }
    return true;
}

// The LV2 spec's grammar for a port symbol: a C identifier.
static bool lv2_symbol(const std::string& s) {
    if (s.empty()) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!(alpha || (i > 0 && c >= '0' && c <= '9'))) return false;
    }
    return true;
}

// A VST3 ParamID is a uint32, written in decimal without leading zeros, so
// one parameter has one spelling.
static bool vst3_param_id(const std::string& s) {
    if (s.empty() || s.size() > 10 || (s.size() > 1 && s[0] == '0')) return false;
    std::uint64_t n = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return n <= 0xFFFFFFFFu;
}

std::string plugin_problem(const PluginRef& p, std::string& where) {
    where = "plugin.id";
    if (p.id.empty()) return "is empty";
    if (p.format == PluginFormat::Lv2) {
        if (!valid_text(p.id)) return "is not valid UTF-8";
        for (const char c : p.id) {
            if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) {
                return "an LV2 plugin URI has no space or control character";
            }
        }
        if (p.id.find(':') == std::string::npos) {
            return "an LV2 plugin URI needs a scheme (\"http:\", \"urn:\")";
        }
    } else if (!hex_digits(p.id, 32, 'A')) {
        return "a VST3 class id is 32 upper-case hex digits";
    }
    where = "plugin.name";
    if (!valid_text(p.name)) return "is not valid UTF-8";
    where = "plugin.version";
    if (!valid_text(p.version)) return "is not valid UTF-8";
    where.clear();
    return {};
}

std::string insert_param_count_problem(std::size_t count) {
    if (count <= kMaxInsertParams) return {};
    return "holds " + str(static_cast<std::int64_t>(count)) + " values; an insert holds at most " +
           str(static_cast<std::int64_t>(kMaxInsertParams));
}

std::string insert_mix_problem(const InsertMix& m, PluginFormat format, std::string& where) {
    where = "params";
    std::string why = insert_param_count_problem(m.params.size());
    if (!why.empty()) return why;
    std::unordered_set<std::string> keys;
    for (std::size_t i = 0; i < m.params.size(); ++i) {
        const ParamValue& p = m.params[i];
        where = "params[" + str(static_cast<std::int64_t>(i)) + "]";
        if (format == PluginFormat::Lv2 && !lv2_symbol(p.key)) {
            return "key \"" + (valid_text(p.key) ? p.key : std::string("?")) +
                   "\" is not an LV2 port symbol";
        }
        if (format == PluginFormat::Vst3 && !vst3_param_id(p.key)) {
            return "key \"" + (valid_text(p.key) ? p.key : std::string("?")) +
                   "\" is not a VST3 ParamID in decimal";
        }
        if (!keys.insert(p.key).second) return "key \"" + p.key + "\" appears twice";
        if (!std::isfinite(p.value)) return "value must be a finite number";
        // Written so that NaN fails it, though isfinite already has.
        if (format == PluginFormat::Vst3 && !(p.value >= 0.0f && p.value <= 1.0f)) {
            return "a VST3 value is normalised, from 0 to 1";
        }
    }
    if (m.state) {
        const StateRef& st = *m.state;
        where = "state";
        if (!hex_digits(st.sha256, 64, 'a')) return "sha256 must be 64 lower-case hex digits";
        if (st.file != plugin_state_file(st.sha256)) {
            return "file must be \"" + plugin_state_file(st.sha256) + "\"";
        }
        if (st.bytes > static_cast<std::uint64_t>(kMaxInteger)) return "bytes is out of range";
    }
    where.clear();
    return {};
}

std::string inserts_problem(const std::vector<Insert>& inserts, std::string& where) {
    for (std::size_t i = 0; i < inserts.size(); ++i) {
        const Insert& in = inserts[i];
        where = "inserts[" + str(static_cast<std::int64_t>(i)) + "]";
        if (in.id == 0) return "id must be at least 1";
        if (in.slot >= kInsertSlots) {
            return "slot " + str(static_cast<std::int64_t>(in.slot)) + " is not one of 0 to " +
                   str(static_cast<std::int64_t>(kInsertSlots - 1));
        }
        if (i > 0 && in.slot == inserts[i - 1].slot) {
            return "slot " + str(static_cast<std::int64_t>(in.slot)) + " is used twice";
        }
        if (i > 0 && in.slot < inserts[i - 1].slot) return "inserts are not sorted by slot";
        std::string inner;
        const std::string why = plugin_problem(in.plugin, inner);
        if (!why.empty()) {
            where += "." + inner;
            return why;
        }
    }
    where.clear();
    return {};
}

std::string send_mix_problem(const SendMix& m, std::string& where) {
    where.clear();
    if (!gain_in_range(m.level_db)) {
        where = "level_db";
        return gain_range();
    }
    if (!(m.pan >= -1.0f && m.pan <= 1.0f)) {
        where = "pan";
        return "must be from -1 to 1";
    }
    return {};
}

std::string bus_mix_problem(const BusMix& m, std::string& where) {
    where.clear();
    if (!gain_in_range(m.gain_db)) {
        where = "gain_db";
        return gain_range();
    }
    if (!(m.pan >= -1.0f && m.pan <= 1.0f)) {
        where = "pan";
        return "must be from -1 to 1";
    }
    return {};
}

std::string sends_problem(const std::vector<Send>& sends, std::string& where) {
    for (std::size_t i = 0; i < sends.size(); ++i) {
        const Send& sd = sends[i];
        where = "sends[" + str(static_cast<std::int64_t>(i)) + "]";
        if (sd.id == 0) return "id must be at least 1";
        if (sd.bus == 0) return "bus must name an aux bus";
        if (sd.slot >= kSendSlots) {
            return "slot " + str(static_cast<std::int64_t>(sd.slot)) + " is not one of 0 to " +
                   str(static_cast<std::int64_t>(kSendSlots - 1));
        }
        if (i > 0 && sd.slot == sends[i - 1].slot) {
            return "slot " + str(static_cast<std::int64_t>(sd.slot)) + " is used twice";
        }
        if (i > 0 && sd.slot < sends[i - 1].slot) return "sends are not sorted by slot";
        for (std::size_t k = 0; k < i; ++k) {
            if (sends[k].bus == sd.bus) {
                return "a strip sends to bus " + str(sd.bus) + " once; slot " +
                       std::string(1, static_cast<char>('A' + sends[k].slot)) +
                       " already does";
            }
        }
    }
    where.clear();
    return {};
}

const Bus* find_bus(const EditState& e, Id bus) {
    for (const Bus& b : e.buses) {
        if (b.id == bus) return &b;
    }
    return nullptr;
}

std::vector<Id> bus_path(const EditState& e, Id from, Id to) {
    // Depth-first with an explicit stack; the graph is small (kMaxBuses)
    // and acyclic when this is asked, but a visited set keeps a malformed
    // one from looping here too.
    std::vector<Id> path;
    std::unordered_set<Id> seen;
    struct Frame {
        Id bus;
        std::size_t next;
    };
    std::vector<Frame> stack{{from, 0}};
    seen.insert(from);
    while (!stack.empty()) {
        Frame& f = stack.back();
        if (f.bus == to) {
            for (const Frame& k : stack) path.push_back(k.bus);
            return path;
        }
        const Bus* b = find_bus(e, f.bus);
        // Its sends, then its output (ADR-0024 §3), as edge sends.size().
        const std::size_t edges = b == nullptr ? 0 : b->sends.size() + 1;
        if (f.next >= edges) {
            stack.pop_back();
            continue;
        }
        const std::size_t k = f.next++;
        const Id next = k < b->sends.size() ? b->sends[k].bus : b->output;
        if (next == kMasterOutput) continue;
        if (seen.insert(next).second) stack.push_back({next, 0});
    }
    return path;
}

std::string bus_loop_problem(const EditState& e, std::string& where) {
    // Kahn's algorithm: whatever is left with an input after the sources
    // are peeled off is on a loop.
    std::unordered_map<Id, std::size_t> inputs;
    for (const Bus& b : e.buses) inputs.emplace(b.id, 0);
    for (const Bus& b : e.buses) {
        for (const Send& sd : b.sends) {
            const auto it = inputs.find(sd.bus);
            if (it != inputs.end()) ++it->second;
        }
        const auto out = inputs.find(b.output);
        if (b.output != kMasterOutput && out != inputs.end()) ++out->second;
    }
    std::vector<Id> ready;
    for (const Bus& b : e.buses) {
        if (inputs[b.id] == 0) ready.push_back(b.id);
    }
    std::size_t done = 0;
    while (!ready.empty()) {
        const Id id = ready.back();
        ready.pop_back();
        ++done;
        const Bus* b = find_bus(e, id);
        for (const Send& sd : b->sends) {
            const auto it = inputs.find(sd.bus);
            if (it != inputs.end() && --it->second == 0) ready.push_back(sd.bus);
        }
        const auto out = inputs.find(b->output);
        if (b->output != kMasterOutput && out != inputs.end() && --out->second == 0) {
            ready.push_back(b->output);
        }
    }
    if (done == e.buses.size()) {
        where.clear();
        return {};
    }
    for (std::size_t i = 0; i < e.buses.size(); ++i) {
        if (inputs[e.buses[i].id] != 0) {
            where = "buses[" + str(static_cast<std::int64_t>(i)) + "]";
            return "its sends and outputs make a feedback loop through the aux buses";
        }
    }
    where.clear();
    return "the sends and outputs make a feedback loop through the aux buses";
}

}  // namespace detail

bool check_invariants(const Snapshot& s, std::string& error) {
    using namespace detail;
    if (!s.edit || !s.sources || !s.edit->markers) {
        error = "the snapshot is missing its edit, sources or markers";
        return false;
    }
    std::string where;
    std::string why = settings_problem(s.settings, where);
    if (!why.empty()) {
        error = where + ": " + why;
        return false;
    }
    why = bus_problem(s.mix.bus, where);
    if (!why.empty()) {
        error = "monitor_bus." + where + ": " + why;
        return false;
    }

    std::unordered_set<Id> ids;
    const auto claim = [&](Id id, const std::string& path) {
        if (id > static_cast<Id>(kMaxInteger)) {
            error = path + ": id " + std::to_string(id) + " is out of range";
            return false;
        }
        if (id >= s.next_id) {
            error = path + ": id " + std::to_string(id) +
                    " is not below the id counter (" + std::to_string(s.next_id) + ")";
            return false;
        }
        if (!ids.insert(id).second) {
            error = path + ": id " + std::to_string(id) + " is used twice";
            return false;
        }
        return true;
    };

    const auto& sources = s.sources->sources;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const std::string path = "sources[" + std::to_string(i) + "]";
        why = source_problem(sources[i]);
        if (why.empty() && i > 0 && sources[i].id <= sources[i - 1].id) {
            why = "sources are not sorted by id";
        }
        if (!why.empty()) {
            error = path + ": " + why;
            return false;
        }
        if (!claim(sources[i].id, path)) return false;
    }
    const auto& midi_sources = s.sources->midi_sources;
    for (std::size_t i = 0; i < midi_sources.size(); ++i) {
        const std::string path = "midi_sources[" + std::to_string(i) + "]";
        why = midi_source_problem(midi_sources[i]);
        if (why.empty() && i > 0 && midi_sources[i].id <= midi_sources[i - 1].id) {
            why = "midi_sources are not sorted by id";
        }
        if (!why.empty()) {
            error = path + ": " + why;
            return false;
        }
        if (!claim(midi_sources[i].id, path)) return false;
    }

    // Sends name buses by id, so the buses are known before any send is
    // checked; their own ids are claimed with them, below.
    if (s.edit->buses.size() > kMaxBuses) {
        error = "buses: a session holds at most " + std::to_string(kMaxBuses) + " aux buses";
        return false;
    }
    std::unordered_set<Id> bus_ids;
    for (const Bus& b : s.edit->buses) bus_ids.insert(b.id);
    const auto check_sends = [&](const std::vector<Send>& sends, const std::string& path) {
        why = sends_problem(sends, where);
        if (!why.empty()) {
            error = path + "." + where + ": " + why;
            return false;
        }
        for (std::size_t i = 0; i < sends.size(); ++i) {
            const Send& sd = sends[i];
            const std::string spath = path + ".sends[" + std::to_string(i) + "]";
            if (!claim(sd.id, spath)) return false;
            if (bus_ids.count(sd.bus) == 0) {
                error = spath + ": bus " + std::to_string(sd.bus) + " is not an aux bus";
                return false;
            }
            const SendMix* sm = s.mix.find_send(sd.id);
            if (sm == nullptr) {
                error = spath + ": has no values (level, pan, mute, pre)";
                return false;
            }
            why = send_mix_problem(*sm, where);
            if (!why.empty()) {
                error = spath + "." + where + ": " + why;
                return false;
            }
        }
        return true;
    };

    const auto& tracks = s.edit->tracks;
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        const TrackEdit& tr = *tracks[t];
        const std::string path = "tracks[" + std::to_string(t) + "]";
        if (tr.id == 0 || !valid_text(tr.name)) {
            error = path + ": needs an id and a UTF-8 name";
            return false;
        }
        why = track_shape_problem(tr);
        if (!why.empty()) {
            error = path + ": " + why;
            return false;
        }
        if (!claim(tr.id, path)) return false;
        const TrackMix* mix = s.mix.find(tr.id);
        if (mix == nullptr) {
            error = path + ": has no mix";
            return false;
        }
        why = mix_problem(*mix, tr.kind, tr.channels, where);
        if (!why.empty()) {
            error = path + "." + where + ": " + why;
            return false;
        }
        why = inserts_problem(tr.inserts, where);
        if (!why.empty()) {
            error = path + "." + where + ": " + why;
            return false;
        }
        if (!check_sends(tr.sends, path)) return false;
        if (tr.output != kMasterOutput &&
            (tr.kind == TrackKind::Midi || bus_ids.count(tr.output) == 0)) {
            error = path + ".output: " +
                    (tr.kind == TrackKind::Midi
                         ? std::string("a MIDI track has no audio output")
                         : std::to_string(tr.output) + " is neither the Master (0) nor an aux bus");
            return false;
        }
        for (std::size_t i = 0; i < tr.inserts.size(); ++i) {
            const Insert& in = tr.inserts[i];
            const std::string ipath = path + ".inserts[" + std::to_string(i) + "]";
            if (!claim(in.id, ipath)) return false;
            const InsertMix* im = s.mix.find_insert(in.id);
            if (im == nullptr) {
                error = ipath + ": has no values (bypass, params, state)";
                return false;
            }
            why = insert_mix_problem(*im, in.plugin.format, where);
            if (!why.empty()) {
                error = ipath + "." + where + ": " + why;
                return false;
            }
        }
        bool active_found = false;
        for (std::size_t p = 0; p < tr.playlists.size(); ++p) {
            const Playlist& pl = *tr.playlists[p];
            const std::string ppath = path + ".playlists[" + std::to_string(p) + "]";
            active_found = active_found || pl.id == tr.active_playlist;
            why = playlist_problem(pl, *s.sources, tr.channels, where);
            if (!why.empty()) {
                error = ppath + (where.empty() ? "" : "." + where) + ": " + why;
                return false;
            }
            if (!claim(pl.id, ppath)) return false;
            for (std::size_t r = 0; r < pl.regions.size(); ++r) {
                if (!claim(pl.regions[r].id,
                           ppath + ".regions[" + std::to_string(r) + "]")) {
                    return false;
                }
            }
        }
        for (std::size_t p = 0; p < tr.midi_playlists.size(); ++p) {
            const MidiPlaylist& pl = *tr.midi_playlists[p];
            const std::string ppath = path + ".playlists[" + std::to_string(p) + "]";
            active_found = active_found || pl.id == tr.active_playlist;
            why = midi_playlist_problem(pl, *s.sources, true, where);
            if (!why.empty()) {
                error = ppath + (where.empty() ? "" : "." + where) + ": " + why;
                return false;
            }
            if (!claim(pl.id, ppath)) return false;
            for (std::size_t r = 0; r < pl.regions.size(); ++r) {
                if (!claim(pl.regions[r].id, ppath + ".regions[" + std::to_string(r) + "]")) {
                    return false;
                }
            }
        }
        if (!active_found) {
            error = path + ".active_playlist: names no playlist of this track";
            return false;
        }
    }

    const auto& buses = s.edit->buses;
    for (std::size_t b = 0; b < buses.size(); ++b) {
        const Bus& bus = buses[b];
        const std::string path = "buses[" + std::to_string(b) + "]";
        if (bus.id == 0 || !valid_text(bus.name)) {
            error = path + ": needs an id and a UTF-8 name";
            return false;
        }
        if (!claim(bus.id, path)) return false;
        const BusMix* bm = s.mix.find_bus(bus.id);
        if (bm == nullptr) {
            error = path + ": has no mix";
            return false;
        }
        why = bus_mix_problem(*bm, where);
        if (!why.empty()) {
            error = path + "." + where + ": " + why;
            return false;
        }
        if (!check_sends(bus.sends, path)) return false;
        for (std::size_t i = 0; i < bus.sends.size(); ++i) {
            if (bus.sends[i].bus == bus.id) {
                error = path + ".sends[" + std::to_string(i) + "]: a bus cannot send to itself";
                return false;
            }
        }
        if (bus.output != kMasterOutput && bus_ids.count(bus.output) == 0) {
            error = path + ".output: " + std::to_string(bus.output) +
                    " is neither the Master (0) nor an aux bus";
            return false;
        }
        if (bus.output == bus.id) {
            error = path + ".output: a bus cannot be its own output";
            return false;
        }
    }
    why = bus_loop_problem(*s.edit, where);
    if (!why.empty()) {
        error = (where.empty() ? std::string("buses") : where) + ": " + why;
        return false;
    }

    const auto& markers = s.edit->markers->markers;
    for (std::size_t i = 0; i < markers.size(); ++i) {
        const std::string path = "markers[" + std::to_string(i) + "]";
        why = marker_problem(markers[i]);
        if (why.empty() && i > 0 &&
            (markers[i].position < markers[i - 1].position ||
             (markers[i].position == markers[i - 1].position &&
              markers[i].id < markers[i - 1].id))) {
            why = "markers are not sorted by position";
        }
        if (!why.empty()) {
            error = path + ": " + why;
            return false;
        }
        if (!claim(markers[i].id, path)) return false;
    }
    return true;
}

}  // namespace sonora::model
