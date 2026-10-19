// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cut, Copy, Paste, Duplicate and Clear on a time range, as compound
// operations (common/model's Compound, ADR-0009 update 2026-10-14). The
// workflow is Pro Tools' (ADR-0004; Avid's Pro Tools Reference Guide,
// "Editing" chapter: "Cut, Copy, Paste and Clear Commands", "Duplicate
// Command"), never its code.
//
// What the clipboard holds is interface state, as the selection is: the
// regions under a range on one track, cut to it, with the fades and
// crossfades that lie wholly inside it. It names its sources and keeps a
// copy of each, so a paste into a session whose pool says something else
// under those ids is refused rather than placing someone else's audio.
//
// Each gesture is built as one compound, so it is one revision, one update
// and one undo step whatever its size, and the whole of it is refused if
// one member is. Members are built to the fewest: a region the range cuts
// at one end is one trim, one it cuts at both is a split and a trim, one
// inside it is one removal. Nothing here is Qt.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "model_access.h"

namespace sonora::ui {

struct Clip {
    model::Frames length = 0;        // the range copied, gaps included
    std::uint16_t channels = 1;      // of the track it was copied from
    // Positions are from the range's start; ids are the originals', for
    // the crossfades to name until a paste gives each a new one.
    std::vector<model::Region> regions;
    std::vector<model::Crossfade> crossfades;
    std::vector<model::Source> sources;   // as they were in the pool at copy

    bool empty() const { return regions.empty(); }
};

// An id for a member that introduces an object, in the order the members
// use them (operations.h: a compound's new ids strictly increase).
using NextId = std::function<model::Id()>;

// The regions of `p` under [a, b), cut to it. False when there are none.
bool copy_range(const model::Snapshot& s, const model::TrackEdit& track, model::Frames a,
                model::Frames b, Clip& out);

// The members that leave [a, b) on `p` empty, appended to `out`. A region
// cut at one end loses the fade on that edge, as model overwrite does.
void clear_range(const model::Playlist& p, model::Frames a, model::Frames b, const NextId& id,
                 std::vector<model::Member>& out);

// The members that put `clip` at `at` on `p`: what lies under its whole
// length is cleared first, gaps included, as a paste of a range does.
void paste_clip(const model::Playlist& p, model::Frames at, const Clip& clip, const NextId& id,
                std::vector<model::Member>& out);

// Empty when `clip` can be pasted on `track` of `s`; otherwise why not, in
// the operator's words.
std::string paste_problem(const model::Snapshot& s, const model::TrackEdit& track,
                          const Clip& clip);

// The refusal of a gesture that needs more than one edit may hold
// (model::kMaxCompoundMembers), empty when it fits: "Paste needs 300
// changes; at most 256 in one edit. Nothing was pasted."
std::string over_limit(const char* gesture, const char* nothing_was, std::size_t members);

// An undo step's label as the Edit menu says it after "Undo " or "Redo ".
// A compound's label is already the operator's word ("Paste"); a single
// operation's is its type ("MoveRegion"), said as the gesture that sends it
// ("Move"). Empty for an empty label.
std::string step_text(const std::string& label);

}  // namespace sonora::ui
