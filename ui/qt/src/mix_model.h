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
// An interrupted ride must not leave the engine and the document apart, so:
//
//   - A ride holds its track, not its strip. If that track moves or leaves,
//     or the document becomes another session (an Open, a reconnection),
//     before release, the ride is dropped with the reason on the status
//     line and saves nothing; the steps a release would have saved onto
//     whatever now sits under the hand are never written.
//   - A ride dropped after its steps reached the engine puts that one
//     engine track back to the document. The engine covers the rest since
//     PR #71: it applies the document's mix on a Resync and on any change
//     of the mix, the track order or the session, and when a client leaves
//     (a lost link, an interface that crashed) it puts back the tracks that
//     client was the last to ride. What it cannot see is a step sent before
//     this interface saw another interface's reorder or Open: the step
//     arrives after the engine applied that mix and lands on whichever
//     track is now at its index, and the engine has nothing to say it is
//     stale. Only the dropped ride's index is sent: sending every gain on
//     each new document (PR #61) overrode another interface's ride, which
//     the engine deliberately leaves alone when a client attaches, and made
//     this interface that track's last rider.
//
// The trim fader (ADR-0027 §8 slice 5). In Trim (Pro Tools' Touch/Trim)
// and Write/Trim the volume fader is the trim fader, as in Pro Tools' Trim
// modes: a ride sends ParamId::VolumeTrim with Touch, Move and Release, the
// engine's Track::trim() atomic, and never Gain, and nothing is saved at
// release. The trim is session state, not a mix field: the engine sets it
// to 0 dB at a Load, so this interface keeps what it last sent to each
// track and forgets it when a session opens. What the strip shows (trims,
// trimLanes) is what plays: the ride while held; in Write/Trim the trim
// fader, which plays until it moves again; in Touch/Trim, released, the
// trim lane at the playhead (trimLanes says a lane plays) or 0 dB. A track
// the engine does not have has no trim atomic: its trim ride is refused,
// since there is no SetMix to fall back on.
//
// Modifier clicks, as the Pro Tools Reference Guide documents them for
// Windows:
//   Alt-click solo or mute    every track to the clicked track's new state
//   Ctrl-click solo           solo safe on or off (TrackMix::solo_safe), a
//                             SetMix like any other mix field; the track's
//                             own solo is left as it was. Control is checked
//                             before Alt, so Ctrl+Alt is solo safe, never a
//                             solo of every track
// A plain click is the track.toggle_solo / track.toggle_mute command, the
// same path as the key, so QML sends it through ShellModel::invoke.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>

#include "sonora/api/messages.h"
#include "sonora/model/model.h"

namespace sonora::ui {

class SessionModel;

class MixModel : public QObject {
    Q_OBJECT
    // The strip whose fader is being ridden: a track, -1 for the monitor
    // bus, kNoRide (-2) for none. Its cap and readout show rideGain.
    Q_PROPERTY(int rideTrack READ rideTrack NOTIFY rideChanged)
    // rideGain is the trim's dB in a trim ride (rideTrim).
    Q_PROPERTY(qreal rideGain READ rideGain NOTIFY rideChanged)
    Q_PROPERTY(bool rideTrim READ rideTrim NOTIFY rideChanged)
    // Per track: the trim the strip shows in a trim mode (dB, 0 elsewhere),
    // and whether that is the trim lane playing rather than the trim fader.
    Q_PROPERTY(QList<qreal> trims READ trims NOTIFY trimsChanged)
    Q_PROPERTY(QList<bool> trimLanes READ trimLanes NOTIFY trimsChanged)
    // What the strips' meters read, said on the strips themselves; and the
    // line above the strips.
    Q_PROPERTY(QString trackMeterCaption READ trackMeterCaption CONSTANT)
    Q_PROPERTY(QString busMeterCaption READ busMeterCaption CONSTANT)
    Q_PROPERTY(QString note READ note CONSTANT)
    // The Mix window's views (ui-design.md, "Mixer strip anatomy"): which
    // sections of a strip are shown. Interface state, outside the session
    // file (session-format.md, ADR-0007 5); it goes back to the defaults
    // when another session opens. Inserts F-J are hidden by default; the
    // sends' F-J is SendModel::bank.
    Q_PROPERTY(bool showIO READ showIO WRITE setShowIO NOTIFY viewsChanged)
    Q_PROPERTY(bool showInsertsAE READ showInsertsAE WRITE setShowInsertsAE NOTIFY viewsChanged)
    Q_PROPERTY(bool showInsertsFJ READ showInsertsFJ WRITE setShowInsertsFJ NOTIFY viewsChanged)
    Q_PROPERTY(bool showSends READ showSends WRITE setShowSends NOTIFY viewsChanged)
    // VCA masters and folders (ADR-0042). vcas: one map each, {name, gainDb,
    // gainText, mute, count}. trackVcas: per track, the names of the VCAs it
    // is on, joined, "" for none. trackFolders: per track, the index in
    // folders or -1. folders: {name, count, open, mute, solo}. A folder
    // closed in the window (interface state, back to open when a session
    // opens) shows as one card at its first track's place and its other
    // tracks have no slot: trackSlots gives each track's place among the
    // slotCount, -1 for a hidden one; folderCards: {slot, folder, name,
    // count, mute, solo} for each closed folder.
    Q_PROPERTY(QVariantList vcas READ vcas NOTIFY structureChanged)
    Q_PROPERTY(QStringList trackVcas READ trackVcas NOTIFY structureChanged)
    Q_PROPERTY(QList<int> trackFolders READ trackFolders NOTIFY structureChanged)
    Q_PROPERTY(QVariantList folders READ folders NOTIFY structureChanged)
    Q_PROPERTY(QList<int> trackSlots READ trackSlots NOTIFY structureChanged)
    Q_PROPERTY(int slotCount READ slotCount NOTIFY structureChanged)
    Q_PROPERTY(QVariantList folderCards READ folderCards NOTIFY structureChanged)

public:
    static constexpr int kNoRide = -2;

