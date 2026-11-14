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
        const MarkerView* view = marker_views_.find(settings.session_id, m.id);
        row[QStringLiteral("hasView")] = view != nullptr;
        row[QStringLiteral("recallZoom")] = view != nullptr && view->recall_zoom;
        row[QStringLiteral("recallTracks")] = view != nullptr && view->recall_tracks;
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

void SessionModel::setEventListOpen(bool on) {
    if (on == event_list_open_) return;
    event_list_open_ = on;
    emit eventListOpenChanged();
}

QString SessionModel::eventListTitle() const {
    const int t = ctl_.selection().track;
    if (t < 0 || t >= static_cast<int>(ctl_.snapshot().edit->tracks.size())) {
        return QStringLiteral("No track selected");
    }
    return q(ctl_.snapshot().edit->tracks[static_cast<std::size_t>(t)]->name) + QStringLiteral(" - ") +
           QString::fromLatin1(lane_name(lane_param(ctl_.event_lane())));
}

QVariantList SessionModel::eventList() const {
    QVariantList rows;
    const model::AutomationLane* l = ctl_.event_points();
    if (l == nullptr) return rows;
    const auto& settings = ctl_.snapshot().settings;
    const model::AutomationParam p = l->param;
    for (std::size_t i = 0; i < l->points.size(); ++i) {
        const model::AutomationPoint& pt = l->points[i];
        QVariantMap row;
        row[QStringLiteral("number")] = static_cast<int>(i) + 1;
        row[QStringLiteral("time")] = q(format_time(main_fmt_, pt.at, settings));
        row[QStringLiteral("value")] = q(lane_value_text(p, pt.value));
        // The second of a step's two points at one frame.
        row[QStringLiteral("step")] = i > 0 && l->points[i - 1].at == pt.at;
        rows.push_back(row);
    }
    return rows;
}

void SessionModel::setEventLane(int lane) {
    ctl_.set_event_lane(lane);
    sync();
}

bool SessionModel::eventEdit(int row, const QString& time, const QString& value) {
    const model::AutomationLane* l = ctl_.event_points();
    if (l == nullptr || row < 0 || static_cast<std::size_t>(row) >= l->points.size()) {
        return run(false, "that event is no longer in the lane");
    }
    const model::AutomationPoint& was = l->points[static_cast<std::size_t>(row)];
    model::Frames at = was.at;
    float v = was.value;
    std::string why;
    if (!time.trimmed().isEmpty() &&
        !parse_time(main_fmt_, time.trimmed().toStdString(), ctl_.snapshot().settings, at, why)) {
        return run(false, why);
    }
    if (!value.trimmed().isEmpty()) {
        // The spelling the strip's own readout takes (fader_taper.h); Volume
        // Trim is in dB too.
        const bool ok = l->param == model::AutomationParam::Pan
                            ? parse_pan_text(value.trimmed().toStdString(), v, why)
                            : parse_gain_text(value.trimmed().toStdString(), v, why);
        if (!ok) return run(false, why);
    }
    return run(ctl_.event_edit(static_cast<std::size_t>(row), at, v, why), why);
}

bool SessionModel::eventDelete(int row) {
    std::string why;
    return run(ctl_.event_delete(static_cast<std::size_t>(std::max(row, 0)), why), why);
}

void SessionModel::setLatencyOpen(bool on) {
    if (on == latency_open_) return;
    latency_open_ = on;
    layouts_.setOpen(QStringLiteral("latency"), on);
    emit latencyOpenChanged();
}

void SessionModel::setStepSequencerOpen(bool on) {
    if (on == step_sequencer_open_) return;
    step_sequencer_open_ = on;
    emit stepSequencerOpenChanged();
}

