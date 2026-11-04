// SPDX-License-Identifier: GPL-3.0-or-later
//
// Consolidate, the engine's half (ADR-0031 §6): render a range of a track's
// playlist (regions, fades, crossfades and region gains, as playback plays
// them) to a new file in the session's "Audio Files/", add it to the pool and
// replace the range with one region of it, as one compound and one undo step.
//
// The control thread's, or a worker's: it reads and writes files and
// allocates. Never the audio thread; it touches nothing of the session but
// whether a take is open.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/replication.h"
#include "sonora/session.h"

namespace sonora {

struct ConsolidateRequest {
    model::Id track = 0;
    model::Id playlist = 0;   // 0 is the track's active one
    model::Frames start = 0;
    model::Frames end = 0;
    std::string name;         // the new region's; empty is "<track> Consolidated"
};

struct ConsolidateHooks {
    // Called with the fraction rendered, in (0, 1], once per block, from the
    // calling thread.
    std::function<void(double)> progress;
    // Polled once per block; set it from another thread to stop. The file
    // is then removed and nothing reaches the document.
    const std::atomic<bool>* cancel = nullptr;
};

// What render_range() made.
struct RenderedRange {
    std::string relative_path;   // "Audio Files/<name>.wav", '/'-separated
    model::Frames frames = 0;
};

// Renders [start, end) of the request's playlist to a new file under
// `session_dir`, never over an existing one: names are numbered like takes
// ("<track>_cons_01.wav", then 02 ...) and claimed exclusively. The file is
// synced, with its folder, before this returns, and has the session's
// format. Frames outside any region, and under a muted one, are silence. A
// file past the 4 GB RIFF limit fails (as a take does). On failure `error`
// says why and no file is left behind.
bool render_range(const model::Snapshot& snapshot, const std::string& session_dir,
                  const ConsolidateRequest& request, const ConsolidateHooks& hooks,
                  RenderedRange& out, std::string& error);

// The two halves consolidate() is made of, for a caller that renders on a
// worker and commits on the document's thread (the control server).
//
// consolidate_check(): resolves the request's playlist into `resolved` and
// asks the model whether the range would be accepted, so a refusal costs no
// render. Reads `now` only.
bool consolidate_check(const model::Snapshot& now, const ConsolidateRequest& request,
                       ConsolidateRequest& resolved, std::string& error);

// consolidate_commit(): the rendered file becomes a source and replaces the
// range, as one Compound labelled "Consolidate", against the document as it
// is now. The file is removed when the track is gone or unreadable, and
// stays out of the pool when the model refuses, as for consolidate().
bool consolidate_commit(model::Document& document, const std::string& session_dir,
                        const ConsolidateRequest& resolved, const RenderedRange& rendered,
                        std::vector<model::ReplicationUpdate>& updates, std::string& error);

// Refused, saying why, while a take is recording or closing. Otherwise
// render_range(), then one Compound labelled "Consolidate" (AddSource and
// the placement model::consolidate_range() asks for) applied to `document`.
// `updates` gets the one update to send every replica. On any failure the
// document is unchanged; the rendered file is removed unless the model
// refuses after the render, when it stays in "Audio Files/" but out of the
// pool, and `error` says so.
bool consolidate(model::Document& document, const Session& session,
                 const std::string& session_dir, const ConsolidateRequest& request,
                 const ConsolidateHooks& hooks,
                 std::vector<model::ReplicationUpdate>& updates, std::string& error);

}  // namespace sonora
