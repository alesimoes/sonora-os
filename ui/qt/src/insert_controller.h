// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window's insert slots, without the window (ADR-0012 checklist 6):
// which plugin sits in which of a track's slots A to J, what the engine
// says about it, and turning a click, a drag or a slider into an
// operation on the TimelineDocument or a PluginParameterChange. No Qt, so
// test_inserts checks every one of them by calling it.
//
// The workflow is Pro Tools' (ADR-0004: reproduced deliberately, never its
// code): click an empty slot to choose a plugin, Ctrl-click (Command on a
// Mac keyboard) to bypass, Ctrl+Start-click to make it inactive, drag to
// move it to another slot or track, Alt-drag to copy it there with its
// settings. Improved where it is clumsy: the chooser groups by category and
// searches, and a plugin that cannot go on this track is shown with the
// reason rather than left out.
//
// What is model state and what is not (ADR-0012 §6.3, owner decision O1's
// default): structure (add, remove, move, replace, active) is an undo step;
// bypass and parameter values are mix state, saved and never undone, as a
// fader is. A parameter drag sends PluginParameterChange while it runs and
// records the gesture's end with one SetInsertParams.
//
// What the engine says: the catalog (the plugins it can host), a standing
// fault per insert by id, and each slot's flags and latency in the state
// block. A slot the state block calls down shows faulted even before the
// fault's text arrives, and says that it has no reason yet: a slot that
// looked healthy while its plugin was down would be a control that lies.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "engine_adapter.h"
#include "timeline_document.h"

namespace sonora::ui {

// "A" to "J".
std::string slot_letter(int slot);

// What one slot shows.
struct SlotView {
    bool present = false;
    model::Id insert = kNoId;
    std::string name;            // the plugin's, as the insert was made
    bool active = true;          // in the chain (Pro Tools' active)
    bool bypass = false;
    bool faulted = false;        // down: crashed, refused, missing, hosting off
    bool missing = false;        // the plugin is not installed here
    bool restarting = false;
    bool running = false;
    std::string fault;           // why it is down, in the engine's words
    uint32_t latency_frames = 0;
    bool has_catalog_entry = false;   // the catalog knows the plugin
};

// A plugin name in at most `max_chars` characters, for a slot on a narrow
// strip: the name itself when it fits; else its words run together and
// their inner lower-case vowels dropped from the end until it fits
// ("Fake Compressor" in 8 is "FkCmprss"); else cut. A slot is ten
// characters wide at 18 strips, and "Fake C..." names nothing.
std::string short_plugin_name(const std::string& name, std::size_t max_chars);

// The value text a parameter shows, and where it sits on its control:
// labels for stepped values, On/Off for a toggle, the number and its unit
// for a continuous one. A VST3 continuous value is normalised (ADR-0012
// §5.2) and its plain value is the plugin's to compute, so it reads as a
// percentage of the travel and never with the plugin's unit: "50 %" is
// true, "0.50 dB" would not be.
std::string param_text(const api::PluginInfo& plugin, const api::PluginParam& p, float value);
// 0..1 along the control; logarithmic parameters on a log scale.
float param_position(const api::PluginParam& p, float value);
// The value at a control position, clamped to the range and snapped to a
// step when the parameter has steps.
float param_value_at(const api::PluginParam& p, float position);
// The step a stepped value is on, from 0; -1 for a continuous parameter.
int param_step(const api::PluginParam& p, float value);
// The value of step `i`.
float param_step_value(const api::PluginParam& p, int i);

class InsertController {
public:
    explicit InsertController(TimelineDocument& doc);

    TimelineDocument& document() { return doc_; }
    const model::Snapshot& snapshot() const { return doc_.snapshot(); }

    // Where a PluginParameterChange goes: the engine, through the shell.
    using ParamSender = std::function<void(model::Id insert, const std::string& key, float value)>;
    void set_param_sender(ParamSender s) { send_param_ = std::move(s); }

    // --- what the engine says ---
    void set_catalog(const api::PluginCatalog& c);
    bool has_catalog() const { return has_catalog_; }
    const std::vector<api::PluginInfo>& plugins() const { return catalog_.plugins; }
    const api::PluginInfo* find_plugin(const model::PluginRef& ref) const;
    // Empty text clears it. Returns true when it changed.
    bool set_fault(model::Id insert, const std::string& text);
    // A new connection replays the faults that stand; the old ones go first.
    void clear_faults() { faults_.clear(); }
    // The state block's insert slots and compensation. Returns true when
    // anything a slot or a readout shows changed.
    bool set_frame(const EngineFrame& f, bool alive);

