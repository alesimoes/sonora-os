// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shuffle mode's ripple for the gestures that move or resize a region (ADR-0153).
// Each builder appends the members of ONE compound (ADR-0009), so a gesture is
// one undo step. Pro Tools' Shuffle semantics (Reference Guide, "Edit Modes"),
// the workflow only (ADR-0004). Header-only: no Qt, no engine.
//
// The orders below are not arbitrary: ShiftRegions is refused when it would land
// on a region that stays, so room is opened before a region grows into it and
// a gap is closed after the region has left it.
#pragma once

#include <algorithm>
#include <vector>

#include "edit_clipboard.h"

namespace sonora::ui {

// True when a region other than `except` starts at or after `from`: what
// ShiftRegions needs to have something to move.
inline bool shuffle_has_tail(const model::Playlist& p, model::Frames from, model::Id except = 0) {
    return std::any_of(p.regions.begin(), p.regions.end(), [&](const model::Region& r) {
        return r.id != except && r.position >= from;
    });
}

// ShiftRegions only when there is something to move.
inline void shuffle_push(const model::Playlist& p, model::Frames from, model::Frames delta,
                         model::Id except, std::vector<model::Member>& out) {
    if (delta != 0 && shuffle_has_tail(p, from, except)) {
        out.push_back(model::ShiftRegions{p.id, from, delta});
    }
}

// Pulls a start trim that would grow the region left into its neighbour back
// to the neighbour's end: Shuffle never overwrites. False when nothing is left
// of the trim.
inline bool shuffle_limit_trim(const model::Playlist& p, const model::Region& r,
                               model::TrimRegion& t) {
    if (t.edge == model::Edge::Start && t.to < r.position) {
        model::Frames floor = 0;
        for (const model::Region& o : p.regions) {
            if (o.id != r.id && o.end() <= r.position) floor = std::max(floor, o.end());
        }
        t.to = std::max(t.to, floor);
    }
    return t.to != (t.edge == model::Edge::Start ? r.position : r.end());
}

// A trim by d moves every later region by d. The region's start stays where it
// was and its end moves, as in Pro Tools; a start trim is the trim plus a move
// back to the old start.
inline void shuffle_trim(const model::Playlist& p, const model::Region& r,
                         const model::TrimRegion& t, std::vector<model::Member>& out) {
    if (t.edge == model::Edge::End) {
        const model::Frames delta = t.to - r.end();
        if (delta > 0) shuffle_push(p, r.end(), delta, r.id, out);
        out.push_back(t);
        if (delta < 0) shuffle_push(p, r.end(), delta, r.id, out);
        return;
    }
    const model::Frames cut = t.to - r.position;  // < 0 grows the region
    if (cut < 0) shuffle_push(p, r.end(), -cut, r.id, out);
    out.push_back(t);
    out.push_back(model::MoveRegion{p.id, r.id, r.position, 0, model::Timebase::Frames});
    if (cut > 0) shuffle_push(p, r.end(), -cut, r.id, out);
}

// The region lands starting at `to` in the layout AFTER the gap it leaves has
// closed. Room opens at the target first (the region cannot land on anything),
// then the gap closes.
inline void shuffle_move(const model::Playlist& p, const model::Region& r, model::Frames to,
                         const NextId& id, std::vector<model::Member>& out) {
    const model::Frames len = r.length;
    const bool right = to > r.position;
    const model::Frames at = right ? to + len : to;  // in the layout before the gap closes
    shuffle_open(p, at, len, id, out);
    out.push_back(model::MoveRegion{p.id, r.id, at, 0, model::Timebase::Frames});
    // Left: the region's old place is now [pos+len, end+len); right: [pos, end).
    const model::Frames from = right ? r.end() : r.end() + len;
    // Right: the moved region itself starts at or after `from`, so there is
    // always a tail; left: only what lay after the region.
    if (right || shuffle_has_tail(p, r.end(), r.id)) {
        out.push_back(model::ShiftRegions{p.id, from, -len});
    }
}

}  // namespace sonora::ui
