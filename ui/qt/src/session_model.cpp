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
#include <memory>

#include "fader_taper.h"
#include "scene_item.h"
#include "edit_clipboard.h"
#include "session_files.h"
#include "sonora/api/messages.h"
#include "sonora/command.h"
#include "sonora/model/session_file.h"
#include "sonora/model/session_template.h"
#include "transport_model.h"

namespace sonora::ui {

using namespace model;

namespace {

// The counters move with the playhead; re-publishing their text every
// fourth tick (15 Hz) is the shell's state rate and faster than anyone
// reads a counter.
constexpr unsigned kCounterEvery = 4;

QString q(const std::string& s) { return QString::fromStdString(s); }

bool same(const Selection& a, const Selection& b) {
    return a.track == b.track && a.tracks == b.tracks && a.region == b.region &&
           a.insertion == b.insertion &&
           a.start == b.start && a.end == b.end;
}

bool same(const ViewState& a, const ViewState& b) {
    return a.fpp == b.fpp && a.scroll == b.scroll && a.scroll_y == b.scroll_y &&
           a.width == b.width && a.height == b.height && a.track_height == b.track_height &&
           a.wave_scale == b.wave_scale && a.open == b.open && a.tops == b.tops;
}

bool same(const Drag& a, const Drag& b) {
    return a.kind == b.kind && a.moved == b.moved && a.start == b.start && a.end == b.end &&
           a.region == b.region && a.track_end == b.track_end;
}

}  // namespace

SessionModel::SessionModel(TimelineDocument& doc, const PeakSource& peaks, bool synthetic_peaks,
                           QObject* parent)
    : QObject(parent),
      doc_(doc),
      peaks_(peaks),
      synthetic_peaks_(synthetic_peaks),
      sessions_dir_(default_sessions_dir()),
      templates_dir_(default_templates_dir()),
      saved_revision_(doc.revision()),
      ctl_(doc) {
    updateCounters(true);
    // The list is a view over this session's work; the next one starts
    // without it.
    connect(this, &SessionModel::sessionOpened, this, [this]() { setWindowConfigsOpen(false); });
}

void SessionModel::installCommands(CommandRegistry& local) {
    using Why = std::string;
    const auto simple = [this, &local](const char* id, auto fn) {
        local.install(id, ExplainingHandler([this, fn](const CommandContext& ctx, Why& why) {
                          const bool ok = fn(ctx, why);
                          sync();
                          return ok;
                      }));
    };
    const auto mode = [this](EditMode m) {
        return [this, m](const CommandContext&, Why&) {
            ctl_.set_mode(m);
            return true;
        };
    };
    simple("edit.mode.shuffle", mode(EditMode::Shuffle));
    simple("edit.mode.slip", mode(EditMode::Slip));
    simple("edit.mode.spot", mode(EditMode::Spot));
    simple("edit.mode.grid", mode(EditMode::Grid));
    simple("edit.mode.next", [this](const CommandContext&, Why&) {
        ctl_.next_mode();
        return true;
    });

    const auto tool = [this](Tool t) {
        return [this, t](const CommandContext&, Why&) {
            ctl_.set_tool(t);
            return true;
        };
    };
    simple("tool.smart", tool(Tool::Smart));
    simple("tool.trim", tool(Tool::Trim));
    simple("tool.select", tool(Tool::Select));
    simple("tool.grab", tool(Tool::Grab));
    simple("tool.pencil", tool(Tool::Pencil));
    simple("tool.next", [this](const CommandContext&, Why&) {
        ctl_.next_tool();
        return true;
    });

    simple("zoom.in", [this](const CommandContext&, Why&) {
        ctl_.zoom_in();
        return true;
    });
    simple("zoom.out", [this](const CommandContext&, Why&) {
        ctl_.zoom_out();
        return true;
    });
    simple("zoom.vertical_in", [this](const CommandContext&, Why&) {
        ctl_.vertical_zoom(true);
        return true;
    });
    simple("zoom.vertical_out", [this](const CommandContext&, Why&) {
        ctl_.vertical_zoom(false);
        return true;
    });
    simple("zoom.fit_session",
           [this](const CommandContext&, Why& why) { return ctl_.zoom_fit_session(why); });
    simple("zoom.fit_selection",
           [this](const CommandContext&, Why& why) { return ctl_.zoom_fit_selection(why); });
    simple("zoom.toggle", [this](const CommandContext&, Why& why) { return ctl_.zoom_toggle(why); });

    simple("edit.undo", [this](const CommandContext&, Why& why) { return ctl_.undo(why); });
    simple("edit.redo", [this](const CommandContext&, Why& why) { return ctl_.redo(why); });
    // How the operator looks at the history, not the session: the
    // interface's own, so it opens with the engine gone too.
    local.install("window.undo_history", [this](const CommandContext&) {
        setUndoHistoryOpen(!undo_history_open_);
        return true;
    });
    simple("edit.clear", [this](const CommandContext&, Why& why) { return ctl_.clear(why); });
    simple("automation.thin",
           [this](const CommandContext&, Why& why) { return ctl_.thin_automation(why); });
    simple("automation.toggle_follows_edit", [this](const CommandContext&, Why&) {
        ctl_.set_automation_follows_edit(!ctl_.automation_follows_edit());
        return true;
    });
    simple("edit.cut", [this](const CommandContext&, Why& why) { return ctl_.cut(why); });
    simple("edit.copy", [this](const CommandContext&, Why& why) { return ctl_.copy(why); });
    // A paste that left lanes out still ran; what it left out goes on the
    // status line rather than nowhere. Queued, because a key's success
    // clears the status line once its handler returns.
    simple("edit.paste", [this](const CommandContext&, Why& why) {
        if (!ctl_.paste(why)) return false;
        if (!why.empty()) {
            QMetaObject::invokeMethod(
                this, [this, said = q(why)]() { emit notice(said); }, Qt::QueuedConnection);
        }
        why.clear();
        return true;
    });
    simple("edit.select_all", [this](const CommandContext&, Why& why) { return ctl_.select_all(why); });

    // --- Pro Tools' Track menu (ADR-0023) ---
    // A track command acts on the track given, or the selected one (the
    // router fills the target from the selection).
    const auto order = [this]() { return model::track_order(*ctl_.snapshot().edit); };
    const auto row = [this](const CommandContext& ctx) {
        return ctx.has_target() ? ctx.target : ctl_.selection().track;
    };
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
    simple("track.playlist_delete_unused", [this, row](const CommandContext& ctx, Why& why) {
        const int i = row(ctx);
        return playlistCommand(i, false,
                               [&](Why& w) { return ctl_.delete_unused_playlists(i, w); }, why);
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
    local.install("window.memory_locations", [this](const CommandContext&) {
        setMemoryLocationsOpen(!memory_locations_open_);
        return true;
    });
    local.install("window.latency", [this](const CommandContext&) {
        setLatencyOpen(!latency_open_);
        return true;
    });
    local.install("window.configurations", [this](const CommandContext&) {
        setWindowConfigsOpen(!window_configs_open_);
        return true;
    });
    // The argument is the slot. A bad or empty one is refused with its
    // reason: the screen must not move on a guess.
    const auto slot_of = [](const CommandContext& ctx, int& slot, Why& why) {
        bool ok = false;
        slot = QString::fromStdString(ctx.argument).trimmed().toInt(&ok);
        if (!ok) why = "name a window configuration, 1 to " + std::to_string(WindowLayouts::kSlots);
        return ok;
    };
    simple("window.config_store", [this, slot_of](const CommandContext& ctx, Why& why) {
        int slot = 0;
        return slot_of(ctx, slot, why) && storeWindowConfig(slot, why);
    });
    simple("window.config_recall", [this, slot_of](const CommandContext& ctx, Why& why) {
        int slot = 0;
        return slot_of(ctx, slot, why) && recallWindowConfig(slot, why);
    });
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
    // The interface's, as mute and solo are: Input mode is the document's
    // (TrackMix::monitor), saved with the session, and the engine follows
    // it through the mix it applies (PlaybackController::apply_monitoring).
    simple("track.toggle_input_monitor", [this](const CommandContext& ctx, Why& why) {
        return ctl_.toggle_input_monitor(ctx.has_target() ? ctx.target : ctl_.selection().track,
                                         why);
    });

    simple("track.step_automation_mode", [this](const CommandContext& ctx, Why& why) {
        return ctl_.step_automation_mode(ctx.has_target() ? ctx.target : ctl_.selection().track,
                                         why);
    });

    // Saving is the engine's once it has session commands (session_files.h);
    // until then the interface writes the file itself.
    simple("app.save", [this](const CommandContext&, Why& why) { return saveSession(why); });
    // Pro Tools' Save As and Save Copy In ask for the name first; the key
    // sends none, so the window's dialog asks and sends it back.
    const auto save_as = [this](bool copy) {
        return [this, copy](const CommandContext& ctx, Why& why) {
            if (ctx.argument.empty()) {
                if (overlay_.recording) {
                    why = std::string("stop recording before ") + (copy ? "Save Copy In" : "Save As");
                    return false;
                }
                const QString root = saveAsRoot();
                const std::string& n = ctl_.snapshot().settings.name;
                const QString base = n.empty() ? QStringLiteral("Untitled") : q(n);
                emit saveAsRequested(copy, unused_session_name(root, copy ? base + QStringLiteral(" copy") : base),
                                     root);
                return true;
            }
            return saveSessionAs(q(ctx.argument), copy, why);
        };
    };
    simple("app.save_as", save_as(false));
    simple("app.save_copy_in", save_as(true));
    // Pro Tools' Save As Template asks for the name as Save As does; the
    // markers are the dialog's check box, kept on the model.
    simple("app.save_as_template", [this](const CommandContext& ctx, Why& why) {
        if (ctx.argument.empty()) {
            if (overlay_.recording) {
                why = "stop recording before Save as Template";
                return false;
            }
            const std::string& n = ctl_.snapshot().settings.name;
            const QString base = n.empty() ? QStringLiteral("Untitled") : q(n);
            emit saveAsTemplateRequested(
                unused_session_name(templates_dir_, base + QStringLiteral(" template")), templates_dir_);
            return true;
        }
        return saveSessionAsTemplate(q(ctx.argument), template_markers_, why);
    });
    simple("app.new_from_template", [this](const CommandContext&, Why&) {
        emit templateBrowserRequested();
        return true;
    });

    simple("session.import_audio", [this](const CommandContext&, Why& why) {
        if (overlay_.recording) {
            why = "stop recording before Import Audio";
            return false;
        }
        emit importBrowserRequested();
        return true;
    });

    simple("window.toggle_mix_edit", [this](const CommandContext&, Why&) {
        setWindow(window_ == QStringLiteral("edit") ? QStringLiteral("mix") : QStringLiteral("edit"));
        return true;
    });
}

QObject* SessionModel::importer() {
    if (import_ == nullptr) {
        ImportModel::Hooks h;
        h.session_folder = [this] { return engine_folder_; };
        h.sessions_dir = [this] { return sessions_dir_; };
        h.recording = [this] { return overlay_.recording; };
        import_ = new ImportModel(ctl_, std::move(h), this);
        connect(import_, &ImportModel::heldNotice, this, &SessionModel::heldNotice);
        connect(import_, &ImportModel::imported, this, [this](int) { sync(); });
    }
    return import_;
}

void SessionModel::attachItem(SceneItem* item) {
    if (std::find(items_.begin(), items_.end(), item) == items_.end()) items_.push_back(item);
}

void SessionModel::detachItem(SceneItem* item) {
    items_.erase(std::remove(items_.begin(), items_.end(), item), items_.end());
}

void SessionModel::sync() {
    // Held: the operator is watching the region that moved, not the status
    // line, and the lane it left behind no longer matches what plays there.
    const std::string left_behind = ctl_.take_follow_notice();
    if (!left_behind.empty()) emit heldNotice(q(left_behind));
    const Snapshot& s = ctl_.snapshot();
    bool content = false;
    if (s.revision != seen_revision_) {
        seen_revision_ = s.revision;
        content = true;
        emit contentChanged();
        emit mixChanged();
        emit markersChanged();
        emit rowsChanged();
        if (dirty() != seen_dirty_) {
            seen_dirty_ = dirty();
            emit sessionChanged();
        }
    }
    const int m = static_cast<int>(ctl_.mode()), t = static_cast<int>(ctl_.tool()),
              g = static_cast<int>(ctl_.grid());
    const int fades = static_cast<int>(ctl_.fade_shape()) * 16 +
                      static_cast<int>(ctl_.crossfade_shape());
    const uint64_t clip = ctl_.clipboard_serial();
    const int follows = ctl_.automation_follows_edit() ? 1 : 0;
    if (m != seen_mode_ || t != seen_tool_ || g != seen_grid_ || fades != seen_fades_ ||
        clip != seen_clip_ || follows != seen_follows_) {
        seen_clip_ = clip;
        seen_follows_ = follows;
        content = content || m != seen_mode_ || g != seen_grid_;  // grid lines follow them
        seen_mode_ = m;
        seen_tool_ = t;
        seen_grid_ = g;
        seen_fades_ = fades;
        emit editStateChanged();
    }
    bool overlay = false;
    if (!seen_sel_valid_ || !same(seen_sel_, ctl_.selection())) {
        seen_sel_ = ctl_.selection();
        seen_sel_valid_ = true;
        content = overlay = true;
        emit selectionChanged();
        updateCounters(true);
    }
    if (!seen_view_valid_ || !same(seen_view_, ctl_.view())) {
        seen_view_ = ctl_.view();
        seen_view_valid_ = true;
        content = overlay = true;
        emit viewChanged();
        emit rowsChanged();
    }
    if (!same(seen_drag_, ctl_.drag())) {
        seen_drag_ = ctl_.drag();
        content = true;
    }
    for (SceneItem* item : items_) {
        if (content) item->markContentDirty();
        if (overlay) item->markOverlayDirty();
    }
}

QString SessionModel::undoText() const {
    if (!doc_.can_undo()) return QStringLiteral("Can't Undo");
    const std::string what = step_text(doc_.undo_label());
    return what.empty() ? QStringLiteral("Undo") : QStringLiteral("Undo ") + q(what);
}

QString SessionModel::redoText() const {
    if (!doc_.can_redo()) return QStringLiteral("Can't Redo");
    const std::string what = step_text(doc_.redo_label());
    return what.empty() ? QStringLiteral("Redo") : QStringLiteral("Redo ") + q(what);
}

QVariantList SessionModel::undoHistory() const {
    const model::History& h = doc_.history();
    QVariantList rows;
    for (std::size_t i = 1; i < h.step_count(); ++i) {
        const std::string what = step_text(h.step_label(i));
        QVariantMap row;
        row[QStringLiteral("label")] = what.empty() ? QStringLiteral("Edit") : q(what);
        row[QStringLiteral("done")] = i <= h.undo_depth();
        rows.push_back(row);
    }
    return rows;
}

bool SessionModel::undoHistoryTrimmed() const {
    const model::History& h = doc_.history();
    return h.step_count() > 0 && !h.step_label(0).empty();
}

void SessionModel::setUndoHistoryOpen(bool on) {
    if (on == undo_history_open_) return;
    undo_history_open_ = on;
    layouts_.setOpen(QStringLiteral("undo_history"), on);
    emit undoHistoryOpenChanged();
}

bool SessionModel::undoHistoryClick(int row) {
    const model::History& h = doc_.history();
    std::string why;
    if (row < 0 || static_cast<std::size_t>(row) + 1 >= h.step_count()) {
        return run(false, "that step is no longer in the undo history");
    }
    const std::size_t step = static_cast<std::size_t>(row) + 1;
    const std::size_t depth = step <= h.undo_depth() ? step - 1 : step;
    return run(ctl_.undo_to(depth, why), why);
}

bool SessionModel::undoHistoryAll(bool undo) {
    const model::History& h = doc_.history();
    std::string why;
    if (undo && !h.can_undo()) return run(false, "nothing to undo");
    if (!undo && !h.can_redo()) return run(false, "nothing to redo");
    return run(ctl_.undo_to(undo ? 0 : h.step_count() - 1, why), why);
}

QVariantList SessionModel::markers() const {
    const auto& settings = ctl_.snapshot().settings;
    QVariantList rows;
    int number = 0;
    for (const Marker& m : ctl_.markers().markers) {
        QVariantMap row;
        row[QStringLiteral("id")] = static_cast<double>(m.id);
        row[QStringLiteral("number")] = ++number;
        row[QStringLiteral("name")] = q(m.name);
        row[QStringLiteral("start")] = q(format_time(main_fmt_, m.position, settings));
        row[QStringLiteral("range")] = m.length > 0;
        row[QStringLiteral("end")] =
            m.length > 0 ? q(format_time(main_fmt_, m.position + m.length, settings)) : QString();
        row[QStringLiteral("length")] = m.length > 0 ? q(format_length(main_fmt_, m.position, m.length, settings))
                                                     : QString();
        rows.push_back(row);
    }
    return rows;
}

void SessionModel::setMemoryLocationsOpen(bool on) {
    if (on == memory_locations_open_) return;
    memory_locations_open_ = on;
    layouts_.setOpen(QStringLiteral("memory_locations"), on);
    emit memoryLocationsOpenChanged();
}

void SessionModel::setLatencyOpen(bool on) {
    if (on == latency_open_) return;
    latency_open_ = on;
    layouts_.setOpen(QStringLiteral("latency"), on);
    emit latencyOpenChanged();
}

QVariantList SessionModel::windowConfigs() const {
    QVariantList list;
    for (int n = 1; n <= WindowLayouts::kSlots; ++n) {
        QVariantMap m;
        m.insert(QStringLiteral("slot"), n);
        m.insert(QStringLiteral("stored"), layouts_.stored(n));
        m.insert(QStringLiteral("summary"), layouts_.summary(n));
        list.push_back(m);
    }
    return list;
}

void SessionModel::setWindowConfigsOpen(bool on) {
    if (on == window_configs_open_) return;
    window_configs_open_ = on;
    emit windowConfigsOpenChanged();
}

void SessionModel::windowPlaced(const QString& id, int x, int y) { layouts_.place(id, x, y); }

QVariantMap SessionModel::windowPlace(const QString& id) const {
    QVariantMap m;
    const auto it = layouts_.current().windows.find(id);
    if (it == layouts_.current().windows.end() || !it->second.placed) return m;
    m.insert(QStringLiteral("x"), it->second.x);
    m.insert(QStringLiteral("y"), it->second.y);
    return m;
}

void SessionModel::noteWindowOpen(const QString& id, bool open) { layouts_.setOpen(id, open); }

void SessionModel::setLayoutsFile(const QString& path) {
    layouts_file_ = path;
    QString why;
    if (!path.isEmpty() && !layouts_.load(path, why)) {
        emit heldNotice(QStringLiteral("Window configurations not loaded: ") + why);
    }
    emit windowConfigsChanged();
}

bool SessionModel::storeWindowConfig(int slot, std::string& why) {
    QString reason;
    if (!layouts_.store(slot, reason)) {
        why = reason.toStdString();
        emit heldNotice(reason);
        return false;
    }
    emit windowConfigsChanged();
    if (!layouts_file_.isEmpty() && !layouts_.save(layouts_file_, reason)) {
        // Kept for this run, lost at the next: say so where it stays in view.
        emit heldNotice(QStringLiteral("Window configuration ") + QString::number(slot) +
                        QStringLiteral(" is stored for now but could not be saved: ") + reason);
    }
    return true;
}

bool SessionModel::recallWindowConfig(int slot, std::string& why) {
    QString reason;
    if (!layouts_.recall(slot, reason)) {
        why = reason.toStdString();
        emit heldNotice(reason);
        return false;
    }
    // Copy first: the setters below write back into layouts_.
    const WindowLayout target = layouts_.current();
    const auto open_of = [&target](const char* id) {
        const auto it = target.windows.find(QString::fromLatin1(id));
        return it != target.windows.end() && it->second.open;
    };
    setWindow(target.view);
    setUndoHistoryOpen(open_of("undo_history"));
    setMemoryLocationsOpen(open_of("memory_locations"));
    setLatencyOpen(open_of("latency"));
    if (transport_open_hook_) transport_open_hook_(open_of("transport"));
    emit windowPlacesRecalled();
    return true;
}

bool SessionModel::recallMarker(int row) {
    const auto& list = ctl_.markers().markers;
    if (row < 0 || static_cast<std::size_t>(row) >= list.size()) {
        return run(false, "that marker is no longer in the session");
    }
    std::string why;
    return run(ctl_.recall_marker(list[static_cast<std::size_t>(row)].id, why), why);
}

bool SessionModel::removeMarker(int row) {
    const auto& list = ctl_.markers().markers;
    if (row < 0 || static_cast<std::size_t>(row) >= list.size()) {
        return run(false, "that marker is no longer in the session");
    }
    std::string why;
    return run(ctl_.remove_marker(list[static_cast<std::size_t>(row)].id, why), why);
}

void SessionModel::markerRulerClick(qreal x, bool alt) {
    const Id id = ctl_.marker_at(x);
    std::string why;
    if (id == kNoId) {
        ctl_.set_insertion(ctl_.view().frame_at(x));
        run(true, why);
        return;
    }
    run(alt ? ctl_.remove_marker(id, why) : ctl_.recall_marker(id, why), why);
}

bool SessionModel::trackCommand(const std::function<bool(std::string&)>& run_op,
                                const std::vector<model::Id>& after, std::string& why) {
    const model::EditState& edit = *ctl_.snapshot().edit;
    const int limit = engine_track_count_ ? engine_track_count_() : 0;
    if (limit > 0 && after.size() > edit.tracks.size() && static_cast<int>(after.size()) > limit) {
        why = "the engine was started with " + std::to_string(limit) +
              " tracks and the session has them all; delete a track before adding another";
        return false;
    }
    // The engine refuses these too; refused here, the reason is held
    // rather than flashed while the operator watches the take.
    if (overlay_.recording && engine_armed_) {
        const std::string r =
            model::pinned_track_refusal(edit, after, engine_armed_(), "is recording", "stop recording");
        if (!r.empty()) {
            setTrackRefusal(q(r));
            why = r;
            return false;
        }
    }
    if (!run_op(why)) return false;
    setTrackRefusal(QString());
    return true;
}

void SessionModel::setRoutingRefusal(const QString& why) {
    if (why == routing_refusal_) return;
    routing_refusal_ = why;
    emit routingRefusalChanged();
}

bool SessionModel::playlistCommand(int track, bool chooses,
                                   const std::function<bool(std::string&)>& run_op,
                                   std::string& why) {
    const model::EditState& edit = *ctl_.snapshot().edit;
    // The engine refuses these too; refused here, the reason is held
    // rather than flashed while the operator watches the take.
    if (chooses && overlay_.recording && engine_armed_ && track >= 0 &&
        track < static_cast<int>(edit.tracks.size())) {
        const std::string r = model::pinned_playlist_refusal(
            edit, {edit.tracks[static_cast<std::size_t>(track)]->id}, engine_armed_());
        if (!r.empty()) {
            setTrackRefusal(q(r));
            why = r;
            return false;
        }
    }
    if (!run_op(why)) return false;
    setTrackRefusal(QString());
    return true;
}

void SessionModel::setTrackRefusal(const QString& why) {
    if (why == track_refusal_) return;
    track_refusal_ = why;
    emit trackRefusalChanged();
}

// Sent as the one field, never as a copy of the settings this window holds,
// which may be behind the engine's (ADR-0016, update "transport settings at
// the authority"). A refusal of the engine's comes back on the status line.
bool SessionModel::setRollText(bool pre, const QString& text) {
    std::string why;
    model::Frames frames = 0;
    if (!parse_roll_text(text.toStdString(), ctl_.snapshot().settings.sample_rate, frames, why)) {
        return run(false, why);
    }
    if (!send_command_) return run(false, "no engine to set the roll on");
    send_command_(pre ? QStringLiteral("transport.set_pre_roll")
                      : QStringLiteral("transport.set_post_roll"),
                  QString::number(frames));
    return true;
}

bool SessionModel::setPreRollText(const QString& text) { return setRollText(true, text); }
bool SessionModel::setPostRollText(const QString& text) { return setRollText(false, text); }

bool SessionModel::run(bool ok, const std::string& why) {
    sync();
    if (!ok && !why.empty()) emit refused(q(why));
    return ok;
}

void SessionModel::documentUpdated() {
    const model::Snapshot& s = ctl_.snapshot();
    if (ctl_.selection().track >= static_cast<int>(s.edit->tracks.size())) {
        ctl_.document_replaced();
    } else if (ctl_.selection().region != kNoId &&
               find_region(s, ctl_.selection().region) == nullptr) {
        ctl_.clear_selection();
    }
    ctl_.clamp_selection();
    // Another interface's edit can arrive at the revision an edit of ours
    // was shown at, so the revision alone does not say the content changed.
    seen_revision_ = UINT64_MAX;
    // A stopped take's region has arrived (or the document moved on without
    // it): what the engine placed is drawn now, not the live overview.
    if (!live_.recording() && !live_.empty()) {
        live_.clear();
        for (SceneItem* item : items_) item->markOverlayDirty();
    }
    sync();
    saveAfterOpen();
}

void SessionModel::onTransport(const TransportSnapshot& t, bool alive) {
    // A stale read is a seqlock miss, not a stop (ADR-0007 §3): keep what is
    // shown, as TransportModel does, or the bar counter drops to the
    // insertion point while the Transport window keeps the playhead.
    if (alive && t.stale) return;
    const bool rolling = alive && t.state != TransportMode::Stopped;
    const bool recording = rolling && t.state == TransportMode::Recording;
    const auto playhead = static_cast<Frames>(t.playhead_frames);
    const bool changed = rolling != overlay_.rolling || recording != overlay_.recording ||
                         (rolling && playhead != overlay_.playhead);
    overlay_.rolling = rolling;
    overlay_.recording = recording;
    if (rolling) overlay_.playhead = playhead;
    if (!changed) return;
    if (rolling && ctl_.follow(playhead)) sync();
    for (SceneItem* item : items_) item->markOverlayDirty();
    updateCounters(++transport_ticks_ % kCounterEvery == 0 || !rolling);
}

void SessionModel::onLiveTake(const EngineFrame& f, const std::vector<float>& peaks, bool alive) {
    const bool drawn = !live_.empty();
    if (!alive) {
        // The engine that comes back says what it kept; a take drawn
        // growing for an engine that is gone would be one nobody records.
        live_.clear();
    } else {
        if (f.transport.stale) return;
        // A count-off records nothing yet.
        const bool recording =
            f.transport.state == TransportMode::Recording && !f.transport.counting_off;
        const auto& tracks = ctl_.snapshot().edit->tracks;
        live_armed_.assign(f.tracks.size(), false);
        live_audio_.assign(tracks.size(), false);
        for (std::size_t i = 0; i < f.tracks.size(); ++i) live_armed_[i] = f.tracks[i].armed;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            live_audio_[i] = tracks[i]->kind == model::TrackKind::Audio;
        }
        live_.tick(recording, static_cast<Frames>(f.transport.playhead_frames), live_armed_,
                   live_audio_, peaks, ctl_.snapshot().settings.transport);
    }
    if (!drawn && live_.empty()) return;
    for (SceneItem* item : items_) item->markOverlayDirty();
}

void SessionModel::updateCounters(bool force) {
    if (!force) return;
    const Frames at = overlay_.rolling ? overlay_.playhead : ctl_.selection().insertion;
    const QString main = q(format_time(main_fmt_, at, ctl_.snapshot().settings));
    const QString sub = q(format_time(sub_fmt_, at, ctl_.snapshot().settings));
    if (main == main_counter_ && sub == sub_counter_) return;
    main_counter_ = main;
    sub_counter_ = sub;
    emit counterChanged();
}

// --- properties ---

QString SessionModel::sessionName() const {
    const std::string& n = ctl_.snapshot().settings.name;
    QString name = n.empty() ? QStringLiteral("Untitled session") : q(n);
    if (dirty()) name += QStringLiteral(" · edited");
    if (synthetic()) name += QStringLiteral(" · synthetic audio");
    return name;
}

bool SessionModel::synthetic() const {
    return synthetic_peaks_ && !ctl_.snapshot().sources->sources.empty();
}

int SessionModel::trackCount() const {
    return static_cast<int>(ctl_.snapshot().edit->tracks.size());
}

QStringList SessionModel::trackNames() const {
    QStringList out;
    for (const auto& t : ctl_.snapshot().edit->tracks) out << q(t->name);
    return out;
}

QList<int> SessionModel::trackColours() const {
    QList<int> out;
    for (const auto& t : ctl_.snapshot().edit->tracks) out << t->colour;
    return out;
}

QVariantList SessionModel::trackRows() const {
    const Snapshot& s = ctl_.snapshot();
    const ViewState& v = ctl_.view();
    QVariantList rows;
    for (std::size_t i = 0; i < s.edit->tracks.size(); ++i) {
        const TrackEdit& t = *s.edit->tracks[i];
        QVariantMap row;
        row[QStringLiteral("top")] = v.tops.size() > i ? v.tops[i] : static_cast<qreal>(i) * v.track_height;
        row[QStringLiteral("shown")] = v.autolanes_open(static_cast<int>(i));
        row[QStringLiteral("canShow")] = t.kind != TrackKind::Midi;
        // What each lane is, said in the words of what the engine does with
        // it: a lane the mode leaves unplayed is not "automation" to the ear.
        const TrackMix* mix = s.mix.find(t.id);
        const char* mode = "Read";
        if (mix != nullptr) {
            switch (mix->automation) {
                case AutomationMode::Off: mode = "Off"; break;
                case AutomationMode::Read: mode = "Read"; break;
                case AutomationMode::Touch: mode = "Touch"; break;
                case AutomationMode::Latch: mode = "Latch"; break;
                case AutomationMode::Write: mode = "Write"; break;
                case AutomationMode::Trim: mode = "Trim"; break;
                case AutomationMode::WriteTrim: mode = "Write/Trim"; break;
            }
        }
        QStringList captions;
        for (int l = 0; l < kLaneCount; ++l) {
            const AutomationLane* lane = t.lane(lane_param(l));
            // A trim with no lane has no static value: it plays 0 dB.
            QString what = lane != nullptr ? QStringLiteral("%1 pts").arg(lane->points.size())
                           : lane_param(l) == AutomationParam::VolumeTrim ? QStringLiteral("0 dB")
                                                                          : QStringLiteral("static");
            captions << what + QStringLiteral("  ") + QChar(0x00B7) + QStringLiteral("  ") + QString::fromLatin1(mode);
        }
        row[QStringLiteral("captions")] = captions;
        rows.push_back(row);
    }
    return rows;
}

QStringList SessionModel::playlistNames() const {
    QStringList out;
    const int n = static_cast<int>(ctl_.snapshot().edit->tracks.size());
    for (int i = 0; i < n; ++i) out << q(ctl_.active_playlist_name(i));
    return out;
}

QStringList SessionModel::trackPlaylists(int track) const {
    QStringList out;
    for (const std::string& name : ctl_.playlist_names(track)) out << q(name);
    return out;
}

QString SessionModel::selectionStart() const {
    return q(format_time(main_fmt_, ctl_.selection().start, ctl_.snapshot().settings));
}

QString SessionModel::selectionEnd() const {
    return q(format_time(main_fmt_, ctl_.selection().end, ctl_.snapshot().settings));
}

QString SessionModel::selectionLength() const {
    const Selection& s = ctl_.selection();
    return q(format_min_sec(s.end - s.start, ctl_.snapshot().settings.sample_rate));
}

QString SessionModel::zoomText() const {
    const double px_per_s = ctl_.snapshot().settings.sample_rate / ctl_.view().fpp;
    return px_per_s >= 10 ? QString::number(px_per_s, 'f', 0) + QStringLiteral(" px/s")
                          : QString::number(1.0 / px_per_s, 'f', 1) + QStringLiteral(" s/px");
}

// The Mix window's strips are the Edit window's tracks, in display order.
// MixState::tracks is keyed by id, in no display order, and keeps the
// entries of undone AddTracks: indexing it by strip showed one track's
// fader on another's strip once tracks were reordered or one was undone.
namespace {
template <typename T, typename F>
QList<T> per_strip(const model::Snapshot& s, F field) {
    QList<T> out;
    for (const auto& t : s.edit->tracks) {
        const model::TrackMix* m = s.mix.find(t->id);
        out << (m != nullptr ? field(*m) : field(model::TrackMix{}));
    }
    return out;
}

const model::TrackMix* strip_mix(const model::Snapshot& s, int track) {
    if (track < 0 || track >= static_cast<int>(s.edit->tracks.size())) return nullptr;
    return s.mix.find(s.edit->tracks[static_cast<std::size_t>(track)]->id);
}
}  // namespace

QList<bool> SessionModel::mutes() const {
    return per_strip<bool>(ctl_.snapshot(), [](const model::TrackMix& m) { return m.mute; });
}

QList<bool> SessionModel::solos() const {
    return per_strip<bool>(ctl_.snapshot(), [](const model::TrackMix& m) { return m.solo; });
}

QList<bool> SessionModel::soloSafes() const {
    return per_strip<bool>(ctl_.snapshot(), [](const model::TrackMix& m) { return m.solo_safe; });
}

QStringList SessionModel::autolaneNames() const {
    QStringList out;
    for (int l = 0; l < kLaneCount; ++l) out << QString::fromLatin1(lane_name(lane_param(l)));
    return out;
}

QStringList SessionModel::automationModes() const {
    // Indexed by AutomationMode: one name for each.
    static const char* const names[] = {"Off",   "Read", "Touch",     "Latch",
                                        "Write", "Trim", "Write/Trim"};
    static_assert(sizeof names / sizeof names[0] ==
                  static_cast<std::size_t>(model::AutomationMode::WriteTrim) + 1);
    QStringList out;
    for (const QString& n : per_strip<QString>(ctl_.snapshot(), [](const model::TrackMix& m) {
             return QString::fromLatin1(names[static_cast<int>(m.automation)]);
         })) {
        out << n;
    }
    return out;
}

QList<bool> SessionModel::inputMonitors() const {
    return per_strip<bool>(ctl_.snapshot(), [](const model::TrackMix& m) {
        return m.monitor == model::MonitorMode::Input;
    });
}

QList<bool> SessionModel::implicitMutes() const {
    const bool any = anySolo();
    return per_strip<bool>(ctl_.snapshot(), [any](const model::TrackMix& m) {
        return any && !m.solo && !m.solo_safe;
    });
}

QList<qreal> SessionModel::gains() const {
    return per_strip<qreal>(ctl_.snapshot(),
                            [](const model::TrackMix& m) { return qreal(m.gain_db); });
}

QList<qreal> SessionModel::pans() const {
    return per_strip<qreal>(ctl_.snapshot(), [](const model::TrackMix& m) { return qreal(m.pan); });
}

bool SessionModel::anySolo() const {
    // An undone track's solo silences nothing that is still there.
    for (const bool solo : solos()) {
        if (solo) return true;
    }
    return false;
}

// The mode, not the range: Pro Tools lights Loop Playback with no
// selection, and a range kept with the mode off does not loop.
bool SessionModel::loopOn() const { return ctl_.snapshot().settings.transport.loop_playback; }

// Pro Tools' Loop Record, apart from Loop Playback: what makes Record a
// loop take.
bool SessionModel::loopRecordOn() const {
    return ctl_.snapshot().settings.transport.loop_record;
}

bool SessionModel::punchOn() const {
    const auto& p = ctl_.snapshot().settings.transport.punch;
    return p[1] > p[0];
}

QString SessionModel::tempoText() const {
    const auto& tempo = ctl_.snapshot().settings.tempo;
    if (tempo.empty()) return {};
    const auto& t = tempo.front();
    return QStringLiteral("%1 bpm  %2/%3")
        .arg(static_cast<double>(t.bpm), 0, 'f', 0)
        .arg(t.meter[0])
        .arg(t.meter[1]);
}

// --- Edit window ---

void SessionModel::setViewport(qreal width, qreal height) {
    ctl_.set_viewport(width, height);
    sync();
}

void SessionModel::press(qreal x, qreal y) {
    std::string why;
    run(ctl_.press(x, y, why), why);
}

void SessionModel::drag(qreal x, qreal y) {
    ctl_.move(x, y);
    sync();
}

void SessionModel::release(qreal x, qreal y) {
    std::string why;
    run(ctl_.release(x, y, why), why);
}

void SessionModel::doubleClick(qreal x, qreal y) {
    const Hit h = ctl_.hit(x, y);
    // The press before it picked the region up only as a Grabber does, so
    // a Selector double-click, which selects time, names nothing.
    if (h.region == kNoId || h.region != ctl_.selection().region) return;
    std::string why;
    const Region* r = ctl_.selected_region(why);
    if (r != nullptr) emit nameRequested(-1, q(r->name));
}

void SessionModel::wheel(qreal x, qreal dx, qreal dy, bool shift, bool ctrl) {
    if (ctrl) {
        ctl_.zoom_by(dy > 0 ? 1.0 / 1.25 : 1.25, x);
    } else if (shift || std::abs(dx) > std::abs(dy)) {
        ctl_.scroll_by(-(shift ? dy : dx), 0);
    } else {
        ctl_.scroll_by(0, -dy);
    }
    sync();
}

void SessionModel::rulerClick(qreal x) {
    ctl_.set_insertion(ctl_.view().frame_at(x));
    sync();
}

void SessionModel::selectTrack(int index) {
    ctl_.select_track(index);
    sync();
}

QList<int> SessionModel::selectedTracks() const {
    const Selection& s = ctl_.selection();
    const int n = static_cast<int>(ctl_.snapshot().edit->tracks.size());
    QList<int> out;
    if (s.track < 0) return out;
    for (int i = s.track; i < s.track + s.tracks && i < n; ++i) out << i;
    return out;
}

QList<int> SessionModel::selectedTracksFor(int track) const {
    const Selection& s = ctl_.selection();
    const int n = static_cast<int>(ctl_.snapshot().edit->tracks.size());
    QList<int> out;
    if (s.track >= 0 && s.covers(track)) {
        for (int i = s.track; i < s.track + s.tracks && i < n; ++i) out << i;
    } else {
        out << track;
    }
    return out;
}

void SessionModel::inputClick(int track, int modifiers) {
    std::string why;
    const auto& tracks = ctl_.snapshot().edit->tracks;
    const int n = static_cast<int>(tracks.size());
    const bool alt = (modifiers & Qt::AltModifier) != 0;
    const bool shift = (modifiers & Qt::ShiftModifier) != 0;
    // A plain click, or one on a track with no input: the toggle's own
    // refusal says why, rather than a group silently skipping the track.
    if ((!alt && !shift) || track < 0 || track >= n ||
        tracks[static_cast<std::size_t>(track)]->kind != model::TrackKind::Audio) {
        run(ctl_.toggle_input_monitor(track, why), why);
        return;
    }
    const QList<bool> now = inputMonitors();
    const bool on = !now[track];
    QList<int> targets;
    if (shift) {
        targets = selectedTracksFor(track);
    } else {
        for (int i = 0; i < n; ++i) targets << i;
    }
    for (int i : targets) {
        if (now[i] == on || tracks[static_cast<std::size_t>(i)]->kind != model::TrackKind::Audio)
            continue;
        if (!run(ctl_.toggle_input_monitor(i, why), why)) return;
    }
}

void SessionModel::toggleAutolanes(int track) {
    std::string why;
    run(ctl_.toggle_autolanes(track, why), why);
}

void SessionModel::cycleGrid() {
    ctl_.set_grid(next_grid_value(ctl_.grid()));
    sync();
}

void SessionModel::cycleFadeShape() {
    ctl_.set_fade_shape(next_fade_shape(ctl_.fade_shape()));
    sync();
}

void SessionModel::cycleCrossfadeShape() {
    ctl_.set_crossfade_shape(next_fade_shape(ctl_.crossfade_shape()));
    sync();
}

void SessionModel::cycleMainCounter() {
    // Bars|Beats -> Min:Secs -> Timecode -> Samples, as the counter's
    // format menu runs; the sub counter shows Min:Secs, or Bars|Beats when
    // the main one already does.
    switch (main_fmt_) {
        case TimeFormat::BarsBeats: main_fmt_ = TimeFormat::MinSec; break;
        case TimeFormat::MinSec: main_fmt_ = TimeFormat::Timecode; break;
        case TimeFormat::Timecode: main_fmt_ = TimeFormat::Samples; break;
        case TimeFormat::Samples: main_fmt_ = TimeFormat::BarsBeats; break;
    }
    sub_fmt_ = main_fmt_ == TimeFormat::MinSec ? TimeFormat::BarsBeats : TimeFormat::MinSec;
    main_counter_.clear();
    updateCounters(true);
    emit selectionChanged();
    emit markersChanged();   // the Memory Locations window shows the main counter's format
}

bool SessionModel::enterCounter(const QString& text) {
    if (overlay_.recording) {
        return run(false, "a take is running: stop it before locating");
    }
    Frames at = 0;
    std::string why;
    if (!parse_time(main_fmt_, text.toStdString(), ctl_.snapshot().settings, at, why)) {
        return run(false, why);
    }
    ctl_.locate_to(at);
    return run(true, why);
}

void SessionModel::setWindow(const QString& w) {
    if (w == window_ || (w != QStringLiteral("edit") && w != QStringLiteral("mix"))) return;
    window_ = w;
    layouts_.setView(w);
    emit windowChanged();
}

// --- File ---

namespace {

bool has_inserts(const Snapshot& s) {
    for (const auto& t : s.edit->tracks) {
        if (!t->inserts.empty()) return true;
    }
    return false;
}

}  // namespace

bool SessionModel::saveSession(std::string& why) {
    // A New from Template whose Save As failed: the engine is still in the
    // template's folder, and a plain Save would leave its takes going there
    // (ADR-0030 §3). The first save moves the session out instead.
    if (file_.isEmpty() && inTemplateFolder()) {
        const std::string& n = ctl_.snapshot().settings.name;
        const QString base = n.empty() ? QStringLiteral("Untitled") : q(n);
        return saveSessionAs(unused_session_name(sessions_dir_, base), false, why);
    }
    return flushThenWrite(
        why, [this](std::string& w, std::string caution) { return writeSession(w, std::move(caution)); });
}

bool SessionModel::saveSessionAs(const QString& name, bool copy, std::string& why) {
    const char* what = copy ? "Save Copy In" : "Save As";
    if (overlay_.recording) {
        why = std::string("stop recording before ") + what +
              ": the take being written belongs to this session's folder";
        return false;
    }
    if (save_waiting_ || save_writing_) {
        why = std::string("a save is still in hand; ") + what + " once it is written";
        return false;
    }
    const QString wanted = name.trimmed();
    std::string bad = session_name_problem(wanted);
    if (!bad.empty()) {
        why = "cannot save as \"" + wanted.toStdString() + "\": " + bad;
        return false;
    }
    const QString root = saveAsRoot();
    if (QFileInfo::exists(QDir(root).filePath(wanted))) {
        why = "a folder named \"" + wanted.toStdString() + "\" is already in " + root.toStdString() +
              "; choose another name";
        return false;
    }
    SaveAsTarget target;
    target.file = session_file_path(root, wanted);
    target.name = wanted;
    target.copy = copy;
    target.audio = !copy || copy_with_audio_;
    return flushThenWrite(why, [this, target](std::string& w, std::string caution) {
        return writeSessionAs(target, w, std::move(caution));
    });
}

bool SessionModel::writeSessionAs(const SaveAsTarget& target, std::string& why, std::string caution) {
    model::TemplateOptions options;
    options.markers = target.markers;
    Snapshot to_write = target.as_template ? model::template_of(ctl_.snapshot(), options) : ctl_.snapshot();
    // Save As renames the session to its new folder; a copy is named after
    // its folder too, so the Open list and the file agree.
    to_write.settings.name = target.name.toStdString();
    // A template is no session: it has no id until a session is made from it.
    if (!target.as_template && to_write.settings.session_id.empty()) {
        to_write.settings.session_id = model::new_session_id();
    }
    model::WriteInfo info;
    info.written_by = "sonora-ui";
    info.save_generation = target.as_template ? 1 : save_generation_ + 1;
    std::string bytes, error;
    if (!model::serialize(to_write, info, bytes, error, &digests_)) {
        why = "the session could not be saved: " + error;
        sync();
        return false;
    }
    Written w;
    w.file = target.file;
    w.revision = doc_.revision();
    w.rename = true;
    w.settings = to_write.settings;
    w.caution = std::move(caution);
    w.generation = info.save_generation;
    w.bytes = bytes;
    const uint32_t flags = (target.copy ? api::kSaveAsCopy : 0u) | (target.audio ? 0u : api::kSaveAsNoAudio);

    save_writing_ = true;
    write_wait_.start();
    const uint64_t ticket = ++write_ticket_;
    struct Inline {
        bool inside = true;
        bool answered = false;
        bool saved = false;
        std::string why;
    };
    auto now = std::make_shared<Inline>();
    auto pending = std::make_shared<Written>(std::move(w));
    const bool copy = target.copy;
    const bool as_template = target.as_template;
    doc_.save_session_as(
        target.file.toStdString(), flags, std::move(bytes),
        [this, ticket, now, pending, copy, as_template](bool ok, const std::string& message) {
            // Answered: the engine has left the template's folder, or a
            // failed Save As left it there and commandRefusal() says so.
            if (!as_template) making_from_template_ = false;
            if (ticket != write_ticket_) return;
            save_writing_ = false;
            std::string why;
            bool saved = false;
            const QString folder = QFileInfo(pending->file).absolutePath();
            if (!ok) {
                why = as_template ? "the template was not saved: " + message
                      : std::string(copy ? "the copy was not saved: " : "the session was not saved as ") +
                            (copy ? message : QFileInfo(folder).fileName().toStdString() + ": " + message);
                // New from Template's Save As (ADR-0030 §3): the operator
                // must know the engine is still in the template's folder,
                // and why Record is refused until the session is saved.
                if (!copy && !as_template && file_.isEmpty() && inTemplateFolder()) {
                    why += ". The session is open unsaved and the engine is still in the template's "
                           "folder, so Record is refused until Save puts the session in " +
                           sessions_dir_.toStdString();
                }
                sync();
            } else if (as_template) {
                saved = true;
                emit notice(QStringLiteral("Saved the template ") + pending->file +
                            (message.empty() ? QString() : QStringLiteral(", but ") + q(message)));
            } else if (copy) {
                saved = true;
                emit notice(QStringLiteral("Saved a copy in ") + pending->file +
                            (message.empty() ? QString() : QStringLiteral(", but ") + q(message)));
            } else if (!engine_folder_.isEmpty() && QDir::cleanPath(engine_folder_) != QDir::cleanPath(folder)) {
                // Written, and the engine stayed where it was (a take started,
                // the new folder's lock): the window goes on with the old file.
                saved = true;
                emit heldNotice(QStringLiteral("Saved ") + pending->file +
                                QStringLiteral(" as a copy only: ") + q(message));
                sync();
            } else {
                saved = finishSave(true, message, std::move(*pending), why);
            }
            if (now->inside) {
                now->answered = true;
                now->saved = saved;
                now->why = why;
            } else if (!saved && !why.empty()) {
                emit heldNotice(q(why));
            }
        });
    now->inside = false;
    if (now->answered) {
        why = now->why;
        return now->saved;
    }
    emit notice(as_template ? QStringLiteral("Saving the template ") + target.file
                : copy      ? QStringLiteral("Saving a copy in ") + target.file
                            : QStringLiteral("Saving as ") + target.file);
    sync();
    return true;
}

bool SessionModel::flushThenWrite(std::string& why,
                                  std::function<bool(std::string&, std::string)> write) {
    if (save_writing_) {
        // While a take is open the engine writes after its next checkpoint,
        // and a large session takes a while on a stick: only an engine that
        // has not answered in the patience is asked again. Saves are written
        // in order, so a late answer to the first changes nothing on disk.
        if (write_wait_.elapsed() < flush_patience_ms_) {
            why = "a save is already being written by the engine";
            return false;
        }
        save_writing_ = false;
        ++write_ticket_;   // its answer, if it ever comes, is ignored
    }
    if (save_waiting_) {
        if (save_wait_.elapsed() < flush_patience_ms_) {
            why = "a save is already waiting for the plugins' state from the engine";
            return false;
        }
        save_waiting_ = false;
        ++save_ticket_;   // its answer, if it ever comes, is ignored
        return write(why, "the engine never sent the plugins' state, so plugin "
                          "settings are as last recorded");
    }
    // No insert, no plugin state: nothing to wait for.
    if (!has_inserts(ctl_.snapshot())) return write(why, {});

    save_waiting_ = true;
    save_wait_.start();
    const uint64_t ticket = ++save_ticket_;
    // The flush may answer inside this call (no engine in between) or from
    // a later tick; the first reports through `why`, the second on its own.
    struct Inline {
        bool inside = true;
        bool answered = false;
        bool saved = false;
        std::string why;
    };
    auto now = std::make_shared<Inline>();
    doc_.flush_plugin_state(kNoId, [this, ticket, now, write](bool ok, const std::string& flush_why) {
        if (ticket != save_ticket_) return;
        save_waiting_ = false;
        std::string why;
        const bool saved = write(
            why, ok ? std::string() : "these plugins' settings are as last recorded: " + flush_why);
        if (now->inside) {
            now->answered = true;
            now->saved = saved;
            now->why = why;
        } else if (!saved && !why.empty()) {
            emit refused(q(why));
        }
    });
    now->inside = false;
    if (now->answered) {
        why = now->why;
        return now->saved;
    }
    sync();
    emit notice(QStringLiteral("Saving: collecting the plugins' state from the engine"));
    // Never left waiting in silence: once the patience is spent the
    // operator is told what a second Save will do.
    QTimer::singleShot(static_cast<int>(flush_patience_ms_), this, [this, ticket]() {
        if (!save_waiting_ || ticket != save_ticket_) return;
        emit refused(QStringLiteral(
            "Not saved yet: the engine has not sent the plugins' state. Save again to write the "
            "session without it; plugin settings would be as last recorded."));
    });
    return true;
}

bool SessionModel::writeSession(std::string& why, std::string caution) {
    QString file = file_;
    // What is written: the session as it is, or, the first time, renamed to
    // its folder's name. The rename reaches the document only once the file
    // is written, so a failed save leaves the name and the disk as they were
    // and a retry is not pushed to "<Name> 2".
    Snapshot to_write = ctl_.snapshot();
    bool rename = false;
    // Beside its takes (ADR-0018 §4), in the folder the engine records this
    // session into, unless another session's file is already there: then
    // the takes stay where they are and the file goes where it went before.
    // Where a new session's folder goes: beside the engine's untitled one,
    // which the engine renames there (ADR-0018 §3), so on its filesystem.
    QString root = sessions_dir_;
    if (file.isEmpty() && !engine_folder_.isEmpty()) {
        const QDir folder(engine_folder_);
        const QString name = QFileInfo(engine_folder_).fileName();
        const QString own = q(to_write.settings.name);
        // A name the operator gave, not the one a new session starts with.
        static const QRegularExpression untitled(QStringLiteral("^Untitled( [0-9]+)?$"));
        const bool named = engine_untitled_ && !own.isEmpty() && own != name &&
                           !untitled.match(own).hasMatch() && session_name_problem(own).empty();
        if (folder.exists() && folder.entryList({QStringLiteral("*.sonora")}, QDir::Files).isEmpty()) {
            if (named) {
                root = QFileInfo(engine_folder_).absolutePath();
            } else if (session_name_problem(name).empty()) {
                file = folder.filePath(name + QStringLiteral(".sonora"));
                if (name.toStdString() != to_write.settings.name) {
                    to_write.settings.name = name.toStdString();
                    rename = true;
                }
            }
        }
    }
    if (file.isEmpty()) {
        const std::string& n = to_write.settings.name;
        const QString wanted = n.empty() ? QStringLiteral("Untitled") : q(n);
        std::string bad = session_name_problem(wanted);
        if (!bad.empty()) {
            why = "cannot save as \"" + n + "\": " + bad;
            return false;
        }
        const QString name = unused_session_name(root, wanted);
        bad = session_name_problem(name);
        if (!bad.empty()) {
            why = "cannot save as \"" + name.toStdString() + "\", the first free name: " + bad;
            return false;
        }
        // The session is called what its folder is called, so the name the
        // window shows is the one the Open list shows.
        if (name.toStdString() != n) {
            to_write.settings.name = name.toStdString();
            rename = true;
        }
        file = session_file_path(root, name);
    }
    // A session saved before format 7 has no id: this save gives it one,
    // and the document takes it once the file is written, as with the name.
    if (to_write.settings.session_id.empty()) {
        to_write.settings.session_id = model::new_session_id();
        rename = true;
    }

    // Canonical bytes (session_file.h): the same content is the same bytes,
    // so a save that would write what the file already holds writes nothing
    // and keeps the generation, as session-format.md asks.
    model::WriteInfo info;
    info.written_by = "sonora-ui";
    info.save_generation = save_generation_;
    std::string bytes, error;
    if (!model::serialize(to_write, info, bytes, error, &digests_)) {
        why = "the session could not be saved: " + error;
        sync();
        return false;
    }
    Written w;
    w.file = file;
    w.revision = doc_.revision();
    w.rename = rename;
    w.settings = to_write.settings;
    w.caution = std::move(caution);
    if (file == file_ && bytes == saved_bytes_) {
        w.generation = save_generation_;
        w.bytes = std::move(bytes);
        return finishSave(true, {}, std::move(w), why);
    }
    info.save_generation = save_generation_ + 1;
    if (!model::serialize(to_write, info, bytes, error, &digests_)) {
        why = "the session could not be saved: " + error;
        sync();
        return false;
    }
    w.generation = info.save_generation;
    w.bytes = bytes;

    // The engine writes the sequence files, then the session file, and
    // cleans up (ADR-0015 §3, §6); it answers now (no engine in between) or
    // from a later tick, which the first reports through `why` and the
    // second on its own.
    save_writing_ = true;
    write_wait_.start();
    const uint64_t ticket = ++write_ticket_;
    struct Inline {
        bool inside = true;
        bool answered = false;
        bool saved = false;
        std::string why;
    };
    auto now = std::make_shared<Inline>();
    auto pending = std::make_shared<Written>(std::move(w));
    doc_.save_session(file.toStdString(), std::move(bytes),
                      [this, ticket, now, pending](bool ok, const std::string& message) {
                          if (ticket != write_ticket_) return;
                          std::string why;
                          const bool saved = finishSave(ok, message, std::move(*pending), why);
                          if (now->inside) {
                              now->answered = true;
                              now->saved = saved;
                              now->why = why;
                          } else if (!saved && !why.empty()) {
                              emit refused(q(why));
                          }
                      });
    now->inside = false;
    if (now->answered) {
        why = now->why;
        return now->saved;
    }
    sync();
    return true;
}

bool SessionModel::finishSave(bool ok, const std::string& message, Written w, std::string& why) {
    save_writing_ = false;
    if (!ok) {
        why = "the session could not be saved: " + message;
        sync();
        return false;
    }
    const bool nothing_new = w.file == file_ && w.bytes == saved_bytes_;
    std::string caution = std::move(w.caution);
    if (!message.empty()) caution = caution.empty() ? message : caution + "; " + message;
    save_generation_ = w.generation;
    saved_bytes_ = std::move(w.bytes);
    file_ = w.file;
    // An edit made while the engine wrote is not in the file.
    const bool edited_since = doc_.revision() != w.revision;
    // The file is on disk under the new name and id; the document follows
    // it. Only those two: any other setting changed while the engine wrote
    // stays as it is now. A refusal here would be a model bug, and the file
    // already says the name.
    std::string rename_why;
    model::SetSettings named{ctl_.snapshot().settings};
    named.settings.name = w.settings.name;
    named.settings.session_id = w.settings.session_id;
    if (w.rename && !doc_.submit(named, rename_why)) {
        caution = caution.empty() ? "the window keeps the old name: " + rename_why
                                  : caution + "; the window keeps the old name: " + rename_why;
    }
    saved_revision_ = edited_since ? w.revision : doc_.revision();
    seen_dirty_ = dirty();
    sync();
    emit sessionChanged();
    QString said = nothing_new ? QStringLiteral("Nothing to save: ") + file_ +
                                     QStringLiteral(" is current")
                               : QStringLiteral("Saved ") + file_;
    if (!caution.empty()) said += QStringLiteral(", but ") + q(caution);
    emit notice(said);
    return true;
}

bool SessionModel::openSessionFile(const QString& file, bool discard, std::string& why) {
    if (overlay_.recording) {
        why = "a take is open; stop recording before opening another session";
        return false;
    }
    if (save_waiting_) {
        why = "a save is waiting for the plugins' state; open the session once it is saved";
        return false;
    }
    if (save_writing_) {
        why = "the engine is still writing a save; open the session once it is saved";
        return false;
    }
    if (dirty() && !discard) {
        why = "this session has unsaved edits; save it first, or open with \"Discard edits\"";
        return false;
    }
    std::string bytes;
    model::Loaded loaded;
    if (!read_session_file(file, bytes, loaded, why)) return false;
    const uint32_t rate = ctl_.snapshot().settings.sample_rate;
    if (loaded.snapshot.settings.sample_rate != rate) {
        why = "the session is at " + std::to_string(loaded.snapshot.settings.sample_rate) +
              " Hz and this system runs at " + std::to_string(rate) +
              " Hz; it cannot play here until sample-rate conversion exists";
        return false;
    }
    const std::shared_ptr<const model::SourcePool> pool = loaded.snapshot.sources;
    const std::string path = QFileInfo(file).absoluteFilePath().toStdString();
    if (!doc_.reset(std::move(loaded.snapshot), path, why)) {
        emit openNotice(file + QStringLiteral(" was not opened: ") + q(why));
        return false;
    }
    ctl_.document_replaced();
    live_.clear();
    // Another session now: a New from Template's Save As still waiting is not its.
    after_open_.reset();
    making_from_template_ = false;
    before_open_.valid = true;
    before_open_.opened = file;
    before_open_.file = file_;
    before_open_.saved_revision = saved_revision_;
    before_open_.save_generation = save_generation_;
    before_open_.saved_bytes = saved_bytes_;
    before_open_.repairs = repairs_;
    emit openNotice(QString());
    file_ = file;
    // A repaired session is not the file on disk: it reads as modified, so
    // Save writes it and another Open asks first.
    saved_revision_ = loaded.repairs.empty() ? doc_.revision() : UINT64_MAX;
    save_generation_ = loaded.save_generation;
    saved_bytes_ = std::move(bytes);
    seen_dirty_ = false;
    // Everything the windows show came from the old session.
    seen_revision_ = UINT64_MAX;
    seen_sel_valid_ = seen_view_valid_ = false;
    updateCounters(true);
    sync();
    emit sessionChanged();
    emit sessionOpened();
    QVariantList repairs;
    for (const model::Repair& r : loaded.repairs) {
        QVariantMap m;
        m.insert(QStringLiteral("path"), q(r.path));
        m.insert(QStringLiteral("was"), q(r.was));
        m.insert(QStringLiteral("now"), q(r.now));
        m.insert(QStringLiteral("why"), q(r.why));
        repairs << m;
    }
    // O2: each missing sequence file shown as a repair is, held until
    // dismissed. Its regions opened silent with their windows; nothing is
    // dropped, and a save names the file again.
    for (const model::MissingSequence& ms : loaded.missing_sequences) {
        std::string where = ms.regions.front();
        if (ms.regions.size() > 1) where += " and " + std::to_string(ms.regions.size() - 1) + " more";
        std::string why_missing = ms.why;
        const model::MidiSource* take =
            ms.sources.empty() ? nullptr : pool->find_midi(ms.sources.front());
        if (take != nullptr) why_missing += "; its take " + take->path + " is kept";
        QVariantMap m;
        m.insert(QStringLiteral("path"), q(where));
        m.insert(QStringLiteral("was"), q(ms.ref.file));
        m.insert(QStringLiteral("now"), QStringLiteral("MIDI data missing: silent, not editable"));
        m.insert(QStringLiteral("why"), q(why_missing));
        repairs << m;
    }
    setRepairs(std::move(repairs));
    QString said = QStringLiteral("Opened ") + file_;
    if (!loaded.repairs.empty()) {
        said += QStringLiteral(", with %1 value%2 repaired to open (listed below); save to keep them")
                    .arg(loaded.repairs.size())
                    .arg(loaded.repairs.size() == 1 ? QString() : QStringLiteral("s"));
    }
    if (!loaded.missing_sequences.empty()) {
        said += QStringLiteral(", with %1 MIDI sequence file%2 missing (listed below)")
                    .arg(loaded.missing_sequences.size())
                    .arg(loaded.missing_sequences.size() == 1 ? QString() : QStringLiteral("s"));
    }
    emit notice(said);
    // Offered, never applied silently (session-format.md, "Autosave"), and
    // held: the engine wrote it after the last save, so it may hold the edits
    // a crash took.
    QString autosave;
    if (newer_autosave(file_, save_generation_, autosave)) {
        emit openNotice(QStringLiteral("An autosave newer than this file is in ") + autosave +
                        QStringLiteral(": open it to get back the edits made after the last save"));
    }
    return true;
}

void SessionModel::setRepairs(QVariantList r) {
    if (r.isEmpty() && repairs_.isEmpty()) return;
    repairs_ = std::move(r);
    emit repairsChanged();
}

void SessionModel::dismissRepair(int index) {
    if (index < 0 || index >= repairs_.size()) return;
    repairs_.removeAt(index);
    emit repairsChanged();
}

void SessionModel::openWasRefused(const QString& why, bool later_load) {
    // A New from Template's Save As is for the session the engine refused,
    // unless a later Load, which may be the template's, is still out.
    if (!later_load) {
        after_open_.reset();
        making_from_template_ = false;
    }
    if (later_load) {
        // What is shown is the later Open's session, and the window names
        // it. What that Open remembered is the refused session, so its own
        // refusal must clear rather than give this one back.
        before_open_ = BeforeOpen{};
        emit openNotice(QStringLiteral("A session was not opened: ") + why);
        return;
    }
    if (!before_open_.valid) {
        const QString refused = file_;
        openWasLost();
        emit openNotice((refused.isEmpty() ? QStringLiteral("A session") : refused) +
                        QStringLiteral(" was not opened: ") + why);
        return;
    }
    BeforeOpen b = std::move(before_open_);
    before_open_ = BeforeOpen{};
    ctl_.document_replaced();
    live_.clear();
    file_ = b.file;
    saved_revision_ = b.saved_revision;
    save_generation_ = b.save_generation;
    saved_bytes_ = std::move(b.saved_bytes);
    seen_dirty_ = false;
    seen_revision_ = UINT64_MAX;
    seen_sel_valid_ = seen_view_valid_ = false;
    updateCounters(true);
    setRepairs(std::move(b.repairs));
    sync();
    emit sessionChanged();
    emit openNotice(b.opened + QStringLiteral(" was not opened: ") + why + QStringLiteral("; ") +
                    (file_.isEmpty() ? QStringLiteral("the session before it")
                                     : file_) +
                    QStringLiteral(" is shown"));
}

void SessionModel::openWasLost() {
    after_open_.reset();
    making_from_template_ = false;
    before_open_ = BeforeOpen{};
    const QString lost = file_;
    file_.clear();
    saved_bytes_.clear();
    save_generation_ = 0;
    // Nothing on disk holds what is shown, so it reads as unsaved.
    saved_revision_ = UINT64_MAX;
    // What is shown is the engine's document, not the repaired file.
    setRepairs({});
    seen_revision_ = UINT64_MAX;
    sync();
    emit sessionChanged();
    if (!lost.isEmpty()) {
        emit openNotice(lost + QStringLiteral(" was not opened: the engine was unreachable. "
                                              "Open it again."));
    }
}

QVariantList SessionModel::sessionList() const {
    QVariantList out;
    for (const SessionEntry& e : list_sessions(sessions_dir_)) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), e.name);
        m.insert(QStringLiteral("file"), e.file);
        m.insert(QStringLiteral("current"), e.file == file_);
        out << m;
    }
    return out;
}

