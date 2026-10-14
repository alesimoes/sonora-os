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
// appends the update every replica must be sent, in order. On failure what
// was already applied stays applied, and is in `updates` too: the replicas
// must hear of it all the same.
//
// The control thread's only, or before the server exists. They allocate
// and read files.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "sonora/model/replication.h"
#include "sonora/session.h"

namespace sonora {

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

// A take the disk thread has closed (Session::closed_takes()): for every
// file of it that holds frames, the file as a Recorded source and a
// RecordTake on the active playlist of the track at the same display index.
// One undo step per track. A file that failed (disk error, 4 GB limit) is
// not placed: the rest are, and the call returns false with `error` naming
// the failed file and why.
bool add_closed_take(model::Document& document, const Session& session,
                     const Session::ClosedTake& take,
                     std::vector<model::ReplicationUpdate>& updates, std::string& error);

}  // namespace sonora
