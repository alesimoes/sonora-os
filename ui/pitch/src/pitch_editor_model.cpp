// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opening, selection, the drag and the inspector. Apply and its reply are in
// pitch_apply.cpp.
#include "sonora/ui_pitch/pitch_editor_model.h"

#include <algorithm>
#include <cmath>

namespace sonora::ui_pitch {
namespace {

double clamp_shift(double cents) {
    return std::clamp(cents, double(api::kPitchCentsMin), double(api::kPitchCentsMax));
}

// The scale degree nearest `cents`, as PitchPlan's own snap finds it: one
// note at that pitch, snapped all the way.
double nearest_in_scale(double cents, int key, pitch::Scale scale) {
    pitch::Note n;
    n.median_cents = cents;
    pitch::PitchPlan one(std::vector<pitch::Note>{n});
    std::string ignored;
    if (!one.snap_to_scale(key, scale, 100.0, ignored)) return cents;
    return cents + one.edit(0).shift_cents;
}

}  // namespace

void PitchEditorModel::reset_session() {
    open_ = false;
    region_ = PitchRegion{};
    plan_ = pitch::PitchPlan(std::vector<pitch::Note>{});
    selected_ = -1;
    drag_index_ = -1;
    pending_seq_ = 0;
    stale_ = false;
    last_argument_.clear();
    notice_.clear();
    status_.clear();
}

bool PitchEditorModel::open(const PitchRegion& region, std::vector<pitch::Note> notes,
                            std::string& why) {
    reset_session();
    if (notes.empty()) return refuse(why, "pitch: no notes were found in " + region.name);
    for (std::size_t i = 0; i < notes.size(); ++i) {
        if (notes[i].start >= notes[i].end || notes[i].end > region.length) {
            return refuse(why, "pitch: note " + std::to_string(i + 1) + " lies outside " + region.name);
        }
    }
    region_ = region;
    const std::size_t count = notes.size();
    plan_ = pitch::PitchPlan(std::move(notes));
    open_ = true;
    status_ = std::to_string(count) + (count == 1 ? " note" : " notes") + " in " + region.name;
    return true;
}

bool PitchEditorModel::analyse(const PitchRegion& region, const std::vector<float>& mono,
                               pitch::PitchTracker& tracker, std::string& why) {
    reset_session();
    pitch::Contour contour;
    std::string reason;
    if (!tracker.track(mono, region.rate, contour, reason)) return refuse(why, reason);
    std::vector<pitch::Note> notes;
    if (!pitch::segment_notes(contour, notes, reason)) return refuse(why, reason);
    return open(region, std::move(notes), why);
}

double PitchEditorModel::pitch_cents(std::size_t i) const {
    return plan_.note(i).median_cents + plan_.edit(i).shift_cents;
}

bool PitchEditorModel::edited() const {
    for (std::size_t i = 0; i < plan_.size(); ++i) {
        const pitch::NoteEdit& e = plan_.edit(i);
        if (e.shift_cents != 0.0 || e.drift_scale != 1.0 || e.vibrato_scale != 1.0 ||
            e.formant_cents != 0 || e.gain_db != 0.0) {
            return true;
        }
    }
    return false;
}

bool PitchEditorModel::select(int index) {
    if (index < -1 || index >= int(plan_.size())) return false;
    selected_ = index;
    return true;
}

bool PitchEditorModel::begin_drag(int index, std::string& why) {
    if (!editable(why)) return false;
    if (!select(index) || index < 0) return refuse(why, "pitch: there is no note there to drag");
    drag_index_ = index;
    drag_origin_ = pitch_cents(std::size_t(index));
    return true;
}

bool PitchEditorModel::drag_to(double delta_cents, std::string& why) {
    if (drag_index_ < 0) return refuse(why, "pitch: press on a note to drag it");
    if (!std::isfinite(delta_cents)) return refuse(why, "pitch: the drag is not a number of cents");
    const std::size_t i = std::size_t(drag_index_);
    const double target = snapped(drag_origin_ + delta_cents);
    plan_.edit(i).shift_cents = clamp_shift(target - plan_.note(i).median_cents);
    return true;
}

double PitchEditorModel::snapped(double cents) const {
    switch (snap_mode_) {
        case SnapMode::Off: return std::round(cents);
        case SnapMode::Semitone: return 100.0 * std::round(cents / 100.0);
        case SnapMode::Scale: return nearest_in_scale(cents, key_, scale_);
    }
    return cents;
}

bool PitchEditorModel::set_key(int key, std::string& why) {
    if (key < 0 || key > 11) return refuse(why, "pitch: the key is a pitch class from C to B");
    key_ = key;
    return true;
}

bool PitchEditorModel::set_amount(double percent, std::string& why) {
    if (!(percent >= 0.0 && percent <= 100.0)) {
        return refuse(why, "pitch: the snap amount is 0 to 100 percent");
    }
    amount_ = percent;
    return true;
}

bool PitchEditorModel::snap_all(std::string& why) {
    if (!editable(why)) return false;
    std::string reason;
    if (!plan_.snap_to_scale(key_, scale_, amount_, reason)) return refuse(why, reason);
    for (std::size_t i = 0; i < plan_.size(); ++i) {
        plan_.edit(i).shift_cents = clamp_shift(plan_.edit(i).shift_cents);
    }
    return true;
}

bool PitchEditorModel::set_straighten(double percent, std::string& why) {
    if (!selected_note(why)) return false;
    if (!(percent >= 0.0 && percent <= 100.0)) {
        return refuse(why, "pitch: straighten is 0 to 100 percent");
    }
    plan_.edit(std::size_t(selected_)).drift_scale = 1.0 - percent / 100.0;
    return true;
}

bool PitchEditorModel::set_vibrato(double percent, std::string& why) {
    if (!selected_note(why)) return false;
    if (!(percent >= 0.0 && percent <= 200.0)) {
        return refuse(why, "pitch: vibrato is 0 to 200 percent of the sung depth");
    }
    plan_.edit(std::size_t(selected_)).vibrato_scale = percent / 100.0;
    return true;
}

bool PitchEditorModel::set_formant(int cents, std::string& why) {
    if (!selected_note(why)) return false;
    if (cents < api::kPitchCentsMin || cents > api::kPitchCentsMax) {
        return refuse(why, "pitch: formant is -2400 to 2400 cents");
    }
    plan_.edit(std::size_t(selected_)).formant_cents = cents;
    return true;
}

bool PitchEditorModel::set_gain(double db, std::string& why) {
    if (!selected_note(why)) return false;
    if (!(db >= api::kPitchGainDbMin && db <= api::kPitchGainDbMax)) {
        return refuse(why, "pitch: note gain is -48 to 12 dB");
    }
    plan_.edit(std::size_t(selected_)).gain_db = db;
    return true;
}

bool PitchEditorModel::selected_note(std::string& why) {
    if (!editable(why)) return false;
    if (selected_ < 0) return refuse(why, "pitch: select a note first");
    return true;
}

}  // namespace sonora::ui_pitch