bool SessionModel::openSession(const QString& file, bool discard) {
    std::string why;
    return run(openSessionFile(file, discard, why), why);
}

QVariantList SessionModel::unsavedRecordingList() const {
    QVariantList out;
    for (const RecordingEntry& e : list_unsaved_recordings(sessions_dir_)) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), e.name);
        m.insert(QStringLiteral("folder"), e.folder);
        m.insert(QStringLiteral("current"), e.folder == engine_folder_);
        out << m;
    }
    return out;
}

bool SessionModel::openRecording(const QString& folder, bool discard) {
    std::string why;
    return run(openRecordingFolder(folder, discard, why), why);
}

bool SessionModel::openRecordingFolder(const QString& folder, bool discard, std::string& why) {
    if (overlay_.recording) {
        why = "a take is open; stop recording before opening another session";
        return false;
    }
    if (save_waiting_ || save_writing_) {
        why = "a save is still being written; open the recording once it is saved";
        return false;
    }
    if (dirty() && !discard) {
        why = "this session has unsaved edits; save it first, or open with \"Discard edits\"";
        return false;
    }
    const Snapshot& now = ctl_.snapshot();
    // The engine records into this session's tracks, so the empty session
    // has as many; its takes are offered for placing once it is open.
    Snapshot empty = make_empty_session(static_cast<uint32_t>(now.edit->tracks.size()),
                                        now.settings.sample_rate);
    const QString name = QFileInfo(folder).fileName();
    empty.settings.name = name.toStdString();
    empty.settings.session_id = model::new_session_id();
    const std::string path = QFileInfo(folder).absoluteFilePath().toStdString();
    if (!doc_.reset(std::move(empty), path, why)) {
        emit openNotice(folder + QStringLiteral(" was not opened: ") + q(why));
        return false;
    }
    ctl_.document_replaced();
    live_.clear();
    // Another session now: a New from Template's Save As still waiting is not its.
    after_open_.reset();
    making_from_template_ = false;
    before_open_.valid = true;
    before_open_.opened = folder;
    before_open_.file = file_;
    before_open_.saved_revision = saved_revision_;
    before_open_.save_generation = save_generation_;
    before_open_.saved_bytes = saved_bytes_;
    before_open_.repairs = repairs_;
    emit openNotice(QString());
    file_.clear();
    saved_bytes_.clear();
    save_generation_ = 0;
    saved_revision_ = doc_.revision();
    seen_dirty_ = false;
    seen_revision_ = UINT64_MAX;
    seen_sel_valid_ = seen_view_valid_ = false;
    setRepairs({});
    updateCounters(true);
    sync();
    emit sessionChanged();
    // The recording is another session: the windows' views of the old one
    // go back to their defaults, as they do for a session file.
    emit sessionOpened();
    emit notice(QStringLiteral("Opened the unsaved recording ") + name +
                QStringLiteral("; place its takes, then save to name it"));
    return true;
}

