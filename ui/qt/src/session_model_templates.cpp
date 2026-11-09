// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_model.h"

#include <QDateTime>
#include <QDir>
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

// --- Session templates (ADR-0030) ---

QString SessionModel::saveAsRoot() const {
    // Beside the folder the session is in, on its filesystem, so Save As can
    // link the audio rather than copy it (ADR-0018 §5).
    if (engine_folder_.isEmpty() || inTemplateFolder()) return sessions_dir_;
    return QFileInfo(engine_folder_).absolutePath();
}

bool SessionModel::inTemplateFolder() const {
    if (engine_folder_.isEmpty() || templates_dir_.isEmpty()) return false;
    // Canonical, so a symlinked templates folder (or one that is also the
    // sessions folder) is still recognised; the cleaned path for one not there.
    const auto resolved = [](const QString& path) {
        const QFileInfo i(path);
        const QString c = i.canonicalFilePath();
        return c.isEmpty() ? QDir::cleanPath(i.absoluteFilePath()) : c;
    };
    const QString root = resolved(templates_dir_);
    // A templates folder that is the sessions folder holds every session:
    // read as templates, each untitled session would be refused a take.
    if (!sessions_dir_.isEmpty() && root == resolved(sessions_dir_)) return false;
    const QString folder = resolved(engine_folder_);
    return folder.startsWith(root + QLatin1Char('/'));
}

bool SessionModel::saveSessionAsTemplate(const QString& name, bool markers, std::string& why) {
    if (overlay_.recording) {
        why = "stop recording before Save as Template: the take being written belongs to this "
              "session's folder";
        return false;
    }
    if (save_waiting_ || save_writing_) {
        why = "a save is still in hand; Save as Template once it is written";
        return false;
    }
    const QString wanted = name.trimmed();
    const std::string bad = session_name_problem(wanted);
    if (!bad.empty()) {
        why = "cannot save the template \"" + wanted.toStdString() + "\": " + bad;
        return false;
    }
    if (QFileInfo::exists(QDir(templates_dir_).filePath(wanted))) {
        why = "a template named \"" + wanted.toStdString() + "\" is already in " +
              templates_dir_.toStdString() + "; choose another name";
        return false;
    }
    SaveAsTarget target;
    target.file = session_file_path(templates_dir_, wanted);
    target.name = wanted;
    target.copy = true;
    target.audio = false;
    target.as_template = true;
    target.markers = markers;
    return flushThenWrite(why, [this, target](std::string& w, std::string caution) {
        return writeSessionAs(target, w, std::move(caution));
    });
}

