// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Import Audio browser's model (ui.editing.import): where the operator
// is browsing, what is chosen, and the import in progress. The appliance has
// no file manager, so the browser is ours; it starts on the session's own
// audio and offers the places audio arrives from (home, the sessions folder,
// removable media).
//
// A refusal is held in `refusal` until dismissed, not put on the 2.5 s
// status line: the operator chose files, waited for a conversion, and must
// be told why nothing came of it even if they looked away.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <functional>
#include <set>
#include <string>

#include "audio_importer.h"

namespace sonora::ui {

class ImportModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString folder READ folder NOTIFY folderChanged)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
    Q_PROPERTY(QVariantList places READ places NOTIFY entriesChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY entriesChanged)
    Q_PROPERTY(bool atInsertion READ atInsertion WRITE setAtInsertion NOTIFY optionsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString status READ status NOTIFY progressChanged)
    Q_PROPERTY(QString refusal READ refusal NOTIFY refusalChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY entriesChanged)

public:
    struct Hooks {
        std::function<QString()> session_folder;  // where the engine writes this session
        std::function<QString()> sessions_dir;
        std::function<bool()> recording;
    };

    ImportModel(EditController& ctl, Hooks hooks, QObject* parent = nullptr);

    QString folder() const { return folder_; }
    QVariantList entries() const { return entries_; }
    QVariantList places() const;
    int selectedCount() const { return static_cast<int>(chosen_.size()); }
    bool atInsertion() const { return at_insertion_; }
    void setAtInsertion(bool on);
    bool busy() const { return importer_.state() == AudioImporter::State::Running; }
    int progress() const { return importer_.progress(); }
    QString status() const;
    QString refusal() const { return refusal_; }
    // What a Import would do, in words, for the button's neighbour.
    QString summary() const;

    // The browser opens: on the session's audio when there is one.
    Q_INVOKABLE void show();
    Q_INVOKABLE void open(const QString& path);  // a folder
    Q_INVOKABLE void up();
    Q_INVOKABLE void toggle(const QString& path);
    Q_INVOKABLE void clearSelection();
    // Converts and places the chosen files; false when refused (refusal says why).
    Q_INVOKABLE bool startImport();
    Q_INVOKABLE void cancelImport();
    Q_INVOKABLE void dismissRefusal() { setRefusal(QString()); }

    // Tests: waits for the worker and commits, as the timer would.
    bool finishNow();

signals:
    void folderChanged();
    void entriesChanged();
    void optionsChanged();
    void busyChanged();
    void progressChanged();
    void refusalChanged();
    // Placed: `tracks` new tracks, one undo step.
    void imported(int tracks);
    void heldNotice(const QString& text);

private:
    void list();
    void poll();
    void setRefusal(const QString& why);

    EditController& ctl_;
    Hooks hooks_;
    AudioImporter importer_;
    QTimer timer_;
    QString folder_;
    QVariantList entries_;
    std::set<QString> chosen_;
    QStringList order_;  // chosen files in the order they were chosen
    bool at_insertion_ = false;
    QString refusal_;
    int files_ = 0;
};

}  // namespace sonora::ui