    // Sends a ParameterChange for an engine track: gain (linear 0..4) or pan
    // (-1..1), with the hand's gesture. A ride sends Touch when the hand
    // lands, Move per step and Release when it lets go, so the engine's
    // Touch, Latch and Write modes know when the operator holds the control
    // (ADR-0027 section 3). Empty: there is no live path, and every step is
    // a SetMix.
    using GainSender = std::function<void(uint32_t track, api::ParamId param, float value,
                                          api::Gesture gesture)>;
    // The engine's track count, as its latest Hello says: asked when used,
    // so a restarted engine with another count is followed (PR #67 review).
    using EngineTracks = std::function<uint32_t()>;

    // `audible`: the engine plays the monitor bus (the real engine, ADR-0009);
    // false on the built-in fake engine, which plays nothing.
    MixModel(SessionModel& session, GainSender sender, EngineTracks engine_tracks, bool audible,
             QObject* parent = nullptr);

    int rideTrack() const { return ride_track_; }
    qreal rideGain() const { return ride_db_; }
    bool rideTrim() const { return ride_track_ != kNoRide && ride_trim_; }
    QList<qreal> trims() const;
    QList<bool> trimLanes() const;
    QString trackMeterCaption() const;
    QString busMeterCaption() const;
    QString note() const;
    bool showIO() const { return show_io_; }
    bool showInsertsAE() const { return show_inserts_ae_; }
    bool showInsertsFJ() const { return show_inserts_fj_; }
    bool showSends() const { return show_sends_; }
    void setShowIO(bool on) { setView(show_io_, on); }
    void setShowInsertsAE(bool on) { setView(show_inserts_ae_, on); }
    void setShowInsertsFJ(bool on) { setView(show_inserts_fj_, on); }
    void setShowSends(bool on) { setView(show_sends_, on); }
    QVariantList vcas() const;
    QStringList trackVcas() const;
    QList<int> trackFolders() const;
    QVariantList folders() const;
    QList<int> trackSlots() const;
    int slotCount() const;
    QVariantList folderCards() const;

    // --- VCA masters (ADR-0042) ---
    // Both take the selected tracks. A refusal is a held notice.
    Q_INVOKABLE bool newVca(const QString& name);
    Q_INVOKABLE bool assignVca(int vca);
    Q_INVOKABLE bool removeVca(int vca);
    // One step of the VCA fader's drag; each is a SetVcaMix, which the
    // engine applies as it goes (a VCA has no live parameter of its own).
    Q_INVOKABLE void vcaDrag(int vca, qreal dy, qreal travel, bool fine);
    Q_INVOKABLE void resetVca(int vca);
    Q_INVOKABLE bool setVcaGainText(int vca, const QString& text);
    Q_INVOKABLE bool toggleVcaMute(int vca);
    // --- folders ---
    Q_INVOKABLE bool newFolder(const QString& name);
    Q_INVOKABLE bool dissolveFolder(int folder);
    Q_INVOKABLE void toggleFolderOpen(int folder);
    Q_INVOKABLE bool toggleFolderMute(int folder);
    Q_INVOKABLE bool toggleFolderSolo(int folder);

    // ParameterChanges sent by rides, for the tests.
    uint64_t liveSteps() const { return live_steps_; }

    // --- a fader ride: press, moves, release ---
    Q_INVOKABLE void beginRide(int track);
    // `dy` pixels down on a fader with `travel` pixels; `fine` is Ctrl.
    Q_INVOKABLE void ride(int track, qreal dy, qreal travel, bool fine);
    Q_INVOKABLE void endRide(int track);
    // Alt-click on a fader in a trim mode: the trim to 0 dB, as one touch
    // and release, leaving the volume as it is.
    Q_INVOKABLE void resetTrim(int track);
    // The track is in Trim or Write/Trim: its fader is the trim fader.
    Q_INVOKABLE bool trimMode(int track) const;