bool SessionModel::newSessionFromTemplate(const QString& file, const QString& name, bool discard,
                                          std::string& why) {
    if (overlay_.recording) {
        why = "a take is open; stop recording before starting a session from a template";
        return false;
    }
    if (save_waiting_ || save_writing_) {
        why = "a save is still being written; start the new session once it is saved";
        return false;
    }
    if (dirty() && !discard) {
        why = "this session has unsaved edits; save it first, or start the new one with \"Discard "
              "edits\"";
        return false;
    }
    const QString wanted = name.trimmed();
    const std::string bad = session_name_problem(wanted);
    if (!bad.empty()) {
        why = "cannot name the session \"" + wanted.toStdString() + "\": " + bad;
        return false;
    }
    // Checked before the Load, so the Save As after it is refused only by
    // the medium.
    if (QFileInfo::exists(QDir(sessions_dir_).filePath(wanted))) {
        why = "a folder named \"" + wanted.toStdString() + "\" is already in " +
              sessions_dir_.toStdString() + "; choose another name";
        return false;
    }
    std::string bytes;
    model::Loaded loaded;
    if (!read_session_file(file, bytes, loaded, why)) {
        why = "the template " + file.toStdString() + " could not be read: " + why;
        return false;
    }
    const uint32_t rate = ctl_.snapshot().settings.sample_rate;
    if (loaded.snapshot.settings.sample_rate != rate) {
        why = "the template is at " + std::to_string(loaded.snapshot.settings.sample_rate) +
              " Hz and this system runs at " + std::to_string(rate) +
              " Hz; it cannot play here until sample-rate conversion exists";
        return false;
    }
    Snapshot made =
        model::session_from_template(loaded.snapshot, wanted.toStdString(), model::new_session_id());
    // The template's path with the Load's template prefix (protocol 54): the
    // engine copies the plugin states the template keeps into a new
    // Untitled folder and enters that, never the template's own
    // (ADR-0030 §3, amended).
    const std::string path =
        std::string(api::kLoadTemplatePrefix) + QFileInfo(file).absoluteFilePath().toStdString();
    if (!doc_.reset(std::move(made), path, why)) {
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
    // Nothing on disk holds it yet, so it reads as unsaved until the Save As.
    file_.clear();
    saved_bytes_.clear();
    save_generation_ = 0;
    saved_revision_ = UINT64_MAX;
    seen_dirty_ = false;
    seen_revision_ = UINT64_MAX;
    seen_sel_valid_ = seen_view_valid_ = false;
    setRepairs({});
    updateCounters(true);
    sync();
    emit sessionChanged();
    emit sessionOpened();

    SaveAsTarget target;
    target.file = session_file_path(sessions_dir_, wanted);
    target.name = wanted;
    after_open_ = target;
    making_from_template_ = true;
    emit notice(QStringLiteral("Making the session ") + wanted + QStringLiteral(" from the template ") +
                QFileInfo(file).dir().dirName());
    // A document held here has taken the Load already.
    if (!doc_.opening()) saveAfterOpen();
    return true;
}

void SessionModel::saveAfterOpen() {
    if (!after_open_ || doc_.opening()) return;
    const SaveAsTarget target = *after_open_;
    after_open_.reset();
    std::string why;
    // Read before the write: the engine's answer to the Save As clears the flag.
    const bool from_template = making_from_template_;
    if (!writeSessionAs(target, why, std::string())) {
        making_from_template_ = false;
        emit heldNotice(QStringLiteral("The session ") + target.name +
                        (from_template ? QStringLiteral(" was not made from the template: ")
                                       : QStringLiteral(" was not written: ")) +
                        q(why) +
                        QStringLiteral(". It is open unsaved; its first save puts it in ") + sessions_dir_);
    }
}

std::string SessionModel::commandRefusal(const std::string& command) const {
    // Play is not refused: it is a take only while record-ready, and a Load
    // clears record-ready at the engine, while record_ready is refused here.
    if (!is_take_command(command)) return {};
    if (making_from_template_) {
        return "the new session is still being made from its template; record once it is saved "
               "in the sessions folder";
    }
    if (file_.isEmpty() && inTemplateFolder()) {
        return "the engine is still in the template's folder, and a take there would stay in the "
               "template; save the session first, which puts it in " +
               sessions_dir_.toStdString();
    }
    return {};
}

namespace {

// A template's place under the templates folder, "Name" or
// "Category/Name": what Rename and Delete are given.
QString template_key(const model::TemplateInfo& t) {
    const QString name = QString::fromStdString(t.name);
    return t.category.empty() ? name : QString::fromStdString(t.category) + QLatin1Char('/') + name;
}

}  // namespace

QVariantList SessionModel::templateList() const {
    QVariantList out;
    const uint32_t rate = ctl_.snapshot().settings.sample_rate;
    // Never a link: Rename and Delete must not reach past the folder.
    for (const model::TemplateInfo& t : list_session_templates(templates_dir_)) {
        const QString file = QString::fromStdString(t.file);
        QVariantMap m;
        m.insert(QStringLiteral("name"), QString::fromStdString(t.name));
        m.insert(QStringLiteral("category"), QString::fromStdString(t.category));
        m.insert(QStringLiteral("path"), template_key(t));
        m.insert(QStringLiteral("file"), file);
        m.insert(QStringLiteral("date"),
                 QFileInfo(file).lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        m.insert(QStringLiteral("formatVersion"), static_cast<qint64>(t.file_format_version));
        m.insert(QStringLiteral("newerThanBuild"), t.newer_than_build);
        // Read as New from Template reads it, so what the browser offers is
        // what the click does: one it would refuse says why here instead. A
        // newer one is not loaded at all, since the loader refuses it.
        int tracks = -1;
        QString problem;
        if (t.newer_than_build) {
            problem = QStringLiteral("made by a newer Sonora (session format %1; this build reads up to %2)")
                          .arg(t.file_format_version)
                          .arg(model::kFormatVersion);
        } else {
            std::string bytes, why;
            model::Loaded loaded;
            if (!read_session_file(file, bytes, loaded, why)) {
                problem = QStringLiteral("cannot be read: ") + q(why);
            } else if (loaded.snapshot.settings.sample_rate != rate) {
                problem = QStringLiteral("made at %1 Hz; this system runs at %2 Hz")
                              .arg(loaded.snapshot.settings.sample_rate)
                              .arg(rate);
            } else {
                tracks = static_cast<int>(loaded.snapshot.edit->tracks.size());
            }
        }
        m.insert(QStringLiteral("tracks"), tracks);
        m.insert(QStringLiteral("problem"), problem);
        out << m;
    }
    return out;
}

QString SessionModel::unusedSessionName(const QString& base) const {
    const QString b = base.trimmed();
    return unused_session_name(sessions_dir_, b.isEmpty() ? QStringLiteral("Untitled") : b);
}

namespace {

// The canonical path, or the cleaned absolute one for a path not there.
QString canonical(const QString& path) {
    const QFileInfo i(path);
    const QString c = i.canonicalFilePath();
    return c.isEmpty() ? QDir::cleanPath(i.absoluteFilePath()) : c;
}

// A template key's folder name, and the folder that holds it.
QString key_name(const QString& key) { return key.section(QLatin1Char('/'), -1); }
QString key_parent(const QString& root, const QString& key) {
    return key.contains(QLatin1Char('/')) ? QDir(root).filePath(key.section(QLatin1Char('/'), 0, 0)) : root;
}

}  // namespace

bool SessionModel::templateFolderSafe(const QString& key, std::string& why) const {
    bool listed = false;
    if (!templates_dir_.isEmpty()) {
        for (const model::TemplateInfo& t : list_session_templates(templates_dir_)) {
            if (template_key(t) == key) listed = true;
        }
    }
    if (!listed) {
        why = "there is no template named \"" + key.toStdString() + "\" in " + templates_dir_.toStdString();
        return false;
    }
    const QString root = canonical(templates_dir_);
    if (!sessions_dir_.isEmpty() && root == canonical(sessions_dir_)) {
        why = "the templates folder " + templates_dir_.toStdString() +
              " is the sessions folder, so its folders are sessions; nothing is renamed or deleted there";
        return false;
    }
    // Re-read now rather than trusted from the listing: a folder or its
    // category swapped for a link since then resolves somewhere else.
    const QString parent = key_parent(templates_dir_, key);
    const QString folder = QDir(templates_dir_).filePath(key);
    // The templates folder itself may be a link; a category may not.
    const bool categorised = key.contains(QLatin1Char('/'));
    if ((categorised && is_link(parent)) || is_link(folder) || is_link(session_file_path(parent, key_name(key))) ||
        canonical(folder) != root + QLatin1Char('/') + key) {
        why = "the template \"" + key.toStdString() + "\" is a link to a folder elsewhere; it is left alone";
        return false;
    }
    return true;
}

bool SessionModel::templateIsFree(const QString& key, std::string& why) const {
    if (making_from_template_ || after_open_) {
        why = "a session is being made from a template; rename or delete once it is saved";
        return false;
    }
    if (save_waiting_ || save_writing_) {
        why = "a save is still in hand; rename or delete the template once it is written";
        return false;
    }
    if (engine_folder_.isEmpty()) return true;
    // Canonical on both sides: the engine may name the folder through a
    // link to the templates folder, or the templates folder be the link.
    const QString folder = canonical(QDir(templates_dir_).filePath(key));
    const QString engine = canonical(engine_folder_);
    if (engine == folder || engine.startsWith(folder + QLatin1Char('/'))) {
        why = "the engine is in this template's folder; save the session first, which moves it to " +
              sessions_dir_.toStdString();
        return false;
    }
    return true;
}

bool SessionModel::renameSessionTemplate(const QString& from, const QString& to, std::string& why) {
    if (!templateFolderSafe(from, why)) return false;
    // Renamed where it is: moving between categories is deferred (ADR-0146 §4).
    const QString old_name = key_name(from);
    const QString wanted = to.trimmed();
    if (wanted == old_name) return true;
    const std::string bad = session_name_problem(wanted);
    if (!bad.empty()) {
        why = "cannot rename the template to \"" + wanted.toStdString() + "\": " + bad;
        return false;
    }
    QDir parent(key_parent(templates_dir_, from));
    if (QFileInfo::exists(parent.filePath(wanted))) {
        why = "a template named \"" + wanted.toStdString() + "\" is already in " + parent.path().toStdString() +
              "; choose another name";
        return false;
    }
    if (!templateIsFree(from, why)) return false;
    // The file inside the folder first, then the folder: a failure at the
    // second step puts the file back, so the template stays listed.
    const QString folder = parent.filePath(old_name);
    const QString old_file = session_file_path(parent.path(), old_name);
    const QString moved_file = QDir(folder).filePath(wanted + QStringLiteral(".sonora"));
    if (!QFile::rename(old_file, moved_file)) {
        why = "could not rename " + old_file.toStdString();
        return false;
    }
    if (!parent.rename(old_name, wanted)) {
        QFile::rename(moved_file, old_file);
        why = "could not rename the folder " + folder.toStdString();
        return false;
    }
    emit notice(QStringLiteral("Renamed the template ") + from + QStringLiteral(" to ") + wanted);
    return true;
}

bool SessionModel::deleteSessionTemplate(const QString& key, std::string& why) {
    if (!templateFolderSafe(key, why)) return false;
    const QString folder = QDir(templates_dir_).filePath(key);
    if (holds_audio(folder)) {
        why = "\"" + key.toStdString() + "\" holds audio, which a template never has: it is a session, "
              "not deleted from here";
        return false;
    }
    if (!templateIsFree(key, why)) return false;
    if (!remove_template_folder(folder, why)) return false;
    emit notice(QStringLiteral("Deleted the template ") + key);
    return true;
}

bool SessionModel::renameTemplate(const QString& from, const QString& to) {
    std::string why;
    return run(renameSessionTemplate(from, to, why), why);
}

bool SessionModel::deleteTemplate(const QString& name) {
    std::string why;
    return run(deleteSessionTemplate(name, why), why);
}

bool SessionModel::saveAsTemplate(const QString& name, bool markers) {
    std::string why;
    return run(saveSessionAsTemplate(name, markers, why), why);
}

bool SessionModel::newFromTemplate(const QString& file, const QString& name, bool discard) {
    std::string why;
    return run(newSessionFromTemplate(file, name, discard, why), why);
}

}  // namespace sonora::ui
