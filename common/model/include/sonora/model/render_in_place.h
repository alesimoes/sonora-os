// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render in place (ADR-0051): Track Freeze, Track Commit and Process
// Selection, the three ways a track's own plug-in chain is printed to a file
// and the file put back in the track. The engine renders (engine/control
// render_in_place.h); this builds the members of the one Compound that puts
// the file in the document, from the operations the model has, so each is
// one undo step. The workflow is Pro Tools' Track Freeze and Track Commit
// and Logic's Bounce in Place (Avid's Pro Tools Reference Guide; Logic Pro
// User Guide), never their code. Pure: no I/O, not for the audio thread.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/operations.h"

namespace sonora::model {

enum class RenderKind {
    // The track's playlist through its active inserts, on a new playlist the
    // track then plays, the inserts made inactive. Undone by Unfreeze.
    Freeze,
    // The same, kept: the new playlist carries no record of the freeze.
    Commit,
    // [start, end) of the active playlist through the inserts, on a copy of
    // it that the track then plays; the inserts are made inactive because
    // their effect is now in the audio.
    Process,
};

const char* render_kind_name(RenderKind k);

// The playlist the track plays when it is a freeze's, else null.
const Playlist* frozen_playlist(const TrackEdit& t);

// The track's active inserts in slot order: what a render passes through
// and what it makes inactive.
std::vector<Id> active_inserts(const TrackEdit& t);

struct RenderRange {
    Frames start = 0;
    Frames end = 0;   // exclusive, before the tail
};

// Whether `track` can be rendered as `kind`, and over what: the regions'
// extent (Freeze, Commit) or the selection (Process, which needs a non-empty
// one on a playlist that holds a region). False with `why` in the operator's
// words otherwise.
bool render_range_of(const Snapshot& s, Id track, RenderKind kind, Frames sel_start,
                     Frames sel_end, RenderRange& out, std::string& why);

struct RenderPlacement {
    Id track = 0;
    RenderKind kind = RenderKind::Freeze;
    Frames start = 0;   // where the rendered file starts on the timeline
    Id source = 0;      // the file, already in the pool (AddSource)
    std::string name;   // the region's, and the playlist's when it is new
};

// The members that put `source` (its whole length, from `start`) in the
// track as `kind` says. New ids come from `next_id`, in member order, every
// split id included. False with `why` when the model would refuse it.
bool render_members(const Snapshot& s, const RenderPlacement& p,
                    const std::function<Id()>& next_id, std::vector<Member>& out,
                    std::string& why);

// Returns the track to the playlist it was frozen from, with the inserts the
// freeze made inactive active again, and takes the frozen playlist off. The
// rendered file stays in the pool. False with `why` when the track is not
// frozen or what it was frozen from is gone.
bool unfreeze_members(const Snapshot& s, Id track, std::vector<Member>& out, std::string& why);

}  // namespace sonora::model
