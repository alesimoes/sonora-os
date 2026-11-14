// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_import_model.h"

#include <QVariantMap>
#include <algorithm>

namespace sonora::ui {

using session_files::CopyOutcome;

SessionImportModel::SessionImportModel(ImportPlanner& planner, ImportCopier& copier,
                                       ImportApplier& applier, QObject* parent)
    : QObject(parent), planner_(planner), copier_(copier), applier_(applier) {}

SessionImportModel::~SessionImportModel() {
    cancel_ = true;
    if (job_.valid()) job_.wait();
}

QVariantList SessionImportModel::tracks() const {
    QVariantList out;
    for (const auto& r : rows_) {
        QVariantMap m;
        m["id"] = static_cast<qlonglong>(r.id);
        m["name"] = QString::fromStdString(r.name);
        m["channels"] = r.channels;
        m["regions"] = static_cast<qlonglong>(r.regions);
        m["importable"] = r.importable;
        m["why"] = QString::fromStdString(r.why);
        m["chosen"] = chosen_.count(r.id) > 0;
        out.push_back(m);
    }
    return out;
}

void SessionImportModel::set(bool& field, bool v) {
    if (field == v) return;
    field = v;
    emit optionsChanged();
}

void SessionImportModel::setRefusal(const QString& why) {
    if (refusal_ == why) return;
    refusal_ = why;
    emit refusalChanged();
}

bool SessionImportModel::openSource(const QString& folder) {
    std::string error;
    rows_.clear();
    chosen_.clear();
    order_.clear();
    notes_.clear();
    if (!planner_.open_source(folder.toStdString(), error)) {
        setRefusal(QString::fromStdString(error));
        emit tracksChanged();
        return false;
    }
    folder_ = folder;
    rows_ = planner_.tracks();
    setRefusal(QString());
    emit tracksChanged();
    return true;
}

void SessionImportModel::toggle(int id) {
    const auto it = std::find_if(rows_.begin(), rows_.end(),
                                 [&](const auto& r) { return r.id == static_cast<model::Id>(id); });
    // A greyed row stays unticked: ticking it would only fail the plan later.
    if (it == rows_.end() || !it->importable) return;
    const auto key = it->id;
    if (chosen_.erase(key)) {
        order_.erase(std::remove(order_.begin(), order_.end(), key), order_.end());
    } else {
        chosen_.insert(key);
        order_.push_back(key);
    }
    emit tracksChanged();
}

bool SessionImportModel::startImport() {
    if (running_) return false;
    session_files::SessionImportRequest req = req_;
    req.tracks = order_;
    req.source_folder = folder_.toStdString();
    req.take_open = applier_.take_open();
    session_files::ImportRefusal refusal;
    plan_ = {};
    if (!planner_.plan(req, plan_, refusal)) {
        setRefusal(QString::fromStdString(refusal.message));
        return false;
    }
    QStringList lines;
    for (const auto& s : plan_.dropped) lines << QString::fromStdString(s);
    for (const auto& s : plan_.approximated) lines << QString::fromStdString(s);
    notes_ = lines.join('\n');
    setRefusal(QString());
    cancel_ = false;
    progress_ = 0;
    running_ = true;
    report_ = {};
    job_ = std::async(std::launch::async, [this] {
        return copier_.copy(plan_, cancel_, [this](const session_files::CopyProgress& p) {
            progress_ = p.bytes_total ? static_cast<int>(p.bytes_done * 100 / p.bytes_total) : 100;
        }, report_);
    });
    emit busyChanged();
    return true;
}

void SessionImportModel::cancelImport() { cancel_ = true; }

bool SessionImportModel::finishNow() {
    if (!running_) return false;
    const CopyOutcome outcome = job_.get();
    running_ = false;
    emit busyChanged();
    conclude(outcome);
    return outcome == CopyOutcome::Done && refusal_.isEmpty();
}

void SessionImportModel::conclude(CopyOutcome outcome) {
    if (outcome == CopyOutcome::Cancelled) return;  // the copier removed its files
    if (outcome != CopyOutcome::Done) {
        setRefusal(QString::fromStdString(report_.message));
        return;
    }
    // Applied only after every file is in place; a refusal here leaves the
    // copied files unreferenced, which the next save ignores.
    if (!applier_.apply(plan_.ops)) {
        setRefusal(QStringLiteral("The session changed while the audio was copied; nothing was imported."));
        return;
    }
    progress_ = 100;
    emit imported(static_cast<int>(order_.size()));
}

}  // namespace sonora::ui
