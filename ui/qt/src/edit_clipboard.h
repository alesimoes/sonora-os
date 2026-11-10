// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cut, Copy, Paste, Duplicate, Repeat and Clear on a time range, as compound
// operations (common/model's Compound, ADR-0009 update 2026-10-14). The
// workflow is Pro Tools' (ADR-0004; Avid's Pro Tools Reference Guide,
// "Editing" chapter: "Cut, Copy, Paste and Clear Commands", "Duplicate
// Command", "Repeat Command"), never its code.
//
// What the clipboard holds is interface state, as the selection is: for each
// track the range covered, the regions under it, cut to it, with the fades
// and crossfades that lie wholly inside it. It names its sources and keeps a
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
#include <map>
#include <string>
#include <vector>

#include "model_access.h"

namespace sonora::ui {

// One track's part of a copied range.
struct Clip {
    model::Frames length = 0;        // the range copied, gaps included
    std::uint16_t channels = 1;      // of the track it was copied from
    std::string track;               // that track's name, for the refusals
    // Positions are from the range's start; ids are the originals', for
    // the crossfades to name until a paste gives each a new one.
    std::vector<model::Region> regions;
    std::vector<model::Crossfade> crossfades;
    std::vector<model::Source> sources;   // as they were in the pool at copy

    bool empty() const { return regions.empty(); }
};

// What Cut and Copy leave: one lane per track the selection covered, top
// first. A lane whose part of the range held no region keeps its place, so
// every lane lands the same number of tracks below the first as it was
// copied from, and pasting it clears what it covers, as its gap would.
struct Clipboard {
    std::vector<Clip> lanes;
    // The session folder and rate it was cut or copied in (ADR-0142). Kept
    // when another session opens: that is how a paste there knows whose audio
    // it holds. Empty/0 when the interface did not know.
    std::string origin_folder;
    std::uint32_t origin_sample_rate = 0;

    bool empty() const;   // no region on any lane
    model::Frames length() const { return lanes.empty() ? 0 : lanes.front().length; }
};

// An id for a member that introduces an object, in the order the members
// use them (operations.h: a compound's new ids strictly increase).
using NextId = std::function<model::Id()>;

// The regions of `track` under [a, b), cut to it. False when there are
// none; `out` is then that empty lane, still the range's length.
bool copy_range(const model::Snapshot& s, const model::TrackEdit& track, model::Frames a,
                model::Frames b, Clip& out);

// The members that leave [a, b) on `p` empty, appended to `out`. A region
// cut at one end loses the fade on that edge, as model overwrite does.
void clear_range(const model::Playlist& p, model::Frames a, model::Frames b, const NextId& id,
                 std::vector<model::Member>& out);

// The members that put `clip` at `at` on `p`: what lies under its whole
// length is cleared first, gaps included, as a paste of a range does.
//
// `clear` false leaves the range alone: Shuffle has opened a gap there
// (shuffle_open) and clearing the playlist as it was would name regions the
// ripple has moved.
void paste_clip(const model::Playlist& p, model::Frames at, const Clip& clip, const NextId& id,
                std::vector<model::Member>& out, bool clear = true);

// Shuffle's ripple (ADR-0009 update of 2026-11-04). shuffle_shift moves
// every region that starts at or after `from` by `delta`, when there is one;
// shuffle_open first splits the region `at` falls inside, so a gap of
// `length` opens there.
void shuffle_shift(const model::Playlist& p, model::Frames from, model::Frames delta,
                   std::vector<model::Member>& out);
void shuffle_open(const model::Playlist& p, model::Frames at, model::Frames length,
                  const NextId& id, std::vector<model::Member>& out);

// `count` copies of `clip` end to end from `at`: what lies under all of them
// is cleared once, so a repeat costs one clear and a placement a region a
// copy, not a paste's clear per copy.
void repeat_clip(const model::Playlist& p, model::Frames at, const Clip& clip, int count,
                 const NextId& id, std::vector<model::Member>& out, bool clear = true);

// How many members repeat_clip would append for `count` copies, without
// building them, so an absurd count is refused before it is allocated.
std::size_t repeat_members(const model::Playlist& p, model::Frames at, const Clip& clip,
                           std::size_t count);

// Empty when `clip` can be pasted on `track` of `s`; otherwise why not, in
// the operator's words.
std::string paste_problem(const model::Snapshot& s, const model::TrackEdit& track,
                          const Clip& clip);

// True when a source the clip names is not in `s`'s pool as the clip kept it:
// the audio of another session.
bool sources_missing(const model::Snapshot& s, const Clip& clip);

// paste_problem's channel check alone.
std::string channel_problem(const model::TrackEdit& track, const Clip& clip);

// What a cross-session paste brings in first (ADR-0142): AddSource members,
// the clipboard source id -> this session's id, and the id the paste's own
// members start from. The seam hides the planner, the file copies and the
// worker thread they run on; on false `why` is the held refusal and nothing
// was sent or copied.
struct BroughtIn {
    std::vector<model::Member> ops;
    std::map<model::Id, model::Id> remap;
    model::Id next_id = 1;
};
using BringInFn = std::function<bool(const model::Snapshot& dest,
                                     const std::vector<model::Source>& sources,
                                     const std::string& origin_folder,
                                     std::uint32_t origin_sample_rate, BroughtIn& out,
                                     std::string& why)>;

// Repeat's typed count: a whole number, with an optional sign and spaces
// around it, that fits an int. "3x", "2.5" and "" are not counts; "0" and
// "-1" are, and Repeat refuses them by value, saying what it needs.
bool parse_repeat_count(const std::string& text, int& out);

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
