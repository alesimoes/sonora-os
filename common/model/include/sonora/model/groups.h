// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mix and edit groups (ADR-0041): what a move on one member does to the
// others. Control-thread code, shared by the interface (which sends the
// result as one Compound of SetMix) and by the engine (which reads who a
// group arms with the track). Nothing here runs on the audio thread: the
// audio callback only ever sees the per-track atomics the results end in.
#pragma once

#include <cstddef>
#include <vector>

#include "sonora/model/model.h"

namespace sonora::model {

// The active group that links `attr` for `track`, or nullptr.
const Group* linking_group(const EditState& e, Id track, std::uint8_t attr);

// The other members `track`'s group links through `attr`: empty when the
// track is in no group, the group is suspended or does not carry `attr`.
std::vector<Id> group_peers(const EditState& e, Id track, std::uint8_t attr);

// The peers' mixes after `changed` replaced the mix `s` holds for
// changed.track. Volume follows the group's mode (relative: the same dB
// added to each peer, clamped; absolute: every peer to the new value); mute
// and solo are copied. Only peers that differ are returned, and a mix field
// a group does not link is left as it is. Empty when nothing is linked.
std::vector<TrackMix> group_follow(const Snapshot& s, const TrackMix& changed);

// A peer's gain when the leader's goes from `from_db` to `to_db`: what a
// fader ride sends the engine for each member (group_follow's volume rule).
float group_follow_gain(const Group& g, float peer_db, float from_db, float to_db);

// Display positions of the group's members in `e`, ascending.
std::vector<int> group_positions(const EditState& e, const Group& g);

}  // namespace sonora::model
