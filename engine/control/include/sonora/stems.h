// SPDX-License-Identifier: GPL-3.0-or-later
//
// Export Stems, the engine's half (interchange.stems, ADR-0085): one offline
// bounce per stem, each on a copy of the snapshot whose solo flags select the
// stem's tracks, so a stem keeps its pan, sends and bus processing and the
// stems sum to the mix. The document is never touched; the snapshot is a
// value. Never the audio thread: the passes run on the caller's thread,
// which is a worker's.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sonora/api/stems.h"
#include "sonora/bounce.h"
#include "sonora/model/model.h"

namespace sonora {

struct Stem {
    // What its file is called after the number: the track's or the output's.
    std::string name;
    // Soloed for its pass.
    std::vector<model::Id> tracks;
};

struct StemsPlan {
    // Relative to the session folder: api::stems_folder() of the name.
    std::string folder;
    std::vector<Stem> stems;
};

// The stems `opts` asks for. by=track: every unmuted audio track in track
// order, or the tracks named, in the order named. by=output: one stem per
// output the unmuted audio tracks feed (an aux bus or the master), in the
// order of the first track feeding each. False, `error` set, when there is
// none or a named track is not an unmuted audio track.
bool stems_plan(const model::Snapshot& snapshot, const api::StemsOptions& opts, StemsPlan& out,
                std::string& error);

// What mix.stems refuses at once, with its reason: no stems, an empty range,
// an active plug-in insert, normalise, a pair the file type cannot hold, and
// a folder of that name already in the session.
bool stems_check(const model::Snapshot& snapshot, const std::string& session_dir,
                 const api::StemsOptions& opts, std::string& error);

// The copy one stem's pass plays: its tracks soloed, every other track's
// solo and solo safe and every bus's solo cleared, so nothing else is heard
// but what the stem's tracks feed.
model::Snapshot stem_snapshot(const model::Snapshot& snapshot, const Stem& stem);

// "<NN> <name>" for the stem at `index` (from 0), file-safe.
std::string stem_file_stem(std::size_t index, const std::string& name);

struct StemsResult {
    std::string folder;               // relative to the session folder
    std::vector<std::string> files;   // relative to the session folder, in plan order
    uint64_t frames = 0;              // of every file: they all have the same
};

// One pass of the export; bounce_offline() unless a test injects another.
using StemRenderer =
    std::function<bool(const model::Snapshot&, const std::string& session_dir,
                       const api::BounceOptions&, const BounceHooks&, BounceResult&, std::string&)>;

// Renders every stem of `opts` into the folder stems_check() names, every
// file over the same [start, end). `hooks.progress` gets the fraction of the
// whole export. On failure or cancel `error` says why and the folder is
// removed, with every file already in it.
bool stems_offline(const model::Snapshot& snapshot, const std::string& session_dir,
                   const api::StemsOptions& opts, const BounceHooks& hooks, StemsResult& out,
                   std::string& error, const StemRenderer& render = bounce_offline);

// Removes the folder of a finished export; for one the operator cancelled
// after its last pass, or that belongs to a session no longer open.
void remove_stems_folder(const std::string& session_dir, const std::string& folder);

}  // namespace sonora
