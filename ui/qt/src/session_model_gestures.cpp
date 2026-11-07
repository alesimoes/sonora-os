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

// --- Edit window ---

void SessionModel::setViewport(qreal width, qreal height) {
    ctl_.set_viewport(width, height);
    sync();
}

void SessionModel::press(qreal x, qreal y) {
    std::string why;
    const bool ok = ctl_.press(x, y, why);
    if (ctl_.drag().kind == Drag::Kind::Swipe) setTrackRefusal(QString());
    run(ok, why);
    if (ctl_.take_spot_request() != kNoId) askSpot();
}

bool SessionModel::askSpot() {
    std::string why;
    const Region* r = ctl_.selected_region(why);
    if (r == nullptr) return run(false, "select a region to spot");
    emit spotRequested(q(r->name), q(format_time(main_fmt_, r->position, ctl_.snapshot().settings)));
    return true;
}

void SessionModel::drag(qreal x, qreal y) {
    ctl_.move(x, y);
    sync();
}

void SessionModel::release(qreal x, qreal y) {
    std::string why;
    const bool swipe = ctl_.drag().kind == Drag::Kind::Swipe;
    const bool ok = ctl_.release(x, y, why);
    // A swipe's refusal, and the note that a join stayed a cut, are held with
    // the other track refusals; the gesture is over, so the status line would
    // be gone before it could be read.
    if (swipe) {
        setTrackRefusal(q(why));
        if (ok) why.clear();
    }
    run(ok, why);
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

void SessionModel::peaksReady() {
    for (SceneItem* item : items_) item->markContentDirty();
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
        case TimeFormat::BarsBeats: setCounterFormat(TimeFormat::MinSec); break;
        case TimeFormat::MinSec: setCounterFormat(TimeFormat::Timecode); break;
        case TimeFormat::Timecode: setCounterFormat(TimeFormat::Samples); break;
        case TimeFormat::Samples: setCounterFormat(TimeFormat::BarsBeats); break;
    }
}

void SessionModel::setCounterFormat(TimeFormat main) {
    main_fmt_ = main;
    sub_fmt_ = main_fmt_ == TimeFormat::MinSec ? TimeFormat::BarsBeats : TimeFormat::MinSec;
    main_counter_.clear();
    updateCounters(true);
    emit selectionChanged();
    emit eventListChanged();
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

}  // namespace sonora::ui
