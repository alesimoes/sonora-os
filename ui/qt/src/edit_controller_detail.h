// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the EditController's translation units share: the Smart Tool's pixel
// zones, the fade reach and refusal wording, a few notice helpers. Internal to
// ui/qt/src (edit_controller.cpp and the edit_*.cpp files); not a public header.
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "edit_controller.h"

namespace sonora::ui::edit_detail {

using namespace model;

inline constexpr double kMinFpp = 1.0;  // one sample a pixel
inline constexpr double kEdgePx = 8.0;  // the Smart Tool's trim zone at each end
inline constexpr double kDragSlopPx = 3.0;
inline constexpr double kFadeZonePx = 12.0;  // the Smart Tool's fade corners, in the lane's top quarter

inline const Playlist* active(const TrackEdit* t) { return active_playlist(t); }

// "Gtr Volume", "Gtr Plugin threshold": a lane as a notice names it.
inline std::vector<std::string> lane_labels(const TrackEdit& track,
                                     const std::vector<const AutomationLane*>& lanes) {
    std::vector<std::string> out;
    for (const AutomationLane* l : lanes) {
        std::string label = track.name + " " + lane_name(l->param);
        if (l->param == AutomationParam::Plugin) label += " " + l->key;
        out.push_back(std::move(label));
    }
    return out;
}

// How far into a region the fades at each end reach, crossfades included:
// what a new fade at the other end may not run into.
inline Frames reach_in(const RegionFades& f) {
    if (f.in_crossfade == nullptr) return f.in;
    Frames before = 0, after = 0;
    crossfade_extent(*f.in_crossfade, before, after);
    return after;
}

inline Frames reach_out(const RegionFades& f) {
    if (f.out_crossfade == nullptr) return f.out;
    Frames before = 0, after = 0;
    crossfade_extent(*f.out_crossfade, before, after);
    return before;
}

// What a new fade at one end may not run into: the other end's reach as it
// is heard, or that end's own stored fade if a crossfade hides it, whichever
// is longer. The model keeps the hidden fade and still counts it
// (region_problem: fade_in + fade_out <= length), so a limit from the heard
// reach alone would pass a drag the model then refuses.
inline Frames taken_at_end(const Region& r, const RegionFades& f) {
    return std::max(reach_out(f), r.fade_out.length);
}

inline Frames taken_at_start(const Region& r, const RegionFades& f) {
    return std::max(reach_in(f), r.fade_in.length);
}

inline const Region* region_on(const Playlist& p, Id id) {
    for (const Region& r : p.regions) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

inline std::string named(const Region& r) {
    return r.name.empty() ? "region " + std::to_string(r.id) : "\"" + r.name + "\"";
}

inline std::string crossfaded_refusal(const Playlist& p, const Region& r, const Crossfade& x, bool in) {
    const Region* other = region_on(p, in ? x.left : x.right);
    return named(r) + "'s " + (in ? "start" : "end") + " is crossfaded with " +
           (other ? named(*other) : std::string("its neighbour")) +
           "; select across the cut and fade the selection to change it";
}

// Why no crossfade fits the handles at a cut (ADR-0021), and where one would.
inline std::string no_handle_refusal(const Region& l, const Region& r, const Crossfade& x, Frames before,
                              Frames after) {
    const std::string none_before = named(r) + " starts where its audio does: no handle before the cut";
    const std::string none_after = named(l) + " ends where its audio does: no handle after the cut";
    switch (x.align) {
        case Align::Pre:
            return none_before + (after > 0 ? "; select from the cut onwards to crossfade after it" : "");
        case Align::Post:
            return none_after + (before > 0 ? "; select up to the cut to crossfade before it" : "");
        case Align::Centre:
            break;
    }
    return "no crossfade fits: " + none_before + ", and " + none_after;
}

// What a crossfade fitted to its handles became, and which handle held it
// back (ADR-0021 §4). A centred one that moved to one side at its full
// length was not shortened, and saying so would send the operator looking
// for frames that are all there.
inline std::string shortened_note(const Region& l, const Region& r, const Crossfade& asked,
                           const Crossfade& made, Frames before, Frames after) {
    Frames want_before = 0, want_after = 0;
    crossfade_extent(asked, want_before, want_after);
    std::string why;
    if (made.align != asked.align || before < want_before) {
        why = named(r) + " has " + std::to_string(before) + " frames of handle before its start";
    }
    if (made.align != asked.align || after < want_after) {
        why += (why.empty() ? "" : ", ") + named(l) + " has " + std::to_string(after) +
               " frames after its end";
    }
    const char* side = made.align == Align::Pre ? "before the cut" : "after the cut";
    if (made.align != asked.align && made.length == asked.length) {
        return std::string("Crossfade moved ") + side + ": " + why;
    }
    return "Crossfade shortened to " + std::to_string(made.length) + " frames" +
           (made.align == asked.align ? std::string() : std::string(", ") + side) + ": " + why;
}

inline std::string no_track(int index) {
    return index < 0 ? "no track is selected" : "there is no track " + std::to_string(index + 1);
}

}  // namespace sonora::ui::edit_detail
