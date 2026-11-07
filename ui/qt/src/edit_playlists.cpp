// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playlists of a track: new, duplicate, choose, rename, delete unused.
#include "edit_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "edit_controller_detail.h"

namespace sonora::ui {

using namespace model;
using namespace edit_detail;

namespace {

// A track's playlists of its kind, as (id, name), in their order.
std::vector<std::pair<Id, std::string>> playlists_of(const TrackEdit& t) {
    std::vector<std::pair<Id, std::string>> out;
    for (const auto& p : t.playlists) out.emplace_back(p->id, p->name);
    for (const auto& p : t.midi_playlists) out.emplace_back(p->id, p->name);
    return out;
}

}  // namespace

std::vector<std::string> EditController::playlist_names(int index) const {
    std::vector<std::string> out;
    if (const TrackEdit* t = track(index)) {
        for (auto& [id, name] : playlists_of(*t)) out.push_back(std::move(name));
    }
    return out;
}

std::string EditController::active_playlist_name(int index) const {
    const TrackEdit* t = track(index);
    if (t == nullptr) return {};
    for (auto& [id, name] : playlists_of(*t))
        if (id == t->active_playlist) return name;
    return {};
}

bool EditController::new_playlist(int index, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    const Id id = doc_.reserve_id();
    const Id on = t->id;
    std::vector<Member> members;
    members.push_back(AddPlaylist{on, id, next_playlist_name(snapshot(), on)});
    members.push_back(SetActivePlaylist{on, id});
    if (!submit(Compound{std::move(members), "New Playlist"}, why)) return false;
    if (sel_.track == index) sel_.region = kNoId;
    return true;
}

bool EditController::duplicate_playlist(int index, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    DuplicatePlaylist d;
    d.track = t->id;
    d.source = t->active_playlist;
    d.name = next_playlist_name(snapshot(), t->id);
    const std::size_t ids = duplicate_playlist_ids(snapshot(), d);
    // The copy's regions take the ids after the copy's, one by one.
    d.playlist = doc_.reserve_id();
    for (std::size_t i = 1; i < ids; ++i) doc_.reserve_id();
    const Id copy = d.playlist;
    const Id on = d.track;
    std::vector<Member> members;
    members.push_back(std::move(d));
    members.push_back(SetActivePlaylist{on, copy});
    if (!submit(Compound{std::move(members), "Duplicate Playlist"}, why)) return false;
    if (sel_.track == index) sel_.region = kNoId;
    return true;
}

bool EditController::choose_playlist(int index, const std::string& name, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    for (const auto& [id, n] : playlists_of(*t)) {
        if (n != name) continue;
        if (id == t->active_playlist) return true;
        if (!submit(SetActivePlaylist{t->id, id}, why)) return false;
        if (sel_.track == index) sel_.region = kNoId;
        return true;
    }
    why = "\"" + t->name + "\" has no playlist named \"" + name + "\"";
    return false;
}

bool EditController::rename_playlist(int index, const std::string& name, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    if (name.empty()) {
        why = "a playlist needs a name";
        return false;
    }
    if (name == active_playlist_name(index)) return true;
    if (playlist_name_taken(snapshot(), name, t->active_playlist)) {
        why = "another playlist is already named \"" + name + "\"";
        return false;
    }
    return submit(SetPlaylistName{t->id, t->active_playlist, name}, why);
}

bool EditController::delete_unused_playlists(int index, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    std::vector<Member> members;
    for (const auto& [id, n] : playlists_of(*t)) {
        if (id != t->active_playlist) members.push_back(RemovePlaylist{t->id, id});
    }
    if (members.empty()) {
        why = "\"" + t->name + "\" has no unused playlists";
        return false;
    }
    // Past the compound limit, the rest go on the next press.
    if (members.size() > kMaxCompoundMembers) members.resize(kMaxCompoundMembers);
    return submit(Compound{std::move(members), "Delete Unused Playlists"}, why);
}

}  // namespace sonora::ui
