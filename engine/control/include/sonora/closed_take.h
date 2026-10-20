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

// What add_closed_take() did with one closed take.
struct TakeAdded {
    // To send to every replica, in order.
    std::vector<model::ReplicationUpdate> updates;
    // One per file that holds frames but could not be placed: "take N is on
    // disk but not in the session: track T, <file>: <why>". The take's
    // other files are placed all the same.
    std::vector<std::string> track_notices;
    // Set when a file failed (disk error, 4 GB limit): "take N was cut
    // short: <file>: <why>, placed up to the failure", and how many more.
    // Such a file is placed up to its last whole frame, like the rest.
    std::string cut_short;
    // Set when the model refused the take, which for operations the engine
    // built is a model bug. The good files' sources were then added on
    // their own, so the audio is in the pool and a save keeps it, but none
    // of it is on the timeline. An error to show as one, not a side line.
    std::string refused;
};

// A take the disk thread has closed (Session::closed_takes(); ADR-0009
// update 2026-10-14, item 8). Every file of it that holds frames and reads
// goes in as one Compound labelled "Record": the file as a Recorded source
// and a RecordTake on the active playlist of the track at the same display
// index, in track order, every id allocated up front. One update, one undo
// step, up to kMaxCompoundMembers / 2 files; a larger take goes in as
// several compounds of that many, one update and one step each.
// `files_per_compound` exists so tests reach the split with a few tracks;
// it is clamped to [1, kMaxCompoundMembers / 2].
//
// A punched take (ClosedTake::punched) goes in over its punch range [A, B)
// only, cut to what was recorded, with the rest of the file as handle
// (timeline-model.md, "Punch in"): the region beneath is trimmed or split
// around it, and at A and at B, where it meets that region, a centred
// crossfade of the punch's length and shape, if both sides have the handle
// for it. A file with nothing recorded inside [A, B) goes into the pool
// only, and makes no undo step.
TakeAdded add_closed_take(model::Document& document, const Session& session,
                          const Session::ClosedTake& take,
                          std::size_t files_per_compound = model::kMaxCompoundMembers / 2);

// The punch the record commands start when the document has a punch range
// [A, B) set (transport.punch, B > A; what the interface shows as punch
// on): capture over [A - H, B + H), H = max(punch_crossfade, one second),
// cut at the timeline's start; Auto monitoring switching at A and B with
// the equal-power crossfade the take will be placed with
// (timeline-model.md, "Punch in"). False, leaving `out` alone, when no
// range is set.
bool document_punch(const model::Snapshot& document, PunchRange& out);

// Whether engine track `engine_track` may be armed: not while a MIDI or
// instrument track sits on it, which records no audio (ADR-0014 M-1). An
// engine track past the document's tracks may: it records as it always has.
bool may_arm_track(const model::Snapshot& document, std::size_t engine_track, std::string& why);

// At open, before anything records: every take in `session_dir`'s audio
// folder that an engine died before closing gets the length on disk
// (recover_take(), session-format.md "Crash recovery"). Returns one line
// per take that needed a word to the operator: recovered, shorter than its
// header, or not recoverable. A take repaired with no audio in it gets no
// line. A recovered take is left in the folder and
// placed nowhere: placing it is offered, never done silently.
std::vector<std::string> recover_unclosed_takes(const std::string& session_dir,
                                                uint32_t sample_rate);

}  // namespace sonora
