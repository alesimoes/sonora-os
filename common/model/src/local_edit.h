// SPDX-License-Identifier: GPL-3.0-or-later
//
// Region edits that touch, and re-check, only the part of a playlist they
// changed (ADR-0009 update 2026-10-14, item 5; BACKLOG Track 2 item 8).
//
// Each operation starts from a valid playlist. Whatever lies outside the
// regions it changed, their neighbours either side, and the crossfades that
// join them is therefore still valid, so checking that neighbourhood gives
// the same verdict as playlist_problem() on the whole playlist, with the same
// words and the same indices. Builds with SONORA_MODEL_AUDIT check that claim
// after every edit (apply.cpp).
//
// Region ids are not checked for uniqueness here: every new one passed
// Applier::fresh(), so it is above every id already in the session.
#pragma once

#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "sonora/model/model.h"

namespace sonora::model::detail {

constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();

// What one operation changed on one playlist, as index ranges into its
// regions as they are now.
class Touched {
public:
    using Range = std::pair<std::size_t, std::size_t>;   // [first, second)

    void clear();
    // Regions [at, at + removed) were replaced by `inserted` regions at `at`.
    // Ranges recorded earlier shift with the regions they name.
    void splice(std::size_t at, std::size_t removed, std::size_t inserted);
    // Regions [first, last] are unchanged but their crossfades are not.
    void mark(std::size_t first, std::size_t last) {
        splice(first, last - first + 1, last - first + 1);
    }
    // A region that left the playlist, so its crossfades can be found.
    void add_gone(Id id) { gone_.push_back(id); }
    // A region away from every range whose crossfade went, so its fades are
    // fitted and checked again (the far side of a region moved away).
    void add_end(Id id) { ends_.push_back(id); }

    const std::vector<Range>& ranges() const { return ranges_; }
    const std::vector<Id>& gone() const { return gone_; }
    const std::vector<Id>& ends() const { return ends_; }

private:
    std::vector<Range> ranges_;   // sorted, disjoint
    std::vector<Id> gone_;
    std::vector<Id> ends_;
};

// A fade that no longer fits is shortened to fit. Only the fade an edit cut
// through is dropped (overwrite_range); the other keeps its shape.
void clamp_fades(Region& r);

// The index of region `id`, looked for near the touched ranges first; kNoIndex
// when it is not on the playlist.
std::size_t find_index(const Playlist& p, Id id, const Touched& t);

// Clears [a, b) (timeline-model.md, "Overwrite"), as the whole-playlist
// version did: a region inside goes, one straddling an edge is cut to it, one
// spanning both is split with its right part taking `split_id`, and a cut
// edge loses its fade. Only the regions that meet [a, b) are visited.
bool overwrite_range(Playlist& p, Frames a, Frames b, Id split_id, bool& used_split,
                     std::string& error, Touched& t);
// Inserted by position.
void insert_region(Playlist& p, Region r, Touched& t);
void erase_region(Playlist& p, std::size_t i, Touched& t);

// After an edit moved region edges, a crossfade stays only if it would still
// be accepted as new (see apply.cpp). Only crossfades on a touched or removed
// region are decided; every other one was valid and nothing it rests on
// changed.
void drop_broken_crossfades_near(Playlist& p, const SourcePool& pool, Touched& t);
// fit_fades_to_crossfades() on the touched regions, their neighbours and
// the recorded ends; elsewhere it would change nothing.
void fit_fades_near(Playlist& p, const Touched& t);
// A new crossfade, after those whose left region is at or before its own.
void insert_crossfade(Playlist& p, const Crossfade& x, std::size_t left_index);

// playlist_problem() for a playlist that was valid before the edits `t`
// records: the touched regions and one either side, the recorded ends, the
// crossfades on any of them, and the crossfades on removed regions.
std::string playlist_problem_near(const Playlist& p, const SourcePool& pool,
                                  std::uint16_t track_channels, const Touched& t,
                                  std::string& where);

// True in a build that audits every edit (SONORA_MODEL_AUDIT), so a test can
// tell that its run checked something.
bool audited();

}  // namespace sonora::model::detail
