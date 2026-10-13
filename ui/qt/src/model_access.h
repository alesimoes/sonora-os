// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session document the windows read is common/model's (ADR-0007 M8,
// brain/specs/timeline-model.md): `sonora::ui::model` is an alias of
// `sonora::model`, not a copy of it. What lives here is only what every
// reader of a Snapshot needs and common/model leaves to its readers: finding
// a track, its active playlist, a region and the track holding it.
//
// Lookups are linear in tracks and regions. A session is tens of tracks and
// hundreds of regions per track, and these run on a click or a key, never
// per pixel; an index would be one more structure to keep in step with
// every edit.
#pragma once

#include "sonora/model/model.h"
#include "sonora/model/operations.h"

namespace sonora::ui {

namespace model = ::sonora::model;

// common/model's "none" for every id.
constexpr model::Id kNoId = 0;

// The playlist a track plays and records into, or null for null.
const model::Playlist* active_playlist(const model::TrackEdit* t);
const model::TrackEdit* find_track(const model::Snapshot& s, model::Id track);
// Display order, or -1.
int track_index(const model::Snapshot& s, model::Id track);
// The region on a track's active playlist, and that track.
const model::Region* find_region(const model::Snapshot& s, model::Id region,
                                 const model::TrackEdit** track = nullptr);

}  // namespace sonora::ui
