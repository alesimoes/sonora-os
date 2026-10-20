// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/history.h"

#include "sonora/model/op_codec.h"

#include <algorithm>
#include <limits>
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

// A note edit copies the whole sequence it touches (midi.md §3), so a drag
// in a region of 50,000 controller events is charged all of them.
std::size_t sequence_bytes(const MidiSequence& s) {
    std::size_t n = kNodeOverhead + sizeof(MidiSequence) + s.notes.size() * sizeof(midi::Note) +
                    s.events.size() * sizeof(MidiEvent) + s.sysex.size() * sizeof(MidiSysEx);
    for (const MidiSysEx& x : s.sysex) n += x.bytes.size();
    return n;
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

std::size_t exclusive_bytes(const EditState& state, const EditState& previous) {
    std::unordered_set<const void*> shared;
    for (const auto& t : previous.tracks) {
        shared.insert(t.get());
        for (const auto& p : t->playlists) shared.insert(p.get());
        for (const auto& p : t->midi_playlists) {
            shared.insert(p.get());
            for (const MidiRegion& r : p->regions) shared.insert(r.data.get());
        }
    }
    std::size_t n = kNodeOverhead + sizeof(EditState) +
                    state.tracks.size() * sizeof(std::shared_ptr<const TrackEdit>);
    for (const auto& t : state.tracks) {
        if (shared.count(t.get()) != 0) continue;
        n += track_bytes(*t);
        for (const auto& p : t->playlists) {
            if (shared.count(p.get()) == 0) n += playlist_bytes(*p);
        }
        for (const auto& p : t->midi_playlists) {
            if (shared.count(p.get()) != 0) continue;
            n += midi_playlist_bytes(*p);
            for (const MidiRegion& r : p->regions) {
                // A sequence two regions share (a split) is charged once.
                if (r.data && shared.insert(r.data.get()).second) n += sequence_bytes(*r.data);
            }
        }
    }
    if (state.markers != previous.markers && state.markers) {
        n += marker_bytes(*state.markers);
    }
    return n;
}

namespace {

template <typename Visit>
void each_sequence(const MidiPlaylist& p, Visit&& visit) {
    for (const MidiRegion& r : p.regions) {
        if (r.data) visit(r.data.get());
    }
}

}  // namespace

std::uint64_t state_midi_entries(const EditState& state) {
    std::unordered_set<const MidiSequence*> seen;
    std::uint64_t n = 0;
    for (const auto& t : state.tracks) {
        for (const auto& p : t->midi_playlists) {
            each_sequence(*p, [&](const MidiSequence* s) {
                if (seen.insert(s).second) n += midi_entries(*s);
            });
        }
    }
    return n;
}

std::uint64_t exclusive_midi_entries(const EditState& state, const EditState& previous) {
    std::unordered_set<const void*> shared;
    for (const auto& t : previous.tracks) {
        shared.insert(t.get());
        for (const auto& p : t->midi_playlists) {
            shared.insert(p.get());
            each_sequence(*p, [&](const MidiSequence* s) { shared.insert(s); });
        }
    }
    std::uint64_t n = 0;
    for (const auto& t : state.tracks) {
        if (shared.count(t.get()) != 0) continue;
        for (const auto& p : t->midi_playlists) {
            if (shared.count(p.get()) != 0) continue;
            each_sequence(*p, [&](const MidiSequence* s) {
                if (shared.insert(s).second) n += midi_entries(*s);
            });
        }
    }
    return n;
}

History::History(Snapshot initial, std::size_t budget_bytes, std::uint64_t midi_max_entries)
    : current_(std::move(initial)), budget_(budget_bytes), max_entries_(midi_max_entries) {
    const std::uint64_t entries = state_midi_entries(*current_.edit);
    steps_.push_back(Step{current_.edit, current_.revision, 0, {}, entries});
    entries_ = entries;
}

History History::replica(Snapshot initial) {
    return History(std::move(initial), std::numeric_limits<std::size_t>::max(),
                   std::numeric_limits<std::uint64_t>::max());
}

std::uint64_t History::current_midi_entries() const { return state_midi_entries(*current_.edit); }

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
        const std::size_t charge =
            i == 0 ? 0 : exclusive_bytes(*steps[i].edit, *h.steps_.back().edit);
        const std::uint64_t entries =
            i == 0 ? state_midi_entries(*steps[i].edit)
                   : exclusive_midi_entries(*steps[i].edit, *h.steps_.back().edit);
        h.steps_.push_back(Step{std::move(steps[i].edit), steps[i].revision, charge,
                                std::move(steps[i].label), entries});
        h.charged_ += charge;
        h.entries_ += entries;
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
    if (undoable && next.edit != current_.edit &&
        max_entries_ != std::numeric_limits<std::uint64_t>::max()) {
        const std::uint64_t n = state_midi_entries(*next.edit);
        if (n > max_entries_) {
            error = "the session would hold " + std::to_string(n) +
                    " MIDI notes and events, over the engine's midi_max_entries of " +
                    std::to_string(max_entries_) +
                    " even with no undo history kept; raise the setting, or edit less at once";
            return false;
        }
    }
    current_ = std::move(next);
    if (undoable) push(current_.edit, current_.revision, display_name(op));
    return true;
}

void History::push(std::shared_ptr<const EditState> edit, std::uint64_t revision,
                   std::string label) {
    // A new edit after an undo discards the redo branch, as every editor's
    // undo does.
    bool dropped = false;
    while (steps_.size() > pos_ + 1) {
        charged_ -= steps_.back().charge;
        entries_ -= steps_.back().entries;
        steps_.pop_back();
        dropped = true;
    }
    const std::size_t charge = exclusive_bytes(*edit, *steps_.back().edit);
    const std::uint64_t entries = exclusive_midi_entries(*edit, *steps_.back().edit);
    steps_.push_back(Step{std::move(edit), revision, charge, std::move(label), entries});
    charged_ += charge;
    entries_ += entries;
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
    steps_.front().entries = state_midi_entries(*steps_.front().edit);
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
