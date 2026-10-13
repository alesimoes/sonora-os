// SPDX-License-Identifier: GPL-3.0-or-later
//
// One Operation to bytes and back: the replication format (ADR-0009 §2).
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

#include <string>
#include <string_view>

#include "sonora/model/operations.h"

namespace sonora::model {

// Canonical: the same operation always gives the same bytes.
bool encode_operation(const Operation& op, std::string& out, std::string& error);
bool decode_operation(std::string_view bytes, Operation& out, std::string& error);

// "PlaceRegion", "AddMarker", ...: for messages and the status line.
const char* operation_name(const Operation& op);

}  // namespace sonora::model
