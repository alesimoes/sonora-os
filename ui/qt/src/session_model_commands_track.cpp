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

void SessionModel::installTrackMenuCommands(CommandRegistry& /*local*/, const InstallSimple& simple) {
    using Why = std::string;
    const auto row = [this](const CommandContext& ctx) { return commandRow(ctx); };
    // --- Pro Tools' Track menu (ADR-0023) ---
    // A track command acts on the track given, or the selected one (the
    // router fills the target from the selection).
    const auto order = [this]() { return model::track_order(*ctl_.snapshot().edit); };
    simple("track.new", [this, order](const CommandContext& ctx, Why& why) {
        // Pro Tools' New Tracks asks how many first.
        if (ctx.argument.empty()) {
            emit newTracksRequested();
            return true;
        }
        int count = 0;
        if (!parse_repeat_count(ctx.argument, count)) {
            why = "\"" + ctx.argument +
                  "\" is not a number of tracks; type a whole number, 1 or more. No track was made.";
            return false;
        }
        std::vector<model::Id> after = order();
        const int n = static_cast<int>(after.size());
        const int sel = ctl_.selection().track;
        const int at = sel >= 0 && sel < n ? sel + 1 : n;
        after.insert(after.begin() + at, static_cast<std::size_t>(std::max(count, 0)), model::Id{0});
        return trackCommand([&](Why& w) { return ctl_.new_tracks(count, w); }, after, why);
    });
    simple("track.duplicate", [this, order, row](const CommandContext& ctx, Why& why) {
        const int i = row(ctx);
        std::vector<model::Id> after = order();
        if (i >= 0 && i < static_cast<int>(after.size())) {
            after.insert(after.begin() + i + 1, model::Id{0});
        }
        return trackCommand([&](Why& w) { return ctl_.duplicate_track(i, w); }, after, why);
    });
    simple("track.delete", [this, order, row](const CommandContext& ctx, Why& why) {
        const int i = row(ctx);
        std::vector<model::Id> after = order();
        if (i >= 0 && i < static_cast<int>(after.size())) after.erase(after.begin() + i);
        return trackCommand([&](Why& w) { return ctl_.delete_track(i, w); }, after, why);
    });
    for (const int delta : {-1, 1}) {
        simple(delta < 0 ? "track.move_up" : "track.move_down",
               [this, order, row, delta](const CommandContext& ctx, Why& why) {
                   const int i = row(ctx);
                   std::vector<model::Id> after = order();
                   const int n = static_cast<int>(after.size());
                   if (i >= 0 && i < n && i + delta >= 0 && i + delta < n) {
                       std::swap(after[static_cast<std::size_t>(i)],
                                 after[static_cast<std::size_t>(i + delta)]);
                   }
                   return trackCommand([&](Why& w) { return ctl_.move_track(i, delta, w); },
                                       after, why);
               });
    }

    // --- the playlist selector (ADR-0026) ---
    simple("track.playlist_new", [this, row](const CommandContext& ctx, Why& why) {
        const int i = row(ctx);
        return playlistCommand(i, true, [&](Why& w) { return ctl_.new_playlist(i, w); }, why);
    });
    simple("track.playlist_duplicate", [this, row](const CommandContext& ctx, Why& why) {
        const int i = row(ctx);
        return playlistCommand(i, true, [&](Why& w) { return ctl_.duplicate_playlist(i, w); },
                               why);
    });
    simple("track.playlist_choose", [this, row](const CommandContext& ctx, Why& why) {
        const int i = row(ctx);
        const bool chooses = ctx.argument != ctl_.active_playlist_name(i);
        return playlistCommand(
            i, chooses, [&](Why& w) { return ctl_.choose_playlist(i, ctx.argument, w); }, why);
    });
    simple("track.playlist_rename", [this, row](const CommandContext& ctx, Why& why) {
        const int i = row(ctx);
        if (ctx.argument.empty()) {
            if (i < 0 || i >= static_cast<int>(ctl_.snapshot().edit->tracks.size())) {
                why = i < 0 ? "no track is selected" : "there is no track " + std::to_string(i + 1);
                return false;
            }
            emit playlistRenameRequested(i, q(ctl_.active_playlist_name(i)));
            return true;
        }
        return playlistCommand(
            i, false, [&](Why& w) { return ctl_.rename_playlist(i, ctx.argument, w); }, why);
    });
}

