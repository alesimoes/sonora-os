// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit and Mix windows' view of the session, for QML. It owns the
// EditController, installs the interface's editing commands in the key
// router, and tells the scene-graph items when to rebuild.
//
// It holds a TimelineDocument (the model seam, timeline_document.h) and is
// fed the engine's transport by ShellModel's one tick (ADR-0006: one 60 Hz
// pull timer, no second clock). The playhead reaches the timeline as a C++
// call and a scheduled redraw, never a signal; the counters are text and
// are re-published at most at the shell's state rate.
#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <vector>

#include "edit_controller.h"
#include "engine_adapter.h"
#include "peak_source.h"
#include "time_format.h"
#include "timeline_document.h"
#include "timeline_scene.h"

namespace sonora {
class CommandRegistry;
}

namespace sonora::ui {

class SceneItem;

class SessionModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString window READ window NOTIFY windowChanged)
    Q_PROPERTY(QString sessionName READ sessionName NOTIFY sessionChanged)
    Q_PROPERTY(bool synthetic READ synthetic NOTIFY sessionChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY sessionChanged)
    Q_PROPERTY(QString sessionFile READ sessionFile NOTIFY sessionChanged)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY contentChanged)
    Q_PROPERTY(QStringList trackNames READ trackNames NOTIFY contentChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY contentChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY contentChanged)
    Q_PROPERTY(QString editMode READ editMode NOTIFY editStateChanged)
    Q_PROPERTY(QString tool READ tool NOTIFY editStateChanged)
    Q_PROPERTY(QString gridValue READ gridValue NOTIFY editStateChanged)
    Q_PROPERTY(int selectedTrack READ selectedTrack NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionStart READ selectionStart NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionEnd READ selectionEnd NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionLength READ selectionLength NOTIFY selectionChanged)
    Q_PROPERTY(qreal scrollY READ scrollY NOTIFY viewChanged)
    Q_PROPERTY(qreal trackHeight READ trackHeight NOTIFY viewChanged)
    Q_PROPERTY(QString zoomText READ zoomText NOTIFY viewChanged)
    Q_PROPERTY(QList<bool> mutes READ mutes NOTIFY mixChanged)
    Q_PROPERTY(QList<bool> solos READ solos NOTIFY mixChanged)
    Q_PROPERTY(QList<qreal> gains READ gains NOTIFY mixChanged)
    Q_PROPERTY(QList<qreal> pans READ pans NOTIFY mixChanged)
    Q_PROPERTY(qreal busGain READ busGain NOTIFY mixChanged)
    Q_PROPERTY(bool anySolo READ anySolo NOTIFY mixChanged)
    Q_PROPERTY(QString mainCounter READ mainCounter NOTIFY counterChanged)
    Q_PROPERTY(QString mainCounterFormat READ mainCounterFormat NOTIFY counterChanged)
    Q_PROPERTY(QString subCounter READ subCounter NOTIFY counterChanged)
    Q_PROPERTY(QString subCounterFormat READ subCounterFormat NOTIFY counterChanged)
    Q_PROPERTY(bool loopOn READ loopOn NOTIFY contentChanged)
    Q_PROPERTY(bool punchOn READ punchOn NOTIFY contentChanged)
    Q_PROPERTY(QString tempoText READ tempoText NOTIFY sessionChanged)

