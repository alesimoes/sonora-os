// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_model.h"

#include <QDir>
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
           a.wave_scale == b.wave_scale;
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
      saved_revision_(doc.revision()),
      ctl_(doc) {
    updateCounters(true);
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
    // dropped at the playhead at once, as Pro Tools does when playing.
    simple("marker.add", [this](const CommandContext& ctx, Why& why) {
        const auto at = ctl_.marker_place(overlay_.rolling, overlay_.playhead);
        if (ctx.argument.empty() && !overlay_.rolling) {
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
    simple("edit.separate_at_selection",
           [this](const CommandContext&, Why& why) { return ctl_.separate(why); });
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
                const QString root =
                    engine_folder_.isEmpty() ? sessions_dir_ : QFileInfo(engine_folder_).absolutePath();
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

    simple("window.toggle_mix_edit", [this](const CommandContext&, Why&) {
        setWindow(window_ == QStringLiteral("edit") ? QStringLiteral("mix") : QStringLiteral("edit"));
        return true;
    });
}

void SessionModel::attachItem(SceneItem* item) {
    if (std::find(items_.begin(), items_.end(), item) == items_.end()) items_.push_back(item);
}

void SessionModel::detachItem(SceneItem* item) {
    items_.erase(std::remove(items_.begin(), items_.end(), item), items_.end());
}

void SessionModel::sync() {
    const Snapshot& s = ctl_.snapshot();
    bool content = false;
    if (s.revision != seen_revision_) {
        seen_revision_ = s.revision;
        content = true;
        emit contentChanged();
        emit mixChanged();
        emit markersChanged();
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
    if (m != seen_mode_ || t != seen_tool_ || g != seen_grid_ || fades != seen_fades_ ||
        clip != seen_clip_) {
        seen_clip_ = clip;
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
        row[QStringLiteral("length")] = m.length > 0 ? q(format_min_sec(m.length, settings.sample_rate))
                                                     : QString();
        rows.push_back(row);
    }
    return rows;
}

void SessionModel::setMemoryLocationsOpen(bool on) {
    if (on == memory_locations_open_) return;
    memory_locations_open_ = on;
    emit memoryLocationsOpenChanged();
}

void SessionModel::setLatencyOpen(bool on) {
    if (on == latency_open_) return;
    latency_open_ = on;
    emit latencyOpenChanged();
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
    sync();
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
    // Beside the folder the session is in, on its filesystem, so Save As can
    // link the audio rather than copy it (ADR-0018 §5).
    const QString root = engine_folder_.isEmpty() ? sessions_dir_ : QFileInfo(engine_folder_).absolutePath();
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
    Snapshot to_write = ctl_.snapshot();
    // Save As renames the session to its new folder; a copy is named after
    // its folder too, so the Open list and the file agree.
    to_write.settings.name = target.name.toStdString();
    if (to_write.settings.session_id.empty()) to_write.settings.session_id = model::new_session_id();
    model::WriteInfo info;
    info.written_by = "sonora-ui";
    info.save_generation = save_generation_ + 1;
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
    doc_.save_session_as(
        target.file.toStdString(), flags, std::move(bytes),
        [this, ticket, now, pending, copy](bool ok, const std::string& message) {
            if (ticket != write_ticket_) return;
            save_writing_ = false;
            std::string why;
            bool saved = false;
            const QString folder = QFileInfo(pending->file).absolutePath();
            if (!ok) {
                why = std::string(copy ? "the copy was not saved: " : "the session was not saved as ") +
                      (copy ? message : QFileInfo(folder).fileName().toStdString() + ": " + message);
                sync();
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
    emit notice(copy ? QStringLiteral("Saving a copy in ") + target.file
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
    emit notice(QStringLiteral("Opened the unsaved recording ") + name +
                QStringLiteral("; place its takes, then save to name it"));
    return true;
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
    run(ctl_.set_pan(track, std::clamp(now + static_cast<float>(2.0 * dx / width), -1.0f, 1.0f), why),
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
    build_timeline_overlay(ctl_.view(), ctl_.selection(), overlay_, out);
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
