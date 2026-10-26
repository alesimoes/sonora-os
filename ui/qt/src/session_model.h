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

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "edit_controller.h"
#include "engine_adapter.h"
#include "peak_source.h"
#include "session_files.h"
#include "sonora/model/sequence_file.h"
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
    // The Edit menu's first two items, as Pro Tools words them: "Undo
    // Paste", "Redo Trim"; "Can't Undo" when there is nothing to undo.
    Q_PROPERTY(QString undoText READ undoText NOTIFY contentChanged)
    Q_PROPERTY(QString redoText READ redoText NOTIFY contentChanged)
    // Pro Tools' Undo History window: [{label, done}] for every step after
    // the floor, oldest first; `done` false for the steps redo would bring
    // back, which the window shows in italics.
    Q_PROPERTY(QVariantList undoHistory READ undoHistory NOTIFY contentChanged)
    // Older steps were dropped as the history filled (the floor is an edit,
    // not the session as it was opened): undo cannot reach back to the start.
    // Where the document is in undoHistory: the number of done rows.
    Q_PROPERTY(int undoHistoryDepth READ undoHistoryDepth NOTIFY contentChanged)
    Q_PROPERTY(bool undoHistoryTrimmed READ undoHistoryTrimmed NOTIFY contentChanged)
    Q_PROPERTY(bool undoHistoryOpen READ undoHistoryOpen NOTIFY undoHistoryOpenChanged)
    // Pro Tools' Memory Locations window: [{id, number, name, start, end,
    // length, range}] for every marker, in time order, the times in the main
    // counter's format; `range` true for a selection marker.
    Q_PROPERTY(QVariantList markers READ markers NOTIFY markersChanged)
    Q_PROPERTY(bool memoryLocationsOpen READ memoryLocationsOpen NOTIFY memoryLocationsOpenChanged)
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY editStateChanged)
    Q_PROPERTY(QString editMode READ editMode NOTIFY editStateChanged)
    Q_PROPERTY(QString tool READ tool NOTIFY editStateChanged)
    Q_PROPERTY(QString gridValue READ gridValue NOTIFY editStateChanged)
    Q_PROPERTY(QString fadeShape READ fadeShape NOTIFY editStateChanged)
    Q_PROPERTY(QString crossfadeShape READ crossfadeShape NOTIFY editStateChanged)
    Q_PROPERTY(int selectedTrack READ selectedTrack NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionStart READ selectionStart NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionEnd READ selectionEnd NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionLength READ selectionLength NOTIFY selectionChanged)
    Q_PROPERTY(qreal scrollY READ scrollY NOTIFY viewChanged)
    Q_PROPERTY(qreal trackHeight READ trackHeight NOTIFY viewChanged)
    Q_PROPERTY(QString zoomText READ zoomText NOTIFY viewChanged)
    Q_PROPERTY(QList<bool> mutes READ mutes NOTIFY mixChanged)
    Q_PROPERTY(QList<bool> solos READ solos NOTIFY mixChanged)
    Q_PROPERTY(QList<bool> soloSafes READ soloSafes NOTIFY mixChanged)
    Q_PROPERTY(QList<bool> implicitMutes READ implicitMutes NOTIFY mixChanged)
    Q_PROPERTY(QList<qreal> gains READ gains NOTIFY mixChanged)
    Q_PROPERTY(QList<qreal> pans READ pans NOTIFY mixChanged)
    Q_PROPERTY(qreal busGain READ busGain NOTIFY mixChanged)
    Q_PROPERTY(bool anySolo READ anySolo NOTIFY mixChanged)
    Q_PROPERTY(QString mainCounter READ mainCounter NOTIFY counterChanged)
    Q_PROPERTY(QString mainCounterFormat READ mainCounterFormat NOTIFY counterChanged)
    Q_PROPERTY(QString subCounter READ subCounter NOTIFY counterChanged)
    Q_PROPERTY(QString subCounterFormat READ subCounterFormat NOTIFY counterChanged)
    // What the main counter takes typed, in its format.
    Q_PROPERTY(QString counterEntryHint READ counterEntryHint NOTIFY counterChanged)
    Q_PROPERTY(bool loopOn READ loopOn NOTIFY contentChanged)
    Q_PROPERTY(bool punchOn READ punchOn NOTIFY contentChanged)
    Q_PROPERTY(QString tempoText READ tempoText NOTIFY sessionChanged)
    // What the loader changed so that the open session's file still opened
    // (model::Repair): [{path, was, now, why}], one notice each, held until
    // the operator dismisses it or opens another session. A status line
    // would elide them and clear them in seconds; a value changed under
    // the operator must not be said only that briefly.
    Q_PROPERTY(QVariantList repairs READ repairs NOTIFY repairsChanged)

