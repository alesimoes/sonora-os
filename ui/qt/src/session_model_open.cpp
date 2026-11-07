// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_model.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "fader_taper.h"
#include "scene_item.h"
#include "edit_clipboard.h"
#include "session_files.h"
#include "sonora/api/consolidate_argument.h"
#include "sonora/api/messages.h"
#include "sonora/api/render.h"
#include "sonora/command.h"
#include "sonora/model/render_in_place.h"
#include "sonora/model/session_file.h"
#include "sonora/model/session_template.h"
#include "transport_model.h"
#include "session_model_support.h"

namespace sonora::ui {

using namespace model;

bool SessionModel::openSessionFile(const QString& file, bool discard, std::string& why) {
    if (overlay_.recording) {
        why = "a take is open; stop recording before opening another session";
        return false;
    }
    if (save_waiting_) {
        why = "a save is waiting for the plugins' state; open the session once it is saved";
        return false;
    }
    if (save_writing_) {
        why = "the engine is still writing a save; open the session once it is saved";
        return false;
    }
    if (dirty() && !discard) {
        why = "this session has unsaved edits; save it first, or open with \"Discard edits\"";
        return false;
    }
    std::string bytes;
    model::Loaded loaded;
    if (!read_session_file(file, bytes, loaded, why)) return false;
    const uint32_t rate = ctl_.snapshot().settings.sample_rate;
    if (loaded.snapshot.settings.sample_rate != rate) {
        why = "the session is at " + std::to_string(loaded.snapshot.settings.sample_rate) +
              " Hz and this system runs at " + std::to_string(rate) +
              " Hz; it cannot play here until sample-rate conversion exists";
        return false;
    }
    const std::shared_ptr<const model::SourcePool> pool = loaded.snapshot.sources;
    const std::string path = QFileInfo(file).absoluteFilePath().toStdString();
    if (!doc_.reset(std::move(loaded.snapshot), path, why)) {
        emit openNotice(file + QStringLiteral(" was not opened: ") + q(why));
        return false;
    }
    ctl_.document_replaced();
    live_.clear();
    // Another session now: a New from Template's Save As still waiting is not its.
    after_open_.reset();
    making_from_template_ = false;
    before_open_.valid = true;
    before_open_.opened = file;
    before_open_.file = file_;
    before_open_.saved_revision = saved_revision_;
    before_open_.save_generation = save_generation_;
    before_open_.saved_bytes = saved_bytes_;
    before_open_.repairs = repairs_;
    emit openNotice(QString());
    file_ = file;
    // A repaired session is not the file on disk: it reads as modified, so
    // Save writes it and another Open asks first.
    saved_revision_ = loaded.repairs.empty() ? doc_.revision() : UINT64_MAX;
    save_generation_ = loaded.save_generation;
    saved_bytes_ = std::move(bytes);
    seen_dirty_ = false;
    // Everything the windows show came from the old session.
    seen_revision_ = UINT64_MAX;
    seen_sel_valid_ = seen_view_valid_ = false;
    updateCounters(true);
    sync();
    emit sessionChanged();
    emit sessionOpened();
    QVariantList repairs;
    for (const model::Repair& r : loaded.repairs) {
        QVariantMap m;
        m.insert(QStringLiteral("path"), q(r.path));
        m.insert(QStringLiteral("was"), q(r.was));
        m.insert(QStringLiteral("now"), q(r.now));
        m.insert(QStringLiteral("why"), q(r.why));
        repairs << m;
    }
    // O2: each missing sequence file shown as a repair is, held until
    // dismissed. Its regions opened silent with their windows; nothing is
    // dropped, and a save names the file again.
    for (const model::MissingSequence& ms : loaded.missing_sequences) {
        std::string where = ms.regions.front();
        if (ms.regions.size() > 1) where += " and " + std::to_string(ms.regions.size() - 1) + " more";
        std::string why_missing = ms.why;
        const model::MidiSource* take =
            ms.sources.empty() ? nullptr : pool->find_midi(ms.sources.front());
        if (take != nullptr) why_missing += "; its take " + take->path + " is kept";
        QVariantMap m;
        m.insert(QStringLiteral("path"), q(where));
        m.insert(QStringLiteral("was"), q(ms.ref.file));
        m.insert(QStringLiteral("now"), QStringLiteral("MIDI data missing: silent, not editable"));
        m.insert(QStringLiteral("why"), q(why_missing));
        repairs << m;
    }
    setRepairs(std::move(repairs));
    QString said = QStringLiteral("Opened ") + file_;
    if (!loaded.repairs.empty()) {
        said += QStringLiteral(", with %1 value%2 repaired to open (listed below); save to keep them")
                    .arg(loaded.repairs.size())
                    .arg(loaded.repairs.size() == 1 ? QString() : QStringLiteral("s"));
    }
    if (!loaded.missing_sequences.empty()) {
        said += QStringLiteral(", with %1 MIDI sequence file%2 missing (listed below)")
                    .arg(loaded.missing_sequences.size())
                    .arg(loaded.missing_sequences.size() == 1 ? QString() : QStringLiteral("s"));
    }
    emit notice(said);
    // Offered, never applied silently (session-format.md, "Autosave"), and
    // held: the engine wrote it after the last save, so it may hold the edits
    // a crash took.
    QString autosave;
    if (newer_autosave(file_, save_generation_, autosave)) {
        emit openNotice(QStringLiteral("An autosave newer than this file is in ") + autosave +
                        QStringLiteral(": open it to get back the edits made after the last save"));
    }
    return true;
}

void SessionModel::setRepairs(QVariantList r) {
    if (r.isEmpty() && repairs_.isEmpty()) return;
    repairs_ = std::move(r);
    emit repairsChanged();
}

void SessionModel::dismissRepair(int index) {
    if (index < 0 || index >= repairs_.size()) return;
    repairs_.removeAt(index);
    emit repairsChanged();
}

void SessionModel::openWasRefused(const QString& why, bool later_load) {
    // A New from Template's Save As is for the session the engine refused,
    // unless a later Load, which may be the template's, is still out.
    if (!later_load) {
        after_open_.reset();
        making_from_template_ = false;
    }
    if (later_load) {
        // What is shown is the later Open's session, and the window names
        // it. What that Open remembered is the refused session, so its own
        // refusal must clear rather than give this one back.
        before_open_ = BeforeOpen{};
        emit openNotice(QStringLiteral("A session was not opened: ") + why);
        return;
    }
    if (!before_open_.valid) {
        const QString refused = file_;
        openWasLost();
        emit openNotice((refused.isEmpty() ? QStringLiteral("A session") : refused) +
                        QStringLiteral(" was not opened: ") + why);
        return;
    }
    BeforeOpen b = std::move(before_open_);
    before_open_ = BeforeOpen{};
    ctl_.document_replaced();
    live_.clear();
    file_ = b.file;
    saved_revision_ = b.saved_revision;
    save_generation_ = b.save_generation;
    saved_bytes_ = std::move(b.saved_bytes);
    seen_dirty_ = false;
    seen_revision_ = UINT64_MAX;
    seen_sel_valid_ = seen_view_valid_ = false;
    updateCounters(true);
    setRepairs(std::move(b.repairs));
    sync();
    emit sessionChanged();
    emit openNotice(b.opened + QStringLiteral(" was not opened: ") + why + QStringLiteral("; ") +
                    (file_.isEmpty() ? QStringLiteral("the session before it")
                                     : file_) +
                    QStringLiteral(" is shown"));
}

void SessionModel::openWasLost() {
    after_open_.reset();
    making_from_template_ = false;
    before_open_ = BeforeOpen{};
    const QString lost = file_;
    file_.clear();
    saved_bytes_.clear();
    save_generation_ = 0;
    // Nothing on disk holds what is shown, so it reads as unsaved.
    saved_revision_ = UINT64_MAX;
    // What is shown is the engine's document, not the repaired file.
    setRepairs({});
    seen_revision_ = UINT64_MAX;
    sync();
    emit sessionChanged();
    if (!lost.isEmpty()) {
        emit openNotice(lost + QStringLiteral(" was not opened: the engine was unreachable. "
                                              "Open it again."));
    }
}

QVariantList SessionModel::sessionList() const {
    QVariantList out;
    for (const SessionEntry& e : list_sessions(sessions_dir_)) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), e.name);
        m.insert(QStringLiteral("file"), e.file);
        m.insert(QStringLiteral("current"), e.file == file_);
        out << m;
    }
    return out;
}

