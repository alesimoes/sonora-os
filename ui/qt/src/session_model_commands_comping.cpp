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

void SessionModel::installCompingCommands(CommandRegistry& /*local*/, const InstallSimple& simple) {
    using Why = std::string;
    const auto row = [this](const CommandContext& ctx) { return commandRow(ctx); };
    // --- take lanes and comping (ADR-0031, ADR-0049) ---
    // A refusal is held where the operator is looking (the track refusal line,
    // with its Dismiss), not on the status line that clears in 2.5 s; so is
    // the note that Quick Swipe left a join as a cut.
    const auto comping = [this](bool ok, Why& why) {
        setTrackRefusal(q(why));
        if (ok) why.clear();
        return ok;
    };
    simple("comp.toggle_lanes", [this, row, comping](const CommandContext& ctx, Why& why) {
        const bool ok = ctl_.toggle_comp_lanes(row(ctx), why);
        return comping(ok, why);
    });
    simple("comp.copy_to_main", [this, comping](const CommandContext&, Why& why) {
        const bool ok = ctl_.comp_copy_to_main(why);
        return comping(ok, why);
    });
    simple("comp.new", [this, row, comping](const CommandContext& ctx, Why& why) {
        const int i = row(ctx);
        const bool ok = playlistCommand(i, true, [&](Why& w) { return ctl_.comp_new(i, w); }, why);
        return comping(ok, why);
    });
    simple("comp.toggle_quick_swipe", [this](const CommandContext&, Why&) {
        ctl_.set_quick_swipe(!ctl_.quick_swipe());
        return true;
    });
    // Consolidate renders on the engine (api/consolidate_argument.h): the
    // request is the selection, the answer arrives as Consolidate events, so a
    // failure is a held notice and the new region arrives as an ordinary edit.
    simple("edit.consolidate", [this](const CommandContext&, Why& why) {
        EditController::ConsolidateRequest req;
        if (!ctl_.consolidate_request(req, why)) return false;
        if (!send_command_) {
            why = "no engine to render the consolidated file";
            return false;
        }
        api::ConsolidateArgument arg;
        arg.track = req.track;
        arg.playlist = req.playlist;
        arg.start = req.start;
        arg.end = req.end;
        arg.name = req.name;
        send_command_(QStringLiteral("session.consolidate"),
                      q(api::format_consolidate_argument(arg)));
        return true;
    });
    simple("edit.duplicate",
           [this](const CommandContext&, Why& why) { return ctl_.duplicate(why); });
    // Pro Tools' Repeat asks for the number of repeats first. The key sends
    // no count, so it opens the window's dialog, which sends the count back
    // as the argument; there is nothing to ask about with nothing to repeat.
    simple("edit.repeat", [this](const CommandContext& ctx, Why& why) {
        if (ctx.argument.empty()) {
            std::string name;
            Frames length = 0;
            if (!ctl_.can_repeat(why, &name, &length)) return false;
            emit repeatRequested(ctl_.repeat_count(),
                                 q(format_min_sec(length, ctl_.snapshot().settings.sample_rate)) +
                                     QStringLiteral(" of ") + q(name));
            return true;
        }
        int count = 0;
        if (!parse_repeat_count(ctx.argument, count)) {
            why = "\"" + ctx.argument +
                  "\" is not a number of repeats; type a whole number, 1 or more. Nothing was "
                  "repeated.";
            return false;
        }
        return ctl_.repeat(count, why);
    });
    simple("edit.insert_time",
           [this](const CommandContext&, Why& why) { return ctl_.insert_time(why); });
    simple("edit.delete_time",
           [this](const CommandContext&, Why& why) { return ctl_.delete_time(why); });
    // Pro Tools' Spot dialog: the start time typed in the counter's format.
    simple("edit.spot", [this](const CommandContext& ctx, Why& why) {
        if (ctx.argument.empty()) return askSpot();
        Frames at = 0;
        if (!parse_time(main_fmt_, ctx.argument, ctl_.snapshot().settings, at, why)) return false;
        return ctl_.spot_region(at, why);
    });
    // Pro Tools' New Memory Location: stopped, the window asks for the name
    // and sends it back as the argument; rolling, the marker is named and
    // dropped at the playhead at once, as Pro Tools does when playing. The
    // playhead is the engine's as the command runs: the last tick's is tens
    // of milliseconds behind a rolling transport.
    simple("marker.add", [this](const CommandContext& ctx, Why& why) {
        bool rolling = overlay_.rolling;
        Frames playhead = overlay_.playhead;
        TransportSnapshot now;
        if (read_transport_ && read_transport_(now)) {
            rolling = now.state != TransportMode::Stopped;
            playhead = static_cast<Frames>(now.playhead_frames);
        }
        const auto at = ctl_.marker_place(rolling, playhead);
        if (ctx.argument.empty() && !rolling) {
            const auto& settings = ctl_.snapshot().settings;
            const QString what =
                at.length > 0
                    ? QStringLiteral("from ") + q(format_time(main_fmt_, at.position, settings)) +
                          QStringLiteral(" to ") +
                          q(format_time(main_fmt_, at.position + at.length, settings))
                    : QStringLiteral("at ") + q(format_time(main_fmt_, at.position, settings));
            emit markerRequested(q(ctl_.next_marker_name()), what, at.length > 0);
            return true;
        }
        return ctl_.add_marker(ctx.argument, at, why);
    });
}

}  // namespace sonora::ui
