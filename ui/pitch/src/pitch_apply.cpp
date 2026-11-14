// SPDX-License-Identifier: GPL-3.0-or-later
//
// Apply, its reply, Reset and the held notice.
#include <string>

#include "sonora/ui_pitch/pitch_editor_model.h"

namespace sonora::ui_pitch {

bool PitchEditorModel::refuse(std::string& why, const std::string& text) {
    why = text;
    notice_ = text;
    return false;
}

bool PitchEditorModel::editable(std::string& why) {
    if (!open_) return refuse(why, "pitch: open a region's notes first");
    if (pending_seq_ != 0) {
        return refuse(why, "pitch: the last edit is still being rendered; wait for it");
    }
    if (stale_) {
        return refuse(why, "pitch: " + region_.name +
                               " was rendered; analyse it again to edit its new notes, "
                               "or undo to go back");
    }
    return true;
}

bool PitchEditorModel::apply(std::string& why) {
    if (!capability_.available) {
        return refuse(why, capability_.reason.empty()
                               ? "pitch: the engine has not said it can render a pitch edit"
                               : capability_.reason);
    }
    if (!editable(why)) return false;
    api::PitchEditSpec spec;
    std::string reason;
    if (!plan_.to_spec(region_.length, spec, reason)) return refuse(why, reason);
    if (!api::pitch_notes_fit(spec, region_.length, reason)) return refuse(why, reason);
    if (sink_ == nullptr) return refuse(why, "pitch: no engine to render the edit; nothing was changed");
    const std::string argument =
        pitch_edit_argument(region_.id, region_.position, region_.length, spec);
    const uint64_t seq = sink_->send(kSampleEditCommand, argument, reason);
    if (seq == 0) return refuse(why, reason.empty() ? "pitch: the edit could not be sent" : reason);
    last_argument_ = argument;
    pending_seq_ = seq;
    drag_index_ = -1;
    status_ = "Rendering " + std::to_string(spec.notes.size()) +
              (spec.notes.size() == 1 ? " span" : " spans") + " of " + region_.name + "...";
    return true;
}

void PitchEditorModel::on_reply(uint64_t seq, bool ok, const std::string& message) {
    if (seq == 0 || seq != pending_seq_) return;
    pending_seq_ = 0;
    if (ok) {
        stale_ = true;
        status_ = region_.name + " now plays a pitch-edited copy; one undo restores the take";
        return;
    }
    status_.clear();
    notice_ = "pitch: not applied: " +
              (message.empty() ? std::string("the engine refused without a reason") : message);
}

bool PitchEditorModel::reset_edits(std::string& why) {
    if (!editable(why)) return false;
    for (std::size_t i = 0; i < plan_.size(); ++i) plan_.edit(i) = pitch::NoteEdit{};
    drag_index_ = -1;
    return true;
}

}  // namespace sonora::ui_pitch
