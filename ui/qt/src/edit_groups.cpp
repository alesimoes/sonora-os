// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mix and edit groups (ADR-0041) on the Edit controller: Pro Tools' Group
// and Ungroup, Suspend, and the one place a fader, mute or solo move is
// expanded to the members its group links. Creating, changing and dissolving
// a group are one operation and one undo step each; a move is one Compound
// of SetMix, so the engine hears the group move together.
#include <algorithm>

#include "edit_controller.h"
#include "fader_taper.h"
#include "sonora/model/groups.h"

namespace sonora::ui {

using namespace model;

namespace {

std::string no_track(int index) {
    return index < 0 ? "no track is selected" : "there is no track " + std::to_string(index + 1);
}

}  // namespace

int EditController::group_index(int index) const {
    const TrackEdit* t = track(index);
    if (t == nullptr) return -1;
    const auto& groups = snapshot().edit->groups;
    for (std::size_t g = 0; g < groups.size(); ++g) {
        const auto& m = groups[g].members;
        if (std::find(m.begin(), m.end(), t->id) != m.end()) return static_cast<int>(g);
    }
    return -1;
}

bool EditController::submit_mix(const TrackMix& m, std::string& why) {
    std::vector<TrackMix> followers = group_follow(snapshot(), m);
    if (followers.empty()) return submit(SetMix{m}, why);
    // The fader's own range, not the model's wider one: a relative move
    // must not push a peer past the end its fader can reach.
    for (TrackMix& f : followers) {
        const TrackMix* now = snapshot().mix.find(f.track);
        if (now == nullptr) continue;
        f.gain_db = std::clamp(f.gain_db, std::min(kFaderSilenceDb, now->gain_db),
                               std::max(kFaderMaxDb, now->gain_db));
    }
    std::vector<Member> members;
    members.push_back(SetMix{m});
    for (const TrackMix& f : followers) members.push_back(SetMix{f});
    return submit_compound(std::move(members), "Group Mix", "changed", why);
}

bool EditController::group_tracks(int first, int count, const std::string& name_in,
                                  std::uint8_t attrs, std::string& why) {
    const auto& tracks = snapshot().edit->tracks;
    if (track(first) == nullptr) {
        why = no_track(first);
        return false;
    }
    const int last = std::min(first + std::max(count, 1), static_cast<int>(tracks.size()));
    if (last - first < 2) {
        why = "select two or more tracks to group; a group of one links nothing";
        return false;
    }
    const auto& groups = snapshot().edit->groups;
    if (groups.size() >= kMaxGroups) {
        why = "a session holds at most " + std::to_string(kMaxGroups) + " groups";
        return false;
    }
    SetGroup op;
    for (int i = first; i < last; ++i) {
        const TrackEdit& t = *tracks[static_cast<std::size_t>(i)];
        const int g = group_index(i);
        if (g >= 0) {
            why = "\"" + t.name + "\" is already in group \"" +
                  groups[static_cast<std::size_t>(g)].name +
                  "\"; a track is in one group at most. Dissolve that group first. Nothing was grouped.";
            return false;
        }
        op.members.push_back(t.id);
    }
    std::string name = name_in;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (name.empty()) {
        for (int n = 1;; ++n) {
            name = "Group " + std::to_string(n);
            const bool taken = std::any_of(groups.begin(), groups.end(),
                                           [&](const Group& g) { return g.name == name; });
            if (!taken) break;
        }
    } else if (std::any_of(groups.begin(), groups.end(),
                           [&](const Group& g) { return g.name == name; })) {
        why = "another group is already named \"" + name + "\"";
        return false;
    }
    if (attrs == 0 || (attrs & ~kGroupAllAttributes) != 0) {
        why = "a group needs at least one attribute to link";
        return false;
    }
    op.group = snapshot().next_id;
    op.name = name;
    op.attrs = attrs;
    return submit(op, why);
}

bool EditController::dissolve_group(int index, std::string& why) {
    const int g = group_index(index);
    if (g < 0) {
        why = track(index) == nullptr ? no_track(index)
                                      : "\"" + track(index)->name + "\" is not in a group";
        return false;
    }
    return submit(RemoveGroup{snapshot().edit->groups[static_cast<std::size_t>(g)].id}, why);
}

namespace {

SetGroup as_op(const Group& g) {
    SetGroup op;
    op.group = g.id;
    op.name = g.name;
    op.members = g.members;
    op.attrs = g.attrs;
    op.relative = g.relative;
    op.active = g.active;
    return op;
}

}  // namespace

bool EditController::toggle_group_suspend(int index, std::string& why) {
    const int g = group_index(index);
    if (g < 0) {
        why = track(index) == nullptr ? no_track(index)
                                      : "\"" + track(index)->name + "\" is not in a group";
        return false;
    }
    SetGroup op = as_op(snapshot().edit->groups[static_cast<std::size_t>(g)]);
    op.active = !op.active;
    return submit(op, why);
}

bool EditController::set_group_relative(int index, bool relative, std::string& why) {
    const int g = group_index(index);
    if (g < 0) {
        why = track(index) == nullptr ? no_track(index)
                                      : "\"" + track(index)->name + "\" is not in a group";
        return false;
    }
    SetGroup op = as_op(snapshot().edit->groups[static_cast<std::size_t>(g)]);
    if (op.relative == relative) return true;
    op.relative = relative;
    return submit(op, why);
}

bool EditController::toggle_group_attribute(int index, std::uint8_t attr, std::string& why) {
    const int g = group_index(index);
    if (g < 0) {
        why = track(index) == nullptr ? no_track(index)
                                      : "\"" + track(index)->name + "\" is not in a group";
        return false;
    }
    SetGroup op = as_op(snapshot().edit->groups[static_cast<std::size_t>(g)]);
    op.attrs = static_cast<std::uint8_t>(op.attrs ^ attr);
    if (op.attrs == 0) {
        why = "a group needs at least one attribute to link; dissolve it instead";
        return false;
    }
    return submit(op, why);
}

}  // namespace sonora::ui
