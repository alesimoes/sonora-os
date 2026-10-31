// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window's sends and aux buses for QML: SendController behind a
// QObject (ADR-0022). QML names a send by strip and slot (0 for A), strips
// being the tracks in display order and then the aux buses; an id never
// reaches QML. The send window holds its strip by id and its slot, so it
// follows a track moved in the display order and a send sent elsewhere.
//
// Pro Tools shows five sends at a time, A-E or F-J (View > Mix Window
// Views); `bank` is which five. The send window is Pro Tools' send window:
// level fader and readout, pan, PRE, mute, FMP and the bus. Every edit goes
// through the TimelineDocument and reports through SessionModel, so a
// refused send reaches the status line the way a refused region edit does.
// A ride of a send or aux fader (press, moves, release) is heard through a
// ParameterChange per step, ParamId::SendLevel or AuxGain, as a track's
// fader is (mix_model.h), and the document is not touched; the release
// saves where it stopped with one SetSendMix or SetBusMix. Without a live
// sender (or outside a ride) every step is a SetSendMix or SetBusMix.
#pragma once

#include <functional>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "send_controller.h"

namespace sonora::ui {

class SessionModel;

class SendModel : public QObject {
    Q_OBJECT
    // [{name, sends, output, slots: [10 x {present, letter, bus, levelText,
    // pre, mute}]}]: the tracks in display order, then the aux buses. `output`
    // is where the strip feeds: "Master" or a bus's name (ADR-0024).
    Q_PROPERTY(QVariantList strips READ strips NOTIFY changed)
    // [{name, gainDb, gainText, pan, panText, mute, solo, output}], one per
    // aux bus.
    Q_PROPERTY(QVariantList auxes READ auxes NOTIFY changed)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY changed)
    // 0: sends A-E shown on the strips; 5: F-J.
    Q_PROPERTY(int bank READ bank NOTIFY bankChanged)

    // The bus chooser, on one slot.
    Q_PROPERTY(bool chooserOpen READ chooserOpen NOTIFY chooserChanged)
    Q_PROPERTY(QString chooserTitle READ chooserTitle NOTIFY chooserChanged)
    Q_PROPERTY(bool chooserIsOutput READ chooserIsOutput NOTIFY chooserChanged)
    // [{bus, name, refusal}]
    Q_PROPERTY(QVariantList chooserItems READ chooserItems NOTIFY chooserChanged)

    // The send window.
    Q_PROPERTY(bool windowOpen READ windowOpen NOTIFY windowChanged)
    Q_PROPERTY(int windowStrip READ windowStrip NOTIFY windowChanged)
    Q_PROPERTY(int windowSlot READ windowSlot NOTIFY windowChanged)
    // {title, bus, levelDb, levelText, pan, panText, followPan, pre, mute}
    Q_PROPERTY(QVariantMap window READ window NOTIFY windowChanged)

public:
    SendModel(TimelineDocument& doc, SessionModel& session, QObject* parent = nullptr);

    SendController& controller() { return ctl_; }

    // Sends the live level of a send (`aux` false, its model id) or of an
    // aux bus (`aux` true, its id), linear 0 .. 4. Empty: no live path.
    using LiveSender = std::function<void(bool aux, model::Id id, float linear)>;
    void setLiveSender(LiveSender sender) { sender_ = std::move(sender); }
    // ParameterChanges sent by rides, for the tests.
    uint64_t liveSteps() const { return live_steps_; }

    QVariantList strips() const { return strips_; }
    QVariantList auxes() const { return auxes_; }
    int trackCount() const { return ctl_.track_count(); }
    int bank() const { return bank_; }

    bool chooserOpen() const { return chooser_strip_ >= 0; }
    QString chooserTitle() const;
    bool chooserIsOutput() const { return chooser_output_; }
    QVariantList chooserItems() const;

    bool windowOpen() const { return window_strip_id_ != kNoId; }
    int windowStrip() const { return window_strip_; }
    int windowSlot() const { return window_slot_; }
    QVariantMap window() const { return window_; }

    Q_INVOKABLE void setBank(int first);
    // A click on a send slot: an empty one opens the bus chooser, a used
    // one its send window.
    Q_INVOKABLE void slotClicked(int strip, int slot);
    Q_INVOKABLE void openChooser(int strip, int slot);
    Q_INVOKABLE void closeChooser();
    // The same chooser on a strip's output (ADR-0024): the Master is bus -1
    // in chooserItems, and "New bus" is not offered.
    Q_INVOKABLE void openOutputChooser(int strip);
    // `bus` from chooserItems; -1 makes a new bus for it (a send), or is the
    // Master (an output).
    Q_INVOKABLE bool choose(int bus);
    Q_INVOKABLE void openWindow(int strip, int slot);
    Q_INVOKABLE void closeWindow();

    // The send window's controls.
    // A fader press and release around the levelDrag calls.
    Q_INVOKABLE void beginLevelRide();
    Q_INVOKABLE void endLevelRide();
    Q_INVOKABLE void levelDrag(qreal dy, qreal travel, bool fine);
    Q_INVOKABLE bool setLevelText(const QString& text);
    Q_INVOKABLE void resetLevel();
    Q_INVOKABLE void panDrag(qreal dx, qreal width, bool fine);
    Q_INVOKABLE bool setPanText(const QString& text);
    Q_INVOKABLE void togglePre();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void toggleFollowPan();
    Q_INVOKABLE void removeSend();
    Q_INVOKABLE void reassign();

    // Aux strips.
    Q_INVOKABLE void newBus();
    Q_INVOKABLE void removeBus(int bus);
    Q_INVOKABLE void beginAuxRide(int bus);
    Q_INVOKABLE void endAuxRide(int bus);
    Q_INVOKABLE void auxFaderDrag(int bus, qreal dy, qreal travel, bool fine);
    Q_INVOKABLE bool setAuxGainText(int bus, const QString& text);
    Q_INVOKABLE void resetAuxGain(int bus);
    Q_INVOKABLE void auxPanDrag(int bus, qreal dx, qreal width, bool fine);
    Q_INVOKABLE void toggleAuxMute(int bus);
    Q_INVOKABLE void toggleAuxSolo(int bus);

signals:
    void changed();
    void bankChanged();
    void chooserChanged();
    void windowChanged();

private:
    void refresh();
    void refreshWindow();
    bool report(bool ok, const std::string& why);
    // An output change: a refusal is held, not flashed (SessionModel).
    bool reportRouting(bool ok, const std::string& why);

    enum class Ride { None, Send, Aux };
    void sendLive(float db);
    // The ride's cap and readout follow the live value, not the document.
    void showRide();
    void overlayRide();
    void dropRide();
    // Ends the ride, saving where it stopped; `ok` false if the engine is to
    // be put back to the document's value.
    void finishRide(bool save);

    SendController ctl_;
    LiveSender sender_;
    uint64_t live_steps_ = 0;
    Ride ride_ = Ride::None;
    model::Id ride_id_ = kNoId;       // the send's or the bus's id
    model::Id ride_strip_id_ = kNoId; // a send's strip
    int ride_slot_ = -1;
    float ride_db_ = 0.0f;
    bool ride_sent_ = false;
    SessionModel& session_;
    QVariantList strips_;
    QVariantList auxes_;
    int bank_ = 0;
    int chooser_strip_ = -1;
    int chooser_slot_ = -1;
    bool chooser_output_ = false;
    model::Id window_strip_id_ = kNoId;
    int window_strip_ = -1;
    int window_slot_ = -1;
    QVariantMap window_;
};

}  // namespace sonora::ui
