// SPDX-License-Identifier: GPL-3.0-or-later
//
// From the API's reply to a reviewable model::Plan (ADR-0155 §4, ADR-0038).
// The reply is untrusted text: it may name only the ops below and only ids
// present in the bundle the request was built from, and every value is
// range-checked. Anything else refuses the whole reply, loudly, rather than
// applying the parts that parsed; the operator then sees native suggestions
// and Unavailable::BadReply.
#pragma once

#include <array>
#include <string>

#include "sonora/assistant/feature_bundle.h"
#include "sonora/model/plan.h"

namespace sonora::assistant {

// The ops a reply may use, each mapping to one plan_revertible member, so
// one undo takes the whole accepted plan back:
//   set_track_gain  {track, gain_db}        -> SetMix
//   set_track_pan   {track, pan}            -> SetMix
//   set_send_level  {send, level_db}        -> SetSendMix
//   set_bus_gain    {bus, gain_db}          -> SetBusMix
constexpr std::array<const char*, 4> kReplyOps = {
    "set_track_gain", "set_track_pan", "set_send_level", "set_bus_gain"};

// Fills `out` with steps whose title and reason are the reply's own words,
// label "Claude: <intent>", base_revision = bundle.base_revision; `at` is
// the snapshot the bundle was taken from, read for the values a step does
// not change. False, with `error` naming the offending step, on malformed
// JSON, an unknown op, an id not in the bundle, a value out of range, or a
// plan that model::plan_problem refuses.
bool parse_reply(const std::string& body, const FeatureBundle& bundle,
                 const model::Snapshot& at, model::Plan& out, std::string& error);

}  // namespace sonora::assistant
