// SPDX-License-Identifier: GPL-3.0-or-later
//
// apply(): a compound folded member by member, every other operation
// handed to the Applier (apply_detail.h), one family's file per domain.
#include "apply_detail.h"

namespace sonora::model {

using namespace apply_detail;

namespace {

bool fold(const Snapshot& before, const Compound& c, Snapshot& after, std::string& error,
          const MemberCheck& check) {
    if (c.ops.empty()) {
        error = "a compound needs at least one operation; this one is empty";
        return false;
    }
    if (c.ops.size() > kMaxCompoundMembers) {
        error = "at most " + std::to_string(kMaxCompoundMembers) +
                " operations in a compound; this one has " + std::to_string(c.ops.size());
        return false;
    }
    if (!detail::valid_text(c.label)) {
        error = "the compound's label is not valid UTF-8";
        return false;
    }
    // The fold's own state: it shares everything with `before` until a
    // member copies something, and from then on that copy is the fold's
    // alone (Applier::edit_playlist).
    Snapshot work = before;
    for (std::size_t i = 0; i < c.ops.size(); ++i) {
        const Member& m = c.ops[i];
        std::string why;
        if (check) why = check(work, MemberRef(m));
        bool ok = why.empty();
        if (ok) {
            Snapshot next;
            Applier applier(work, next, why, true);
            ok = m.visit(applier);
            if (ok) work = std::move(next);
        }
        if (!ok) {
            error = "operation " + std::to_string(i + 1) + " of " +
                    std::to_string(c.ops.size()) + " (" + member_name(m) + "): " + why;
            return false;
        }
    }
    after = std::move(work);
    return true;
}

}  // namespace


// Both at family level: a whole family crosses at once, never leaf by leaf.
Operation to_operation(const Member& m) {
    return std::visit([](const auto& f) { return Operation(f); }, m.families());
}

bool to_member(Operation op, Member& out) {
    return std::visit(
        [&out](auto&& f) {
            if constexpr (std::is_same_v<std::decay_t<decltype(f)>, Compound>) {
                return false;
            } else {
                out = Member(std::move(f));
                return true;
            }
        },
        std::move(op).families());
}

namespace {

// Mix, plug-in values, sources and settings are outside edit undo.
bool leaf_is_undoable(MemberRef m) {
    return !(m.holds<AddSource>() || m.holds<SetMix>() || m.holds<SetMonitorBus>() ||
             m.holds<SetSettings>() || m.holds<SetInsertBypass>() ||
             m.holds<SetInsertParams>() || m.holds<SetInsertState>() ||
             m.holds<AddMidiSource>() || m.holds<SetMidiRouting>() ||
             m.holds<SetSendMix>() || m.holds<SetBusMix>() || m.holds<SetVcaMix>());
}

}  // namespace

bool member_is_undoable(const Member& m) { return leaf_is_undoable(MemberRef(m)); }

bool is_undoable(const Operation& op) {
    if (const auto* c = get_if<Compound>(&op)) {
        return std::any_of(c->ops.begin(), c->ops.end(), member_is_undoable);
    }
    return leaf_is_undoable(MemberRef(op));
}

std::size_t duplicate_track_ids(const Snapshot& s, const DuplicateTrack& op) {
    const std::size_t t = locate_track(*s.edit, op.source);
    if (t == kNone) return 0;
    const TrackEdit& src = *s.edit->tracks[t];
    std::size_t n = 1;
    const auto count = [&](const auto& playlists) {
        for (const auto& p : playlists) {
            if (op.all_playlists || p->id == src.active_playlist) n += 1 + p->regions.size();
        }
    };
    count(src.playlists);
    count(src.midi_playlists);
    if (op.inserts) n += src.inserts.size();
    if (op.sends) n += src.sends.size();
    return n;
}

std::size_t duplicate_playlist_ids(const Snapshot& s, const DuplicatePlaylist& op) {
    const std::size_t t = locate_track(*s.edit, op.track);
    if (t == kNone) return 0;
    const TrackEdit& track = *s.edit->tracks[t];
    for (const auto& p : track.playlists)
        if (p->id == op.source) return 1 + p->regions.size();
    for (const auto& p : track.midi_playlists)
        if (p->id == op.source) return 1 + p->regions.size();
    return 0;
}

bool playlist_name_taken(const Snapshot& s, const std::string& name, Id except) {
    for (const auto& t : s.edit->tracks) {
        for (const auto& p : t->playlists)
            if (p->id != except && p->name == name) return true;
        for (const auto& p : t->midi_playlists)
            if (p->id != except && p->name == name) return true;
    }
    return false;
}

std::string next_playlist_name(const Snapshot& s, Id track) {
    const std::size_t t = locate_track(*s.edit, track);
    if (t == kNone) return {};
    const std::string& base = s.edit->tracks[t]->name;
    for (std::uint64_t n = 1;; ++n) {
        std::string number = std::to_string(n);
        if (number.size() < 2) number.insert(0, 1, '0');
        std::string name = base + "." + number;
        if (!playlist_name_taken(s, name)) return name;
    }
}

std::string duplicate_track_name(const Snapshot& s, Id source) {
    const std::size_t t = locate_track(*s.edit, source);
    if (t == kNone) return {};
    const std::string& base = s.edit->tracks[t]->name;
    for (std::uint64_t n = 1;; ++n) {
        std::string name = base + ".dup" + std::to_string(n);
        const bool taken = std::any_of(s.edit->tracks.begin(), s.edit->tracks.end(),
                                       [&](const auto& k) { return k->name == name; });
        if (!taken) return name;
    }
}

std::vector<Id> track_order(const EditState& e) {
    std::vector<Id> ids;
    ids.reserve(e.tracks.size());
    for (const auto& t : e.tracks) ids.push_back(t->id);
    return ids;
}

bool track_order_after(const Snapshot& at, MemberRef member, std::vector<Id>& order) {
    if (const auto* a = member.get_if<AddTrack>()) {
        order = track_order(*at.edit);
        order.push_back(a->track);
        return true;
    }
    if (const auto* r = member.get_if<RemoveTrack>()) {
        const std::size_t t = locate_track(*at.edit, r->track);
        if (t == kNone) return false;
        order = track_order(*at.edit);
        order.erase(order.begin() + static_cast<std::ptrdiff_t>(t));
        return true;
    }
    if (const auto* r = member.get_if<ReorderTracks>()) {
        if (r->order.size() != at.edit->tracks.size()) return false;
        order = r->order;
        return true;
    }
    if (const auto* d = member.get_if<DuplicateTrack>()) {
        const std::size_t t = locate_track(*at.edit, d->source);
        if (t == kNone) return false;
        order = track_order(*at.edit);
        order.insert(order.begin() + static_cast<std::ptrdiff_t>(t) + 1, d->track);
        return true;
    }
    return false;
}

std::string pinned_track_refusal(const EditState& before, const std::vector<Id>& after,
                                 const std::vector<bool>& pinned, const std::string& doing,
                                 const std::string& remedy) {
    for (std::size_t i = 0; i < pinned.size() && i < before.tracks.size(); ++i) {
        if (!pinned[i]) continue;
        const TrackEdit& t = *before.tracks[i];
        if (i < after.size() && after[i] == t.id) continue;
        const bool kept = std::find(after.begin(), after.end(), t.id) != after.end();
        return "\"" + t.name + "\" " + doing + ": " + remedy +
               (kept ? " before moving it, or a track above it" : " before deleting it");
    }
    return {};
}

std::string take_starting_track_refusal() {
    return "a take is about to start: add, delete or move tracks before Record or after Stop";
}

std::string take_closing_track_refusal(int take) {
    return "take " + std::to_string(take) +
           " is still closing: add, delete or move tracks once it is in the session";
}

std::vector<Id> active_playlist_changes(const EditState& before, const EditState& after) {
    std::vector<Id> changed;
    for (const auto& t : before.tracks) {
        const std::size_t a = locate_track(after, t->id);
        if (a != kNone && after.tracks[a]->active_playlist != t->active_playlist) {
            changed.push_back(t->id);
        }
    }
    return changed;
}

std::vector<Id> active_playlist_changes(const Snapshot& at, MemberRef member) {
    const auto* set = member.get_if<SetActivePlaylist>();
    if (set == nullptr) return {};
    const std::size_t t = locate_track(*at.edit, set->track);
    if (t == kNone || at.edit->tracks[t]->active_playlist == set->playlist) return {};
    return {set->track};
}

std::string pinned_playlist_refusal(const EditState& before, const std::vector<Id>& changed,
                                    const std::vector<bool>& pinned) {
    for (std::size_t i = 0; i < pinned.size() && i < before.tracks.size(); ++i) {
        if (!pinned[i]) continue;
        const TrackEdit& t = *before.tracks[i];
        if (std::find(changed.begin(), changed.end(), t.id) == changed.end()) continue;
        return "\"" + t.name + "\" is recording: stop recording before choosing another playlist";
    }
    return {};
}

std::string take_starting_playlist_refusal() {
    return "a take is about to start: choose another playlist before Record or after Stop";
}

std::string take_closing_playlist_refusal(int take) {
    return "take " + std::to_string(take) +
           " is still closing: choose another playlist once it is in the session";
}

bool apply(const Snapshot& before, const Operation& op, Snapshot& after,
           std::string& error, const MemberCheck& check) {
    Snapshot result;
    if (const auto* c = get_if<Compound>(&op)) {
        if (!fold(before, *c, result, error, check)) return false;
    } else {
        if (check) {
            std::string why = check(before, MemberRef(op));
            if (!why.empty()) {
                error = std::move(why);
                return false;
            }
        }
        Applier applier(before, result, error, false);
        if (!op.visit(applier)) return false;
    }
    result.revision = before.revision + 1;
    after = std::move(result);
    return true;
}

}  // namespace sonora::model
