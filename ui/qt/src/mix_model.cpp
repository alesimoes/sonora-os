// SPDX-License-Identifier: GPL-3.0-or-later
#include "mix_model.h"

#include <QList>
#include <QStringList>
#include <algorithm>
#include <utility>

#include "fader_taper.h"
#include "session_model.h"

namespace sonora::ui {

MixModel::MixModel(SessionModel& session, GainSender sender, EngineTracks engine_tracks,
                   bool audible, QObject* parent)
    : QObject(parent),
      session_(session),
      sender_(std::move(sender)),
      engine_tracks_(std::move(engine_tracks)),
      audible_(audible) {
    // Every change to the document reaches the windows through
    // contentChanged (SessionModel::sync), a whole new document included.
    connect(&session_, &SessionModel::contentChanged, this, &MixModel::documentChanged);
    connect(&session_, &SessionModel::sessionOpened, this, &MixModel::resetViews);
}

void MixModel::resetViews() {
    setShowIO(true);
    setShowInsertsAE(true);
    setShowInsertsFJ(false);
    setShowSends(true);
}

void MixModel::documentChanged() {
    if (ride_track_ != kNoRide) rideStillHolds();
}

bool MixModel::rideStillHolds() {
    if (ride_track_ == kNoRide) return false;
    if (session_.sessionGeneration() != ride_generation_) {
        dropRide("The session was replaced while a fader was being ridden: the ride was dropped "
                 "and nothing of it was saved");
        return false;
    }
    if (ride_track_ < 0) return true;   // the bus is always there
    const int now = session_.trackIndex(ride_id_);
    if (now == ride_track_) return true;
    if (now < 0) {
        dropRide("The track whose fader was being ridden is gone: the ride was dropped");
    } else {
        const QStringList names = session_.trackNames();
        dropRide(names[now].toStdString() +
                     " moved while its fader was being ridden: the ride was dropped, and its "
                     "gain is the one the session holds");
    }
    return false;
}

void MixModel::dropRide(const std::string& why) {
    const int index = ride_track_;
    const bool sent = ride_sent_live_;
    ride_track_ = kNoRide;
    ride_id_ = 0;
    ride_sent_live_ = false;
    // The steps went to an engine gain atomic by index, and one that
    // crossed another interface's edit may sit on whichever track is there
    // now (the header says why the engine cannot see it). Past the
    // document's tracks nothing is sent: the engine keeps that fader until
    // the plan stops rendering it.
    if (sent && live(index) && index < session_.trackCount()) {
        sender_(static_cast<uint32_t>(index), fader_linear_gain(session_.stripGain(index)));
    }
    session_.reportEdit(false, why);
    emit rideChanged();
}

QString MixModel::trackMeterCaption() const { return QStringLiteral("IN"); }

QString MixModel::busMeterCaption() const { return QStringLiteral("NO DATA"); }

QString MixModel::note() const {
    // The state block carries each track's input peak and nothing after the
    // fader (state_block.h, layout 2), so that is what the meters are said
    // to read. BACKLOG Track 1 asks for post-fader and bus peaks.
    return audible_
               ? QStringLiteral("Faders, pan, mute and solo play through the monitor bus. "
                                "Strip meters read each track's input; the engine does not "
                                "publish post-fader or bus levels yet.")
               : QStringLiteral("The built-in fake engine plays nothing back: the mix is kept "
                                "with the session and changes nothing you hear. Strip meters "
                                "read its synthetic inputs.");
}

bool MixModel::live(int track) const {
    return sender_ && engine_tracks_ && track >= 0 &&
           static_cast<uint32_t>(track) < engine_tracks_();
}

void MixModel::sendLive(int track, float db) {
    sender_(static_cast<uint32_t>(track), fader_linear_gain(db));
    ++live_steps_;
}

void MixModel::beginRide(int track) {
    if (track < -1 || track >= session_.trackCount()) return;
    ride_track_ = track;
    ride_id_ = track < 0 ? 0 : session_.trackId(track);
    ride_generation_ = session_.sessionGeneration();
    ride_from_db_ = ride_db_ = session_.stripGain(track);
    ride_sent_live_ = false;
    emit rideChanged();
}

void MixModel::ride(int track, qreal dy, qreal travel, bool fine) {
    if (track != ride_track_ || track == kNoRide || !rideStillHolds()) return;
    const float db = fader_drag(ride_db_, static_cast<float>(dy), static_cast<float>(travel), fine);
    if (db == ride_db_) return;
    ride_db_ = db;
    if (live(track)) {
        sendLive(track, db);
        ride_sent_live_ = true;
    } else {
        session_.setGain(track, db);
    }
    emit rideChanged();
}

void MixModel::endRide(int track) {
    if (track != ride_track_ || track == kNoRide || !rideStillHolds()) return;
    const float db = ride_db_;
    const bool sent = ride_sent_live_;
    ride_track_ = kNoRide;
    ride_id_ = 0;
    ride_sent_live_ = false;
    if (sent) {
        std::string why;
        const bool ok = session_.controller().set_gain(track, db, why);
        session_.reportEdit(ok, why);
        // The engine is where the ride left it; the document refused that,
        // so the engine goes back to what the document holds, which is what
        // the cap now shows.
        if (!ok && live(track)) sendLive(track, session_.stripGain(track));
    }
    emit rideChanged();
}

bool MixModel::setGainText(int track, const QString& text) {
    float db = 0.0f;
    std::string why;
    if (!parse_gain_text(text.toStdString(), db, why)) return session_.reportEdit(false, why);
    const bool ok = track < 0 ? session_.controller().set_bus_gain(db, why)
                              : session_.controller().set_gain(track, db, why);
    return session_.reportEdit(ok, why);
}

bool MixModel::setPanText(int track, const QString& text) {
    float pan = 0.0f;
    std::string why;
    if (!parse_pan_text(text.toStdString(), pan, why)) return session_.reportEdit(false, why);
    return session_.reportEdit(session_.controller().set_pan(track, pan, why), why);
}

void MixModel::panDrag(int track, qreal dx, qreal width, bool fine) {
    session_.panDrag(track, dx, fine ? width * 10.0 : width);
}

void MixModel::setAll(bool solo, bool on) {
    const QList<bool> now = solo ? session_.solos() : session_.mutes();
    for (int i = 0; i < now.size(); ++i) {
        if (now[i] == on) continue;
        std::string why;
        EditController& c = session_.controller();
        const bool ok = solo ? c.toggle_solo(i, why) : c.toggle_mute(i, why);
        if (!session_.reportEdit(ok, why)) return;
    }
}

bool MixModel::soloClick(int track, int modifiers) {
    if (modifiers & Qt::ControlModifier) {
        std::string why;
        session_.reportEdit(session_.controller().toggle_solo_safe(track, why), why);
        return true;
    }
    if (modifiers & Qt::AltModifier) {
        const QList<bool> solos = session_.solos();
        if (track < 0 || track >= solos.size()) return true;
        setAll(true, !solos[track]);
        return true;
    }
    return false;
}

bool MixModel::muteClick(int track, int modifiers) {
    if (modifiers & Qt::AltModifier) {
        const QList<bool> mutes = session_.mutes();
        if (track < 0 || track >= mutes.size()) return true;
        setAll(false, !mutes[track]);
        return true;
    }
    return false;
}

}  // namespace sonora::ui