    // --- what a slot shows ---
    int track_count() const { return static_cast<int>(snapshot().edit->tracks.size()); }
    // Where an insert is now: display track and slot. False when it is gone.
    bool locate(model::Id insert, int& track, int& slot) const;
    SlotView slot(int track, int slot) const;
    // The track's chain latency as the engine last published it, IPC block
    // included; -1 when the engine is not there to say.
    int64_t track_latency(int track) const;
    int64_t compensation() const { return alive_ ? compensation_ : -1; }
    int64_t compensation_shortfall() const { return alive_ ? shortfall_ : 0; }
    bool plugins_disabled() const { return alive_ && plugins_disabled_; }

    // --- the plugin chooser ---
    // Every category a plugin declares, sorted; VST3's catch-all "Fx" is
    // left out, as it groups nothing.
    std::vector<std::string> categories() const;
    // Indices into plugins(): in `category` (empty: all), whose name or
    // vendor contains `search` (any case), by name.
    std::vector<int> browse(const std::string& category, const std::string& search) const;
    // Why plugin `index` cannot go on display track `track`; empty when it can.
    std::string refusal(int index, int track) const;

    // --- structure: each one undo step ---
    bool add(int track, int slot, int plugin, std::string& why);
    bool replace(int track, int slot, int plugin, std::string& why);
    bool remove(int track, int slot, std::string& why);
    bool move(int from_track, int from_slot, int to_track, int to_slot, std::string& why);
    // Alt-drag: the plugin and its values onto another slot, a new insert.
    // The source's state is flushed from its running plugin first, so the
    // copy holds what the operator hears. `done` runs once, now or after
    // the engine answers; on success its text is a caution when the copy
    // differs from its source (a follow-up step refused) or took the last
    // saved state because the plugin's own could not be had. A copy whose
    // wait spans another session arriving copies nothing.
    using Done = std::function<void(bool ok, const std::string& text)>;
    void copy(int from_track, int from_slot, int to_track, int to_slot, Done done);
    bool set_active(int track, int slot, bool active, std::string& why);

    // --- values: mix state, never undone ---
    bool toggle_bypass(int track, int slot, std::string& why);

    // --- parameters ---
    const api::PluginInfo* plugin_of(model::Id insert) const;
    // What the panel shows: the value being dragged, else the document's,
    // else the plugin's default.
    float value(model::Id insert, const api::PluginParam& p) const;
    // A gesture: begin, any number of sets (each a PluginParameterChange),
    // end (one SetInsertParams with the insert's whole list).
    bool param_begin(model::Id insert, std::string& why);
    bool param_set(model::Id insert, const std::string& key, float value, std::string& why);
    bool param_end(std::string& why);
    bool gesture_running() const { return gesture_insert_ != kNoId; }

private:
    const model::TrackEdit* track(int index) const;
    const model::Insert* insert_at(int track, int slot) const;
    const model::Insert* find(model::Id insert, int* track_index = nullptr) const;
    bool target_slot(int track, int slot, std::string& why) const;

    TimelineDocument& doc_;
    ParamSender send_param_;
    api::PluginCatalog catalog_;
    bool has_catalog_ = false;
    std::map<model::Id, std::string> faults_;

    bool alive_ = false;
    bool plugins_disabled_ = false;
    int64_t compensation_ = 0;
    int64_t shortfall_ = 0;
    struct TrackState {
        uint32_t latency = 0;
        std::array<InsertSlotSnapshot, kInsertSlotCount> cells{};
        bool operator==(const TrackState& o) const {
            if (latency != o.latency) return false;
            for (std::size_t i = 0; i < cells.size(); ++i) {
                if (cells[i].flags != o.cells[i].flags ||
                    cells[i].latency_frames != o.cells[i].latency_frames) {
                    return false;
                }
            }
            return true;
        }
    };
    std::vector<TrackState> tracks_;

    model::Id gesture_insert_ = kNoId;
    std::map<std::string, float> gesture_;   // by key, the values moved
};

}  // namespace sonora::ui
