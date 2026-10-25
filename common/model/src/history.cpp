// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/history.h"

#include "sonora/model/op_codec.h"
#include "sonora/model/sequence_file.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace sonora::model {

namespace {

// A shared_ptr's control block and the allocator's own header. The figure
// only has to be the same on every run: a replica follows the engine's
// floor rather than its own count (ADR-0009 §1).
constexpr std::size_t kNodeOverhead = 32;

std::size_t playlist_bytes(const Playlist& p) {
    std::size_t n = kNodeOverhead + sizeof(Playlist) + p.name.size() +
                    p.regions.size() * sizeof(Region) +
                    p.crossfades.size() * sizeof(Crossfade);
    for (const Region& r : p.regions) {
        n += r.name.size() + r.channels.size() * sizeof(SourceRef);
    }
    return n;
}

std::size_t midi_playlist_bytes(const MidiPlaylist& p) {
    std::size_t n = kNodeOverhead + sizeof(MidiPlaylist) + p.name.size() +
                    p.regions.size() * sizeof(MidiRegion);
    for (const MidiRegion& r : p.regions) n += r.name.size();
    return n;
}

// Fixed sizes rather than sizeof, so a MIDI charge is the same on every
// compiler and allocator (ADR-0015 §2). A spine entry is a pointer and three
// counts, with its chunk or leaf index.
constexpr std::size_t kNoteBytes = 24;
constexpr std::size_t kEventBytes = 16;
constexpr std::size_t kSysexBytes = 32;   // and its own bytes
constexpr std::size_t kChunkBytes = 80;
constexpr std::size_t kSpineEntryBytes = 40;
constexpr std::size_t kLeafBytes = 48;
constexpr std::size_t kSequenceBytes = 64;

std::size_t chunk_bytes(const MidiChunk& c) {
    std::size_t n = kNodeOverhead + kChunkBytes + c.data.notes.size() * kNoteBytes +
                    c.data.events.size() * kEventBytes + c.data.sysex.size() * kSysexBytes;
    for (const MidiSysEx& x : c.data.sysex) n += x.bytes.size();
    return n;
}

std::size_t leaf_bytes(const MidiSpineLeaf& l) {
    return kNodeOverhead + kLeafBytes + l.chunks.size() * kSpineEntryBytes;
}

std::size_t sequence_bytes(const MidiSequence& s) {
    return kNodeOverhead + kSequenceBytes + s.spine().size() * kSpineEntryBytes;
}

std::size_t track_bytes(const TrackEdit& t) {
    std::size_t n = kNodeOverhead + sizeof(TrackEdit) + t.name.size() +
                    t.playlists.size() * sizeof(std::shared_ptr<const Playlist>) +
                    t.midi_playlists.size() * sizeof(std::shared_ptr<const MidiPlaylist>) +
                    t.inserts.size() * sizeof(Insert);
    for (const Insert& in : t.inserts) {
        n += in.plugin.id.size() + in.plugin.name.size() + in.plugin.version.size();
    }
    return n;
}

std::size_t marker_bytes(const MarkerList& m) {
    std::size_t n = kNodeOverhead + sizeof(MarkerList) + m.markers.size() * sizeof(Marker);
    for (const Marker& k : m.markers) n += k.name.size();
    return n;
}

}  // namespace

namespace {

template <typename Visit>
void each_sequence(const MidiPlaylist& p, Visit&& visit) {
    for (const MidiRegion& r : p.regions) {
        if (r.data) visit(*r.data);
    }
}

// What a state holds, for telling what another state shares with it. A
// chunk lives at one index for as long as it lives, the one its contents
// fix, so it is looked for only among the leaves at that index.
class Held {
public:
    explicit Held(const EditState& state) {
        for (const auto& t : state.tracks) {
            objects_.insert(t.get());
            for (const auto& p : t->playlists) objects_.insert(p.get());
            for (const auto& p : t->midi_playlists) {
                if (!objects_.insert(p.get()).second) continue;
                each_sequence(*p, [&](const MidiSequence& s) {
                    if (!objects_.insert(&s).second) return;
                    for (const MidiSpineNode& n : s.spine()) {
                        if (objects_.insert(n.leaf.get()).second) {
                            leaves_[n.leaf->index].push_back(n.leaf.get());
                        }
                    }
                });
            }
        }
    }

    bool has(const void* p) const { return objects_.count(p) != 0; }