public:
    // `synthetic_peaks`: the waveforms `peaks` draws are not the sources'
    // audio, and the window must say so whenever there is a region to draw.
    SessionModel(TimelineDocument& doc, const PeakSource& peaks, bool synthetic_peaks,
                 QObject* parent = nullptr);

    // Where File > Save puts a new session and File > Open looks
    // (session_files.h). Set before the first save.
    void setSessionsDir(const QString& dir) { sessions_dir_ = dir; }
    const QString& sessionsDir() const { return sessions_dir_; }

    // File > Save: to the file the session came from, or, the first time, to
    // a new folder named after the session ("Untitled" when it has no
    // name). A save with nothing changed since the last one writes nothing.
    bool saveSession(std::string& why);
    // File > Open. Refused while a take is open, at another sample rate
    // than this session's, and over unsaved changes unless `discard`.
    bool openSessionFile(const QString& file, bool discard, std::string& why);

    // The interface's editing commands, installed before KeyRouter::prepare().
    void installCommands(CommandRegistry& local);
    // The track a target-less track command acts on: the selection.
    int selectedTrackForCommands() const { return ctl_.selection().track; }

    // From ShellModel's tick, on the GUI thread, every tick.
    void onTransport(const TransportSnapshot& t, bool alive);

    void attachItem(SceneItem* item);
    void detachItem(SceneItem* item);

    EditController& controller() { return ctl_; }
    const PeakSource& peaks() const { return peaks_; }
    const TransportOverlay& transportOverlay() const { return overlay_; }

    QString window() const { return window_; }
    QString sessionName() const;
    bool synthetic() const;
    bool dirty() const { return doc_.revision() != saved_revision_; }
    QString sessionFile() const { return file_; }
    int trackCount() const;
    QStringList trackNames() const;
    bool canUndo() const { return doc_.can_undo(); }
    bool canRedo() const { return doc_.can_redo(); }
    QString editMode() const { return QString::fromLatin1(edit_mode_name(ctl_.mode())); }
    QString tool() const { return QString::fromLatin1(tool_name(ctl_.tool())); }
    QString gridValue() const { return QString::fromLatin1(grid_value_name(ctl_.grid())); }
    int selectedTrack() const { return ctl_.selection().track; }
    QString selectionStart() const;
    QString selectionEnd() const;
    QString selectionLength() const;
    qreal scrollY() const { return ctl_.view().scroll_y; }
    qreal trackHeight() const { return ctl_.view().track_height; }
    QString zoomText() const;
    QList<bool> mutes() const;
    QList<bool> solos() const;
    QList<qreal> gains() const;
    QList<qreal> pans() const;
    qreal busGain() const { return ctl_.snapshot().mix.bus.gain_db; }
    bool anySolo() const;
    QString mainCounter() const { return main_counter_; }
    QString mainCounterFormat() const { return QString::fromLatin1(time_format_name(main_fmt_)); }
    QString subCounter() const { return sub_counter_; }
    QString subCounterFormat() const { return QString::fromLatin1(time_format_name(sub_fmt_)); }
    bool loopOn() const;
    bool punchOn() const;
    QString tempoText() const;

    // --- Edit window: track-area pixels ---
    Q_INVOKABLE void setViewport(qreal width, qreal height);
    Q_INVOKABLE void press(qreal x, qreal y);
    Q_INVOKABLE void drag(qreal x, qreal y);
    Q_INVOKABLE void release(qreal x, qreal y);
    // A wheel step: vertical scrolls the lanes, horizontal (or Shift)
    // scrolls time, Ctrl zooms about the pointer.
    Q_INVOKABLE void wheel(qreal x, qreal dx, qreal dy, bool shift, bool ctrl);
    Q_INVOKABLE void rulerClick(qreal x);
    Q_INVOKABLE void selectTrack(int index);
    Q_INVOKABLE void cycleGrid();
    Q_INVOKABLE void cycleMainCounter();
    Q_INVOKABLE void setWindow(const QString& w);

    // --- File ---
    // Save is the app.save command, so the button and the key are one path.
    Q_INVOKABLE QString sessionsFolder() const { return sessions_dir_; }
    // [{name, file, current}], for the Open panel.
    Q_INVOKABLE QVariantList sessionList() const;
    Q_INVOKABLE bool openSession(const QString& file, bool discard);

    // --- Mix window ---
    Q_INVOKABLE qreal faderPosition(qreal db) const;
    Q_INVOKABLE QString gainText(qreal db) const;
    Q_INVOKABLE QString panText(qreal pan) const;
    // track -1 is the monitor bus.
    Q_INVOKABLE void faderDrag(int track, qreal dy, qreal travel, bool fine);
    Q_INVOKABLE void resetGain(int track);
    Q_INVOKABLE void setGain(int track, qreal db);
    Q_INVOKABLE void panDrag(int track, qreal dx, qreal width);
    Q_INVOKABLE void resetPan(int track);

    // Pixel geometry the timeline item draws, for the scene builders.
    void buildTimeline(Scene& out) const;
    void buildOverlay(std::vector<SceneRect>& out) const;
    void buildRuler(TimeFormat fmt, double height, Scene& out) const;

signals:
    void windowChanged();
    void contentChanged();
    void editStateChanged();
    void selectionChanged();
    void viewChanged();
    void mixChanged();
    void counterChanged();
    void sessionChanged();
    // A refusal from a mouse gesture, for the status line. Keys report
    // through the router.
    void refused(const QString& why);
    // Something done that the operator should see confirmed: "Saved ...".
    void notice(const QString& text);

private:
    // Re-publishes what changed since the last call and marks the items.
    void sync();
    bool run(bool ok, const std::string& why);
    void updateCounters(bool force);

    TimelineDocument& doc_;
    const PeakSource& peaks_;
    bool synthetic_peaks_;
    QString sessions_dir_;
    QString file_;                 // empty until the session is saved or opened
    uint64_t saved_revision_ = 0;  // the revision the file holds
    uint64_t save_generation_ = 0;
    std::string saved_bytes_;      // what the file holds, to skip a save that changes nothing
    EditController ctl_;
    std::vector<SceneItem*> items_;
    QString window_ = QStringLiteral("edit");

    TransportOverlay overlay_;
    TimeFormat main_fmt_ = TimeFormat::BarsBeats;
    TimeFormat sub_fmt_ = TimeFormat::MinSec;
    QString main_counter_, sub_counter_;
    unsigned transport_ticks_ = 0;

    // What was last published, to emit only on change.
    uint64_t seen_revision_ = UINT64_MAX;
    bool seen_dirty_ = false;
    int seen_mode_ = -1, seen_tool_ = -1, seen_grid_ = -1;
    Selection seen_sel_;
    bool seen_sel_valid_ = false;
    ViewState seen_view_;
    bool seen_view_valid_ = false;
    Drag seen_drag_;
};

}  // namespace sonora::ui