public:
    // `synthetic_peaks`: the waveforms `peaks` draws are not the sources'
    // audio, and the window must say so whenever there is a region to draw.
    SessionModel(TimelineDocument& doc, const PeakSource& peaks, bool synthetic_peaks,
                 QObject* parent = nullptr);

    // Where File > Save puts a new session and File > Open looks
    // (session_files.h). Set before the first save.
    void setSessionsDir(const QString& dir) { sessions_dir_ = dir; }
    const QString& sessionsDir() const { return sessions_dir_; }
    // The engine's SessionFolder (ADR-0018 §4): where it writes this
    // session's takes. A first Save goes there while the folder holds no
    // session file, as <folder>/<folder's name>.sonora, so the session file
    // is beside its audio. Empty, or a folder another session's file is
    // in: the sessions folder above, as before the engine said.
    void setEngineFolder(const QString& folder) { engine_folder_ = folder; }
    const QString& engineFolder() const { return engine_folder_; }

    // File > Save: to the file the session came from, or, the first time, to
    // a new folder named after the session ("Untitled" when it has no
    // name). A save with nothing changed since the last one writes nothing.
    // The engine writes the folder (TimelineDocument::save_session, ADR-0015
    // update of 2026-10-25): the bytes are serialised here and the file is
    // named here, and the save is done when the engine answers, which while
    // a take is open is after its next checkpoint.
    //
    // A session with inserts is first brought up to date with its running
    // plugins (TimelineDocument::flush_plugin_state, ADR-0012 §8): the file
    // is written only once the engine has answered, so it holds the values
    // moved in a plugin's own editor and the state each plugin has now.
    // Until then the save is waiting, and says so; a flush that could not
    // have every plugin's state still saves, and the notice names what is
    // not current.
    bool saveSession(std::string& why);
    // Waiting for the plugins' state, or for the engine to write the file.
    bool saveWaiting() const { return save_waiting_ || save_writing_; }
    // How long a save waits for the engine's FlushPluginState reply before
    // the operator is told, and a second Save may write without it. The
    // engine bounds each GetState itself (ADR-0012 §8) and answers well
    // inside this; past it the engine is wedged, and the session matters
    // more than one plugin's latest state. The same patience bounds the
    // wait for the engine's answer to the Save itself, after which a second
    // Save asks again. Tests shorten it.
    void setFlushPatienceMs(qint64 ms) { flush_patience_ms_ = ms; }
    // File > Open. Refused while a take is open, at another sample rate
    // than this session's, and over unsaved changes unless `discard`. A
    // file the loader repaired opens modified: what is shown is not what
    // the file holds, and the next save writes the current format knowingly.
    bool openSessionFile(const QString& file, bool discard, std::string& why);
    // The File > Open just made never reached the engine (its connection was
    // lost first). What is shown is the engine's document, not that file's,
    // so the window stops naming the file and Save never writes over it.
    void openWasLost();
    // The engine refused the File > Open just made, and kept its session,
    // which is shown again. The window names that session's file again,
    // with its saved bytes and repairs, so Save writes it and never the
    // refused file, and the engine's cleanup never runs in the refused
    // file's folder. `later_load`: another Open is in flight and shown, so
    // the window keeps naming it, and that one's refusal can only clear.
    void openWasRefused(const QString& why, bool later_load);

    // The interface's editing commands, installed before KeyRouter::prepare().
    void installCommands(CommandRegistry& local);
    // The track a target-less track command acts on: the selection.
    int selectedTrackForCommands() const { return ctl_.selection().track; }

    // An edit made beside the Edit controller (an insert slot) went to the
    // document: re-publish what changed, and say `why` when it was refused.
    bool reportEdit(bool ok, const std::string& why) { return run(ok, why); }

    // From ShellModel's tick, on the GUI thread, every tick.
    void onTransport(const TransportSnapshot& t, bool alive);
    // The document changed under the windows: the engine's update, another
    // interface's edit, or a refused edit taken back (ReplicaDocument).
    void documentUpdated();

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
    QString undoText() const;
    QString redoText() const;
    QVariantList undoHistory() const;
    int undoHistoryDepth() const { return static_cast<int>(doc_.history().undo_depth()); }
    bool undoHistoryTrimmed() const;
    bool undoHistoryOpen() const { return undo_history_open_; }
    void setUndoHistoryOpen(bool on);
    // A click on row `row` of undoHistory, as in Pro Tools: on a done step it
    // undoes that step and every later one; on an undone step it redoes up
    // to and including it.
    Q_INVOKABLE bool undoHistoryClick(int row);
    // Undo All and Redo All, from the window's buttons.
    Q_INVOKABLE bool undoHistoryAll(bool undo);
    QVariantList markers() const;
    bool memoryLocationsOpen() const { return memory_locations_open_; }
    void setMemoryLocationsOpen(bool on);
    // A row of `markers`: recalled (the selection moves to it), or cleared,
    // one undo step.
    Q_INVOKABLE bool recallMarker(int row);
    Q_INVOKABLE bool removeMarker(int row);
    // A click on the Markers ruler: on a marker it recalls it, and with Alt
    // clears it, as Pro Tools' Option-click does; elsewhere it places the
    // insertion point, as the other rulers do.
    Q_INVOKABLE void markerRulerClick(qreal x, bool alt);
    bool canPaste() const { return !ctl_.clipboard().empty(); }
    QString editMode() const { return QString::fromLatin1(edit_mode_name(ctl_.mode())); }
    QString tool() const { return QString::fromLatin1(tool_name(ctl_.tool())); }
    QString gridValue() const { return QString::fromLatin1(grid_value_name(ctl_.grid())); }
    QString fadeShape() const { return QString::fromLatin1(fade_shape_name(ctl_.fade_shape())); }
    QString crossfadeShape() const {
        return QString::fromLatin1(fade_shape_name(ctl_.crossfade_shape()));
    }
    int selectedTrack() const { return ctl_.selection().track; }
    QString selectionStart() const;
    QString selectionEnd() const;
    QString selectionLength() const;
    qreal scrollY() const { return ctl_.view().scroll_y; }
    qreal trackHeight() const { return ctl_.view().track_height; }
    QString zoomText() const;
    QList<bool> mutes() const;
    QList<bool> solos() const;
    QList<bool> soloSafes() const;
    // Per strip, whether another track's solo silences it: some track is
    // soloed, this one is not, and it is not solo safe. The engine's rule
    // (Session::process()), so the Mix window's outline says what is heard.
    QList<bool> implicitMutes() const;
    QList<qreal> gains() const;
    QList<qreal> pans() const;
    qreal busGain() const { return ctl_.snapshot().mix.bus.gain_db; }
    bool anySolo() const;
    QString mainCounter() const { return main_counter_; }
    QString mainCounterFormat() const { return QString::fromLatin1(time_format_name(main_fmt_)); }
    QString subCounter() const { return sub_counter_; }
    QString subCounterFormat() const { return QString::fromLatin1(time_format_name(sub_fmt_)); }
    QString counterEntryHint() const { return QString::fromLatin1(time_entry_hint(main_fmt_)); }
    bool loopOn() const;
    bool punchOn() const;
    QString tempoText() const;
    QVariantList repairs() const { return repairs_; }
    Q_INVOKABLE void dismissRepair(int index);

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
    // The shape the next fade, or crossfade, is made with: the no-dialog
    // fades use the last shape chosen, as Pro Tools' do.
    Q_INVOKABLE void cycleFadeShape();
    Q_INVOKABLE void cycleCrossfadeShape();
    Q_INVOKABLE void cycleMainCounter();
    // Counter entry (Pro Tools: click the main counter, type, Enter): `text`
    // in the main counter's format. The insertion point moves there and the
    // playhead follows through transport.locate. False, with the reason on
    // the status line, for text that is not a position, and during a take:
    // the engine refuses that locate, and the insertion point would have
    // moved under a counter that shows the playhead.
    Q_INVOKABLE bool enterCounter(const QString& text);
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
    // A strip's gain and pan in the document, by display track (-1: the
    // monitor bus's gain); 0 for a track that has no mix entry.
    float stripGain(int track) const;
    float stripPan(int track) const;
    // The track a strip shows (kNoId past the last), and where a track is
    // shown now (-1 when it is not), so a gesture can hold the track rather
    // than the strip it began on.
    model::Id trackId(int track) const;
    int trackIndex(model::Id id) const;
    // TimelineDocument::session_generation: changes when the document may
    // be another session, or the engine's copy again after a reconnection.
    uint64_t sessionGeneration() const { return doc_.session_generation(); }

    // Pixel geometry the timeline item draws, for the scene builders.
    void buildTimeline(Scene& out) const;
    void buildOverlay(std::vector<SceneRect>& out) const;
    void buildRuler(TimeFormat fmt, double height, Scene& out) const;
    void buildMarkerRuler(double height, Scene& out) const;
    void buildConductorRuler(ConductorRuler kind, double height, Scene& out) const;

