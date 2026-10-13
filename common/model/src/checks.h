// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session format's invariants, once. apply() runs them on what it
// built, check_invariants() on a whole snapshot, and the loader on what it
// read, so an edit can never produce a file the loader would refuse.
//
// Each returns an empty string when the value is fine, or the reason. `where`
// receives the offending element relative to the value checked
// ("regions[3]", "crossfades[0]"), for the caller to prefix with its path.
#pragma once

#include <string>

#include "sonora/model/model.h"

namespace sonora::model::detail {

bool valid_text(const std::string& s);
std::string source_problem(const Source& s);
std::string region_problem(const Region& r, const SourcePool& pool,
                           std::uint16_t track_channels);
std::string crossfade_problem(const Crossfade& x, const Playlist& p,
                              const SourcePool& pool);
std::string playlist_problem(const Playlist& p, const SourcePool& pool,
                             std::uint16_t track_channels, std::string& where);
std::string mix_problem(const TrackMix& m, std::uint16_t track_channels,
                        std::string& where);
std::string bus_problem(const MonitorBus& b, std::string& where);
std::string settings_problem(const SessionSettings& s, std::string& where);
std::string marker_problem(const Marker& m);

const Region* find_region(const Playlist& p, Id id);
// Canonical order: by the left region's position.
void sort_crossfades(Playlist& p);
void sort_markers(MarkerList& m);

}  // namespace sonora::model::detail
