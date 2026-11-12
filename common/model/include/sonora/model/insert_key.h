// SPDX-License-Identifier: GPL-3.0-or-later
//
// The side-chain key of an insert (ADR-0058): which aux bus's sum a keyed
// plug-in listens to. Its own header so operations.h does not grow.
#pragma once

#include "sonora/model/model.h"

namespace sonora::model {

// Undoable, self-inverse: keys insert `insert` on `track` by aux bus
// `key_bus`, or unkeys it when `key_bus` is 0. Refused for an unknown track,
// insert or bus, for a second keyed insert on the same chain (the plug-in
// host's key is per chain), and for a key that closes a loop through the
// buses (ADR-0024's graph, which the key edge joins). Whether the plug-in
// has a side-chain input is the catalogue's, so the engine refuses that one
// before apply(); the model cannot see the catalogue.
struct SetInsertKey {
    Id track = 0;
    Id insert = 0;
    Id key_bus = 0;
};

// The reasons a SetInsertKey is refused, as the start of its error text, so
// a client can tell them apart without parsing the rest.
namespace insert_key_refusal {
inline constexpr const char* kUnknownTrack = "no track";
inline constexpr const char* kUnknownInsert = "no insert";
inline constexpr const char* kUnknownBus = "no aux bus";
inline constexpr const char* kSecondKey = "another insert on the chain is keyed";
inline constexpr const char* kLoop = "a key that makes a feedback loop";
inline constexpr const char* kNoKeyInput = "the plug-in has no side-chain input";
}  // namespace insert_key_refusal

}  // namespace sonora::model
