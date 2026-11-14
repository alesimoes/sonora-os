// SPDX-License-Identifier: GPL-3.0-or-later
#include "step_sequencer_model.h"

#include <QTimer>
#include <algorithm>

#include "sonora/stepseq/pattern_publisher.h"

namespace sonora::ui {

StepSequencerModel::StepSequencerModel(Publisher publish, QObject* parent)
    : QObject(parent), publish_(std::move(publish)) {}

StepSequencerModel::StepSequencerModel(stepseq::PatternExchange& exchange, QObject* parent)
    : QObject(parent),
      publisher_(std::make_unique<stepseq::PatternPublisher>(exchange)),
      retry_timer_(new QTimer(this)) {
    retry_timer_->setInterval(kRetryIntervalMs);
    connect(retry_timer_, &QTimer::timeout, this, &StepSequencerModel::retryPublish);
    // The exchange starts on an empty pattern; hand it ours from the start.
    publish_out();
}

StepSequencerModel::~StepSequencerModel() = default;

bool StepSequencerModel::publishPending() const {
    return publisher_ && publisher_->pending();
}

void StepSequencerModel::publish_out() {
    if (!publisher_) {
        if (publish_) publish_(pattern_);
        return;
    }
    if (!publisher_->submit(pattern_)) retry_timer_->start();
}

void StepSequencerModel::retryPublish() {
    if (publisher_ && publisher_->retry(pattern_)) retry_timer_->stop();
}

bool StepSequencerModel::valid(int row, int step) const {
    return valid_row(row) && step >= 0 && step < pattern_.length();
}

void StepSequencerModel::commit() {
    ++revision_;
    publish_out();
    emit changed();
}

void StepSequencerModel::edit_selected(const std::function<void(stepseq::Step&)>& f) {
    if (!valid(row_, step_)) return;
    stepseq::Step s = pattern_.step(row_, step_);
    f(s);
    pattern_.set_step(row_, step_, s);
    commit();
}

bool StepSequencerModel::selectedOn() const {
    return valid(row_, step_) && pattern_.step(row_, step_).on;
}
int StepSequencerModel::velocity() const {
    return valid(row_, step_) ? pattern_.step(row_, step_).velocity : 0;
}
int StepSequencerModel::probability() const {
    return valid(row_, step_) ? pattern_.step(row_, step_).probability : 0;
}
int StepSequencerModel::pitch() const {
    return valid(row_, step_) ? pattern_.step(row_, step_).pitch_semitones : 0;
}

void StepSequencerModel::setLength(int steps) {
    pattern_.set_length(steps);
    // A shorter pattern can leave the selection beyond its end.
    if (row_ >= 0 && !valid(row_, step_)) {
        row_ = step_ = -1;
        emit selectionChanged();
    }
    commit();
}

void StepSequencerModel::setSwing(int percent) {
    pattern_.set_swing_percent(percent);
    commit();
}

void StepSequencerModel::setVelocity(int v) {
    edit_selected([v](stepseq::Step& s) {
        s.velocity = static_cast<std::uint8_t>(std::max(0, std::min(stepseq::kMaxVelocity, v)));
    });
}
void StepSequencerModel::setProbability(int p) {
    edit_selected([p](stepseq::Step& s) {
        s.probability =
            static_cast<std::uint8_t>(std::max(0, std::min(stepseq::kMaxProbability, p)));
    });
}
void StepSequencerModel::setPitch(int semitones) {
    edit_selected([semitones](stepseq::Step& s) {
        const int m = stepseq::kMaxPitchSemitones;
        s.pitch_semitones = static_cast<std::int8_t>(std::max(-m, std::min(m, semitones)));
    });
}

bool StepSequencerModel::stepOn(int row, int step) const {
    return valid(row, step) && pattern_.step(row, step).on;
}

void StepSequencerModel::toggleStep(int row, int step) {
    if (!valid(row, step)) return;
    stepseq::Step s = pattern_.step(row, step);
    s.on = !s.on;
    pattern_.set_step(row, step, s);
    row_ = row;
    step_ = step;
    emit selectionChanged();
    commit();
}

void StepSequencerModel::select(int row, int step) {
    if (!valid(row, step) || (row == row_ && step == step_)) return;
    row_ = row;
    step_ = step;
    emit selectionChanged();
    emit changed();
}

bool StepSequencerModel::rowMuted(int row) const {
    return valid_row(row) && pattern_.row(row).mute;
}
bool StepSequencerModel::rowSoloed(int row) const {
    return valid_row(row) && pattern_.row(row).solo;
}
bool StepSequencerModel::rowHasSource(int row) const {
    return valid_row(row) && pattern_.row(row).source != stepseq::kNoSource;
}

void StepSequencerModel::toggleMute(int row) {
    if (!valid_row(row)) return;
    stepseq::Row r = pattern_.row(row);
    r.mute = !r.mute;
    pattern_.set_row(row, r);
    commit();
}
void StepSequencerModel::toggleSolo(int row) {
    if (!valid_row(row)) return;
    stepseq::Row r = pattern_.row(row);
    r.solo = !r.solo;
    pattern_.set_row(row, r);
    commit();
}

QString StepSequencerModel::rowName(int row) const {
    return QStringLiteral("Lane %1").arg(row + 1);
}

void StepSequencerModel::setRowSource(int row, stepseq::SourceId source) {
    if (!valid_row(row)) return;
    stepseq::Row r = pattern_.row(row);
    r.source = source;
    pattern_.set_row(row, r);
    commit();
}

}  // namespace sonora::ui
