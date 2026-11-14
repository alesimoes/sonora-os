// SPDX-License-Identifier: GPL-3.0-or-later
#include "insert_key_controller.h"

#include "sonora/model/insert_key.h"

namespace sonora::ui {

using namespace model;

const Insert* InsertKeyController::insert_at(int track, int slot) const {
    const auto& tracks = ctl_.snapshot().edit->tracks;
    if (track < 0 || track >= static_cast<int>(tracks.size())) return nullptr;
    for (const Insert& in : tracks[static_cast<std::size_t>(track)]->inserts) {
        if (in.slot == slot) return &in;
    }
    return nullptr;
}

KeyView InsertKeyController::view(int track, int slot) const {
    KeyView v;
    const Insert* in = insert_at(track, slot);
    if (in == nullptr) return v;
    const api::PluginInfo* p = ctl_.plugin_of(in->id);
    v.capable = p != nullptr && p->key_inputs > 0;
    v.bus = in->key_bus;
    if (in->key_bus == kNoId) return v;
    for (const Bus& b : ctl_.snapshot().edit->buses) {
        if (b.id == in->key_bus) {
            v.name = b.name;
            return v;
        }
    }
    v.missing = true;
    return v;
}

std::vector<KeyChoice> InsertKeyController::choices(int track, int slot) const {
    std::vector<KeyChoice> out;
    const KeyView v = view(track, slot);
    if (!v.capable) return out;
    out.push_back({kNoId, "None", v.bus == kNoId});
    for (const Bus& b : ctl_.snapshot().edit->buses) out.push_back({b.id, b.name, b.id == v.bus});
    return out;
}

bool InsertKeyController::set(int track, int slot, Id bus, std::string& why) {
    const Insert* in = insert_at(track, slot);
    if (in == nullptr) {
        why = "insert " + slot_letter(slot) + " is empty";
        return false;
    }
    if (!view(track, slot).capable) {
        why = std::string("cannot key: ") + insert_key_refusal::kNoKeyInput;
        return false;
    }
    if (in->key_bus == bus) return true;
    const Id track_id = ctl_.snapshot().edit->tracks[static_cast<std::size_t>(track)]->id;
    return ctl_.document().submit(SetInsertKey{track_id, in->id, bus}, why);
}

}  // namespace sonora::ui