bool SessionModel::openSession(const QString& file, bool discard) {
    std::string why;
    return run(openSessionFile(file, discard, why), why);
}

QVariantList SessionModel::unsavedRecordingList() const {
    QVariantList out;
    for (const RecordingEntry& e : list_unsaved_recordings(sessions_dir_)) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), e.name);
        m.insert(QStringLiteral("folder"), e.folder);
        m.insert(QStringLiteral("current"), e.folder == engine_folder_);
        out << m;
    }
    return out;
}

bool SessionModel::openRecording(const QString& folder, bool discard) {
    std::string why;
    return run(openRecordingFolder(folder, discard, why), why);
}

bool SessionModel::openRecordingFolder(const QString& folder, bool discard, std::string& why) {
    if (overlay_.recording) {
        why = "a take is open; stop recording before opening another session";
        return false;
    }
    if (save_waiting_ || save_writing_) {
        why = "a save is still being written; open the recording once it is saved";
        return false;
    }
    if (dirty() && !discard) {
        why = "this session has unsaved edits; save it first, or open with \"Discard edits\"";
        return false;
    }
    const Snapshot& now = ctl_.snapshot();
    // The engine records into this session's tracks, so the empty session
    // has as many; its takes are offered for placing once it is open.
    Snapshot empty = make_empty_session(static_cast<uint32_t>(now.edit->tracks.size()),
                                        now.settings.sample_rate);
    const QString name = QFileInfo(folder).fileName();
    empty.settings.name = name.toStdString();
    empty.settings.session_id = model::new_session_id();
    const std::string path = QFileInfo(folder).absoluteFilePath().toStdString();
    if (!doc_.reset(std::move(empty), path, why)) {
        emit openNotice(folder + QStringLiteral(" was not opened: ") + q(why));
        return false;
    }
    ctl_.document_replaced();
    live_.clear();
    // Another session now: a New from Template's Save As still waiting is not its.
    after_open_.reset();
    making_from_template_ = false;
    before_open_.valid = true;
    before_open_.opened = folder;
    before_open_.file = file_;
    before_open_.saved_revision = saved_revision_;
    before_open_.save_generation = save_generation_;
    before_open_.saved_bytes = saved_bytes_;
    before_open_.repairs = repairs_;
    emit openNotice(QString());
    file_.clear();
    saved_bytes_.clear();
    save_generation_ = 0;
    saved_revision_ = doc_.revision();
    seen_dirty_ = false;
    seen_revision_ = UINT64_MAX;
    seen_sel_valid_ = seen_view_valid_ = false;
    setRepairs({});
    updateCounters(true);
    sync();
    emit sessionChanged();
    // The recording is another session: the windows' views of the old one
    // go back to their defaults, as they do for a session file.
    emit sessionOpened();
    emit notice(QStringLiteral("Opened the unsaved recording ") + name +
                QStringLiteral("; place its takes, then save to name it"));
    return true;
}

}  // namespace sonora::ui