// --- Session templates (ADR-0030) ---

QString SessionModel::saveAsRoot() const {
    // Beside the folder the session is in, on its filesystem, so Save As can
    // link the audio rather than copy it (ADR-0018 §5).
    if (engine_folder_.isEmpty() || inTemplateFolder()) return sessions_dir_;
    return QFileInfo(engine_folder_).absolutePath();
}

bool SessionModel::inTemplateFolder() const {
    if (engine_folder_.isEmpty() || templates_dir_.isEmpty()) return false;
    const QString root = QDir::cleanPath(QFileInfo(templates_dir_).absoluteFilePath());
    const QString folder = QDir::cleanPath(QFileInfo(engine_folder_).absoluteFilePath());
    return folder.startsWith(root + QLatin1Char('/'));
}

bool SessionModel::saveSessionAsTemplate(const QString& name, bool markers, std::string& why) {
    if (overlay_.recording) {
        why = "stop recording before Save as Template: the take being written belongs to this "
              "session's folder";
        return false;
    }
    if (save_waiting_ || save_writing_) {
        why = "a save is still in hand; Save as Template once it is written";
        return false;
    }
    const QString wanted = name.trimmed();
    const std::string bad = session_name_problem(wanted);
    if (!bad.empty()) {
        why = "cannot save the template \"" + wanted.toStdString() + "\": " + bad;
        return false;
    }
    if (QFileInfo::exists(QDir(templates_dir_).filePath(wanted))) {
        why = "a template named \"" + wanted.toStdString() + "\" is already in " +
              templates_dir_.toStdString() + "; choose another name";
        return false;
    }
    SaveAsTarget target;
    target.file = session_file_path(templates_dir_, wanted);
    target.name = wanted;
    target.copy = true;
    target.audio = false;
    target.as_template = true;
    target.markers = markers;
    return flushThenWrite(why, [this, target](std::string& w, std::string caution) {
        return writeSessionAs(target, w, std::move(caution));
    });
}

