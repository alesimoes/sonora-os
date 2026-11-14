// SPDX-License-Identifier: GPL-3.0-or-later
//
// Take-folder view logic (ADR-0159): a track's take lanes collapse to one
// folder row showing the comp coloured by the lane each stretch came from.
// Expanded or collapsed is view state, never the document: it is not undone
// and is not saved. Qt-free so the unit test needs no toolkit.
#pragma once

#include <set>
#include <vector>

#include "model_access.h"

namespace sonora::ui {

struct FolderSegment {
    model::Frames start = 0;
    model::Frames end = 0;
    model::Id origin = 0;   // playlist the audio came from; 0 when none holds it
    int lane = -1;          // index among the take lanes, -1 when origin is 0
    int colour = -1;        // lane colour slot, -1 for a stretch of no lane
};

class TakeFolderView {
public:
    // Palette slots lane colours cycle through.
    static constexpr int kLaneColours = 8;

    bool expanded(model::Id track) const { return expanded_.count(track) != 0; }
    void set_expanded(model::Id track, bool on);
    bool toggle(model::Id track);   // returns the new state
    // A session opened or the track is gone: view state resets.
    void reset() { expanded_.clear(); }

    // The take lanes: the playlists other than the active one, in order.
    static std::vector<model::Id> lanes(const model::TrackEdit& track);
    // The comp, region by region, tagged with its source lane.
    static std::vector<FolderSegment> segments(const model::TrackEdit& track);
    // Rows the folder takes: 1 collapsed, 1 + lanes expanded.
    int rows(const model::TrackEdit& track) const;

private:
    std::set<model::Id> expanded_;
};

}  // namespace sonora::ui
