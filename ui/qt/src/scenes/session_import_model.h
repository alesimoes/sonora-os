// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import Session Data panel's model (ADR-0123 item 6): the source session's
// tracks with the operator's ticks and request flags, the copy running on a
// worker, and the Compound applied only when every file is in place.
//
// Planner, copier and applier are injected interfaces so the model is tested
// with fakes and without a disk. A refusal or a failed copy is a held notice
// (`refusal`), never the status line: the operator waited and must read why.
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <atomic>
#include <future>
#include <set>
#include <vector>

#include "sonora/session_files/session_import.h"
#include "sonora/session_files/session_import_copy.h"

namespace sonora::ui {

class ImportPlanner {
public:
    virtual ~ImportPlanner() = default;
    // Opens `folder` read-only; false with `error` when it is not a session.
    virtual bool open_source(const std::string& folder, std::string& error) = 0;
    virtual std::vector<session_files::ImportableTrack> tracks() = 0;
    virtual bool plan(const session_files::SessionImportRequest& request,
                      session_files::ImportPlan& plan,
                      session_files::ImportRefusal& refusal) = 0;
};

class ImportCopier {
public:
    virtual ~ImportCopier() = default;
    // Blocking; runs on the worker thread.
    virtual session_files::CopyOutcome copy(const session_files::ImportPlan& plan,
                                            const std::atomic<bool>& cancel,
                                            const session_files::CopyProgressFn& progress,
                                            session_files::CopyReport& report) = 0;
};

class ImportApplier {
public:
    virtual ~ImportApplier() = default;
    virtual bool take_open() = 0;
    virtual bool apply(const model::Compound& ops) = 0;
};

class SessionImportModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY tracksChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY tracksChanged)
    Q_PROPERTY(bool material READ material WRITE setMaterial NOTIFY optionsChanged)
    Q_PROPERTY(bool mix READ mix WRITE setMix NOTIFY optionsChanged)
    Q_PROPERTY(bool inserts READ inserts WRITE setInserts NOTIFY optionsChanged)
    Q_PROPERTY(bool automation READ automation WRITE setAutomation NOTIFY optionsChanged)
    Q_PROPERTY(bool markers READ markers WRITE setMarkers NOTIFY optionsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString refusal READ refusal NOTIFY refusalChanged)
    Q_PROPERTY(QString notes READ notes NOTIFY tracksChanged)

public:
    SessionImportModel(ImportPlanner& planner, ImportCopier& copier, ImportApplier& applier,
                       QObject* parent = nullptr);
    ~SessionImportModel() override;

    QVariantList tracks() const;
    int selectedCount() const { return static_cast<int>(chosen_.size()); }
    bool material() const { return req_.material; }
    bool mix() const { return req_.mix; }
    bool inserts() const { return req_.inserts; }
    bool automation() const { return req_.automation; }
    bool markers() const { return req_.markers; }
    void setMaterial(bool v) { set(req_.material, v); }
    void setMix(bool v) { set(req_.mix, v); }
    void setInserts(bool v) { set(req_.inserts, v); }
    void setAutomation(bool v) { set(req_.automation, v); }
    void setMarkers(bool v) { set(req_.markers, v); }
    bool busy() const { return running_; }
    int progress() const { return progress_.load(); }
    QString refusal() const { return refusal_; }
    // What arrives dropped or changed, one per line, from the last plan.
    QString notes() const { return notes_; }

    // Reads the source session's track list; false with a held refusal.
    Q_INVOKABLE bool openSource(const QString& folder);
    Q_INVOKABLE void toggle(int id);
    // Plans, then copies on a worker; false when refused before any copy.
    Q_INVOKABLE bool startImport();
    Q_INVOKABLE void cancelImport();
    Q_INVOKABLE void dismissRefusal() { setRefusal(QString()); }

    // Waits for the worker and applies, as the poll would. Tests, and the
    // app's timer.
    bool finishNow();

signals:
    void tracksChanged();
    void optionsChanged();
    void busyChanged();
    void progressChanged();
    void refusalChanged();
    void imported(int tracks);

private:
    void set(bool& field, bool v);
    void setRefusal(const QString& why);
    void conclude(session_files::CopyOutcome outcome);

    ImportPlanner& planner_;
    ImportCopier& copier_;
    ImportApplier& applier_;
    session_files::SessionImportRequest req_;
    std::vector<session_files::ImportableTrack> rows_;
    std::vector<model::Id> order_;
    std::set<model::Id> chosen_;
    session_files::ImportPlan plan_;
    session_files::CopyReport report_;
    std::future<session_files::CopyOutcome> job_;
    std::atomic<bool> cancel_{false};
    std::atomic<int> progress_{0};
    bool running_ = false;
    QString refusal_;
    QString notes_;
    QString folder_;
};

}  // namespace sonora::ui