bool SessionModel::newSessionFromTemplate(const QString& file, const QString& name, bool discard,
                                          std::string& why) {
    if (overlay_.recording) {
        why = "a take is open; stop recording before starting a session from a template";
        return false;
    }
    if (save_waiting_ || save_writing_) {
        why = "a save is still being written; start the new session once it is saved";
        return false;
    }
    if (dirty() && !discard) {
        why = "this session has unsaved edits; save it first, or start the new one with \"Discard "
              "edits\"";
        return false;
    }
    const QString wanted = name.trimmed();
    const std::string bad = session_name_problem(wanted);
    if (!bad.empty()) {
        why = "cannot name the session \"" + wanted.toStdString() + "\": " + bad;
        return false;
    }
    // Checked before the Load, so the Save As after it is refused only by
    // the medium.
    if (QFileInfo::exists(QDir(sessions_dir_).filePath(wanted))) {
        why = "a folder named \"" + wanted.toStdString() + "\" is already in " +
              sessions_dir_.toStdString() + "; choose another name";
        return false;
    }
    std::string bytes;
    model::Loaded loaded;
    if (!read_session_file(file, bytes, loaded, why)) {
        why = "the template " + file.toStdString() + " could not be read: " + why;
        return false;
    }
    const uint32_t rate = ctl_.snapshot().settings.sample_rate;
    if (loaded.snapshot.settings.sample_rate != rate) {
        why = "the template is at " + std::to_string(loaded.snapshot.settings.sample_rate) +
              " Hz and this system runs at " + std::to_string(rate) +
              " Hz; it cannot play here until sample-rate conversion exists";
        return false;
    }
    Snapshot made =
        model::session_from_template(loaded.snapshot, wanted.toStdString(), model::new_session_id());
    // The template's path: the engine enters its folder, so each plugin
    // starts from the state the template keeps there (ADR-0030 §3).
    const std::string path = QFileInfo(file).absoluteFilePath().toStdString();
    if (!doc_.reset(std::move(made), path, why)) {
        emit openNotice(file + QStringLiteral(" was not opened: ") + q(why));
        return false;
    }
    ctl_.document_replaced();
    live_.clear();
    // Another session now: a New from Template's Save As still waiting is not its.
    after_open_.reset();
    making_from_template_ = false;
    before_open_.valid = true;
    before_open_.opened = file;
    before_open_.file = file_;
    before_open_.saved_revision = saved_revision_;
    before_open_.save_generation = save_generation_;
    before_open_.saved_bytes = saved_bytes_;
    before_open_.repairs = repairs_;
    emit openNotice(QString());
    // Nothing on disk holds it yet, so it reads as unsaved until the Save As.
    file_.clear();
    saved_bytes_.clear();
    save_generation_ = 0;
    saved_revision_ = UINT64_MAX;
    seen_dirty_ = false;
    seen_revision_ = UINT64_MAX;
    seen_sel_valid_ = seen_view_valid_ = false;
    setRepairs({});
    updateCounters(true);
    sync();
    emit sessionChanged();
    emit sessionOpened();

    SaveAsTarget target;
    target.file = session_file_path(sessions_dir_, wanted);
    target.name = wanted;
    after_open_ = target;
    making_from_template_ = true;
    emit notice(QStringLiteral("Making the session ") + wanted + QStringLiteral(" from the template ") +
                QFileInfo(file).dir().dirName());
    // A document held here has taken the Load already.
    if (!doc_.opening()) saveAfterOpen();
    return true;
}

