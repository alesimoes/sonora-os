// SPDX-License-Identifier: GPL-3.0-or-later
#include "send_controller.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace sonora::ui {

using namespace model;

namespace {

std::string letter(int slot) { return std::string(1, static_cast<char>('A' + slot)); }

// Whether `from` reaches `to` along the buses' sends and outputs.
bool reaches(const EditState& e, Id from, Id to) {
    std::vector<Id> stack{from};
    std::unordered_set<Id> seen{from};
    while (!stack.empty()) {
        const Id id = stack.back();
        stack.pop_back();
        if (id == to) return true;
        for (const Bus& b : e.buses) {
            if (b.id != id) continue;
            for (const Send& sd : b.sends) {
                if (seen.insert(sd.bus).second) stack.push_back(sd.bus);
            }
            if (b.output != kMasterOutput && seen.insert(b.output).second) stack.push_back(b.output);
        }
    }
    return false;
}

}  // namespace

SendController::SendController(TimelineDocument& doc) : doc_(doc) {}

const std::vector<Send>* SendController::sends_of(int strip) const {
    const EditState& e = *snapshot().edit;
    if (strip < 0) return nullptr;
    if (strip < track_count()) return &e.tracks[static_cast<std::size_t>(strip)]->sends;
    const int b = strip - track_count();
    if (b < bus_count()) return &e.buses[static_cast<std::size_t>(b)].sends;
    return nullptr;
}

Id SendController::strip_id(int strip) const {
    const EditState& e = *snapshot().edit;
    if (strip < 0) return kNoId;
    if (strip < track_count()) return e.tracks[static_cast<std::size_t>(strip)]->id;
    const int b = strip - track_count();
    return b < bus_count() ? e.buses[static_cast<std::size_t>(b)].id : kNoId;
}

bool SendController::strip_sends(int strip) const {
    if (strip >= 0 && strip < track_count()) {
        return snapshot().edit->tracks[static_cast<std::size_t>(strip)]->kind != TrackKind::Midi;
    }
    return sends_of(strip) != nullptr;
}

std::string SendController::strip_name(int strip) const {
    const EditState& e = *snapshot().edit;
    if (strip >= 0 && strip < track_count()) return e.tracks[static_cast<std::size_t>(strip)]->name;
    const int b = strip - track_count();
    return b >= 0 && b < bus_count() ? e.buses[static_cast<std::size_t>(b)].name : std::string();
}

const Send* SendController::send_at(int strip, int slot) const {
    const std::vector<Send>* sends = sends_of(strip);
    if (sends == nullptr) return nullptr;
    for (const Send& sd : *sends) {
        if (sd.slot == slot) return &sd;
    }
    return nullptr;
}

bool SendController::strip_slot(int strip, int slot, std::string& why) const {
    if (sends_of(strip) == nullptr) {
        why = "that strip is gone";
        return false;
    }
    if (!strip_sends(strip)) {
        why = "\"" + strip_name(strip) + "\" is a MIDI track; it has no audio to send";
        return false;
    }
    if (slot < 0 || slot >= kSendSlots) {
        why = "sends are A to J";
        return false;
    }
    return true;
}

SendSlotView SendController::slot(int strip, int slot) const {
    SendSlotView v;
    const Send* sd = send_at(strip, slot);
    if (sd == nullptr) return v;
    v.present = true;
    v.send = sd->id;
    v.bus = sd->bus;
    for (const Bus& b : snapshot().edit->buses) {
        if (b.id == sd->bus) v.bus_name = b.name;
    }
    if (const SendMix* m = snapshot().mix.find_send(sd->id)) {
        v.level_db = m->level_db;
        v.pan = m->pan;
        v.follow_pan = m->follow_pan;
        v.mute = m->mute;
        v.pre = m->pre;
    }
    return v;
}

AuxView SendController::aux(int bus) const {
    AuxView v;
    if (bus < 0 || bus >= bus_count()) return v;
    const Bus& b = snapshot().edit->buses[static_cast<std::size_t>(bus)];
    v.id = b.id;
    v.name = b.name;
    if (const BusMix* m = snapshot().mix.find_bus(b.id)) {
        v.gain_db = m->gain_db;
        v.pan = m->pan;
        v.mute = m->mute;
        v.solo = m->solo;
    }
    return v;
}

int SendController::output(int strip) const {
    const EditState& e = *snapshot().edit;
    Id out = kMasterOutput;
    if (strip >= 0 && strip < track_count()) {
        out = e.tracks[static_cast<std::size_t>(strip)]->output;
    } else if (strip >= track_count() && strip < strip_count()) {
        out = e.buses[static_cast<std::size_t>(strip - track_count())].output;
    }
    for (int b = 0; b < bus_count(); ++b) {
        if (e.buses[static_cast<std::size_t>(b)].id == out) return b;
    }
    return kMaster;
}