    bool has_chunk(const MidiChunk* c) const {
        const auto it = leaves_.find(c->index / kMidiLeafChunks);
        if (it == leaves_.end()) return false;
        for (const MidiSpineLeaf* l : it->second) {
            const auto e = std::lower_bound(
                l->chunks.begin(), l->chunks.end(), c->index,
                [](const MidiSpineEntry& x, std::int64_t k) { return x.chunk->index < k; });
            if (e != l->chunks.end() && e->chunk.get() == c) return true;
        }
        return false;
    }

private:
    std::unordered_set<const void*> objects_;
    std::unordered_map<std::int64_t, std::vector<const MidiSpineLeaf*>> leaves_;
};

struct Own {
    std::size_t bytes = 0;
    std::uint64_t midi_entries = 0;
};

// What `state` allocated that `previous` does not share with it.
Own own(const EditState& state, const EditState& previous, std::string* oversized = nullptr) {
    const Held shared(previous);
    // What `state` holds twice is counted once.
    std::unordered_set<const void*> seen;
    Own o;
    o.bytes = kNodeOverhead + sizeof(EditState) +
              state.tracks.size() * sizeof(std::shared_ptr<const TrackEdit>);
    for (const auto& t : state.tracks) {
        if (shared.has(t.get()) || !seen.insert(t.get()).second) continue;
        o.bytes += track_bytes(*t);
        for (const auto& p : t->playlists) {
            if (!shared.has(p.get()) && seen.insert(p.get()).second) o.bytes += playlist_bytes(*p);
        }
        for (const auto& p : t->midi_playlists) {
            if (shared.has(p.get()) || !seen.insert(p.get()).second) continue;
            o.bytes += midi_playlist_bytes(*p);
            each_sequence(*p, [&](const MidiSequence& s) {
                // A sequence two regions share (a split) is charged once.
                if (shared.has(&s) || !seen.insert(&s).second) return;
                o.bytes += sequence_bytes(s);
                for (const MidiSpineNode& n : s.spine()) {
                    const MidiSpineLeaf* l = n.leaf.get();
                    if (shared.has(l) || !seen.insert(l).second) continue;
                    o.bytes += leaf_bytes(*l);
                    for (const MidiSpineEntry& e : l->chunks) {
                        const MidiChunk* c = e.chunk.get();
                        if (shared.has_chunk(c) || !seen.insert(c).second) continue;
                        o.bytes += chunk_bytes(*c);
                        o.midi_entries += c->data.entries();
                        if (oversized != nullptr && oversized->empty()) {
                            *oversized = oversized_chunk(*c, t->name);
                        }
                    }
                }
            });
        }
    }
    if (state.markers != previous.markers && state.markers) {
        o.bytes += marker_bytes(*state.markers);
    }
    return o;
}

}  // namespace

std::size_t exclusive_bytes(const EditState& state, const EditState& previous) {
    return own(state, previous).bytes;
}

std::uint64_t state_midi_entries(const EditState& state) {
    std::unordered_set<const void*> seen;
    std::uint64_t n = 0;
    for (const auto& t : state.tracks) {
        for (const auto& p : t->midi_playlists) {
            if (!seen.insert(p.get()).second) continue;
            each_sequence(*p, [&](const MidiSequence& s) {
                if (!seen.insert(&s).second) return;
                for (const MidiSpineNode& node : s.spine()) {
                    if (!seen.insert(node.leaf.get()).second) continue;
                    for (const MidiSpineEntry& e : node.leaf->chunks) {
                        if (seen.insert(e.chunk.get()).second) n += e.chunk->data.entries();
                    }
                }
            });
        }
    }
    return n;
}

std::uint64_t exclusive_midi_entries(const EditState& state, const EditState& previous) {
    return own(state, previous).midi_entries;
}

History::Cost History::cost(const EditState& next, const Step& previous, std::string* oversized) {
    const Own fresh = own(next, *previous.edit, oversized);
    const Own gone = own(*previous.edit, next);
    Cost c;
    c.charge = fresh.bytes;
    c.entries = fresh.midi_entries;
    c.total = previous.total + fresh.midi_entries - gone.midi_entries;
    return c;
}

History::History(Snapshot initial, std::size_t budget_bytes, std::uint64_t midi_max_entries)
    : current_(std::move(initial)), budget_(budget_bytes), max_entries_(midi_max_entries) {
    const std::uint64_t entries = state_midi_entries(*current_.edit);
    steps_.push_back(Step{current_.edit, current_.revision, 0, {}, entries, entries});
    entries_ = entries;
}

History History::replica(Snapshot initial) {
    return History(std::move(initial), std::numeric_limits<std::size_t>::max(),
                   std::numeric_limits<std::uint64_t>::max());
}

std::uint64_t History::current_midi_entries() const { return steps_[pos_].total; }

bool History::restore(Snapshot current, std::vector<WindowStep> steps, std::size_t position,
                      std::optional<History>& out, std::string& error) {
    if (steps.empty() || position >= steps.size()) {
        error = "the undo window has no step at its position";
        return false;
    }
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (!steps[i].edit) {
            error = "undo step " + std::to_string(i) + " has no state";
            return false;
        }
        if (i > 0 && steps[i].revision <= steps[i - 1].revision) {
            error = "the undo window's revisions do not rise";
            return false;
        }
    }
    if (current.edit != steps[position].edit) {
        error = "the current state is not the undo window's at its position";
        return false;
    }
    History h = replica(std::move(current));
    h.steps_.clear();
    h.charged_ = 0;
    h.entries_ = 0;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        Cost c;
        if (i == 0) {
            c.entries = c.total = state_midi_entries(*steps[i].edit);
        } else {
            c = cost(*steps[i].edit, h.steps_.back());
        }
        h.steps_.push_back(Step{std::move(steps[i].edit), steps[i].revision, c.charge,
                                std::move(steps[i].label), c.entries, c.total});
        h.charged_ += c.charge;
        h.entries_ += c.entries;
    }
    h.pos_ = position;
    out.emplace(std::move(h));
    return true;
}

