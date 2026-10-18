// SPDX-License-Identifier: GPL-3.0-or-later
//
// One Operation to bytes and back: the replication format (ADR-0009 §2). A
// Compound is {"op": "Compound", "ops": [<member>, ...], "label": "..."},
// each member as it would be alone and `label` only when there is one
// (ADR-0009 update 2026-10-14); a member is refused by its path, "ops[3]".
//
// contracts/api carries an operation as opaque bytes, because it may not
// depend on this module (ADR-0007 §1). These are those bytes: one strict
// JSON object, {"op": "<kind>", ...fields}, with the session file's
// spellings for every enum and every number an integer except gains and
// pans. Strict as the session file is: an unknown or missing key, a wrong
// type or an out-of-range integer is an error naming the field. Whether the
// operation makes sense is apply()'s question, not the decoder's.
//
// Never on the audio thread: it allocates.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "sonora/model/history.h"
#include "sonora/model/operations.h"

namespace sonora::model {

// Canonical: the same operation always gives the same bytes.
bool encode_operation(const Operation& op, std::string& out, std::string& error);
bool decode_operation(std::string_view bytes, Operation& out, std::string& error);

// The whole replicated document with its undo window: what a Snapshot
// update carries (ADR-0009, 2026-10-13 update). One strict JSON object
// holding the current state's settings, mix (an entry left behind by an
// undone AddTrack included, so a redo brings its fader back), source pool,
// revision and id counter, then every undo step from the floor up and the
// pointer. Playlists, tracks, marker lists and edit states are tables
// referred to by index, so what the steps share crosses once and is shared
// again on the replica. A session file cannot stand in for it: it holds one
// state, and drops what undo needs.
//
// The decoder checks the current state's invariants and refuses anything
// that would not make a consistent History. What it builds is a replica's.
bool encode_window(const History& history, std::string& out, std::string& error);
bool decode_window(std::string_view bytes, std::optional<History>& out, std::string& error);

// "PlaceRegion", "AddMarker", ..., "Compound": for messages and the status
// line.
const char* operation_name(const Operation& op);
const char* member_name(const Member& m);

// What the Edit menu and the undo history call it ("Undo Paste"): a
// compound's label, or with none the name of its first edit member (of its
// first member when none edits); any other operation's name.
std::string display_name(const Operation& op);

}  // namespace sonora::model
