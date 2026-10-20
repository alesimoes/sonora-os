// SPDX-License-Identifier: GPL-3.0-or-later
//
// A MIDI sequence's three lists as JSON, one spelling for the session file
// and the operation codec (midi.md §4.4): a note is
// [start, length, channel, pitch, velocity, release_velocity], an event
// [tick, status, data1, data2], a SysEx message [tick, "f0...f7"] in
// lower-case hex. Each row is an array of scalars, which the writer puts on
// one line.
//
// The readers check shape and byte ranges only; whether the values make a
// valid sequence is detail::sequence_problem()'s question, asked by both
// callers afterwards.
#pragma once

#include <string>
#include <vector>

#include "sonora/json/json.h"
#include "sonora/model/model.h"

namespace sonora::model::detail {

json::Value note_row(const midi::Note& n);
json::Value event_row(const MidiEvent& e);
json::Value sysex_row(const MidiSysEx& x);

// Why a list did not read: `where` is relative to the list ("[3][2]") and
// `at` the node, for a caller that reports lines.
struct RowError {
    std::string message;
    std::string where;
    const json::Value* at = nullptr;
};

bool read_note_rows(const json::Value& list, std::vector<midi::Note>& out, RowError& error);
bool read_event_rows(const json::Value& list, std::vector<MidiEvent>& out, RowError& error);
bool read_sysex_rows(const json::Value& list, std::vector<MidiSysEx>& out, RowError& error);

}  // namespace sonora::model::detail
