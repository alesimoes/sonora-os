// SPDX-License-Identifier: GPL-3.0-or-later
//
// The side-chain key's place in the bus graph (ADR-0058 §3). A key edge runs
// from the key bus's sum into the keyed chain, so through that chain's track
// into the track's output and its sends' buses: a key loops when those reach
// the key bus again.
#include <string>
#include <vector>

#include "checks.h"
#include "sonora/model/insert_key.h"

namespace sonora::model::detail {

namespace {

Id chain_key(const TrackEdit& t) {
    for (const Insert& in : t.inserts) {
        if (in.key_bus != 0) return in.key_bus;
    }
    return 0;
}

// Where a track's audio goes on the bus graph: its output when that is a
// bus, then its sends' buses.
std::vector<Id> track_buses(const TrackEdit& t) {
    std::vector<Id> out;
    if (t.output != kMasterOutput) out.push_back(t.output);
    for (const Send& sd : t.sends) out.push_back(sd.bus);
    return out;
}

std::string bus_label(const EditState& e, Id id) {
    const Bus* b = find_bus(e, id);
    return b == nullptr ? "bus " + std::to_string(id) : "bus \"" + b->name + "\"";
}

// "track "T" -> bus "A" -> bus "K" -> key of track "T"": the loop through
// `path`, which runs from one of the track's buses to its key bus.
std::string loop_text(const EditState& e, const TrackEdit& t, const std::vector<Id>& path) {
    std::string text = "track \"" + t.name + "\"";
    for (const Id id : path) text += " -> " + bus_label(e, id);
    return text + " -> key of track \"" + t.name + "\"";
}

}  // namespace

std::vector<Id> key_fed_buses(const EditState& e, Id bus) {
    std::vector<Id> out;
    for (const auto& t : e.tracks) {
        if (chain_key(*t) != bus) continue;
        for (const Id id : track_buses(*t)) out.push_back(id);
    }
    return out;
}

std::string key_loop(const EditState& e, const TrackEdit& track, Id key_bus) {
    for (const Id from : track_buses(track)) {
        const std::vector<Id> path = bus_path(e, from, key_bus);
        if (!path.empty()) return loop_text(e, track, path);
    }
    return {};
}

std::string track_route_loop(const EditState& e, const TrackEdit& track, Id bus) {
    const Id key = chain_key(track);
    if (key == 0 || bus == kMasterOutput || find_bus(e, key) == nullptr) return {};
    const std::vector<Id> path = bus_path(e, bus, key);
    return path.empty() ? std::string() : loop_text(e, track, path);
}

std::string key_refusal(const EditState& e, const TrackEdit& track, Id insert, Id key_bus) {
    if (key_bus == 0) return {};
    for (const Insert& in : track.inserts) {
        if (in.id != insert && in.key_bus != 0) {
            return std::string(insert_key_refusal::kSecondKey) + ": insert " +
                   std::to_string(in.id) + " on track \"" + track.name +
                   "\" already listens to a key, and a chain takes one";
        }
    }
    const std::string loop = key_loop(e, track, key_bus);
    if (!loop.empty()) return std::string(insert_key_refusal::kLoop) + ": " + loop;
    return {};
}

}  // namespace sonora::model::detail
