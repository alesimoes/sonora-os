// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Track menu: new, duplicate, delete and move.
#include "edit_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "edit_controller_detail.h"

namespace sonora::ui {

using namespace model;
using namespace edit_detail;

bool EditController::new_tracks(int count, std::string& why) {
    // The tracks and the reorder that places them, in one compound.
    constexpr int kMost = static_cast<int>(kMaxCompoundMembers) - 1;
    if (count < 1 || count > kMost) {
        why = "make 1 to " + std::to_string(kMost) + " tracks at a time";
        return false;
    }
    const Snapshot& s = snapshot();
    const auto& tracks = s.edit->tracks;
    std::vector<std::string> names;
    std::vector<std::uint16_t> inputs;
    for (const auto& t : tracks) {
        names.push_back(t->name);
        const TrackMix* m = s.mix.find(t->id);
        if (t->kind == TrackKind::Audio && m != nullptr && !m->input.empty()) {
            inputs.push_back(m->input.front());
        }
    }
    const auto has = [](const auto& list, const auto& v) {
        return std::find(list.begin(), list.end(), v) != list.end();
    };
    std::vector<Member> members;
    std::vector<Id> added;
    int number = 1;
    std::uint16_t input = 0;
    for (int i = 0; i < count; ++i) {
        std::string name;
        do {
            name = "Audio " + std::to_string(number++);
        } while (has(names, name));
        names.push_back(name);
        while (has(inputs, input)) ++input;
        inputs.push_back(input);
        AddTrack a;
        a.track = doc_.reserve_id();
        a.playlist = doc_.reserve_id();
        a.name = name;
        a.playlist_name = name + ".01";
        a.mix.input = {input};
        added.push_back(a.track);
        members.push_back(std::move(a));
    }
    const int n = static_cast<int>(tracks.size());
    const int at = sel_.track >= 0 && sel_.track < n ? sel_.track + 1 : n;
    if (at < n) {
        std::vector<Id> order = track_order(*s.edit);
        order.insert(order.begin() + at, added.begin(), added.end());
        members.push_back(ReorderTracks{std::move(order)});
    }
    if (!submit(Compound{std::move(members), count == 1 ? "New Track" : "New Tracks"}, why)) {
        return false;
    }
    select_track(at);
    return true;
}

bool EditController::duplicate_track(int index, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    DuplicateTrack d;
    d.source = t->id;
    d.name = duplicate_track_name(snapshot(), t->id);
    const std::size_t ids = duplicate_track_ids(snapshot(), d);
    // The copy's objects take the ids after `track`, one by one.
    d.track = doc_.reserve_id();
    for (std::size_t i = 1; i < ids; ++i) doc_.reserve_id();
    if (!submit(d, why)) return false;
    select_track(index + 1);
    return true;
}

bool EditController::delete_track(int index, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    if (!submit(RemoveTrack{t->id}, why)) return false;
    // The track that took its row, or the one above at the bottom; the
    // insertion point stays where it was.
    const int n = static_cast<int>(snapshot().edit->tracks.size());
    sel_.region = kNoId;
    sel_.start = sel_.end = sel_.insertion;
    sel_.tracks = 1;
    sel_.track = n > 0 ? std::min(index, n - 1) : -1;
    return true;
}

bool EditController::move_track(int index, int delta, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    const int to = index + delta;
    if (track(to) == nullptr) {
        why = "\"" + t->name + "\" is already the " + (delta < 0 ? "top" : "bottom") + " track";
        return false;
    }
    std::vector<Id> order = track_order(*snapshot().edit);
    std::swap(order[static_cast<std::size_t>(index)], order[static_cast<std::size_t>(to)]);
    const bool selected = sel_.track == index;
    if (!submit(ReorderTracks{std::move(order)}, why)) return false;
    if (selected) sel_.track = to;
    return true;
}

}  // namespace sonora::ui
