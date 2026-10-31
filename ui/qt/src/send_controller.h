// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window's sends and aux buses, without the window (ADR-0022): what
// each strip's send slots A to J show, and turning a click, a drag or a
// typed value into an operation on the TimelineDocument. No Qt, so
// test_sends checks every one of them by calling it.
//
// The workflow is Pro Tools' (ADR-0004: reproduced, never its code): click
// an empty send slot to choose its bus, click a used one for its send
// window (level, pan, PRE, mute, FMP), and the send window's bus button to
// send it elsewhere. A new send starts at -inf with FMP on and post-fader,
// as Pro Tools' defaults are; Logic's habit of making the aux strip with
// the bus is kept: "New bus" in the chooser adds the bus and the send as
// one undo step.
//
// Strips are numbered tracks first, in display order, then aux buses in
// display order. What is model state and what is not (ADR-0022 §3): a bus,
// a send, its slot and its bus are undo steps; levels, pans, mutes and
// pre/post are mix state, saved and never undone, as a fader is. A bus a
// send would close a loop with is offered with the reason, never hidden,
// and the model refuses it all the same.
#pragma once

#include <string>
#include <vector>

#include "timeline_document.h"

namespace sonora::ui {

struct SendSlotView {
    bool present = false;
    model::Id send = kNoId;
    model::Id bus = kNoId;
    std::string bus_name;
    float level_db = model::kMinGainDb;
    float pan = 0.0f;
    bool follow_pan = true;
    bool mute = false;
    bool pre = false;
};

struct AuxView {
    model::Id id = kNoId;
    std::string name;
    float gain_db = 0.0f;
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
};

// One bus a strip's send could go to, and why it cannot, when it cannot.
struct SendDestination {
    int bus = 0;
    std::string name;
    std::string refusal;
};

class SendController {
public:
    explicit SendController(TimelineDocument& doc);

    TimelineDocument& document() { return doc_; }
    const model::Snapshot& snapshot() const { return doc_.snapshot(); }

    int track_count() const { return static_cast<int>(snapshot().edit->tracks.size()); }
    int bus_count() const { return static_cast<int>(snapshot().edit->buses.size()); }
    int strip_count() const { return track_count() + bus_count(); }
    // A MIDI track has no audio to send.
    bool strip_sends(int strip) const;
    std::string strip_name(int strip) const;

    SendSlotView slot(int strip, int slot) const;
    AuxView aux(int bus) const;
    // Every bus, in display order, with the reason when `strip` cannot send
    // to it from `slot`: itself, a loop, a bus another of its slots feeds.
    std::vector<SendDestination> destinations(int strip, int slot) const;

    // --- outputs (ADR-0024) ---
    // Where `strip` feeds: the aux bus index, or kMaster for the Master.
    static constexpr int kMaster = -1;
    int output(int strip) const;
    std::string output_name(int strip) const;
    // The Master first, then every bus in display order, with the reason when
    // `strip` cannot output to it: itself, a loop (named as the model names
    // it), a MIDI track, which has no audio.
    std::vector<SendDestination> output_choices(int strip) const;
    // One undo step. `bus` is an aux bus index, or kMaster.
    bool set_output(int strip, int bus, std::string& why);

    // --- structure: each one undo step ---
    // `slot` sends to bus `bus`. A slot already in use is sent elsewhere,
    // keeping its level, pan and mutes: RemoveSend and AddSend in one
    // compound, as Pro Tools keeps a reassigned send's values.
    bool assign(int strip, int slot, int bus, std::string& why);
    // A new bus at the end, and `slot` sending to it: one step.
    bool assign_new_bus(int strip, int slot, std::string& why);
    bool remove(int strip, int slot, std::string& why);
    bool new_bus(std::string& why);
    // The bus and every send that feeds it: one step, so undo brings them
    // all back.
    bool remove_bus(int bus, std::string& why);

    // --- values: mix state, never undone ---
    bool set_level(int strip, int slot, float db, std::string& why);
    bool set_pan(int strip, int slot, float pan, std::string& why);
    bool toggle_pre(int strip, int slot, std::string& why);
    bool toggle_mute(int strip, int slot, std::string& why);
    bool toggle_follow_pan(int strip, int slot, std::string& why);
    bool set_bus_gain(int bus, float db, std::string& why);
    bool set_bus_pan(int bus, float pan, std::string& why);
    bool toggle_bus_mute(int bus, std::string& why);
    bool toggle_bus_solo(int bus, std::string& why);

    // "Bus 1", "Bus 2", ...: the first number no bus is named with.
    std::string next_bus_name() const;

private:
    const std::vector<model::Send>* sends_of(int strip) const;
    model::Id strip_id(int strip) const;
    const model::Send* send_at(int strip, int slot) const;
    bool strip_slot(int strip, int slot, std::string& why) const;
    template <typename F>
    bool edit_send(int strip, int slot, std::string& why, F&& f);
    template <typename F>
    bool edit_bus(int bus, std::string& why, F&& f);

    TimelineDocument& doc_;
};

}  // namespace sonora::ui
