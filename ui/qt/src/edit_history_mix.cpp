// SPDX-License-Identifier: GPL-3.0-or-later
//
// Undo and redo, and the mixer parameters the Edit window writes.
#include "edit_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "fader_taper.h"
#include "edit_controller_detail.h"

namespace sonora::ui {

using namespace model;
using namespace edit_detail;

bool EditController::undo(std::string& why) {
    drag_ = Drag{};
    if (!doc_.undo(why)) return false;
    after_history_step();
    return true;
}

bool EditController::redo(std::string& why) {
    drag_ = Drag{};
    if (!doc_.redo(why)) return false;
    after_history_step();
    return true;
}

void EditController::after_history_step() {
    if (sel_.region != kNoId && find_region(snapshot(), sel_.region) == nullptr) clear_selection();
    // A breakpoint is picked by its index, and the step may have added or
    // removed points before it: Clear would then delete a different one.
    sel_.point = -1;
}

bool EditController::undo_to(std::size_t depth, std::string& why) {
    const model::History& h = doc_.history();
    if (depth >= h.step_count()) {
        why = "that step is no longer in the undo history";
        return false;
    }
    const std::size_t from = h.undo_depth();
    const bool back = depth < from;
    const std::size_t total = back ? from - depth : depth - from;
    for (std::size_t done = 0; done < total; ++done) {
        if (!(back ? undo(why) : redo(why))) return false;
    }
    return true;
}

// --- mix ---
//
// A mix operation replaces a track's whole TrackMix (operations.h, SetMix),
// so each of these is a small resolver: the current mix with one field
// changed. A gain is clamped to the fader's travel here, because the model
// accepts any finite number and the fader cannot show one beyond its ends.
// A gain already beyond an end (a file saved at +18 dB) widens the range to
// include it, so touching the fader does not quietly take 6 dB off: it stays
// until the operator moves it back onto the travel.

namespace {
bool mix_target(const Snapshot& s, int index, TrackMix& mix, std::string& why) {
    if (index < 0 || index >= static_cast<int>(s.edit->tracks.size())) {
        why = index < 0 ? "no track is selected" : "there is no track " + std::to_string(index + 1);
        return false;
    }
    const TrackEdit& t = *s.edit->tracks[static_cast<std::size_t>(index)];
    const TrackMix* m = s.mix.find(t.id);
    if (m == nullptr) {
        why = t.name + " has no mix settings";
        return false;
    }
    mix = *m;
    return true;
}

// `gain` is the current value on entry and the new one on success.
bool fader_gain(float db, float& gain, std::string& why) {
    if (!std::isfinite(db)) {
        why = "a gain must be a number of dB";
        return false;
    }
    gain = std::clamp(db, std::min(kFaderSilenceDb, gain), std::max(kFaderMaxDb, gain));
    return true;
}
}  // namespace

bool EditController::set_gain(int index, float db, std::string& why) {
    TrackMix m;
    if (!mix_target(snapshot(), index, m, why) || !fader_gain(db, m.gain_db, why)) return false;
    return submit_mix(m, why);
}

bool EditController::set_pan(int index, float pan, std::string& why) {
    TrackMix m;
    if (!mix_target(snapshot(), index, m, why)) return false;
    if (!std::isfinite(pan)) {
        why = "a pan position must be a number";
        return false;
    }
    m.pan = std::clamp(pan, -1.0f, 1.0f);
    return submit(SetMix{m}, why);
}

bool EditController::toggle_mute(int index, std::string& why) {
    TrackMix m;
    if (!mix_target(snapshot(), index, m, why)) return false;
    m.mute = !m.mute;
    return submit_mix(m, why);
}

bool EditController::toggle_solo(int index, std::string& why) {
    TrackMix m;
    if (!mix_target(snapshot(), index, m, why)) return false;
    m.solo = !m.solo;
    return submit_mix(m, why);
}

bool EditController::toggle_solo_safe(int index, std::string& why) {
    TrackMix m;
    if (!mix_target(snapshot(), index, m, why)) return false;
    m.solo_safe = !m.solo_safe;
    return submit(SetMix{m}, why);
}

bool EditController::set_automation_mode(int index, AutomationMode mode, std::string& why) {
    TrackMix m;
    if (!mix_target(snapshot(), index, m, why)) return false;
    if (m.automation == mode) return true;
    m.automation = mode;
    return submit(SetMix{m}, why);
}

bool EditController::step_automation_mode(int index, std::string& why) {
    TrackMix m;
    if (!mix_target(snapshot(), index, m, why)) return false;
    switch (m.automation) {
        case AutomationMode::Off: m.automation = AutomationMode::Read; break;
        case AutomationMode::Read: m.automation = AutomationMode::Touch; break;
        case AutomationMode::Touch: m.automation = AutomationMode::Latch; break;
        case AutomationMode::Latch: m.automation = AutomationMode::Write; break;
        case AutomationMode::Write: m.automation = AutomationMode::Trim; break;
        case AutomationMode::Trim: m.automation = AutomationMode::WriteTrim; break;
        case AutomationMode::WriteTrim: m.automation = AutomationMode::Off; break;
    }
    return submit(SetMix{m}, why);
}

bool EditController::toggle_input_monitor(int index, std::string& why) {
    TrackMix m;
    if (!mix_target(snapshot(), index, m, why)) return false;
    const TrackEdit& t = *snapshot().edit->tracks[static_cast<std::size_t>(index)];
    if (t.kind != TrackKind::Audio) {
        why = t.name + (t.kind == TrackKind::Midi ? " is a MIDI track" : " is an instrument track") +
              ": it has no audio input to monitor";
        return false;
    }
    m.monitor = m.monitor == MonitorMode::Input ? MonitorMode::Auto : MonitorMode::Input;
    return submit(SetMix{m}, why);
}

bool EditController::toggle_track_punch(int index, std::string& why) {
    TrackMix m;
    if (!mix_target(snapshot(), index, m, why)) return false;
    const TrackEdit& t = *snapshot().edit->tracks[static_cast<std::size_t>(index)];
    if (t.kind != TrackKind::Audio) {
        why = t.name + (t.kind == TrackKind::Midi ? " is a MIDI track" : " is an instrument track") +
              ": it records no audio to punch";
        return false;
    }
    m.track_punch = !m.track_punch;
    return submit(SetMix{m}, why);
}

bool EditController::set_bus_gain(float db, std::string& why) {
    MonitorBus bus = snapshot().mix.bus;
    if (!fader_gain(db, bus.gain_db, why)) return false;
    return submit(SetMonitorBus{bus}, why);
}

}  // namespace sonora::ui
