// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window's console gestures on the monitor bus, for QML: fader
// rides, typed values, and the Pro Tools modifier clicks on solo and mute.
// The values themselves are SessionModel's (the document's); this is what
// a hand on a strip does to them.
//
// A fader ride (PT-RG, "Mix Window"): while the fader moves, the engine
// hears it through a ParameterChange per step, a track's gain atomic, and
// the document is not touched; on release one SetMix saves where it stopped
// (messages.h: "the end of the gesture is saved with the model's SetMix").
// So a ride is one document change, not one per pixel, and the cap follows
// the hand without waiting for the engine's echo. The cap shows the ride's
// value until then (rideTrack/rideGain).
//
// The live path is used only where it is true: a track the engine has
// (ParameterChange addresses engine track i, which plays the document's
// display track i, engine_control_server.h). The monitor bus has no
// ParamId, and a track past the engine's count has no atomic, so those
// ride as before: a SetMix (or SetMonitorBus) per step, which the engine
// applies as it goes. Never a cap that moves while the sound does not.
//
// Modifier clicks, as the Pro Tools Reference Guide documents them for
// Windows:
//   Alt-click solo or mute    every track to the clicked track's new state
//   Ctrl-click solo           solo safe; refused, loudly, until the session
//                             format has somewhere to keep it (BACKLOG
//                             Track 2) -- a solo button that soloed instead
//                             would do the opposite of what was asked
// A plain click is the track.toggle_solo / track.toggle_mute command, the
// same path as the key, so QML sends it through ShellModel::invoke.
#pragma once

#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>

namespace sonora::ui {

class SessionModel;

class MixModel : public QObject {
    Q_OBJECT
    // The strip whose fader is being ridden: a track, -1 for the monitor
    // bus, kNoRide (-2) for none. Its cap and readout show rideGain.
    Q_PROPERTY(int rideTrack READ rideTrack NOTIFY rideChanged)
    Q_PROPERTY(qreal rideGain READ rideGain NOTIFY rideChanged)
    // What the strips' meters read, said on the strips themselves; and the
    // line above the strips.
    Q_PROPERTY(QString trackMeterCaption READ trackMeterCaption CONSTANT)
    Q_PROPERTY(QString busMeterCaption READ busMeterCaption CONSTANT)
    Q_PROPERTY(QString note READ note CONSTANT)

public:
    static constexpr int kNoRide = -2;

    // Sends a ParameterChange (ParamId::Gain, linear 0..4) for an engine
    // track. Empty: there is no live path, and every step is a SetMix.
    using GainSender = std::function<void(uint32_t track, float linear)>;

    // `audible`: the engine plays the monitor bus (the real engine, ADR-0009);
    // false on the built-in fake engine, which plays nothing.
    MixModel(SessionModel& session, GainSender sender, uint32_t engine_tracks, bool audible,
             QObject* parent = nullptr);

    int rideTrack() const { return ride_track_; }
    qreal rideGain() const { return ride_db_; }
    QString trackMeterCaption() const;
    QString busMeterCaption() const;
    QString note() const;
    // ParameterChanges sent by rides, for the tests.
    uint64_t liveSteps() const { return live_steps_; }

    // --- a fader ride: press, moves, release ---
    Q_INVOKABLE void beginRide(int track);
    // `dy` pixels down on a fader with `travel` pixels; `fine` is Ctrl.
    Q_INVOKABLE void ride(int track, qreal dy, qreal travel, bool fine);
    Q_INVOKABLE void endRide(int track);

    // --- typed values: the readouts are fields (Return sets, Escape leaves) ---
    // False, with the reason on the status line, for text that is not one.
    Q_INVOKABLE bool setGainText(int track, const QString& text);
    Q_INVOKABLE bool setPanText(int track, const QString& text);
    // Ctrl-drag is a tenth of the motion, as on the fader.
    Q_INVOKABLE void panDrag(int track, qreal dx, qreal width, bool fine);

    // --- solo and mute clicks with a modifier (Qt::KeyboardModifiers) ---
    // True when the modifier was this one's to handle; false for a plain
    // click, which QML sends as the command.
    Q_INVOKABLE bool soloClick(int track, int modifiers);
    Q_INVOKABLE bool muteClick(int track, int modifiers);

signals:
    void rideChanged();

private:
    bool live(int track) const;
    void sendLive(int track, float db);
    // Every track's solo (or mute) to `on`, one SetMix per track that differs.
    void setAll(bool solo, bool on);

    SessionModel& session_;
    GainSender sender_;
    uint32_t engine_tracks_;
    bool audible_;
    int ride_track_ = kNoRide;
    float ride_db_ = 0.0f;
    float ride_from_db_ = 0.0f;
    bool ride_sent_live_ = false;
    uint64_t live_steps_ = 0;
};

}  // namespace sonora::ui