std::string SendController::output_name(int strip) const {
    const int b = output(strip);
    return b == kMaster ? "Master" : snapshot().edit->buses[static_cast<std::size_t>(b)].name;
}

std::vector<SendDestination> SendController::output_choices(int strip) const {
    std::vector<SendDestination> out;
    const EditState& e = *snapshot().edit;
    const Id from = strip_id(strip);
    const bool from_bus = strip >= track_count();
    const bool audio = strip_sends(strip);
    const std::string midi = "\"" + strip_name(strip) + "\" is a MIDI track; it has no audio output";
    out.push_back({kMaster, "Master", audio ? std::string() : midi});
    for (int b = 0; b < bus_count(); ++b) {
        const Bus& bus = e.buses[static_cast<std::size_t>(b)];
        SendDestination d{b, bus.name, {}};
        if (!audio) {
            d.refusal = midi;
        } else if (from_bus && bus.id == from) {
            d.refusal = "a bus cannot output to itself";
        } else if (from_bus && reaches(e, bus.id, from)) {
            d.refusal = "\"" + bus.name + "\" already feeds \"" + strip_name(strip) +
                        "\": a feedback loop";
        }
        out.push_back(std::move(d));
    }
    return out;
}

bool SendController::set_output(int strip, int bus, std::string& why) {
    const Id id = strip_id(strip);
    if (id == kNoId) {
        why = "that strip is gone";
        return false;
    }
    if (bus != kMaster && (bus < 0 || bus >= bus_count())) {
        why = "that bus is gone";
        return false;
    }
    if (output(strip) == bus) return true;
    const Id dest =
        bus == kMaster ? kMasterOutput : snapshot().edit->buses[static_cast<std::size_t>(bus)].id;
    return doc_.submit(SetOutput{id, dest}, why);
}

std::vector<SendDestination> SendController::destinations(int strip, int slot) const {
    std::vector<SendDestination> out;
    const EditState& e = *snapshot().edit;
    const Id from = strip_id(strip);
    const bool from_bus = strip >= track_count();
    const Send* here = send_at(strip, slot);
    const std::vector<Send>* sends = sends_of(strip);
    for (int b = 0; b < bus_count(); ++b) {
        const Bus& bus = e.buses[static_cast<std::size_t>(b)];
        SendDestination d{b, bus.name, {}};
        if (from_bus && bus.id == from) {
            d.refusal = "a bus cannot send to itself";
        } else if (from_bus && reaches(e, bus.id, from)) {
            d.refusal = "\"" + bus.name + "\" already feeds \"" + strip_name(strip) +
                        "\": a feedback loop";
        } else if (sends != nullptr) {
            for (const Send& sd : *sends) {
                if (sd.bus == bus.id && (here == nullptr || sd.id != here->id)) {
                    d.refusal = "send " + letter(sd.slot) + " already goes there";
                }
            }
        }
        out.push_back(std::move(d));
    }
    return out;
}

bool SendController::assign(int strip, int slot, int bus, std::string& why) {
    if (!strip_slot(strip, slot, why)) return false;
    if (bus < 0 || bus >= bus_count()) {
        why = "that bus is gone";
        return false;
    }
    const Bus& dest = snapshot().edit->buses[static_cast<std::size_t>(bus)];
    AddSend add;
    add.strip = strip_id(strip);
    add.slot = static_cast<std::uint8_t>(slot);
    add.bus = dest.id;
    const Send* old = send_at(strip, slot);
    if (old == nullptr) {
        add.send = doc_.reserve_id();
        return doc_.submit(add, why);
    }
    if (old->bus == dest.id) return true;
    if (const SendMix* m = snapshot().mix.find_send(old->id)) add.mix = *m;
    Compound c;
    c.label = "Assign Send";
    c.ops.push_back(RemoveSend{old->id});
    add.send = doc_.reserve_id();
    c.ops.push_back(add);
    return doc_.submit(c, why);
}

bool SendController::assign_new_bus(int strip, int slot, std::string& why) {
    if (!strip_slot(strip, slot, why)) return false;
    Compound c;
    c.label = "New Bus Send";
    AddBus bus;
    bus.bus = doc_.reserve_id();
    bus.name = next_bus_name();
    const Send* old = send_at(strip, slot);
    AddSend add;
    add.strip = strip_id(strip);
    add.slot = static_cast<std::uint8_t>(slot);
    add.bus = bus.bus;
    if (old != nullptr) {
        if (const SendMix* m = snapshot().mix.find_send(old->id)) add.mix = *m;
        c.ops.push_back(RemoveSend{old->id});
    }
    c.ops.push_back(bus);
    add.send = doc_.reserve_id();
    c.ops.push_back(add);
    return doc_.submit(c, why);
}

bool SendController::remove(int strip, int slot, std::string& why) {
    if (!strip_slot(strip, slot, why)) return false;
    const Send* sd = send_at(strip, slot);
    if (sd == nullptr) {
        why = "send " + letter(slot) + " is empty";
        return false;
    }
    return doc_.submit(RemoveSend{sd->id}, why);
}

