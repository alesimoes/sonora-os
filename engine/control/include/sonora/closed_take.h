// SPDX-License-Identifier: GPL-3.0-or-later
//
// What reaches the engine's one document from disk (ADR-0009, update
// "playback on the M4/M7 control server", item 1): a closed take, and a file
// --place puts on the timeline. Lengths and positions come from the files
// (their data size and BWF TimeReference), never from the transport
// (ADR-0009, Consequences: "the length in the session is the length on
// disk").
//
// Each function applies resolved operations through Document::apply() and
// hands back the update every replica must be sent, in order. On failure
// what was already applied stays applied, and is in `updates` too: the
// replicas must hear of it all the same.
//
// The control thread's only, or before the server exists. They allocate
// and read files.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "sonora/model/replication.h"

namespace sonora {

class Session;

// The document the engine starts with when no session file is opened: one
// model track per engine track, as many channels as it records, each
// channel's input the engine track's own.
model::Snapshot engine_document(const Session& session);

// Places `relative_path` (from `session_dir`) on the track at display index
// `track`, at the file's TimeReference, overwriting what is beneath it, as
// an imported source. Refused, naming both rates, if the file is not at the
// document's rate.
bool place_file(model::Document& document, const std::string& session_dir,
                const std::string& relative_path, std::size_t track,
                std::vector<model::ReplicationUpdate>& updates, std::string& error);

struct ClosedTake {
    // To send to every replica, in order.
    std::vector<model::ReplicationUpdate> updates;
    // One per armed track whose file does not read, naming the track and
    // the file: that track's take is not in the session. The others' are.
    std::vector<std::string> track_notices;
    // Set when the model refused the take, which for operations the engine
    // built is a model bug. The good tracks' sources were then added on
    // their own, so the audio is in the pool and a save keeps it, but none
    // of it is on the timeline. An error to show as one, not a side line.
    std::string refused;
};

// The take numbered `take` has just closed (ADR-0009 update 2026-10-14,
// item 8). Every armed track that wrote frames and whose file reads goes in
// as one Compound labelled "Record": its file as a Recorded source and a
// RecordTake on the active playlist of the track at the same display index,
// in track order, every id allocated up front. One update, one undo step.
ClosedTake add_closed_take(model::Document& document, const Session& session, int take);

}  // namespace sonora
