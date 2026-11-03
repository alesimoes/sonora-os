// SPDX-License-Identifier: GPL-3.0-or-later
//
// Take lanes and swipe comping (ADR-0031). A lane is one of a track's
// playlists; a swipe is Pro Tools' Copy Selection to Main Playlist, built
// here as the members of one Compound from the operations the model already
// has, so it needs no new operation, no protocol and no format change. The
// workflow is Pro Tools' and Logic's (Avid's Pro Tools Reference Guide,
// "Playlists"; Logic Pro User Guide, "Comp your audio recordings"), never
// their code. Pure: no I/O, not for the audio thread.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/operations.h"

namespace sonora::model {

struct CompRange {
    Id track = 0;
    Id from = 0;   // the take lane
    Id to = 0;     // the comp, usually the active playlist
    Frames start = 0;
    Frames end = 0;
    // 0 leaves each join a cut (Pro Tools). Otherwise the length of the
    // centred equal-power crossfade asked for at each join (Logic's Quick
    // Swipe), shortened to fit (ADR-0031 §3).
    Frames seam_crossfade = 0;
};

struct Comp {
    std::vector<Member> members;   // one Compound's worth, in order
    std::size_t seams = 0;         // joins between a placed region and the comp's
    std::size_t seams_faded = 0;   // of those, the ones given a crossfade
};

// The members that make [start, end) of `to` play what [start, end) of
// `from` plays, its silence included (ADR-0031 §2). New ids are taken from
// `next_id` in member order, every split id included. False with `why` in
// the operator's words when the swipe is refused, `out` then empty; true
// with no member when the range is empty on both playlists.
bool comp_range(const Snapshot& s, const CompRange& c, const std::function<Id()>& next_id,
                Comp& out, std::string& why);

// The first playlist of `track` other than `exclude` that holds `r`'s audio
// at `r`'s place: a region covering r's span with the same channels and the
// same source_offset - position (ADR-0031 §4). 0 when none does.
Id comp_origin(const TrackEdit& track, const Region& r, Id exclude);

}  // namespace sonora::model