bool SendController::new_bus(std::string& why) {
    AddBus op;
    op.bus = doc_.reserve_id();
    op.name = next_bus_name();
    return doc_.submit(op, why);
}

bool SendController::remove_bus(int bus, std::string& why) {
    if (bus < 0 || bus >= bus_count()) {
        why = "that bus is gone";
        return false;
    }
    const EditState& e = *snapshot().edit;
    const Id id = e.buses[static_cast<std::size_t>(bus)].id;
    Compound c;
    c.label = "Remove Bus";
    // A strip that outputs to the bus goes back to the Master first, in the
    // same step: the model refuses the removal while one is routed to it.
    for (const auto& t : e.tracks) {
        if (t->output == id) c.ops.push_back(SetOutput{t->id, kMasterOutput});
    }
    for (const Bus& other : e.buses) {
        if (other.output == id) c.ops.push_back(SetOutput{other.id, kMasterOutput});
    }
    const auto feeding = [&](const std::vector<Send>& sends) {
        for (const Send& sd : sends) {
            if (sd.bus == id) c.ops.push_back(RemoveSend{sd.id});
        }
    };
    for (const auto& t : e.tracks) feeding(t->sends);
    for (const Bus& b : e.buses) feeding(b.sends);
    c.ops.push_back(RemoveBus{id});
    return doc_.submit(c, why);
}

template <typename F>
bool SendController::edit_send(int strip, int slot, std::string& why, F&& f) {
    if (!strip_slot(strip, slot, why)) return false;
    const Send* sd = send_at(strip, slot);
    if (sd == nullptr) {
        why = "send " + letter(slot) + " is empty";
        return false;
    }
    const SendMix* m = snapshot().mix.find_send(sd->id);
    SendMix v = m != nullptr ? *m : SendMix{sd->id};
    f(v);
    if (m != nullptr && v == *m) return true;
    return doc_.submit(SetSendMix{v}, why);
}

template <typename F>
bool SendController::edit_bus(int bus, std::string& why, F&& f) {
    if (bus < 0 || bus >= bus_count()) {
        why = "that bus is gone";
        return false;
    }
    const Id id = snapshot().edit->buses[static_cast<std::size_t>(bus)].id;
    const BusMix* m = snapshot().mix.find_bus(id);
    BusMix v = m != nullptr ? *m : BusMix{id};
    f(v);
    if (m != nullptr && v == *m) return true;
    return doc_.submit(SetBusMix{v}, why);
}

bool SendController::set_level(int strip, int slot, float db, std::string& why) {
    db = std::clamp(db, kMinGainDb, kMaxGainDb);
    return edit_send(strip, slot, why, [db](SendMix& v) { v.level_db = db; });
}

bool SendController::set_pan(int strip, int slot, float pan, std::string& why) {
    pan = std::clamp(pan, -1.0f, 1.0f);
    // A pan set by hand is the send's own: FMP goes off, as in Pro Tools.
    return edit_send(strip, slot, why, [pan](SendMix& v) {
        v.pan = pan;
        v.follow_pan = false;
    });
}

bool SendController::toggle_pre(int strip, int slot, std::string& why) {
    return edit_send(strip, slot, why, [](SendMix& v) { v.pre = !v.pre; });
}

bool SendController::toggle_mute(int strip, int slot, std::string& why) {
    return edit_send(strip, slot, why, [](SendMix& v) { v.mute = !v.mute; });
}

bool SendController::toggle_follow_pan(int strip, int slot, std::string& why) {
    return edit_send(strip, slot, why, [](SendMix& v) { v.follow_pan = !v.follow_pan; });
}

bool SendController::set_bus_gain(int bus, float db, std::string& why) {
    db = std::clamp(db, kMinGainDb, kMaxGainDb);
    return edit_bus(bus, why, [db](BusMix& v) { v.gain_db = db; });
}

bool SendController::set_bus_pan(int bus, float pan, std::string& why) {
    pan = std::clamp(pan, -1.0f, 1.0f);
    return edit_bus(bus, why, [pan](BusMix& v) { v.pan = pan; });
}

bool SendController::toggle_bus_mute(int bus, std::string& why) {
    return edit_bus(bus, why, [](BusMix& v) { v.mute = !v.mute; });
}

bool SendController::toggle_bus_solo(int bus, std::string& why) {
    return edit_bus(bus, why, [](BusMix& v) { v.solo = !v.solo; });
}

std::string SendController::next_bus_name() const {
    for (int n = 1;; ++n) {
        const std::string name = "Bus " + std::to_string(n);
        bool taken = false;
        for (const Bus& b : snapshot().edit->buses) taken = taken || b.name == name;
        if (!taken) return name;
    }
}

}  // namespace sonora::ui