signals:
    void windowChanged();
    void contentChanged();
    void editStateChanged();
    void selectionChanged();
    void viewChanged();
    void mixChanged();
    void counterChanged();
    void sessionChanged();
    void repairsChanged();
    void undoHistoryOpenChanged();
    void markersChanged();
    void memoryLocationsOpenChanged();
    // marker.add with no name, stopped: the window asks for one, offering
    // `name`, and sends marker.add again with it. `what` says where the
    // marker goes ("at 5|1|000", "from 5|1|000 to 9|1|000").
    void markerRequested(const QString& name, const QString& what, bool range);
    // A refusal from a mouse gesture, for the status line. Keys report
    // through the router.
    void refused(const QString& why);
    // Something done that the operator should see confirmed: "Saved ...".
    void notice(const QString& text);
    // Why an Open did not happen, held until the next Open (empty).
    void openNotice(const QString& text);
    // edit.repeat without a count, with something to repeat: the window
    // asks for the number, offering `count` (the last one used), and sends
    // edit.repeat again with it. `what` says what one copy is ("2.000 s of
    // Gtr"), so the number is asked about something the operator can see.
    void repeatRequested(int count, const QString& what);

private:
    // Re-publishes what changed since the last call and marks the items.
    void sync();
    bool run(bool ok, const std::string& why);
    void updateCounters(bool force);
    // The save itself, once the document holds what the file must;
    // `caution` is said with the result. True when it was done, or handed to
    // the engine, whose answer is then said on its own.
    bool writeSession(std::string& why, std::string caution);
    // What a save the engine has answered leaves behind.
    struct Written {
        QString file;
        std::string bytes;
        uint64_t generation = 0;
        uint64_t revision = 0;   // the document's when the bytes were made
        bool rename = false;
        model::SessionSettings settings;   // the name and id the file holds, when renamed
        std::string caution;
    };
    bool finishSave(bool ok, const std::string& message, Written w, std::string& why);

    TimelineDocument& doc_;
    const PeakSource& peaks_;
    bool synthetic_peaks_;
    QString sessions_dir_;
    QString engine_folder_;
    // What the saves of this window have hashed, so serialize() hashes only
    // what changed since (ADR-0015 §3).
    model::SequenceDigests digests_;
    QString file_;                 // empty until the session is saved or opened
    uint64_t saved_revision_ = 0;  // the revision the file holds
    uint64_t save_generation_ = 0;
    std::string saved_bytes_;      // what the file holds, to skip a save that changes nothing
    bool save_waiting_ = false;    // a save waits for the plugins' state
    bool save_writing_ = false;    // a save waits for the engine to write it
    uint64_t write_ticket_ = 0;    // which save the engine's answer is for
    QElapsedTimer write_wait_;
    uint64_t save_ticket_ = 0;     // which save a flush answers, so an abandoned one is ignored
    QElapsedTimer save_wait_;
    qint64 flush_patience_ms_ = 10'000;
    EditController ctl_;
    std::vector<SceneItem*> items_;
    QString window_ = QStringLiteral("edit");
    bool undo_history_open_ = false;
    bool memory_locations_open_ = false;

    QVariantList repairs_;
    void setRepairs(QVariantList r);

    // What the window named before the Open the engine has not answered,
    // given back if the engine refuses it.
    struct BeforeOpen {
        bool valid = false;
        QString opened;   // the file that Open named
        QString file;
        uint64_t saved_revision = 0;
        uint64_t save_generation = 0;
        std::string saved_bytes;
        QVariantList repairs;
    };
    BeforeOpen before_open_;

    TransportOverlay overlay_;
    TimeFormat main_fmt_ = TimeFormat::BarsBeats;
    TimeFormat sub_fmt_ = TimeFormat::MinSec;
    QString main_counter_, sub_counter_;
    unsigned transport_ticks_ = 0;

    // What was last published, to emit only on change.
    uint64_t seen_revision_ = UINT64_MAX;
    bool seen_dirty_ = false;
    int seen_mode_ = -1, seen_tool_ = -1, seen_grid_ = -1, seen_fades_ = -1;
    uint64_t seen_clip_ = UINT64_MAX;
    Selection seen_sel_;
    bool seen_sel_valid_ = false;
    ViewState seen_view_;
    bool seen_view_valid_ = false;
    Drag seen_drag_;
};

}  // namespace sonora::ui
