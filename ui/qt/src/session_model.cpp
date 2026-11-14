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

namespace {

// The counters move with the playhead; re-publishing their text every
// fourth tick (15 Hz) is the shell's state rate and faster than anyone
// reads a counter.
constexpr unsigned kCounterEvery = 4;

bool same(const Selection& a, const Selection& b) {
    return a.track == b.track && a.tracks == b.tracks && a.region == b.region &&
           a.insertion == b.insertion &&
           a.start == b.start && a.end == b.end;
}

bool same(const ViewState& a, const ViewState& b) {
    return a.fpp == b.fpp && a.scroll == b.scroll && a.scroll_y == b.scroll_y &&
           a.width == b.width && a.height == b.height && a.track_height == b.track_height &&
           a.wave_scale == b.wave_scale && a.open == b.open && a.tops == b.tops &&
           a.lanes == b.lanes && a.comp_lane_height == b.comp_lane_height;
}

bool same(const Drag& a, const Drag& b) {
    // A Pencil stroke grows with every move while the rest stays put: its
    // preview is redrawn as it grows (ADR-0096).
    return a.kind == b.kind && a.moved == b.moved && a.start == b.start && a.end == b.end &&
           a.region == b.region && a.track_end == b.track_end && a.lane == b.lane &&
           a.pencil.size() == b.pencil.size();
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
    ctl_.set_marker_views(&marker_views_, {});
    updateCounters(true);
    // The list is a view over this session's work; the next one starts
    // without it.
    connect(this, &SessionModel::sessionOpened, this, [this]() { setWindowConfigsOpen(false); });
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
        emit eventListChanged();
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
    const int follows = (ctl_.automation_follows_edit() ? 1 : 0) | (ctl_.quick_swipe() ? 2 : 0);
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
        emit eventListChanged();
        updateCounters(true);
    }
    if (ctl_.event_lane() != seen_event_lane_) {
        seen_event_lane_ = ctl_.event_lane();
        emit eventListChanged();
    }
    if (!seen_view_valid_ || !same(seen_view_, ctl_.view())) {
        seen_view_ = ctl_.view();
        seen_view_valid_ = true;
        content = overlay = true;
        emit viewChanged();
        emit rowsChanged();
    }
    {
        const auto lane = ctl_.lane_selection();
        if (lane.lane != seen_lane_.lane || lane.track != seen_lane_.track ||
            lane.start != seen_lane_.start || lane.end != seen_lane_.end) {
            seen_lane_ = lane;
            content = true;
            emit selectionChanged();
        }
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
    noteEngineToggles(t, alive);
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

}  // namespace sonora::ui
