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
#include <vector>

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
// Inserts (ADR-0012 §6). Values are checked against the format of the
// insert they belong to, which the InsertMix does not carry.
std::string plugin_problem(const PluginRef& p, std::string& where);
std::string inserts_problem(const std::vector<Insert>& inserts, std::string& where);
std::string insert_mix_problem(const InsertMix& m, PluginFormat format, std::string& where);
// Empty when `count` values fit one insert.
std::string insert_param_count_problem(std::size_t count);

// A fade left running into the crossfade at its region's other end is
// shortened to meet it, as clamp_fades() shortens one that outgrew its
// region. Used by edits that move edges, clear a crossfade or add a take's
// crossfades, and by the loader for files older builds wrote. SetFade and
// SetCrossfade are refused instead (playlist_problem), because there the
// operator asked for exactly that length. Returns what it shortened.
struct FadeFit {
    std::size_t region = 0;   // index in Playlist::regions
    bool fade_in = false;     // else the fade-out
    Frames from = 0, to = 0;
};
std::vector<FadeFit> fit_fades_to_crossfades(Playlist& p);

const Region* find_region(const Playlist& p, Id id);
// Canonical order: by the left region's position.
void sort_crossfades(Playlist& p);
void sort_markers(MarkerList& m);

}  // namespace sonora::model::detail
