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

#include "consolidate_send.h"
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

void SessionModel::installClipCommands(CommandRegistry& /*local*/, const InstallSimple& simple) {
    using Why = std::string;
    const auto row = [this](const CommandContext& ctx) { return commandRow(ctx); };
    // Clip gain. A level is a number of dB, with or without a trailing "dB".
    const auto db_of = [](const std::string& text, float& out) {
        char* end = nullptr;
        const double v = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || !std::isfinite(v)) return false;
        while (*end == ' ') ++end;
        const std::string rest(end);
        if (!rest.empty() && rest != "dB" && rest != "db") return false;
        out = static_cast<float>(v);
        return true;
    };
    const auto bad_db = [](const std::string& arg) {
        return "\"" + arg + "\" is not a level in dB; type a number such as -3.5";
    };
    simple("clip.gain", [this, db_of, bad_db](const CommandContext& ctx, Why& why) {
        if (ctx.argument.empty()) {
            const Region* r = ctl_.selected_region(why);
            if (r == nullptr) return false;
            emit clipGainRequested(static_cast<double>(r->gain_db));
            return true;
        }
        float db = 0.0f;
        if (!db_of(ctx.argument, db)) {
            why = bad_db(ctx.argument);
            return false;
        }
        return ctl_.set_clip_gain(db, why);
    });
    const auto nudge = [this, db_of, bad_db](float sign) {
        return [this, db_of, bad_db, sign](const CommandContext& ctx, Why& why) {
            float step = 1.0f;
            if (!ctx.argument.empty() && (!db_of(ctx.argument, step) || step <= 0.0f)) {
                why = bad_db(ctx.argument);
                return false;
            }
            return ctl_.nudge_clip_gain(sign * step, why);
        };
    };
    simple("clip.gain_up", nudge(1.0f));
    simple("clip.gain_down", nudge(-1.0f));
    const auto gain_place = [this](const Region& r) {
        const model::Frames i = ctl_.selection().insertion;
        return i > r.position && i < r.end() ? i : r.position + r.length / 2;
    };
    simple("clip.gain_point", [this, db_of, bad_db, gain_place](const CommandContext& ctx, Why& why) {
        const Region* r = ctl_.selected_region(why);
        if (r == nullptr) return false;
        // "DB" or "DB@SECONDS" into the clip. Without a place: the insertion
        // point when it is inside the clip past its start, else the middle
        // (selecting a clip puts the insertion point at its start).
        model::Frames at = gain_place(*r);
        std::string level = ctx.argument;
        const std::size_t atsign = level.find('@');
        if (atsign != std::string::npos) {
            char* end = nullptr;
            const std::string secs = level.substr(atsign + 1);
            const double s = std::strtod(secs.c_str(), &end);
            if (end == secs.c_str() || *end != '\0' || !(s >= 0.0)) {
                why = "\"" + secs + "\" is not a time in seconds into the clip";
                return false;
            }
            at = r->position + static_cast<model::Frames>(s * ctl_.snapshot().settings.sample_rate);
            level = level.substr(0, atsign);
        }
        float db = EditController::clip_line_db_at(*r, at);
        if (!level.empty() && !db_of(level, db)) {
            why = bad_db(level);
            return false;
        }
        return ctl_.set_gain_point(at, db, why);
    });
    simple("clip.gain_point_remove", [this, gain_place](const CommandContext&, Why& why) {
        const Region* r = ctl_.selected_region(why);
        if (r == nullptr) return false;
        return ctl_.remove_gain_point(gain_place(*r), why);
    });
    simple("clip.gain_line_clear",
           [this](const CommandContext&, Why& why) { return ctl_.clear_gain_line(why); });
    simple("clip.gain_render", [this](const CommandContext&, Why& why) {
        // Render Clip Gain is Consolidate of the selected region (ADR-0042 §4);
        // a range or no selection is not a clip to render.
        if (ctl_.selected_region(why) == nullptr) return false;
        EditController::ConsolidateRequest req;
        if (!ctl_.consolidate_request(req, why)) return false;
        return send_consolidate(send_command_, req, "the clip gain", why);
    });
    simple("tempo.edit", [this](const CommandContext& ctx, Why& why) {
        if (ctx.argument.empty()) {
            const model::Frames at = ctl_.selection().has_range() ? ctl_.selection().start
                                                                  : ctl_.selection().insertion;
            emit tempoRequested(q(format_bpm(tempo_at(at, ctl_.snapshot().settings).bpm)));
            return true;
        }
        return ctl_.edit_tempo(ctx.argument, why);
    });
    simple("tempo.undo", [this](const CommandContext&, Why& why) { return ctl_.tempo_undo(why); });
    simple("tempo.redo", [this](const CommandContext&, Why& why) { return ctl_.tempo_redo(why); });
    simple("tempo.track", [this](const CommandContext&, Why&) {
        tempo_track_ = !tempo_track_;
        emit tempoTrackChanged();
        return true;
    });
    // --- clip groups and loops (ADR-0052) ---
    simple("clip.group", [this](const CommandContext&, Why& why) { return ctl_.group_clips(why); });
    simple("clip.ungroup", [this](const CommandContext&, Why& why) { return ctl_.ungroup_clip(why); });
    simple("clip.unloop", [this](const CommandContext&, Why& why) { return ctl_.unloop_clip(why); });
    simple("clip.loop", [this](const CommandContext& ctx, Why& why) {
        int count = ctl_.loop_count();
        // The menu's "Loop Clip..." (ADR-0212) asks; the key sends no argument
        // and loops silently with the last count.
        if (ctx.argument == "ask") {
            const model::Region* r = ctl_.selected_region(why);
            if (r == nullptr) return false;
            emit repeatRequested(count, q(r->name));
            return true;
        }
        if (!ctx.argument.empty() && !parse_repeat_count(ctx.argument, count)) {
            why = "\"" + ctx.argument +
                  "\" is not a number of repeats; type a whole number, 1 or more. Nothing was "
                  "looped.";
            return false;
        }
        return ctl_.loop_clip(count, why);
    });
    // --- sample-level editing (ADR-0052): the engine writes the new file ---
    const auto sample = [this, db_of, &simple](const char* command, const char* verb, bool takes_db,
                                      bool needs_arg) {
        simple(command, [this, db_of, command, verb, takes_db, needs_arg](const CommandContext& ctx,
                                                                 Why& why) {
            std::string param = ctx.argument;
            if (takes_db && !param.empty()) {
                float db = 0.0f;
                if (!db_of(param, db)) {
                    why = "\"" + param + "\" is not a level in dB; type a number such as -3.5";
                    return false;
                }
                char buf[32];
                std::snprintf(buf, sizeof buf, "%g", static_cast<double>(db));
                param = buf;
            }
            if (needs_arg && param.empty()) {
                why = takes_db ? "give the change in dB, such as -3"
                               : "give the points as FRAME:LEVEL,FRAME:LEVEL";
                return false;
            }
            if (!takes_db && !needs_arg && !param.empty()) {
                why = std::string(command) + " takes no argument";
                return false;
            }
            std::string argument;
            if (!ctl_.sample_edit_argument(verb, param, argument, why)) return false;
            if (!send_command_) {
                why = "no engine to edit the samples; nothing was edited";
                return false;
            }
            send_command_(QStringLiteral("sample.edit"), q(argument));
            return true;
        });
    };
    sample("sample.repair", "repair", false, false);
    sample("sample.draw", "draw", false, true);
    sample("sample.silence", "silence", false, false);
    sample("sample.reverse", "reverse", false, false);
    sample("sample.invert", "invert", false, false);
    sample("sample.gain", "gain", true, true);
    sample("sample.normalize", "normalize", true, false);
    sample("sample.fade_in", "fade_in", false, false);
    sample("sample.fade_out", "fade_out", false, false);
    simple("track.playlist_delete_unused", [this, row](const CommandContext& ctx, Why& why) {
        const int i = row(ctx);
        return playlistCommand(i, false,
                               [&](Why& w) { return ctl_.delete_unused_playlists(i, w); }, why);
    });
}

}  // namespace sonora::ui
