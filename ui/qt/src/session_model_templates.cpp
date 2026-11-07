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
std::vector<SessionEntry> list_templates(const QString& dir);
}  // namespace

QVariantList SessionModel::templateList() const {
    QVariantList out;
    const uint32_t rate = ctl_.snapshot().settings.sample_rate;
    // Never a link: Rename and Delete must not reach past the folder.
    for (const SessionEntry& e : list_templates(templates_dir_)) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), e.name);
        m.insert(QStringLiteral("file"), e.file);
        m.insert(QStringLiteral("date"),
                 QFileInfo(e.file).lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        // Read as New from Template reads it, so what the browser offers is
        // what the click does: one it would refuse says why here instead.
        std::string bytes, why;
        model::Loaded loaded;
        int tracks = -1;
        QString problem;
        if (!read_session_file(e.file, bytes, loaded, why)) {
            problem = QStringLiteral("cannot be read: ") + q(why);
        } else if (loaded.snapshot.settings.sample_rate != rate) {
            problem = QStringLiteral("made at %1 Hz; this system runs at %2 Hz")
                          .arg(loaded.snapshot.settings.sample_rate)
                          .arg(rate);
        } else {
            tracks = static_cast<int>(loaded.snapshot.edit->tracks.size());
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

// A link of any kind: a symlink, or on Windows a junction, either of which
// would take a rename or a delete to a folder that is not the template.
bool is_link(const QFileInfo& i) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    if (i.isJunction()) return true;
#endif
    return i.isSymLink();
}

// The canonical path, or the cleaned absolute one for a path not there.
QString canonical(const QString& path) {
    const QFileInfo i(path);
    const QString c = i.canonicalFilePath();
    return c.isEmpty() ? QDir::cleanPath(i.absoluteFilePath()) : c;
}

// Removes `path` without following a link: a link inside is removed as a
// link, never walked into. False when anything is left.
bool remove_without_following(const QString& path) {
    bool ok = true;
    const QDir d(path);
    for (const QFileInfo& i :
         d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        const QString p = i.absoluteFilePath();
        if (is_link(i)) {
            // A link to a directory is removed as a directory entry on Windows.
            ok = (QFile::remove(p) || QDir().rmdir(p)) && ok;
        } else if (i.isDir()) {
            ok = remove_without_following(p) && ok;
        } else {
            ok = QFile::remove(p) && ok;
        }
    }
    return QDir().rmdir(path) && ok;
}

// Any audio in `folder`, links not followed: a template has none, so a
// folder that has some is a session in the wrong place.
bool holds_audio(const QString& folder) {
    if (QFileInfo::exists(QDir(folder).filePath(QStringLiteral("Audio Files")))) return true;
    QDirIterator it(folder, {QStringLiteral("*.wav"), QStringLiteral("*.bwf"), QStringLiteral("*.flac")},
                    QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    return it.hasNext();
}

// The templates of `dir` the browser lists: as list_sessions(), without a
// folder or a session file that is a link.
std::vector<SessionEntry> list_templates(const QString& dir) {
    std::vector<SessionEntry> out;
    for (const SessionEntry& e : list_sessions(dir)) {
        if (is_link(QFileInfo(QDir(dir).filePath(e.name))) || is_link(QFileInfo(e.file))) continue;
        out.push_back(e);
    }
    return out;
}

}  // namespace

bool SessionModel::templateFolderSafe(const QString& name, std::string& why) const {
    bool listed = !templates_dir_.isEmpty() && session_name_problem(name).empty();
    if (listed) {
        listed = false;
        for (const SessionEntry& e : list_templates(templates_dir_)) {
            if (e.name == name) listed = true;
        }
    }
    if (!listed) {
        why = "there is no template named \"" + name.toStdString() + "\" in " + templates_dir_.toStdString();
        return false;
    }
    const QString root = canonical(templates_dir_);
    if (!sessions_dir_.isEmpty() && root == canonical(sessions_dir_)) {
        why = "the templates folder " + templates_dir_.toStdString() +
              " is the sessions folder, so its folders are sessions; nothing is renamed or deleted there";
        return false;
    }
    const QString folder = QDir(templates_dir_).filePath(name);
    if (is_link(QFileInfo(folder)) || is_link(QFileInfo(session_file_path(templates_dir_, name))) ||
        QFileInfo(canonical(folder)).absolutePath() != root) {
        why = "the template \"" + name.toStdString() + "\" is a link to a folder elsewhere; it is left alone";
        return false;
    }
    return true;
}

bool SessionModel::templateIsFree(const QString& name, std::string& why) const {
    if (making_from_template_ || after_open_) {
        why = "a session is being made from a template; rename or delete once it is saved";
        return false;
    }
    if (save_waiting_ || save_writing_) {
        why = "a save is still in hand; rename or delete the template once it is written";
        return false;
    }
    if (engine_folder_.isEmpty()) return true;
    const QString folder = canonical(QDir(templates_dir_).filePath(name));
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
    const QString wanted = to.trimmed();
    if (wanted == from) return true;
    const std::string bad = session_name_problem(wanted);
    if (!bad.empty()) {
        why = "cannot rename the template to \"" + wanted.toStdString() + "\": " + bad;
        return false;
    }
    QDir root(templates_dir_);
    if (QFileInfo::exists(root.filePath(wanted))) {
        why = "a template named \"" + wanted.toStdString() + "\" is already in " +
              templates_dir_.toStdString() + "; choose another name";
        return false;
    }
    if (!templateIsFree(from, why)) return false;
    // The file inside the folder first, then the folder: a failure at the
    // second step puts the file back, so the template stays listed.
    const QString folder = root.filePath(from);
    const QString old_file = session_file_path(templates_dir_, from);
    const QString moved_file = QDir(folder).filePath(wanted + QStringLiteral(".sonora"));
    if (!QFile::rename(old_file, moved_file)) {
        why = "could not rename " + old_file.toStdString();
        return false;
    }
    if (!root.rename(from, wanted)) {
        QFile::rename(moved_file, old_file);
        why = "could not rename the folder " + folder.toStdString();
        return false;
    }
    emit notice(QStringLiteral("Renamed the template ") + from + QStringLiteral(" to ") + wanted);
    return true;
}

bool SessionModel::deleteSessionTemplate(const QString& name, std::string& why) {
    if (!templateFolderSafe(name, why)) return false;
    const QString folder = QDir(templates_dir_).filePath(name);
    if (holds_audio(folder)) {
        why = "\"" + name.toStdString() + "\" holds audio, which a template never has: it is a session, "
              "not deleted from here";
        return false;
    }
    if (!templateIsFree(name, why)) return false;
    if (!remove_without_following(folder)) {
        why = "could not delete all of " + folder.toStdString() + "; what is left stays there";
        return false;
    }
    emit notice(QStringLiteral("Deleted the template ") + name);
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
