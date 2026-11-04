// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/groups.h"

#include <algorithm>

namespace sonora::model {

const Group* linking_group(const EditState& e, Id track, std::uint8_t attr) {
    for (const Group& g : e.groups) {
        if (!g.active || (g.attrs & attr) == 0) continue;
        if (std::find(g.members.begin(), g.members.end(), track) != g.members.end()) return &g;
    }
    return nullptr;
}

std::vector<Id> group_peers(const EditState& e, Id track, std::uint8_t attr) {
    std::vector<Id> out;
    const Group* g = linking_group(e, track, attr);
    if (g == nullptr) return out;
    for (const Id m : g->members) {
        if (m != track) out.push_back(m);
    }
    return out;
}

float group_follow_gain(const Group& g, float peer_db, float from_db, float to_db) {
    const float target = g.relative ? peer_db + (to_db - from_db) : to_db;
    return std::clamp(target, kMinGainDb, kMaxGainDb);
}

std::vector<TrackMix> group_follow(const Snapshot& s, const TrackMix& changed) {
    std::vector<TrackMix> out;
    const TrackMix* was = s.mix.find(changed.track);
    if (was == nullptr) return out;
    const auto follow = [&](std::uint8_t attr, auto&& apply) {
        const Group* g = linking_group(*s.edit, changed.track, attr);
        if (g == nullptr) return;
        for (const Id peer : g->members) {
            if (peer == changed.track) continue;
            const TrackMix* cur = s.mix.find(peer);
            if (cur == nullptr) continue;
            auto at = std::find_if(out.begin(), out.end(),
                                   [&](const TrackMix& m) { return m.track == peer; });
            if (at == out.end()) {
                out.push_back(*cur);
                at = out.end() - 1;
            }
            apply(*g, *at);
        }
    };
    if (changed.gain_db != was->gain_db) {
        follow(kGroupVolume, [&](const Group& g, TrackMix& m) {
            m.gain_db = group_follow_gain(g, m.gain_db, was->gain_db, changed.gain_db);
        });
    }
    if (changed.mute != was->mute) {
        follow(kGroupMute, [&](const Group&, TrackMix& m) { m.mute = changed.mute; });
    }
    if (changed.solo != was->solo) {
        follow(kGroupSolo, [&](const Group&, TrackMix& m) { m.solo = changed.solo; });
    }
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const TrackMix& m) { return m == *s.mix.find(m.track); }),
              out.end());
    return out;
}

std::vector<int> group_positions(const EditState& e, const Group& g) {
    std::vector<int> out;
    for (std::size_t i = 0; i < e.tracks.size(); ++i) {
        if (std::find(g.members.begin(), g.members.end(), e.tracks[i]->id) != g.members.end()) {
            out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

}  // namespace sonora::model
