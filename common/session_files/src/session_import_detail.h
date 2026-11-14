// SPDX-License-Identifier: GPL-3.0-or-later
//
// The planner's builder (ADR-0123 decision 2), internal to
// session_import.cpp: it turns the source's tracks into members of one
// Compound, handing out ids from the destination's next_id in the order the
// members are emitted, which is what apply() requires of a compound and what
// makes the same inputs give the same plan.
#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sonora/session_files/session_import.h"

namespace sonora::session_files::import_detail {

// "Take.wav" with n = 2 is "Take 2.wav"; a name with no extension takes the
// suffix at its end.
std::string with_suffix(const std::string& name, int n);

// ASCII lower case: the stick's exFAT compares names without case, so two
// files that differ only in case are one file there.
std::string fold(const std::string& s);

// The last component of a '/'-separated relative path.
std::string basename_of(const std::string& path);

// The active playlist of an audio track, or nullptr.
const model::Playlist* active_playlist(const model::TrackEdit& t);

class Builder {
public:
    Builder(const model::Snapshot& dest, const model::Snapshot& src,
            const SessionImportRequest& request);

    // Pass 1: every source the tracks' active playlists name, an AddSource
    // and a FileCopy for each that the destination does not already hold
    // (by umid). `introduced[i]` is how many new sources track i brought
    // first, for TooLarge's count of what fits.
    void add_sources(const std::vector<const model::TrackEdit*>& tracks,
                     std::vector<std::size_t>& introduced);
    // Pass 2: one track and what it holds; how many members it took (0 for
    // a track v1 does not import, which leaves a `dropped` line).
    std::size_t add_track(const model::TrackEdit& t);
    // The source's markers, shifted by the offset; how many members.
    std::size_t add_markers();

    std::vector<model::Member> ops;
    std::vector<FileCopy> copies;
    std::vector<std::string> dropped;
    std::vector<std::string> approximated;
    // Set when the request cannot be planned (a position the offset would
    // push past the timeline): the planner refuses with it.
    std::string invalid;

private:
    model::Id take_id() { return next_++; }
    std::string unique_track_name(const std::string& name);
    std::string unique_audio_path(const std::string& source_path);
    model::TrackMix track_mix(const model::TrackEdit& t, model::Id id) const;
    bool shifted(model::Frames at, model::Frames& out);

    void add_material(const model::TrackEdit& t, model::Id playlist, const std::string& name);
    void add_inserts(const model::TrackEdit& t, model::Id track, const std::string& name);
    void add_automation(const model::TrackEdit& t, model::Id track, const std::string& name);
    void report_structure(const model::TrackEdit& t, const std::string& name);

    const model::Snapshot& dest_;
    const model::Snapshot& src_;
    const SessionImportRequest& request_;
    model::Id next_;
    std::size_t tracks_added_ = 0;
    // Source session's source id -> destination's.
    std::map<model::Id, model::Id> sources_;
    std::set<std::string> paths_;   // folded, relative to the session folder
    std::set<std::string> names_;   // track names, exact
};

}  // namespace sonora::session_files::import_detail
