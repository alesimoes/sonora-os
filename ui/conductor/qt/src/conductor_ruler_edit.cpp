// SPDX-License-Identifier: GPL-3.0-or-later
#include "conductor_ruler_edit.h"

#include <cmath>
#include <cstdlib>
#include <vector>

#include "edit_controller.h"
#include "sonora/ui_conductor/conductor.h"
#include "time_format.h"

namespace sonora::ui::conductor_edit {

namespace {

using model::TempoEntry;

const std::vector<TempoEntry>& map_of(const EditController& ctl) {
    return ctl.snapshot().settings.tempo;
}

bool valid_mark(const EditController& ctl, int index) {
    return index >= 0 && static_cast<std::size_t>(index) < map_of(ctl).size();
}

bool refuse_take(bool recording, std::string& why) {
    if (!recording) return false;
    why = "a take is open; edit the tempo and meter once recording stops";
    return true;
}

// The typed value: a number, a trailing "bpm" allowed, nothing else.
bool bpm_of(std::string s, double& out) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.size() >= 3 && s.compare(s.size() - 3, 3, "bpm") == 0) s.resize(s.size() - 3);
    const auto b = s.find_first_not_of(' ');
    if (b == std::string::npos) return false;
    s = s.substr(b, s.find_last_not_of(' ') - b + 1);
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != nullptr && end != s.c_str() && *end == '\0' && std::isfinite(out);
}

}  // namespace

int mark_at(const EditController& ctl, double x) {
    const ViewState& v = ctl.view();
    const auto hit = conductor::hit_test(map_of(ctl), ctl.snapshot().settings.sample_rate, v.fpp,
                                         x + v.scroll / v.fpp);
    return hit ? static_cast<int>(*hit) : -1;
}

std::string drag_label(const EditController& ctl, int index, double dy, bool whole_bpm) {
    if (!valid_mark(ctl, index)) return {};
    const conductor::TempoDrag d =
        conductor::drag_tempo(map_of(ctl), static_cast<std::size_t>(index), dy, whole_bpm);
    return d.ok ? format_bpm(d.bpm) + " bpm" : std::string();
}

bool drag_end(EditController& ctl, bool recording, int index, double dy, bool whole_bpm,
              std::string& why) {
    if (refuse_take(recording, why)) return false;
    if (!valid_mark(ctl, index)) {
        why = "there is no tempo mark to drag";
        return false;
    }
    // The drag is measured from the map as it was when it began, which is
    // the map now: nothing is sent until the button comes up.
    const auto i = static_cast<std::size_t>(index);
    const conductor::TempoDrag d = conductor::drag_tempo(map_of(ctl), i, dy, whole_bpm);
    if (!d.ok) {
        why = d.why;
        return false;
    }
    return ctl.set_tempo(map_of(ctl)[i].at, d.bpm, why);
}

std::string tempo_text(const EditController& ctl, int index) {
    if (valid_mark(ctl, index)) return format_bpm(map_of(ctl)[static_cast<std::size_t>(index)].bpm);
    return format_bpm(tempo_at(ctl.selection().insertion, ctl.snapshot().settings).bpm);
}

bool type_tempo(EditController& ctl, bool recording, int index, const std::string& text,
                std::string& why) {
    if (refuse_take(recording, why)) return false;
    double bpm = 0.0;
    if (!bpm_of(text, bpm)) {
        why = "\"" + text + "\" is not a tempo; type a number of bpm, such as 96";
        return false;
    }
    if (index >= 0 && !valid_mark(ctl, index)) {
        why = "there is no tempo mark to set";
        return false;
    }
    const model::Frames at = index >= 0 ? map_of(ctl)[static_cast<std::size_t>(index)].at
                                        : ctl.selection().insertion;
    return ctl.set_tempo(at, static_cast<float>(bpm), why);
}

bool remove(EditController& ctl, bool recording, int index, std::string& why) {
    if (refuse_take(recording, why)) return false;
    // Checked on a copy first, for the reason: the controller's remove takes
    // the nearest change and would never say why the first one stays.
    std::vector<TempoEntry> trial = map_of(ctl);
    const std::uint32_t rate = ctl.snapshot().settings.sample_rate;
    if (index < 0 || !conductor::remove_mark(trial, rate, static_cast<std::size_t>(index), why)) {
        if (index < 0) why = "there is no tempo mark to remove";
        return false;
    }
    return ctl.remove_tempo(map_of(ctl)[static_cast<std::size_t>(index)].at, why);
}

std::string meter_text(const EditController& ctl, double x) {
    return format_meter(tempo_at(ctl.view().frame_at(x), ctl.snapshot().settings));
}

bool set_meter(EditController& ctl, bool recording, double x, std::uint32_t beats,
               std::uint32_t note, std::string& why) {
    if (refuse_take(recording, why)) return false;
    return ctl.set_meter(ctl.view().frame_at(x), beats, note, why);
}

}  // namespace sonora::ui::conductor_edit
