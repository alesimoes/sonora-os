// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/history.h"

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

std::size_t track_bytes(const TrackEdit& t) {
    std::size_t n = kNodeOverhead + sizeof(TrackEdit) + t.name.size() +
                    t.playlists.size() * sizeof(std::shared_ptr<const Playlist>) +
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
    }
    std::size_t n = kNodeOverhead + sizeof(EditState) +
                    state.tracks.size() * sizeof(std::shared_ptr<const TrackEdit>);
    for (const auto& t : state.tracks) {
        if (shared.count(t.get()) != 0) continue;
        n += track_bytes(*t);
        for (const auto& p : t->playlists) {
            if (shared.count(p.get()) == 0) n += playlist_bytes(*p);
        }
    }
    if (state.markers != previous.markers && state.markers) {
        n += marker_bytes(*state.markers);
    }
    return n;
}

History::History(Snapshot initial, std::size_t budget_bytes)
    : current_(std::move(initial)), budget_(budget_bytes) {
    steps_.push_back(Step{current_.edit, current_.revision, 0});
}

History History::replica(Snapshot initial) {
    return History(std::move(initial), std::numeric_limits<std::size_t>::max());
}

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
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const std::size_t charge =
            i == 0 ? 0 : exclusive_bytes(*steps[i].edit, *h.steps_.back().edit);
        h.steps_.push_back(Step{std::move(steps[i].edit), steps[i].revision, charge});
        h.charged_ += charge;
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
    current_ = std::move(next);
    if (undoable) push(current_.edit, current_.revision);
    return true;
}

void History::push(std::shared_ptr<const EditState> edit, std::uint64_t revision) {
    // A new edit after an undo discards the redo branch, as every editor's
    // undo does.
    bool dropped = false;
    while (steps_.size() > pos_ + 1) {
        charged_ -= steps_.back().charge;
        steps_.pop_back();
        dropped = true;
    }
    const std::size_t charge = exclusive_bytes(*edit, *steps_.back().edit);
    steps_.push_back(Step{std::move(edit), revision, charge});
    charged_ += charge;
    pos_ = steps_.size() - 1;
    // Two states always stay, the newest and the one before it, so the
    // last edit can be undone however large it was.
    while (charged_ > budget_ && steps_.size() > 2) {
        pop_front();
        dropped = true;
    }
    if (dropped) prune_insert_values();
}

void History::pop_front() {
    steps_.pop_front();
    // The new floor becomes the base the others are counted against.
    charged_ -= steps_.front().charge;
    steps_.front().charge = 0;
    --pos_;
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