    // --- typed values: the readouts are fields (Return sets, Escape leaves) ---
    // False, with the reason on the status line, for text that is not one.
    Q_INVOKABLE bool setGainText(int track, const QString& text);
    Q_INVOKABLE bool setPanText(int track, const QString& text);
    // Ctrl-drag is a tenth of the motion, as on the fader. The hand landing
    // and lifting are the pan's touch and release (see a fader ride).
    Q_INVOKABLE void beginPan(int track);
    Q_INVOKABLE void endPan(int track);
    Q_INVOKABLE void panDrag(int track, qreal dx, qreal width, bool fine);

    // --- the strip's automation mode selector (ADR-0027) ---
    // One SetMix: "Off", "Read", "Touch", "Latch", "Write", "Trim" or
    // "Write/Trim", as SessionModel::automationModes names them.
    Q_INVOKABLE bool setAutomationMode(int track, const QString& mode);

    // --- solo and mute clicks with a modifier (Qt::KeyboardModifiers) ---
    // True when the modifier was this one's to handle; false for a plain
    // click, which QML sends as the command.
    Q_INVOKABLE bool soloClick(int track, int modifiers);
    Q_INVOKABLE bool muteClick(int track, int modifiers);

signals:
    void rideChanged();
    void viewsChanged();
    void trimsChanged();
    void structureChanged();

private:
    void setView(bool& field, bool on) {
        if (field == on) return;
        field = on;
        emit viewsChanged();
    }
    void resetViews();
    // The first member of folder `folder` in display order, -1 for none.
    int folderFirst(int folder) const;
    bool folderOpen(int folder) const;
    // A refusal the operator must see: held, not the status line.
    bool vcaResult(bool ok, const std::string& why);
    void sessionOpened();
    bool live(int track) const;
    // Where a trim ride on `track` starts, so nothing jumps: the trim fader
    // in Write/Trim, the trim lane at the playhead (or 0 dB) in Touch/Trim.
    float trimNow(int track, bool& lane) const;
    float trimNow(int track, const QString& mode, bool& lane) const;
    float heldTrim(model::Id track) const;
    // A live ride on `track` now is a Touch pass's: its lane holds it and the
    // engine returns to the static at the release.
    bool writes(int track) const;
    void sendLive(int track, float db, api::Gesture gesture = api::Gesture::Move);
    // Every track's solo (or mute) to `on`, one SetMix per track that differs.
    void setAll(bool solo, bool on);
    // The document changed: a ride whose track or session is not there any
    // more is dropped.
    void documentChanged();
    // False, with the ride dropped and the reason said, when the ride's
    // track is no longer the strip it began on or its session was replaced.
    bool rideStillHolds();
    // A dropped hold always ends with a Release to the engine index it
    // touched, so that index is never left touched (Latch and Write would
    // keep writing the held value into whatever track takes the index).
    // The Release carries the value the document holds for the track now
    // at that index; past the document's tracks it carries the hold's own
    // last value, so what the engine plays does not jump. dropRide sends
    // only when the ride touched or stepped the engine.
    void dropRide(const std::string& why);
    // The same for a pan hold: false, with a Release to the touched index.
    bool panStillHolds();
    void dropPan(const std::string& why);

    SessionModel& session_;
    GainSender sender_;
    EngineTracks engine_tracks_;
    bool audible_;
    int ride_track_ = kNoRide;
    model::Id ride_id_ = 0;       // the ridden track; 0 for the bus
    uint64_t ride_generation_ = 0;
    float ride_db_ = 0.0f;
    float ride_from_db_ = 0.0f;
    bool ride_sent_live_ = false;
    bool ride_touched_ = false;   // a Touch went out and wants its Release
    // Some of the ride played in Touch while rolling: the
    // pass's lane holds it, so the static is not set (ADR-0027 §8).
    bool ride_wrote_ = false;
    bool ride_trim_ = false;      // the ride is the trim fader's (VolumeTrim)
    // What each track's engine trim atomic holds, as this interface last
    // sent it; a track not here is at 0 dB. Cleared when a session opens.
    std::map<model::Id, float> held_trim_db_;
    int pan_track_ = kNoRide;     // the engine track whose pan the hand holds
    model::Id pan_id_ = 0;
    uint64_t pan_generation_ = 0;
    float pan_value_ = 0.0f;      // the held pan's last value sent
    uint64_t live_steps_ = 0;
    bool show_io_ = true;
    bool show_inserts_ae_ = true;
    bool show_inserts_fj_ = false;
    bool show_sends_ = true;
    // Folders shown closed; the interface's, cleared when a session opens.
    std::set<model::Id> closed_folders_;
};

}  // namespace sonora::ui
