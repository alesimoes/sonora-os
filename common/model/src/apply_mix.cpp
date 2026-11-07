// SPDX-License-Identifier: GPL-3.0-or-later
//
// apply() for the family MixOp (ADR-0059):
// the mixer: faders, buses, sends, outputs and automation.
#include "apply_detail.h"

namespace sonora::model::apply_detail {

bool Applier::operator()(const SetMix& op) {
    const std::size_t t = locate_track(*before_.edit, op.mix.track);
    if (t == kNone) return fail("track " + str(op.mix.track) + " does not exist");
    std::string where;
    const TrackEdit& track = *before_.edit->tracks[t];
    const std::string why = detail::mix_problem(op.mix, track.kind, track.channels, where);
    if (!why.empty()) return fail("mix." + where + ": " + why);
    after_ = before_;
    set_mix(after_.mix, op.mix);
    return true;
}

bool Applier::operator()(const AddBus& op) {
    if (!detail::valid_text(op.name)) return fail("the bus name is not valid UTF-8");
    if (!fresh(op.bus)) return false;
    if (before_.edit->buses.size() >= kMaxBuses) {
        return fail("a session holds at most " + str(static_cast<std::uint64_t>(kMaxBuses)) +
                    " aux buses");
    }
    BusMix mix = op.mix;
    mix.bus = op.bus;
    std::string where;
    const std::string why = detail::bus_mix_problem(mix, where);
    if (!why.empty()) return fail("mix." + where + ": " + why);
    auto edit = std::make_shared<EditState>(*before_.edit);
    Bus bus;
    bus.id = op.bus;
    bus.name = op.name;
    edit->buses.push_back(std::move(bus));
    after_ = before_;
    after_.edit = std::move(edit);
    set_bus_mix(after_.mix, mix);
    after_.next_id = op.bus + 1;
    return true;
}

bool Applier::operator()(const RemoveBus& op) {
    const std::size_t b = bus_index(op.bus);
    if (b == kNone) return fail("bus " + str(op.bus) + " is not an aux bus");
    const auto feeds = [&](const std::vector<Send>& sends) {
        for (const Send& sd : sends) {
            if (sd.bus == op.bus) return true;
        }
        return false;
    };
    const std::string& name = before_.edit->buses[b].name;
    for (const auto& t : before_.edit->tracks) {
        if (feeds(t->sends)) {
            return fail("bus \"" + name + "\" is fed by a send on track \"" + t->name +
                        "\"; remove the send first");
        }
    }
    for (const Bus& other : before_.edit->buses) {
        if (feeds(other.sends)) {
            return fail("bus \"" + name + "\" is fed by a send on bus \"" + other.name +
                        "\"; remove the send first");
        }
    }
    // A strip routed to it would have nowhere to play (ADR-0024 §3).
    for (const auto& t : before_.edit->tracks) {
        if (t->output == op.bus) {
            return fail("bus \"" + name + "\" is the output of track \"" + t->name +
                        "\"; route the track elsewhere first");
        }
    }
    for (const Bus& other : before_.edit->buses) {
        if (other.output == op.bus) {
            return fail("bus \"" + name + "\" is the output of bus \"" + other.name +
                        "\"; route the bus elsewhere first");
        }
    }
    auto edit = std::make_shared<EditState>(*before_.edit);
    edit->buses.erase(edit->buses.begin() + static_cast<std::ptrdiff_t>(b));
    after_ = before_;
    after_.edit = std::move(edit);
    return true;
}

bool Applier::operator()(const AddSend& op) {
    SendMix values = op.mix;
    values.send = op.send;
    std::string where;
    const std::string why = detail::send_mix_problem(values, where);
    if (!why.empty()) return fail("mix." + where + ": " + why);
    if (!fresh(op.send)) return false;
    if (op.slot >= kSendSlots) {
        return fail("slot " + str(static_cast<std::uint64_t>(op.slot)) + " is not one of the " +
                    str(static_cast<std::uint64_t>(kSendSlots)) + " send slots (A to J)");
    }
    const std::size_t dest = bus_index(op.bus);
    if (dest == kNone) return fail("bus " + str(op.bus) + " is not an aux bus");
    auto edit = std::make_shared<EditState>(*before_.edit);
    std::vector<Send>* sends = nullptr;
    std::shared_ptr<TrackEdit> track;
    const std::size_t t = locate_track(*before_.edit, op.strip);
    if (t != kNone) {
        if (before_.edit->tracks[t]->kind == TrackKind::Midi) {
            return fail("track " + str(op.strip) + " is a MIDI track; it has no audio to send");
        }
        track = std::make_shared<TrackEdit>(*before_.edit->tracks[t]);
        sends = &track->sends;
    } else {
        const std::size_t b = bus_index(op.strip);
        if (b == kNone) {
            return fail("strip " + str(op.strip) + " is neither a track nor an aux bus");
        }
        if (op.strip == op.bus) return fail("a bus cannot send to itself");
        // The new send closes a loop if its destination already reaches
        // its strip (ADR-0022 §4).
        const std::vector<Id> path = detail::bus_path(*before_.edit, op.bus, op.strip);
        if (!path.empty()) {
            std::string chain;
            for (const Id id : path) {
                chain += (chain.empty() ? "" : " -> ") + bus_name(id);
            }
            return fail("a send from bus " + bus_name(op.strip) + " to bus " +
                        bus_name(op.bus) + " would make a feedback loop: " + chain +
                        " already");
        }
        sends = &edit->buses[b].sends;
    }
    for (const Send& sd : *sends) {
        if (sd.slot == op.slot) {
            return fail("send slot " + std::string(1, static_cast<char>('A' + op.slot)) +
                        " on strip " + str(op.strip) + " already holds send " + str(sd.id));
        }
        if (sd.bus == op.bus) {
            return fail("strip " + str(op.strip) + " already sends to bus " +
                        bus_name(op.bus) + " (slot " +
                        std::string(1, static_cast<char>('A' + sd.slot)) + ")");
        }
    }
    const auto at = std::lower_bound(
        sends->begin(), sends->end(), op.slot,
        [](const Send& a, std::uint8_t slot) { return a.slot < slot; });
    sends->insert(at, Send{op.send, op.slot, op.bus});
    if (track) edit->tracks[t] = std::move(track);
    after_ = before_;
    after_.edit = std::move(edit);
    set_send_mix(after_.mix, values);
    after_.next_id = op.send + 1;
    return true;
}

bool Applier::operator()(const RemoveSend& op) {
    auto edit = std::make_shared<EditState>(*before_.edit);
    const auto erase = [&](std::vector<Send>& sends) {
        for (auto it = sends.begin(); it != sends.end(); ++it) {
            if (it->id == op.send) {
                sends.erase(it);
                return true;
            }
        }
        return false;
    };
    // Its lanes go with it (ADR-0027 §9); undo brings both back.
    const auto drop_lanes = [&](AutomationLanes& lanes) {
        lanes.erase(std::remove_if(lanes.begin(), lanes.end(),
                                   [&](const std::shared_ptr<const AutomationLane>& l) {
                                       return l->send == op.send;
                                   }),
                    lanes.end());
    };
    bool found = false;
    for (std::size_t t = 0; t < edit->tracks.size() && !found; ++t) {
        if (!has_send(edit->tracks[t]->sends, op.send)) continue;
        auto track = std::make_shared<TrackEdit>(*edit->tracks[t]);
        found = erase(track->sends);
        drop_lanes(track->automation);
        edit->tracks[t] = std::move(track);
    }
    for (std::size_t b = 0; b < edit->buses.size() && !found; ++b) {
        found = erase(edit->buses[b].sends);
        if (found) drop_lanes(edit->buses[b].automation);
    }
    if (!found) return no_send(op.send);
    after_ = before_;
    after_.edit = std::move(edit);
    return true;
}

bool Applier::operator()(const SetSendMix& op) {
    if (!send_exists(op.mix.send)) return no_send(op.mix.send);
    std::string where;
    const std::string why = detail::send_mix_problem(op.mix, where);
    if (!why.empty()) return fail("send " + str(op.mix.send) + "." + where + ": " + why);
    after_ = before_;
    set_send_mix(after_.mix, op.mix);
    return true;
}

bool Applier::operator()(const SetBusMix& op) {
    if (bus_index(op.mix.bus) == kNone) {
        return fail("bus " + str(op.mix.bus) + " is not an aux bus");
    }
    std::string where;
    const std::string why = detail::bus_mix_problem(op.mix, where);
    if (!why.empty()) return fail("bus " + str(op.mix.bus) + "." + where + ": " + why);
    after_ = before_;
    set_bus_mix(after_.mix, op.mix);
    return true;
}

bool Applier::operator()(const SetOutput& op) {
    if (op.output != kMasterOutput && bus_index(op.output) == kNone) {
        return fail("output " + str(op.output) + " is neither the Master nor an aux bus");
    }
    auto edit = std::make_shared<EditState>(*before_.edit);
    const std::size_t t = locate_track(*before_.edit, op.strip);
    if (t != kNone) {
        if (before_.edit->tracks[t]->kind == TrackKind::Midi) {
            return fail("track " + str(op.strip) + " is a MIDI track; it has no audio output");
        }
        auto track = std::make_shared<TrackEdit>(*before_.edit->tracks[t]);
        track->output = op.output;
        edit->tracks[t] = std::move(track);
    } else {
        const std::size_t b = bus_index(op.strip);
        if (b == kNone) {
            return fail("strip " + str(op.strip) + " is neither a track nor an aux bus");
        }
        if (op.output == op.strip) return fail("a bus cannot be its own output");
        if (edit->buses[b].hardware != kNoHardwareOut) {
            return fail("bus " + bus_name(op.strip) +
                        " is an output path; it plays on the interface");
        }
        // Its new output closes a loop if that bus already reaches it
        // over sends and outputs (ADR-0024 §3). The search stops at the
        // strip, so the output being replaced is never followed.
        if (op.output != kMasterOutput) {
            const std::vector<Id> path = detail::bus_path(*before_.edit, op.output, op.strip);
            if (!path.empty()) {
                std::string chain;
                for (const Id id : path) {
                    chain += (chain.empty() ? "" : " -> ") + bus_name(id);
                }
                return fail("bus " + bus_name(op.strip) + " to bus " + bus_name(op.output) +
                            " would make a feedback loop: " + chain + " already");
            }
        }
        edit->buses[b].output = op.output;
    }
    after_ = before_;
    after_.edit = std::move(edit);
    return true;
}

bool Applier::operator()(const SetBusPath& op) {
    const std::size_t b = bus_index(op.bus);
    if (b == kNone) return fail("bus " + str(op.bus) + " is not an aux bus");
    if (!detail::valid_text(op.name) || op.name.empty() || op.name.size() > 64) {
        return fail("an output path's name is 1 to 64 characters of valid UTF-8");
    }
    if (op.first != kNoHardwareOut) {
        if (op.first % 2 != 0 || op.first + 1 >= kMaxOutputChannels) {
            return fail("an output path is a pair of outputs from an odd number: 1-2, 3-4"
                        " ... up to " + str(static_cast<std::uint64_t>(kMaxOutputChannels)));
        }
        const auto& master = before_.mix.bus.outputs;
        if (in_pair(op.first, master[0]) || in_pair(op.first, master[1])) {
            return fail("the Master plays on outputs " + std::to_string(master[0] + 1u) + " and " +
                        std::to_string(master[1] + 1u) + "; pick another pair");
        }
        for (const Bus& other : before_.edit->buses) {
            if (other.id != op.bus && other.hardware == op.first) {
                return fail("outputs " + std::to_string(other.hardware + 1u) + " and " +
                            std::to_string(other.hardware + 2u) + " already carry the path \"" +
                            other.name + "\"");
            }
        }
    }
    auto edit = std::make_shared<EditState>(*before_.edit);
    Bus& bus = edit->buses[b];
    bus.name = op.name;
    bus.hardware = op.first;
    // A path ends at the interface, so it leaves the bus it fed.
    if (op.first != kNoHardwareOut) bus.output = kMasterOutput;
    after_ = before_;
    after_.edit = std::move(edit);
    return true;
}

bool Applier::operator()(const EditAutomation& op) {
    const std::size_t t =
        op.track == kMasterOutput ? kNone : locate_track(*before_.edit, op.track);
    const std::size_t b = op.track == kMasterOutput || t != kNone ? kNone : bus_index(op.track);
    detail::LaneStrip strip = detail::master_strip();
    const AutomationLanes* before_lanes = &before_.edit->master_automation;
    if (t != kNone) {
        const TrackEdit& before_track = *before_.edit->tracks[t];
        if (before_track.kind == TrackKind::Midi) {
            return fail("track " + str(op.track) +
                        " is a MIDI track; it has no audio automation");
        }
        strip = detail::lane_strip(before_track);
        before_lanes = &before_track.automation;
    } else if (b != kNone) {
        strip = detail::lane_strip(before_.edit->buses[b]);
        before_lanes = &before_.edit->buses[b].automation;
    } else if (op.track != kMasterOutput) {
        return fail("track " + str(op.track) + " does not exist, nor an aux bus");
    }
    PluginFormat format = PluginFormat::Lv2;
    std::string why = detail::automation_target_problem(strip, op.param, op.insert, op.send,
                                                        op.key, format);
    if (!why.empty()) return fail(why);
    if (op.from < 0 || op.to > kMaxInteger || op.from >= op.to) {
        return fail("the range must run forward, from 0 to at most 2^53 - 1");
    }
    std::string where;
    why = detail::automation_points_problem(op.points, op.param, where, format);
    if (!why.empty()) return fail(where + ": " + why);
    for (const AutomationPoint& p : op.points) {
        if (p.at < op.from || p.at >= op.to) {
            return fail("a point at " + str(p.at) + " lies outside the range it replaces");
        }
    }
    AutomationLane target;
    target.param = op.param;
    target.insert = op.insert;
    target.key = op.key;
    target.send = op.send;
    std::vector<AutomationPoint> points;
    const AutomationLane* old = find_lane(*before_lanes, target);
    if (old != nullptr) {
        for (const AutomationPoint& p : old->points) {
            if (p.at < op.from) points.push_back(p);
        }
    }
    points.insert(points.end(), op.points.begin(), op.points.end());
    if (old != nullptr) {
        for (const AutomationPoint& p : old->points) {
            if (p.at >= op.to) points.push_back(p);
        }
    }
    if (points.size() > kMaxAutomationPoints) {
        return fail("the lane would hold " + str(static_cast<std::int64_t>(points.size())) +
                    " points; it holds at most " +
                    str(static_cast<std::int64_t>(kMaxAutomationPoints)));
    }
    if (op.param == AutomationParam::Mute) {
        // Where the edit meets the points it keeps, the mute could change
        // between two frames, which the edit's own points cannot show.
        why = detail::automation_points_problem(points, op.param, where, format);
        if (!why.empty()) return fail("the lane after the edit: " + why);
    }
    if (old == nullptr && !points.empty() && op.param == AutomationParam::Plugin) {
        std::size_t held = 0;
        for (const auto& l : *before_lanes) {
            if (l->param == AutomationParam::Plugin && l->insert == op.insert) ++held;
        }
        if (held >= kMaxInsertParams) {
            return fail("insert " + str(static_cast<std::uint64_t>(op.insert)) +
                        " has " + str(static_cast<std::int64_t>(kMaxInsertParams)) +
                        " automated parameters, the most an insert has");
        }
    }
    auto edit = std::make_shared<EditState>(*before_.edit);
    std::shared_ptr<TrackEdit> track;
    AutomationLanes* lanes_out = &edit->master_automation;
    if (t != kNone) {
        track = std::make_shared<TrackEdit>(*before_.edit->tracks[t]);
        lanes_out = &track->automation;
    } else if (b != kNone) {
        lanes_out = &edit->buses[b].automation;
    }
    auto& lanes = *lanes_out;
    const auto at = std::lower_bound(
        lanes.begin(), lanes.end(), target,
        [](const std::shared_ptr<const AutomationLane>& l, const AutomationLane& x) {
            return lane_before(*l, x);
        });
    const bool present = at != lanes.end() && !lane_before(target, **at);
    if (points.empty()) {
        if (present) lanes.erase(at);
    } else {
        auto lane = std::make_shared<AutomationLane>(std::move(target));
        lane->points = std::move(points);
        if (present) {
            *at = std::move(lane);
        } else {
            lanes.insert(at, std::move(lane));
        }
    }
    if (track) edit->tracks[t] = std::move(track);
    after_ = before_;
    after_.edit = std::move(edit);
    return true;
}

}  // namespace sonora::model::apply_detail
