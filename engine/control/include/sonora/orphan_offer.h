// SPDX-License-Identifier: GPL-3.0-or-later
//
// Takes on disk that the open session does not name (session-format.md,
// "Crash recovery", and "Opening" step 6): a take an engine died recording
// (recover_unclosed_takes() has given it its length by then), or one
// closed after the last save that the session file never heard of. Each
// carries its placement in its own header, the BWF TimeReference and the
// Description (take_description()), so it can be put back where it was
// recorded. It is offered, never placed silently, and never deleted.
//
// The control thread's only: these read files and allocate.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/loop_take.h"
#include "sonora/model/replication.h"
#include "sonora/session.h"

namespace sonora {

// One take file in the audio folder that no source of the document names.
struct OrphanTake {
    std::string path;   // relative to the session folder: "Audio Files/<name>"
    TakeDescription said;
    uint64_t time_reference = 0;
    uint64_t frames = 0;
    uint16_t channels = 0;
    model::AudioFormat format = model::AudioFormat::Int24;
    // A loop take's passes, from its wrap list and its header.
    RecoveredLoop loop;
};

struct OrphanOffer {
    std::vector<OrphanTake> takes;   // in file name order
    // What the operator reads: one line per take, saying where it would
    // go. Empty when there is nothing to offer.
    std::string text;
};

// Every take in `session_dir`'s "Audio Files/" that `document` has no
// source for and has not left (settings.left_takes), recorded by this
// engine for this session at its rate. One engine folder holds the takes
// of every session opened on it, and another session's take is not this
// one's orphan: a take is this session's when its Description carries the
// document's session id. One written before session ids carries none, and
// is this session's when it names the document's name, unless that name
// is "Untitled", which every new session has. A file with no audio, one
// another program wrote, or one that does not read is not offered.
OrphanOffer find_orphan_takes(const model::Snapshot& document, const std::string& session_dir);

// Whether a take described as `said` is of the session `settings` holds,
// by the rule above.
bool take_is_of_session(const TakeDescription& said, const model::SessionSettings& settings);

// Adds every take `offer` holds to `settings.left_takes`, sorted, with no
// file twice: "Leave in Audio Files", remembered with the session. False
// when the list already held them all.
bool leave_orphan_takes(model::SessionSettings& settings, const OrphanOffer& offer);

// What place_orphan_takes() did.
struct OrphansPlaced {
    // To send to every replica, in order.
    std::vector<model::ReplicationUpdate> updates;
    // For the operator: what went in other than as offered, a crossfade
    // left out, a fade shortened, a refusal.
    std::vector<std::string> notices;
    std::size_t placed = 0;   // files that went in as takes
    std::size_t pooled = 0;   // files in the audio pool only
};

// Places what `offer` holds through the path a closed take takes
// (add_closed_take()), one undo step labelled kRecoveredLabel per take:
// on the track whose id its Description names, at its TimeReference; a
// punch take over its punch range only, with the session's punch
// crossfades; a loop take as its passes on new playlists. A take whose
// track is gone, whose header names none, or whose length does not cover
// its punch range goes into the audio pool as an unused source, never over
// existing material. A file a source has named since the offer is skipped.
OrphansPlaced place_orphan_takes(model::Document& document, const Session& session,
                                 const OrphanOffer& offer);

// The undo step's label.
inline constexpr const char* kRecoveredLabel = "Place Recovered Takes";

}  // namespace sonora
