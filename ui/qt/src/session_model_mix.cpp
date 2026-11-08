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
                         ctl_.grid_lines(), ctl_.grid(), peaks_, out, ctl_.lane_selection(),
                         ctl_.sample_source());
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