void SessionModel::saveAfterOpen() {
    if (!after_open_ || doc_.opening()) return;
    const SaveAsTarget target = *after_open_;
    after_open_.reset();
    std::string why;
    if (!writeSessionAs(target, why, std::string())) {
        making_from_template_ = false;
        emit heldNotice(QStringLiteral("The session ") + target.name +
                        QStringLiteral(" was not made from the template: ") + q(why) +
                        QStringLiteral(". It is open unsaved; its first save puts it in ") + sessions_dir_);
    }
}

std::string SessionModel::commandRefusal(const std::string& command) const {
    // Play is not refused: it is a take only while record-ready, and a Load
    // clears record-ready at the engine, while record_ready is refused here.
    if (!is_take_command(command)) return {};
    if (making_from_template_) {
        return "the new session is still being made from its template; record once it is saved "
               "in the sessions folder";
    }
    if (file_.isEmpty() && inTemplateFolder()) {
        return "the engine is still in the template's folder, and a take there would stay in the "
               "template; save the session first, which puts it in " +
               sessions_dir_.toStdString();
    }
    return {};
}

namespace {
std::vector<SessionEntry> list_templates(const QString& dir);
}  // namespace

QVariantList SessionModel::templateList() const {
    QVariantList out;
    const uint32_t rate = ctl_.snapshot().settings.sample_rate;
    // Never a link: Rename and Delete must not reach past the folder.
    for (const SessionEntry& e : list_templates(templates_dir_)) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), e.name);
        m.insert(QStringLiteral("file"), e.file);
        m.insert(QStringLiteral("date"),
                 QFileInfo(e.file).lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        // Read as New from Template reads it, so what the browser offers is
        // what the click does: one it would refuse says why here instead.
        std::string bytes, why;
        model::Loaded loaded;
        int tracks = -1;
        QString problem;
        if (!read_session_file(e.file, bytes, loaded, why)) {
            problem = QStringLiteral("cannot be read: ") + q(why);
        } else if (loaded.snapshot.settings.sample_rate != rate) {
            problem = QStringLiteral("made at %1 Hz; this system runs at %2 Hz")
                          .arg(loaded.snapshot.settings.sample_rate)
                          .arg(rate);
        } else {
            tracks = static_cast<int>(loaded.snapshot.edit->tracks.size());
        }
        m.insert(QStringLiteral("tracks"), tracks);
        m.insert(QStringLiteral("problem"), problem);
        out << m;
    }
    return out;
}

