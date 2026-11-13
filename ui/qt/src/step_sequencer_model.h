// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Step Sequencer window's model (logic.step_sequencer, ADR-0169): the
// editing copy of a stepseq::Pattern. The window edits this copy; every edit
// is handed to the injected publisher, which in the application puts it in
// the PatternExchange the audio thread reads. The model never touches the
// audio thread itself.
//
// Setters clamp, as the pattern does, and every read goes back to the
// pattern, so a control cannot show a value the player will not play.
#pragma once

#include <QObject>
#include <QString>
#include <functional>

#include "sonora/stepseq/pattern.h"

namespace sonora::ui {

class StepSequencerModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(int rows READ rows NOTIFY changed)
    Q_PROPERTY(int length READ length WRITE setLength NOTIFY changed)
    Q_PROPERTY(int swing READ swing WRITE setSwing NOTIFY changed)
    Q_PROPERTY(int stepsPerBeat READ stepsPerBeat NOTIFY changed)
    Q_PROPERTY(int selectedRow READ selectedRow NOTIFY selectionChanged)
    Q_PROPERTY(int selectedStep READ selectedStep NOTIFY selectionChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedOn READ selectedOn NOTIFY changed)
    Q_PROPERTY(int velocity READ velocity WRITE setVelocity NOTIFY changed)
    Q_PROPERTY(int probability READ probability WRITE setProbability NOTIFY changed)
    Q_PROPERTY(int pitch READ pitch WRITE setPitch NOTIFY changed)
    // Bumped on every edit, so a cell's binding re-reads its state.
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    using Publisher = std::function<void(const stepseq::Pattern&)>;

    explicit StepSequencerModel(Publisher publish = {}, QObject* parent = nullptr);

    int rows() const { return pattern_.row_count(); }
    int length() const { return pattern_.length(); }
    int swing() const { return pattern_.swing_percent(); }
    int stepsPerBeat() const { return pattern_.steps_per_beat(); }
    int selectedRow() const { return row_; }
    int selectedStep() const { return step_; }
    bool hasSelection() const { return row_ >= 0; }
    bool selectedOn() const;
    int velocity() const;
    int probability() const;
    int pitch() const;
    int revision() const { return revision_; }

    void setLength(int steps);
    void setSwing(int percent);
    void setVelocity(int v);
    void setProbability(int p);
    void setPitch(int semitones);

    Q_INVOKABLE bool stepOn(int row, int step) const;
    Q_INVOKABLE void toggleStep(int row, int step);
    Q_INVOKABLE void select(int row, int step);
    Q_INVOKABLE bool rowMuted(int row) const;
    Q_INVOKABLE bool rowSoloed(int row) const;
    Q_INVOKABLE void toggleMute(int row);
    Q_INVOKABLE void toggleSolo(int row);
    // A row with no source plays nothing; the window says so.
    Q_INVOKABLE bool rowHasSource(int row) const;
    Q_INVOKABLE QString rowName(int row) const;
    // Gives a row something to trigger (the demo and the importer use it).
    void setRowSource(int row, stepseq::SourceId source);

    const stepseq::Pattern& pattern() const { return pattern_; }

signals:
    void changed();
    void selectionChanged();

private:
    bool valid(int row, int step) const;
    bool valid_row(int row) const { return row >= 0 && row < pattern_.row_count(); }
    void edit_selected(const std::function<void(stepseq::Step&)>& f);
    void commit();

    stepseq::Pattern pattern_;
    Publisher publish_;
    int row_ = -1;
    int step_ = -1;
    int revision_ = 0;
};

}  // namespace sonora::ui
