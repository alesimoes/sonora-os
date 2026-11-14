// SPDX-License-Identifier: GPL-3.0-or-later
//
// The side-chain key of an insert slot, without the window (ADR-0058 item
// 6): whether the slot's plugin has a key input (the catalogue's flag), which
// aux bus keys it, and choosing another as one undo step. No Qt. A refusal is
// the model's own text (a loop, a second keyed insert on the chain).
#pragma once

#include <string>
#include <vector>

#include "insert_controller.h"

namespace sonora::ui {

// What a slot shows of its key.
struct KeyView {
    bool capable = false;    // the plugin has a side-chain input
    model::Id bus = kNoId;   // 0: none
    bool missing = false;    // keyed by an id no bus has (an old file)
    std::string name;        // the bus's name, empty when none or missing
};

// One line of the chooser: "None", then every aux bus by name.
struct KeyChoice {
    model::Id bus = kNoId;
    std::string name;
    bool current = false;
};

class InsertKeyController {
public:
    explicit InsertKeyController(InsertController& inserts) : ctl_(inserts) {}

    KeyView view(int track, int slot) const;
    // Empty when the slot has no key-capable plugin: nothing is offered for
    // a plugin that cannot take a key.
    std::vector<KeyChoice> choices(int track, int slot) const;
    // One undo step. `bus` is a bus id from choices(), or kNoId for None.
    bool set(int track, int slot, model::Id bus, std::string& why);

private:
    const model::Insert* insert_at(int track, int slot) const;
    InsertController& ctl_;
};

}  // namespace sonora::ui