QString SessionModel::unusedSessionName(const QString& base) const {
    const QString b = base.trimmed();
    return unused_session_name(sessions_dir_, b.isEmpty() ? QStringLiteral("Untitled") : b);
}

namespace {

// A link of any kind: a symlink, or on Windows a junction, either of which
// would take a rename or a delete to a folder that is not the template.
bool is_link(const QFileInfo& i) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    if (i.isJunction()) return true;
#endif
    return i.isSymLink();
}

// The canonical path, or the cleaned absolute one for a path not there.
QString canonical(const QString& path) {
    const QFileInfo i(path);
    const QString c = i.canonicalFilePath();
    return c.isEmpty() ? QDir::cleanPath(i.absoluteFilePath()) : c;
}

// Removes `path` without following a link: a link inside is removed as a
// link, never walked into. False when anything is left.
bool remove_without_following(const QString& path) {
    bool ok = true;
    const QDir d(path);
    for (const QFileInfo& i :
         d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        const QString p = i.absoluteFilePath();
        if (is_link(i)) {
            // A link to a directory is removed as a directory entry on Windows.
            ok = (QFile::remove(p) || QDir().rmdir(p)) && ok;
        } else if (i.isDir()) {
            ok = remove_without_following(p) && ok;
        } else {
            ok = QFile::remove(p) && ok;
        }
    }
    return QDir().rmdir(path) && ok;
}