bool History::drop_oldest() {
    if (steps_.size() < 2 || pos_ == 0) return false;
    pop_front();
    prune_insert_values();
    return true;
}

void History::prune_insert_values() {
    std::vector<InsertMix>& values = current_.mix.inserts;
    if (values.empty()) return;
    std::unordered_set<Id> live;
    std::unordered_set<const void*> seen;
    for (const Step& s : steps_) {
        for (const auto& t : s.edit->tracks) {
            // Steps share most tracks; each is walked once.
            if (!seen.insert(t.get()).second) continue;
            for (const Insert& in : t->inserts) live.insert(in.id);
        }
    }
    values.erase(std::remove_if(values.begin(), values.end(),
                                [&](const InsertMix& m) { return live.count(m.insert) == 0; }),
                 values.end());
}

bool History::apply(const Operation& op, std::string& error, const MemberCheck& check) {
    Snapshot next;
    if (!model::apply(current_, op, next, error, check)) return false;
    const bool undoable = is_undoable(op);
    if (!undoable) {
        current_ = std::move(next);
        return true;
    }
    std::string oversized;
    const Cost c = cost(*next.edit, steps_[pos_], &oversized);
    if (!oversized.empty()) {
        error = oversized;
        return false;
    }
    if (next.edit != current_.edit && max_entries_ != std::numeric_limits<std::uint64_t>::max()) {
        const std::uint64_t n = c.total;
        if (n > max_entries_) {
            error = "the session would hold " + std::to_string(n) +
                    " MIDI notes and events, over the engine's midi_max_entries of " +
                    std::to_string(max_entries_) +
                    " even with no undo history kept; raise the setting, or edit less at once";
            return false;
        }
    }
    current_ = std::move(next);
    push(current_.edit, current_.revision, display_name(op), c);
    return true;
}

void History::push(std::shared_ptr<const EditState> edit, std::uint64_t revision,
                   std::string label, const Cost& c) {
    // A new edit after an undo discards the redo branch, as every editor's
    // undo does.
    bool dropped = false;
    while (steps_.size() > pos_ + 1) {
        charged_ -= steps_.back().charge;
        entries_ -= steps_.back().entries;
        steps_.pop_back();
        dropped = true;
    }
    // `c` was counted against the state at the pointer, which is now the
    // last step.
    steps_.push_back(Step{std::move(edit), revision, c.charge, std::move(label), c.entries,
                          c.total});
    charged_ += c.charge;
    entries_ += c.entries;
    pos_ = steps_.size() - 1;
    // Two states always stay, the newest and the one before it, so the
    // last edit can be undone however large it was.
    while (charged_ > budget_ && steps_.size() > 2) {
        pop_front();
        dropped = true;
    }
    // MIDI's budget lets the last edit's undo go too (ADR-0015 §5): an edit
    // or a take refused while history it could drop is kept would be worse.
    // apply() has already refused a state over it on its own.
    while (entries_ > max_entries_ && steps_.size() > 1) {
        pop_front();
        dropped = true;
    }
    if (dropped) prune_insert_values();
}

void History::pop_front() {
    entries_ -= steps_.front().entries;
    steps_.pop_front();
    // The new floor becomes the base the others are counted against.
    charged_ -= steps_.front().charge;
    steps_.front().charge = 0;
    entries_ -= steps_.front().entries;
    steps_.front().entries = steps_.front().total;
    entries_ += steps_.front().entries;
    --pos_;
}

namespace {
const std::string kNoLabel;
}  // namespace

// Undo leaves the step at the pointer; redo enters the one after it.
const std::string& History::undo_label() const {
    return can_undo() ? steps_[pos_].label : kNoLabel;
}

const std::string& History::redo_label() const {
    return can_redo() ? steps_[pos_ + 1].label : kNoLabel;
}

bool History::undo() {
    if (!can_undo()) return false;
    --pos_;
    current_.edit = steps_[pos_].edit;
    ++current_.revision;
    return true;
}

bool History::redo() {
    if (!can_redo()) return false;
    ++pos_;
    current_.edit = steps_[pos_].edit;
    ++current_.revision;
    return true;
}

void History::evict_below(std::uint64_t floor) {
    bool dropped = false;
    while (steps_.size() > 1 && pos_ > 0 && steps_.front().revision < floor) {
        pop_front();
        dropped = true;
    }
    if (dropped) prune_insert_values();
}

}  // namespace sonora::model