void SessionModel::setBounceOpen(bool on) {
    if (on == bounce_open_) return;
    bounce_open_ = on;
    emit bounceOpenChanged();
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

void SessionModel::setMarkerViewsFile(const QString& path) {
    marker_views_file_ = path;
    QString why;
    if (!path.isEmpty() && !marker_views_.load(path, why)) {
        emit heldNotice(QStringLiteral("Marker views not loaded: ") + why);
    }
    emit markersChanged();
}

void SessionModel::setMarkerTrackHider(EditController::TrackHider hide) {
    ctl_.set_marker_views(&marker_views_, std::move(hide));
}

bool SessionModel::markerViewsEdit(int row, const std::function<bool(model::Id, std::string&)>& act) {
    const auto& list = ctl_.markers().markers;
    if (row < 0 || static_cast<std::size_t>(row) >= list.size()) {
        return run(false, "that marker is no longer in the session");
    }
    std::string why;
    if (!act(list[static_cast<std::size_t>(row)].id, why)) {
        // A refusal the operator must see stays up until the next try.
        emit heldNotice(q(why));
        return run(false, why);
    }
    QString reason;
    if (!marker_views_file_.isEmpty() && !marker_views_.save(marker_views_file_, reason)) {
        emit heldNotice(QStringLiteral("The marker view is kept for now but could not be saved: ") + reason);
    }
    emit markersChanged();
    return run(true, why);
}

bool SessionModel::storeMarkerView(int row) {
    return markerViewsEdit(row, [this](model::Id id, std::string& why) { return ctl_.store_marker_view(id, why); });
}

bool SessionModel::clearMarkerView(int row) {
    return markerViewsEdit(row, [this](model::Id id, std::string& why) { return ctl_.clear_marker_view(id, why); });
}

bool SessionModel::setMarkerViewPart(int row, bool zoom, bool on) {
    return markerViewsEdit(row, [this, zoom, on](model::Id id, std::string& why) {
        return ctl_.set_marker_view_part(id, zoom, on, why);
    });
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

bool SessionModel::selectedTrackFrozen() const {
    const int i = ctl_.selection().track;
    const auto& tracks = ctl_.snapshot().edit->tracks;
    return i >= 0 && i < static_cast<int>(tracks.size()) &&
           model::frozen_playlist(*tracks[static_cast<std::size_t>(i)]) != nullptr;
}

bool SessionModel::rangeOnSelectedTrack() const {
    const Selection& sel = ctl_.selection();
    return sel.track >= 0 && sel.has_range() && sel.covers(sel.track);
}

QVariantList SessionModel::selectedTrackInserts() const {
    QVariantList out;
    const int i = ctl_.selection().track;
    const model::Snapshot& s = ctl_.snapshot();
    if (i < 0 || i >= static_cast<int>(s.edit->tracks.size())) return out;
    const model::TrackEdit& t = *s.edit->tracks[static_cast<std::size_t>(i)];
    std::vector<const model::Insert*> ordered;
    for (const model::Insert& in : t.inserts) ordered.push_back(&in);
    std::sort(ordered.begin(), ordered.end(),
              [](const model::Insert* a, const model::Insert* b) { return a->slot < b->slot; });
    std::string why;
    // The model's own check, so the menu never offers what the engine refuses.
    for (const model::Insert* in : ordered) {
        if (!model::render_insert_valid(s, t.id, in->id, why)) continue;
        QVariantMap row;
        row.insert(QStringLiteral("id"), QString::number(in->id));
        row.insert(QStringLiteral("name"), q(in->plugin.name));
        out.push_back(row);
    }
    return out;
}

// Track > Freeze, Commit, Unfreeze and Process Selection (ADR-0051): the
// engine prints the track through its inserts and puts the file in the
// document, so this only names the track and, for Process, the range and
// the one insert it may go through (ADR-0087).
bool SessionModel::renderInPlace(const QString& what, const QString& insert) {
    const int i = ctl_.selection().track;
    const auto& tracks = ctl_.snapshot().edit->tracks;
    if (i < 0 || i >= static_cast<int>(tracks.size())) return run(false, "select a track first");
    if (!send_command_) return run(false, "no engine to render with");
    const model::TrackEdit& t = *tracks[static_cast<std::size_t>(i)];
    api::RenderOptions o;
    o.track = t.id;
    QString command, verb;
    if (what == QStringLiteral("freeze")) {
        command = QStringLiteral("track.freeze");
        verb = QStringLiteral("Freezing ");
    } else if (what == QStringLiteral("commit")) {
        command = QStringLiteral("track.commit");
        verb = QStringLiteral("Committing ");
    } else if (what == QStringLiteral("unfreeze")) {
        command = QStringLiteral("track.unfreeze");
    } else if (what == QStringLiteral("process")) {
        const Selection& sel = ctl_.selection();
        if (!sel.has_range() || !sel.covers(i)) {
            return run(false, "select a range on " + t.name + " to process");
        }
        o.start = static_cast<uint64_t>(sel.start);
        o.end = static_cast<uint64_t>(sel.end);
        command = QStringLiteral("track.process_selection");
        verb = QStringLiteral("Processing ");
    } else {
        return run(false, "unknown render \"" + what.toStdString() + "\"");
    }
    if (!insert.isEmpty()) {
        bool ok = false;
        o.only_insert = insert.toULongLong(&ok);
        if (!ok || o.only_insert == 0 || command != QLatin1String(api::kCommandProcessSelection)) {
            return run(false, "only Process Selection runs through one plug-in");
        }
        std::string why;
        if (!model::render_insert_valid(ctl_.snapshot(), t.id, o.only_insert, why)) {
            return run(false, why);
        }
    }
    render_what_ = verb.isEmpty() ? QString() : verb + q(t.name);
    emit renderWhatChanged();
    send_command_(command, q(api::format_render_argument(o)));
    return true;
}

bool SessionModel::setPreRollText(const QString& text) { return setRollText(true, text); }
bool SessionModel::setPostRollText(const QString& text) { return setRollText(false, text); }

}  // namespace sonora::ui