// Any audio in `folder`, links not followed: a template has none, so a
// folder that has some is a session in the wrong place.
bool holds_audio(const QString& folder) {
    if (QFileInfo::exists(QDir(folder).filePath(QStringLiteral("Audio Files")))) return true;
    QDirIterator it(folder, {QStringLiteral("*.wav"), QStringLiteral("*.bwf"), QStringLiteral("*.flac")},
                    QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    return it.hasNext();
}

// The templates of `dir` the browser lists: as list_sessions(), without a
// folder or a session file that is a link.
std::vector<SessionEntry> list_templates(const QString& dir) {
    std::vector<SessionEntry> out;
    for (const SessionEntry& e : list_sessions(dir)) {
        if (is_link(QFileInfo(QDir(dir).filePath(e.name))) || is_link(QFileInfo(e.file))) continue;
        out.push_back(e);
    }
    return out;
}

}  // namespace

bool SessionModel::templateFolderSafe(const QString& name, std::string& why) const {
    bool listed = !templates_dir_.isEmpty() && session_name_problem(name).empty();
    if (listed) {
        listed = false;
        for (const SessionEntry& e : list_templates(templates_dir_)) {
            if (e.name == name) listed = true;
        }
    }
    if (!listed) {
        why = "there is no template named \"" + name.toStdString() + "\" in " + templates_dir_.toStdString();
        return false;
    }
    const QString root = canonical(templates_dir_);
    if (!sessions_dir_.isEmpty() && root == canonical(sessions_dir_)) {
        why = "the templates folder " + templates_dir_.toStdString() +
              " is the sessions folder, so its folders are sessions; nothing is renamed or deleted there";
        return false;
    }
    const QString folder = QDir(templates_dir_).filePath(name);
    if (is_link(QFileInfo(folder)) || is_link(QFileInfo(session_file_path(templates_dir_, name))) ||
        QFileInfo(canonical(folder)).absolutePath() != root) {
        why = "the template \"" + name.toStdString() + "\" is a link to a folder elsewhere; it is left alone";
        return false;
    }
    return true;
}

