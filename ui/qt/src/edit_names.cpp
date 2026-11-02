// SPDX-License-Identifier: GPL-3.0-or-later
//
// Names and colours of tracks and regions (ADR-0023, update of 2026-11-02):
// Pro Tools' Rename and Color Palette, each change one operation and one
// undo step.
#include <algorithm>

#include "edit_controller.h"

namespace sonora::ui {

using namespace model;

namespace {

std::string trimmed(const std::string& s) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    std::size_t a = 0, b = s.size();
    while (a < b && space(s[a])) ++a;
    while (b > a && space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string no_track(int index) {
    return index < 0 ? "no track is selected" : "there is no track " + std::to_string(index + 1);
}

}  // namespace

bool EditController::rename_track(int index, const std::string& name, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    const std::string n = trimmed(name);
    if (n.empty()) {
        why = "a track needs a name; \"" + t->name + "\" keeps its own";
        return false;
    }
    if (n == t->name) return true;
    const auto& tracks = snapshot().edit->tracks;
    const bool taken = std::any_of(tracks.begin(), tracks.end(), [&](const auto& k) {
        return k->id != t->id && k->name == n;
    });
    if (taken) {
        why = "another track is already named \"" + n + "\"";
        return false;
    }
    return submit(SetTrackName{t->id, n}, why);
}

bool EditController::set_track_colour(int index, int count, int colour, std::string& why) {
    if (track(index) == nullptr) {
        why = no_track(index);
        return false;
    }
    if (colour < 0 || colour >= kColours) {
        why = "the palette has colours 1 to " + std::to_string(kColours);
        return false;
    }
    const auto& tracks = snapshot().edit->tracks;
    const int last = std::min(index + std::max(count, 1), static_cast<int>(tracks.size()));
    std::vector<Member> members;
    for (int i = index; i < last; ++i) {
        const TrackEdit& t = *tracks[static_cast<std::size_t>(i)];
        if (t.colour != colour) members.push_back(SetTrackColour{t.id, static_cast<std::uint8_t>(colour)});
    }
    if (members.empty()) return true;
    if (members.size() == 1) return submit(to_operation(members.front()), why);
    if (members.size() > kMaxCompoundMembers) members.resize(kMaxCompoundMembers);
    return submit(Compound{std::move(members), "Colour Tracks"}, why);
}

const Region* EditController::selected_region(std::string& why) const {
    const Region* r = sel_.region != kNoId ? find_region(snapshot(), sel_.region) : nullptr;
    if (r == nullptr) why = "select a region first: click it with the Grabber or the Smart Tool";
    return r;
}

bool EditController::rename_region(const std::string& name, std::string& why) {
    const TrackEdit* t = nullptr;
    if (selected_region(why) == nullptr) return false;
    const Region* r = find_region(snapshot(), sel_.region, &t);
    const std::string n = trimmed(name);
    if (n.empty()) {
        why = "a region needs a name; \"" + r->name + "\" keeps its own";
        return false;
    }
    if (n == r->name) return true;
    return submit(SetRegionName{t->active_playlist, r->id, n}, why);
}

bool EditController::set_region_colour(int colour, std::string& why) {
    const TrackEdit* t = nullptr;
    if (selected_region(why) == nullptr) return false;
    const Region* r = find_region(snapshot(), sel_.region, &t);
    if ((colour < 0 || colour >= kColours) && colour != kTrackColour) {
        why = "the palette has colours 1 to " + std::to_string(kColours) +
              ", and the track's colour";
        return false;
    }
    if (r->colour == colour) return true;
    return submit(SetRegionColour{t->active_playlist, r->id, static_cast<std::uint8_t>(colour)},
                  why);
}

}  // namespace sonora::ui
