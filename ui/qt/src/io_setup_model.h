// SPDX-License-Identifier: GPL-3.0-or-later
//
// Setup > I/O Setup (Pro Tools Reference Guide, I/O Setup): the hardware
// inputs and outputs by name, and each audio track's input picked from
// those names.
//
// What exists is the engine's to say. The inputs are the ones the state
// block says are connected (inputs_connected: 18 on the UMC1820 and on the
// fake engine's stand-in for it), the outputs the ones the engine opened
// (its Latency event's output count). While the engine is not answering
// the window lists no input rather than guessing at how many there are,
// and refuses a pick.
//
// A track's input is TrackMix::input, mix state: saved with the session and
// not undone, as a fader is (owner question O5). A pick goes through the
// TimelineDocument as SetMix and reports through SessionModel. A track whose
// input is past the last connected one says so in its row: a meter at zero
// there is not silence.
//
// The engine keeps an armed track's input fixed for the take it is
// recording (PlaybackController::apply_mix), so a pick on such a track while
// a take is open is refused here; sent, it would show an input the take is
// not being recorded from.
//
// Output paths (ADR-0060) are aux buses that play on a pair of the opened
// outputs: named here, on the Output page, and picked as a strip's output in
// the Mix window like any bus. Making, naming, moving and removing one are
// operations on the document (undoable, saved with the session), refused by
// the model for a pair another path or the Master plays on, and here for a
// pair the interface does not have.
//
// A refusal is held in the window until the next action, not left to the
// status line, which clears before anyone has read it. It is cleared when
// another session opens.
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <functional>
#include <string>

#include "io_names.h"
#include "sonora/model/model.h"

namespace sonora::ui {

class SessionModel;
class TimelineDocument;

class IoSetupModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool open READ open WRITE setOpen NOTIFY openChanged)
    // False while the engine is not answering: no input is listed then.
    Q_PROPERTY(bool known READ known NOTIFY changed)
    // [{number, name, renamed}], one per connected input / opened output.
    Q_PROPERTY(QVariantList inputs READ inputs NOTIFY changed)
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY changed)
    // [{row, name, audio, channels, input, inputText, connected, note}] for
    // every track in display order. `input` is the first input's index
    // from 0, -1 for a MIDI or instrument track; `note` says why a row
    // cannot record what its input shows, empty when it can.
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY changed)
    // [{row, name, pair, pairText, strips, connected, note}], the output
    // paths in display order; `pair` is the left output from 0.
    Q_PROPERTY(QVariantList paths READ paths NOTIFY changed)
    // [{pair, text, taken, by}], one per pair of opened outputs: `by` names
    // the Master or the path that plays on it, empty when free.
    Q_PROPERTY(QVariantList pairs READ pairs NOTIFY changed)
    Q_PROPERTY(QString refusal READ refusal NOTIFY refusalChanged)

public:
    // How many inputs are connected, -1 when the engine is not answering;
    // how many outputs the engine opened, 0 when not known yet.
    using Count = std::function<int()>;
    // Whether a take is being recorded, and whether engine track `index` is
    // armed (the engine's order, edit->tracks').
    using Recording = std::function<bool()>;
    using Armed = std::function<bool(int index)>;

    IoSetupModel(TimelineDocument& doc, SessionModel& session, QObject* parent = nullptr);

    void setEngine(Count inputs, Count outputs, Recording recording, Armed armed);
    // Where the names are kept (io_names_path()); empty: not kept.
    void setNamesFile(const std::string& path);
    const IoNames& names() const { return names_; }

    bool open() const { return open_; }
    void setOpen(bool on);
    bool known() const { return input_count_ >= 0; }
    QVariantList inputs() const { return inputs_; }
    QVariantList outputs() const { return outputs_; }
    QVariantList tracks() const { return tracks_; }
    QVariantList paths() const { return paths_; }
    QVariantList pairs() const { return pairs_; }
    QString refusal() const { return refusal_; }

    // Names input or output `index` (from 0). An empty name gives it its
    // default back. False, with the reason held, when refused.
    Q_INVOKABLE bool renameInput(int index, const QString& name);
    Q_INVOKABLE bool renameOutput(int index, const QString& name);
    // Track `row` (display order) records from input `input` (from 0) and,
    // for a stereo track, the one after it.
    Q_INVOKABLE bool setTrackInput(int row, int input);
    // A new output path called `name` (empty: named for its pair) on the
    // pair whose left output is `pair`, from 0.
    Q_INVOKABLE bool addPath(const QString& name, int pair);
    Q_INVOKABLE bool setPathPair(int row, int pair);
    Q_INVOKABLE bool renamePath(int row, const QString& name);
    // Strips routed to the path go back to the Master, in the same step.
    Q_INVOKABLE bool removePath(int row);
    Q_INVOKABLE void dismissRefusal() { setRefusal(QString()); }
    // Reads the engine's counts and the session again.
    Q_INVOKABLE void refresh();

    // The same as setTrackInput(), with the reason in `why`.
    bool pickInput(int row, int input, std::string& why);
    bool makePath(const std::string& name, int pair, std::string& why);
    bool movePath(int row, int pair, std::string& why);
    bool namePath(int row, const std::string& name, std::string& why);
    bool dropPath(int row, std::string& why);
    bool rename(IoSide side, int index, const std::string& name, std::string& why);

signals:
    void openChanged();
    void changed();
    void refusalChanged();

private:
    bool done(bool ok, const std::string& why);
    void setRefusal(const QString& r);
    QString inputText(int first, int channels) const;
    QString pairText(int first) const;
    void buildPaths(QVariantList& paths, QVariantList& pairs) const;
    // The path at display row `row`, or null with `why` set.
    const model::Bus* pathAt(int row, std::string& why) const;
    bool pairProblem(int pair, std::string& why) const;

    TimelineDocument& doc_;
    SessionModel& session_;
    Count inputs_fn_;
    Count outputs_fn_;
    Recording recording_fn_;
    Armed armed_fn_;
    std::string names_file_;
    IoNames names_;
    bool open_ = false;
    int input_count_ = -1;
    int output_count_ = 0;
    QVariantList inputs_;
    QVariantList outputs_;
    QVariantList tracks_;
    QVariantList paths_;
    QVariantList pairs_;
    QString refusal_;
    // While the window is open: an input unpatched, or the engine gone,
    // shows within a second.
    QTimer poll_;
};

}  // namespace sonora::ui
