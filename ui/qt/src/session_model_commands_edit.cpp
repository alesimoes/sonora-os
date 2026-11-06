// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_model.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "fader_taper.h"
#include "scene_item.h"
#include "edit_clipboard.h"
#include "session_files.h"
#include "sonora/api/consolidate_argument.h"
#include "sonora/api/messages.h"
#include "sonora/api/render.h"
#include "sonora/command.h"
#include "sonora/model/render_in_place.h"
#include "sonora/model/session_file.h"
#include "sonora/model/session_template.h"
#include "transport_model.h"
#include "session_model_support.h"

namespace sonora::ui {

using namespace model;

void SessionModel::installEditCommands(CommandRegistry& /*local*/, const InstallSimple& simple) {
    using Why = std::string;
    simple("edit.separate_at_selection",
           [this](const CommandContext&, Why& why) { return ctl_.separate(why); });
    simple("edit.heal_separation",
           [this](const CommandContext&, Why& why) { return ctl_.heal_separation(why); });
    simple("edit.strip_silence", [this](const CommandContext& ctx, Why& why) {
        StripParams params;
        return parse_strip_params(ctx.argument, params, why) &&
               ctl_.strip_silence(peaks_, params, why);
    });
    simple("edit.trim_start_to_insertion",
           [this](const CommandContext&, Why& why) { return ctl_.trim_start_to_insertion(why); });
    simple("edit.trim_end_to_insertion",
           [this](const CommandContext&, Why& why) { return ctl_.trim_end_to_insertion(why); });
    simple("edit.fade_to_start",
           [this](const CommandContext&, Why& why) { return ctl_.fade_to_start(why); });
    simple("edit.fade_to_end",
           [this](const CommandContext&, Why& why) { return ctl_.fade_to_end(why); });
    // A crossfade shortened to its handles still ran; what it became goes
    // on the status line, queued as paste's is.
    simple("edit.fade_selection", [this](const CommandContext&, Why& why) {
        if (!ctl_.fade_selection(why)) return false;
        if (!why.empty()) {
            QMetaObject::invokeMethod(
                this, [this, said = q(why)]() { emit notice(said); }, Qt::QueuedConnection);
        }
        why.clear();
        return true;
    });
    simple("nudge.forward", [this](const CommandContext&, Why& why) { return ctl_.nudge(1, why); });
    simple("nudge.back", [this](const CommandContext&, Why& why) { return ctl_.nudge(-1, why); });

    simple("track.select", [this](const CommandContext& ctx, Why& why) {
        if (ctx.target < 0 || ctx.target >= trackCount()) {
            why = "there is no track " + std::to_string(ctx.target + 1);
            return false;
        }
        ctl_.select_track(ctx.target);
        return true;
    });
    simple("track.select_next",
           [this](const CommandContext&, Why& why) { return ctl_.select_next_track(1, why); });
    simple("track.select_previous",
           [this](const CommandContext&, Why& why) { return ctl_.select_next_track(-1, why); });
    simple("track.toggle_mute", [this](const CommandContext& ctx, Why& why) {
        return ctl_.toggle_mute(ctx.has_target() ? ctx.target : ctl_.selection().track, why);
    });
    simple("track.toggle_solo", [this](const CommandContext& ctx, Why& why) {
        return ctl_.toggle_solo(ctx.has_target() ? ctx.target : ctl_.selection().track, why);
    });
    // Mix and edit groups (ADR-0041). New groups the selected tracks; the
    // others act on the track given or the selected one.
    simple("group.new", [this](const CommandContext& ctx, Why& why) {
        const Selection& s = ctl_.selection();
        return ctl_.group_tracks(s.track, s.tracks, ctx.argument,
                                 model::kGroupVolume | model::kGroupMute | model::kGroupSolo |
                                     model::kGroupArm,
                                 why);
    });
    simple("group.dissolve", [this](const CommandContext& ctx, Why& why) {
        return ctl_.dissolve_group(ctx.has_target() ? ctx.target : ctl_.selection().track, why);
    });
    simple("group.toggle_suspend", [this](const CommandContext& ctx, Why& why) {
        return ctl_.toggle_group_suspend(ctx.has_target() ? ctx.target : ctl_.selection().track,
                                         why);
    });
    // The interface's, as mute and solo are: Input mode is the document's
    // (TrackMix::monitor), saved with the session, and the engine follows
    // it through the mix it applies (PlaybackController::apply_monitoring).
    simple("track.toggle_input_monitor", [this](const CommandContext& ctx, Why& why) {
        return ctl_.toggle_input_monitor(ctx.has_target() ? ctx.target : ctl_.selection().track,
                                         why);
    });

    // TrackMix::track_punch, saved with the session and no undo step.
    simple("track.toggle_track_punch", [this](const CommandContext& ctx, Why& why) {
        return ctl_.toggle_track_punch(ctx.has_target() ? ctx.target : ctl_.selection().track, why);
    });

    simple("track.step_automation_mode", [this](const CommandContext& ctx, Why& why) {
        return ctl_.step_automation_mode(ctx.has_target() ? ctx.target : ctl_.selection().track,
                                         why);
    });
}

}  // namespace sonora::ui