void SessionModel::installNameCommands(CommandRegistry& /*local*/, const InstallSimple& simple) {
    using Why = std::string;
    const auto row = [this](const CommandContext& ctx) { return commandRow(ctx); };
    // --- names and colours (ADR-0023, update of 2026-11-02) ---
    const auto no_row = [this](int i, Why& why) {
        if (i >= 0 && i < static_cast<int>(ctl_.snapshot().edit->tracks.size())) return false;
        why = i < 0 ? "no track is selected" : "there is no track " + std::to_string(i + 1);
        return true;
    };
    simple("track.rename", [this, row, no_row](const CommandContext& ctx, Why& why) {
        const int i = row(ctx);
        if (ctx.argument.empty()) {
            if (no_row(i, why)) return false;
            emit nameRequested(i, q(ctl_.snapshot().edit->tracks[static_cast<std::size_t>(i)]->name));
            return true;
        }
        return ctl_.rename_track(i, ctx.argument, why);
    });
    // --- VCA masters and folders (ADR-0048). A refusal is held: it says what
    // was not done and why, and the status line would drop it in seconds.
    const auto held = [this](bool ok, Why& why) {
        if (!ok && !why.empty()) emit heldNotice(q(why));
        return ok;
    };
    simple("vca.new", [this, held](const CommandContext& ctx, Why& why) {
        const auto& sel = ctl_.selection();
        return held(ctl_.vca_new(sel.track, sel.tracks, ctx.argument, why), why);
    });
    simple("vca.delete", [this, held](const CommandContext& ctx, Why& why) {
        const auto& vcas = ctl_.snapshot().edit->vcas;
        for (std::size_t i = 0; i < vcas.size(); ++i) {
            if (vcas[i].name == ctx.argument) {
                return held(ctl_.vca_remove(static_cast<int>(i), why), why);
            }
        }
        why = ctx.argument.empty() ? "give the name of the VCA to delete"
                                   : "there is no VCA named \"" + ctx.argument + "\"";
        return held(false, why);
    });
    simple("folder.new", [this, held](const CommandContext& ctx, Why& why) {
        const auto& sel = ctl_.selection();
        return held(ctl_.folder_new(sel.track, sel.tracks, ctx.argument, why), why);
    });
    simple("folder.dissolve", [this, row, held](const CommandContext& ctx, Why& why) {
        return held(ctl_.folder_dissolve(row(ctx), why), why);
    });
    simple("region.rename", [this](const CommandContext& ctx, Why& why) {
        if (ctx.argument.empty()) {
            const Region* r = ctl_.selected_region(why);
            if (r == nullptr) return false;
            emit nameRequested(-1, q(r->name));
            return true;
        }
        return ctl_.rename_region(ctx.argument, why);
    });
    // The palette numbers its colours from 1, as the operator sees them.
    const auto palette_number = [](const std::string& a, int& colour) {
        if (a.empty() || a.size() > 2 ||
            !std::all_of(a.begin(), a.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return false;
        }
        colour = std::stoi(a) - 1;
        return colour >= 0 && colour < model::kColours;
    };
    const auto bad_colour = [](const std::string& a, bool region) {
        return "\"" + a + "\" is not a palette colour; give 1 to " +
               std::to_string(model::kColours) + (region ? ", or \"track\"" : "") +
               ". Nothing was coloured.";
    };
    simple("track.colour", [this, row, no_row, palette_number, bad_colour](
                               const CommandContext& ctx, Why& why) {
        const int i = row(ctx);
        if (ctx.argument.empty()) {
            if (no_row(i, why)) return false;
            emit colourRequested(i, ctl_.snapshot().edit->tracks[static_cast<std::size_t>(i)]->colour);
            return true;
        }
        int colour = 0;
        if (!palette_number(ctx.argument, colour)) {
            why = bad_colour(ctx.argument, false);
            return false;
        }
        // A key or the palette with no track named colours every selected
        // track, as Pro Tools' palette does.
        const auto& sel = ctl_.selection();
        const int count = !ctx.has_target() && sel.track == i ? sel.tracks : 1;
        return ctl_.set_track_colour(i, count, colour, why);
    });
    simple("region.colour", [this, palette_number, bad_colour](const CommandContext& ctx,
                                                                Why& why) {
        if (ctx.argument.empty()) {
            const Region* r = ctl_.selected_region(why);
            if (r == nullptr) return false;
            emit colourRequested(-1, r->colour == model::kTrackColour ? -1 : r->colour);
            return true;
        }
        int colour = model::kTrackColour;
        if (ctx.argument != "track" && !palette_number(ctx.argument, colour)) {
            why = bad_colour(ctx.argument, true);
            return false;
        }
        return ctl_.set_region_colour(colour, why);
    });
}

}  // namespace sonora::ui
