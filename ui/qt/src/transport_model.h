// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Transport window (Pro Tools' Window > Transport, expanded): where the
// transport is, read every way at once; the tempo and meter in force there
// and the tempo map around it; pre- and post-roll, loop and punch as the
// session holds them; and the click, said to be absent.
//
// Fed by ShellModel's one tick, like SessionModel (ADR-0006: no second
// clock), and reading the session's settings through a getter, so it holds
// no TimelineDocument (timeline_document.h's rule). Text is re-published
// only when it changes, and at most at the counters' rate while rolling.
//
// What it must not do is show a control the engine does not run as if it
// ran. Pre- and post-roll are saved in the session and the engine does not
// use them yet (progress.json engine.transport.preroll); there is no click
// in the engine (engine.core.click). Both are drawn as such, with the
// reason, never as a live toggle that does nothing.
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <functional>

#include "engine_adapter.h"
#include "time_format.h"

namespace sonora {
class CommandRegistry;
}

namespace sonora::ui {

class SessionModel;
class ShellModel;
class TransportModel;

// Feeds both models from the shell's tick and publishes the window's
// position text on the main counter's cue, so the counter in the bar and
// the Transport window show one reading while rolling, not two a few ticks
// apart. Replaces any transport sink the shell had. The positions in the
// engine's refusals and notices read in the main counter's format too.
void link_transport(ShellModel& shell, SessionModel& session, TransportModel& transport);

class TransportModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool open READ open WRITE setOpen NOTIFY openChanged)
    Q_PROPERTY(QString stateText READ stateText NOTIFY stateChanged)
    Q_PROPERTY(bool rolling READ rolling NOTIFY stateChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(bool alive READ alive NOTIFY stateChanged)
    // What the position is: the playhead while rolling, else the insertion
    // point, which is what the counters show and where play starts.
    Q_PROPERTY(QString positionSource READ positionSource NOTIFY stateChanged)
    Q_PROPERTY(QString barsBeats READ barsBeats NOTIFY positionChanged)
    Q_PROPERTY(QString timecode READ timecode NOTIFY positionChanged)
    Q_PROPERTY(QString minSecs READ minSecs NOTIFY positionChanged)
    Q_PROPERTY(QString samples READ samples NOTIFY positionChanged)
    Q_PROPERTY(QString tempoText READ tempoText NOTIFY tempoChanged)
    Q_PROPERTY(QString meterText READ meterText NOTIFY tempoChanged)
    Q_PROPERTY(QString nextChangeText READ nextChangeText NOTIFY tempoChanged)
    Q_PROPERTY(int tempoChanges READ tempoChanges NOTIFY tempoChanged)
    // One entry per tempo map entry: {at, bpm, meter, current}.
    Q_PROPERTY(QVariantList tempoMap READ tempoMap NOTIFY tempoChanged)
    Q_PROPERTY(QString preRollText READ preRollText NOTIFY settingsChanged)
    Q_PROPERTY(QString postRollText READ postRollText NOTIFY settingsChanged)
    Q_PROPERTY(bool loopSet READ loopSet NOTIFY settingsChanged)
    Q_PROPERTY(qint64 loopStart READ loopStart NOTIFY settingsChanged)
    Q_PROPERTY(qint64 loopEnd READ loopEnd NOTIFY settingsChanged)
    Q_PROPERTY(bool punchSet READ punchSet NOTIFY settingsChanged)
    Q_PROPERTY(qint64 punchStart READ punchStart NOTIFY settingsChanged)
    Q_PROPERTY(qint64 punchEnd READ punchEnd NOTIFY settingsChanged)
    Q_PROPERTY(bool clickAvailable READ clickAvailable CONSTANT)
    Q_PROPERTY(QString clickWhy READ clickWhy CONSTANT)
    Q_PROPERTY(bool rollsAvailable READ rollsAvailable CONSTANT)
    Q_PROPERTY(QString rollsWhy READ rollsWhy CONSTANT)

public:
    using Settings = std::function<const model::SessionSettings&()>;
    using Insertion = std::function<model::Frames()>;
    TransportModel(Settings settings, Insertion insertion, QObject* parent = nullptr);

    // `window.transport`, the interface's own command: shows or hides the
    // window. Installed before KeyRouter::prepare().
    void installCommands(CommandRegistry& local);

    // From ShellModel's tick, on the GUI thread, every tick. Also where a
    // changed tempo map or transport setting is noticed: comparing the
    // settings is cheaper than one more signal to keep in step.
    void onTransport(const TransportSnapshot& t, bool alive);
    // The main counter has just published (SessionModel::counterChanged):
    // while rolling, publish the same position. Wired by link_transport().
    void followCounter();

    bool open() const { return open_; }
    void setOpen(bool on);
    QString stateText() const { return state_text_; }
    bool rolling() const { return rolling_; }
    bool recording() const { return recording_; }
    bool alive() const { return alive_; }
    QString positionSource() const;
    QString barsBeats() const { return bars_beats_; }
    QString timecode() const { return timecode_; }
    QString minSecs() const { return min_secs_; }
    QString samples() const { return samples_; }
    QString tempoText() const { return tempo_text_; }
    QString meterText() const { return meter_text_; }
    QString nextChangeText() const { return next_change_; }
    int tempoChanges() const { return static_cast<int>(settings_.tempo.size()) - 1; }
    QVariantList tempoMap() const { return tempo_map_; }
    QString preRollText() const;
    QString postRollText() const;
    bool loopSet() const { return range_set(settings_.transport.loop); }
    qint64 loopStart() const { return settings_.transport.loop[0]; }
    qint64 loopEnd() const { return settings_.transport.loop[1]; }
    bool punchSet() const { return range_set(settings_.transport.punch); }
    qint64 punchStart() const { return settings_.transport.punch[0]; }
    qint64 punchEnd() const { return settings_.transport.punch[1]; }
    static bool clickAvailable() { return false; }
    static QString clickWhy();
    static bool rollsAvailable() { return false; }
    static QString rollsWhy();

    // `frames` in the named format ("Bars|Beats", "Timecode", "Min:Secs",
    // "Samples": SessionModel's counter format names), so the loop and punch
    // ranges read the way the main counter does.
    Q_INVOKABLE QString timeText(qint64 frames, const QString& format) const;
    Q_INVOKABLE QString rangeText(qint64 start, qint64 end, const QString& format) const;

    model::Frames position() const { return position_; }

signals:
    void openChanged();
    void stateChanged();
    void positionChanged();
    void tempoChanged();
    void settingsChanged();

private:
    friend void link_transport(ShellModel&, SessionModel&, TransportModel&);
    static bool range_set(const std::array<model::Frames, 2>& r) { return r[1] > r[0]; }
    void refreshPosition(bool force);
    void refreshTempo(bool force);

    Settings settings_fn_;
    Insertion insertion_fn_;
    model::SessionSettings settings_;
    bool open_ = false;
    bool rolling_ = false;
    bool recording_ = false;
    bool alive_ = false;
    QString state_text_ = QStringLiteral("STOP");
    model::Frames position_ = -1;
    model::Frames shown_ = -1;
    unsigned ticks_ = 0;
    // Set by link_transport(): while rolling, the position text follows the
    // main counter's cue instead of a tick count of its own.
    bool counter_led_ = false;
    QString bars_beats_, timecode_, min_secs_, samples_;
    std::size_t tempo_index_ = static_cast<std::size_t>(-1);
    QString tempo_text_, meter_text_, next_change_;
    QVariantList tempo_map_;
};

}  // namespace sonora::ui
