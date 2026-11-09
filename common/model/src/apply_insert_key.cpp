// SPDX-License-Identifier: GPL-3.0-or-later
//
// apply() for SetInsertKey (ADR-0058), of the family MixOp, and what a
// removed bus does to the keys naming it.
#include "apply_detail.h"

namespace sonora::model::apply_detail {

bool Applier::operator()(const SetInsertKey& op) {
    namespace why = insert_key_refusal;
    const std::size_t t = locate_track(*before_.edit, op.track);
    if (t == kNone) return fail(std::string(why::kUnknownTrack) + " " + str(op.track));
    const TrackEdit& track = *before_.edit->tracks[t];
    std::size_t i = 0;
    while (i < track.inserts.size() && track.inserts[i].id != op.insert) ++i;
    if (i == track.inserts.size()) {
        return fail(std::string(why::kUnknownInsert) + " " + str(op.insert) + " on track \"" +
                    track.name + "\"");
    }
    if (op.key_bus != 0 && bus_index(op.key_bus) == kNone) {
        return fail(std::string(why::kUnknownBus) + " " + str(op.key_bus) +
                    " to key insert " + str(op.insert));
    }
    const std::string refused = detail::key_refusal(*before_.edit, track, op.insert, op.key_bus);
    if (!refused.empty()) return fail(refused);
    return edit_track(op.track, [&](TrackEdit& copy) {
        copy.inserts[i].key_bus = op.key_bus;
        return true;
    });
}

void clear_keys(EditState& edit, Id bus) {
    for (auto& t : edit.tracks) {
        bool keyed = false;
        for (const Insert& in : t->inserts) keyed = keyed || in.key_bus == bus;
        if (!keyed) continue;
        auto copy = std::make_shared<TrackEdit>(*t);
        for (Insert& in : copy->inserts) {
            if (in.key_bus == bus) in.key_bus = 0;
        }
        t = std::move(copy);
    }
}

}  // namespace sonora::model::apply_detail