bool SessionModel::templateIsFree(const QString& name, std::string& why) const {
    if (making_from_template_ || after_open_) {
        why = "a session is being made from a template; rename or delete once it is saved";
        return false;
    }
    if (save_waiting_ || save_writing_) {
        why = "a save is still in hand; rename or delete the template once it is written";
        return false;
    }
    if (engine_folder_.isEmpty()) return true;
    const QString folder = canonical(QDir(templates_dir_).filePath(name));
    const QString engine = canonical(engine_folder_);
    if (engine == folder || engine.startsWith(folder + QLatin1Char('/'))) {
        why = "the engine is in this template's folder; save the session first, which moves it to " +
              sessions_dir_.toStdString();
        return false;
    }
    return true;
}

bool SessionModel::renameSessionTemplate(const QString& from, const QString& to, std::string& why) {
    if (!templateFolderSafe(from, why)) return false;
    const QString wanted = to.trimmed();
    if (wanted == from) return true;
    const std::string bad = session_name_problem(wanted);
    if (!bad.empty()) {
        why = "cannot rename the template to \"" + wanted.toStdString() + "\": " + bad;
        return false;
    }
    QDir root(templates_dir_);
    if (QFileInfo::exists(root.filePath(wanted))) {
        why = "a template named \"" + wanted.toStdString() + "\" is already in " +
              templates_dir_.toStdString() + "; choose another name";
        return false;
    }
    if (!templateIsFree(from, why)) return false;
    // The file inside the folder first, then the folder: a failure at the
    // second step puts the file back, so the template stays listed.
    const QString folder = root.filePath(from);
    const QString old_file = session_file_path(templates_dir_, from);
    const QString moved_file = QDir(folder).filePath(wanted + QStringLiteral(".sonora"));
    if (!QFile::rename(old_file, moved_file)) {
        why = "could not rename " + old_file.toStdString();
        return false;
    }
    if (!root.rename(from, wanted)) {
        QFile::rename(moved_file, old_file);
        why = "could not rename the folder " + folder.toStdString();
        return false;
    }
    emit notice(QStringLiteral("Renamed the template ") + from + QStringLiteral(" to ") + wanted);
    return true;
}

bool SessionModel::deleteSessionTemplate(const QString& name, std::string& why) {
    if (!templateFolderSafe(name, why)) return false;
    const QString folder = QDir(templates_dir_).filePath(name);
    if (holds_audio(folder)) {
        why = "\"" + name.toStdString() + "\" holds audio, which a template never has: it is a session, "
              "not deleted from here";
        return false;
    }
    if (!templateIsFree(name, why)) return false;
    if (!remove_without_following(folder)) {
        why = "could not delete all of " + folder.toStdString() + "; what is left stays there";
        return false;
    }
    emit notice(QStringLiteral("Deleted the template ") + name);
    return true;
}

bool SessionModel::renameTemplate(const QString& from, const QString& to) {
    std::string why;
    return run(renameSessionTemplate(from, to, why), why);
}

bool SessionModel::deleteTemplate(const QString& name) {
    std::string why;
    return run(deleteSessionTemplate(name, why), why);
}

bool SessionModel::saveAsTemplate(const QString& name, bool markers) {
    std::string why;
    return run(saveSessionAsTemplate(name, markers, why), why);
}

bool SessionModel::newFromTemplate(const QString& file, const QString& name, bool discard) {
    std::string why;
    return run(newSessionFromTemplate(file, name, discard, why), why);
}

// --- Mix window ---

qreal SessionModel::faderPosition(qreal db) const {
    return fader_position_from_db(static_cast<float>(db));
}

QString SessionModel::gainText(qreal db) const { return q(sonora::ui::gain_text(static_cast<float>(db))); }

QString SessionModel::panText(qreal pan) const { return q(sonora::ui::pan_text(static_cast<float>(pan))); }

void SessionModel::faderDrag(int track, qreal dy, qreal travel, bool fine) {
    const float now = stripGain(track);
    setGain(track, fader_drag(now, static_cast<float>(dy), static_cast<float>(travel), fine));
}

float SessionModel::stripGain(int track) const {
    const model::Snapshot& s = ctl_.snapshot();
    if (track < 0) return s.mix.bus.gain_db;
    const model::TrackMix* m = strip_mix(s, track);
    return m != nullptr ? m->gain_db : 0.0f;
}

model::Id SessionModel::trackId(int track) const {
    const auto& tracks = ctl_.snapshot().edit->tracks;
    if (track < 0 || track >= static_cast<int>(tracks.size())) return kNoId;
    return tracks[static_cast<std::size_t>(track)]->id;
}

int SessionModel::trackIndex(model::Id id) const {
    const auto& tracks = ctl_.snapshot().edit->tracks;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i]->id == id) return static_cast<int>(i);
    }
    return -1;
}

float SessionModel::stripPan(int track) const {
    const model::TrackMix* m = strip_mix(ctl_.snapshot(), track);
    return m != nullptr ? m->pan : 0.0f;
}

void SessionModel::resetGain(int track) { setGain(track, 0.0); }

void SessionModel::setGain(int track, qreal db) {
    std::string why;
    run(track < 0 ? ctl_.set_bus_gain(static_cast<float>(db), why)
                  : ctl_.set_gain(track, static_cast<float>(db), why),
        why);
}

void SessionModel::panDrag(int track, qreal dx, qreal width) {
    const model::TrackMix* m = strip_mix(ctl_.snapshot(), track);
    if (m == nullptr || width <= 0) return;
    const float now = m->pan;
    std::string why;
    run(ctl_.set_pan(track, pan_drag(now, static_cast<float>(dx), static_cast<float>(width)), why),
        why);
}

void SessionModel::resetPan(int track) {
    std::string why;
    run(ctl_.set_pan(track, 0.0f, why), why);
}

// --- scene ---

void SessionModel::buildTimeline(Scene& out) const {
    build_timeline_scene(ctl_.snapshot(), ctl_.view(), ctl_.selection(), ctl_.drag(),
                         ctl_.grid_lines(), ctl_.grid(), peaks_, out);
}

void SessionModel::buildOverlay(std::vector<SceneRect>& out) const {
    build_timeline_overlay(ctl_.view(), ctl_.selection(), overlay_, out, &live_);
}

void SessionModel::buildRuler(TimeFormat fmt, double height, Scene& out) const {
    build_ruler_scene(fmt, ctl_.snapshot().settings, ctl_.view(), height, out);
}

void SessionModel::buildMarkerRuler(double height, Scene& out) const {
    build_marker_ruler_scene(ctl_.markers(), ctl_.view(), height, out);
}

void SessionModel::buildConductorRuler(ConductorRuler kind, double height, Scene& out) const {
    build_conductor_ruler_scene(kind, ctl_.snapshot().settings, ctl_.view(), height, out);
}

}  // namespace sonora::ui
