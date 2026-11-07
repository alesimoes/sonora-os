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

// --- File ---

namespace {

bool has_inserts(const Snapshot& s) {
    for (const auto& t : s.edit->tracks) {
        if (!t->inserts.empty()) return true;
    }
    return false;
}

}  // namespace

bool SessionModel::saveSession(std::string& why) {
    // A New from Template whose Save As failed: the engine is still in the
    // template's folder, and a plain Save would leave its takes going there
    // (ADR-0030 §3). The first save moves the session out instead.
    if (file_.isEmpty() && inTemplateFolder()) {
        const std::string& n = ctl_.snapshot().settings.name;
        const QString base = n.empty() ? QStringLiteral("Untitled") : q(n);
        return saveSessionAs(unused_session_name(sessions_dir_, base), false, why);
    }
    return flushThenWrite(
        why, [this](std::string& w, std::string caution) { return writeSession(w, std::move(caution)); });
}

bool SessionModel::saveSessionAs(const QString& name, bool copy, std::string& why) {
    const char* what = copy ? "Save Copy In" : "Save As";
    if (overlay_.recording) {
        why = std::string("stop recording before ") + what +
              ": the take being written belongs to this session's folder";
        return false;
    }
    if (save_waiting_ || save_writing_) {
        why = std::string("a save is still in hand; ") + what + " once it is written";
        return false;
    }
    const QString wanted = name.trimmed();
    std::string bad = session_name_problem(wanted);
    if (!bad.empty()) {
        why = "cannot save as \"" + wanted.toStdString() + "\": " + bad;
        return false;
    }
    const QString root = saveAsRoot();
    if (QFileInfo::exists(QDir(root).filePath(wanted))) {
        why = "a folder named \"" + wanted.toStdString() + "\" is already in " + root.toStdString() +
              "; choose another name";
        return false;
    }
    SaveAsTarget target;
    target.file = session_file_path(root, wanted);
    target.name = wanted;
    target.copy = copy;
    target.audio = !copy || copy_with_audio_;
    return flushThenWrite(why, [this, target](std::string& w, std::string caution) {
        return writeSessionAs(target, w, std::move(caution));
    });
}

bool SessionModel::writeSessionAs(const SaveAsTarget& target, std::string& why, std::string caution) {
    model::TemplateOptions options;
    options.markers = target.markers;
    Snapshot to_write = target.as_template ? model::template_of(ctl_.snapshot(), options) : ctl_.snapshot();
    // Save As renames the session to its new folder; a copy is named after
    // its folder too, so the Open list and the file agree.
    to_write.settings.name = target.name.toStdString();
    // A template is no session: it has no id until a session is made from it.
    if (!target.as_template && to_write.settings.session_id.empty()) {
        to_write.settings.session_id = model::new_session_id();
    }
    model::WriteInfo info;
    info.written_by = "sonora-ui";
    info.save_generation = target.as_template ? 1 : save_generation_ + 1;
    std::string bytes, error;
    if (!model::serialize(to_write, info, bytes, error, &digests_)) {
        why = "the session could not be saved: " + error;
        sync();
        return false;
    }
    Written w;
    w.file = target.file;
    w.revision = doc_.revision();
    w.rename = true;
    w.settings = to_write.settings;
    w.caution = std::move(caution);
    w.generation = info.save_generation;
    w.bytes = bytes;
    const uint32_t flags = (target.copy ? api::kSaveAsCopy : 0u) | (target.audio ? 0u : api::kSaveAsNoAudio);

    save_writing_ = true;
    write_wait_.start();
    const uint64_t ticket = ++write_ticket_;
    struct Inline {
        bool inside = true;
        bool answered = false;
        bool saved = false;
        std::string why;
    };
    auto now = std::make_shared<Inline>();
    auto pending = std::make_shared<Written>(std::move(w));
    const bool copy = target.copy;
    const bool as_template = target.as_template;
    doc_.save_session_as(
        target.file.toStdString(), flags, std::move(bytes),
        [this, ticket, now, pending, copy, as_template](bool ok, const std::string& message) {
            // Answered: the engine has left the template's folder, or a
            // failed Save As left it there and commandRefusal() says so.
            if (!as_template) making_from_template_ = false;
            if (ticket != write_ticket_) return;
            save_writing_ = false;
            std::string why;
            bool saved = false;
            const QString folder = QFileInfo(pending->file).absolutePath();
            if (!ok) {
                why = as_template ? "the template was not saved: " + message
                      : std::string(copy ? "the copy was not saved: " : "the session was not saved as ") +
                            (copy ? message : QFileInfo(folder).fileName().toStdString() + ": " + message);
                // New from Template's Save As (ADR-0030 §3): the operator
                // must know the engine is still in the template's folder,
                // and why Record is refused until the session is saved.
                if (!copy && !as_template && file_.isEmpty() && inTemplateFolder()) {
                    why += ". The session is open unsaved and the engine is still in the template's "
                           "folder, so Record is refused until Save puts the session in " +
                           sessions_dir_.toStdString();
                }
                sync();
            } else if (as_template) {
                saved = true;
                emit notice(QStringLiteral("Saved the template ") + pending->file +
                            (message.empty() ? QString() : QStringLiteral(", but ") + q(message)));
            } else if (copy) {
                saved = true;
                emit notice(QStringLiteral("Saved a copy in ") + pending->file +
                            (message.empty() ? QString() : QStringLiteral(", but ") + q(message)));
            } else if (!engine_folder_.isEmpty() && QDir::cleanPath(engine_folder_) != QDir::cleanPath(folder)) {
                // Written, and the engine stayed where it was (a take started,
                // the new folder's lock): the window goes on with the old file.
                saved = true;
                emit heldNotice(QStringLiteral("Saved ") + pending->file +
                                QStringLiteral(" as a copy only: ") + q(message));
                sync();
            } else {
                saved = finishSave(true, message, std::move(*pending), why);
            }
            if (now->inside) {
                now->answered = true;
                now->saved = saved;
                now->why = why;
            } else if (!saved && !why.empty()) {
                emit heldNotice(q(why));
            }
        });
    now->inside = false;
    if (now->answered) {
        why = now->why;
        return now->saved;
    }
    emit notice(as_template ? QStringLiteral("Saving the template ") + target.file
                : copy      ? QStringLiteral("Saving a copy in ") + target.file
                            : QStringLiteral("Saving as ") + target.file);
    sync();
    return true;
}

bool SessionModel::flushThenWrite(std::string& why,
                                  std::function<bool(std::string&, std::string)> write) {
    if (save_writing_) {
        // While a take is open the engine writes after its next checkpoint,
        // and a large session takes a while on a stick: only an engine that
        // has not answered in the patience is asked again. Saves are written
        // in order, so a late answer to the first changes nothing on disk.
        if (write_wait_.elapsed() < flush_patience_ms_) {
            why = "a save is already being written by the engine";
            return false;
        }
        save_writing_ = false;
        ++write_ticket_;   // its answer, if it ever comes, is ignored
    }
    if (save_waiting_) {
        if (save_wait_.elapsed() < flush_patience_ms_) {
            why = "a save is already waiting for the plugins' state from the engine";
            return false;
        }
        save_waiting_ = false;
        ++save_ticket_;   // its answer, if it ever comes, is ignored
        return write(why, "the engine never sent the plugins' state, so plugin "
                          "settings are as last recorded");
    }
    // No insert, no plugin state: nothing to wait for.
    if (!has_inserts(ctl_.snapshot())) return write(why, {});

    save_waiting_ = true;
    save_wait_.start();
    const uint64_t ticket = ++save_ticket_;
    // The flush may answer inside this call (no engine in between) or from
    // a later tick; the first reports through `why`, the second on its own.
    struct Inline {
        bool inside = true;
        bool answered = false;
        bool saved = false;
        std::string why;
    };
    auto now = std::make_shared<Inline>();
    doc_.flush_plugin_state(kNoId, [this, ticket, now, write](bool ok, const std::string& flush_why) {
        if (ticket != save_ticket_) return;
        save_waiting_ = false;
        std::string why;
        const bool saved = write(
            why, ok ? std::string() : "these plugins' settings are as last recorded: " + flush_why);
        if (now->inside) {
            now->answered = true;
            now->saved = saved;
            now->why = why;
        } else if (!saved && !why.empty()) {
            emit refused(q(why));
        }
    });
    now->inside = false;
    if (now->answered) {
        why = now->why;
        return now->saved;
    }
    sync();
    emit notice(QStringLiteral("Saving: collecting the plugins' state from the engine"));
    // Never left waiting in silence: once the patience is spent the
    // operator is told what a second Save will do.
    QTimer::singleShot(static_cast<int>(flush_patience_ms_), this, [this, ticket]() {
        if (!save_waiting_ || ticket != save_ticket_) return;
        emit refused(QStringLiteral(
            "Not saved yet: the engine has not sent the plugins' state. Save again to write the "
            "session without it; plugin settings would be as last recorded."));
    });
    return true;
}

bool SessionModel::writeSession(std::string& why, std::string caution) {
    QString file = file_;
    // What is written: the session as it is, or, the first time, renamed to
    // its folder's name. The rename reaches the document only once the file
    // is written, so a failed save leaves the name and the disk as they were
    // and a retry is not pushed to "<Name> 2".
    Snapshot to_write = ctl_.snapshot();
    bool rename = false;
    // Beside its takes (ADR-0018 §4), in the folder the engine records this
    // session into, unless another session's file is already there: then
    // the takes stay where they are and the file goes where it went before.
    // Where a new session's folder goes: beside the engine's untitled one,
    // which the engine renames there (ADR-0018 §3), so on its filesystem.
    QString root = sessions_dir_;
    if (file.isEmpty() && !engine_folder_.isEmpty()) {
        const QDir folder(engine_folder_);
        const QString name = QFileInfo(engine_folder_).fileName();
        const QString own = q(to_write.settings.name);
        // A name the operator gave, not the one a new session starts with.
        static const QRegularExpression untitled(QStringLiteral("^Untitled( [0-9]+)?$"));
        const bool named = engine_untitled_ && !own.isEmpty() && own != name &&
                           !untitled.match(own).hasMatch() && session_name_problem(own).empty();
        if (folder.exists() && folder.entryList({QStringLiteral("*.sonora")}, QDir::Files).isEmpty()) {
            if (named) {
                root = QFileInfo(engine_folder_).absolutePath();
            } else if (session_name_problem(name).empty()) {
                file = folder.filePath(name + QStringLiteral(".sonora"));
                if (name.toStdString() != to_write.settings.name) {
                    to_write.settings.name = name.toStdString();
                    rename = true;
                }
            }
        }
    }
    if (file.isEmpty()) {
        const std::string& n = to_write.settings.name;
        const QString wanted = n.empty() ? QStringLiteral("Untitled") : q(n);
        std::string bad = session_name_problem(wanted);
        if (!bad.empty()) {
            why = "cannot save as \"" + n + "\": " + bad;
            return false;
        }
        const QString name = unused_session_name(root, wanted);
        bad = session_name_problem(name);
        if (!bad.empty()) {
            why = "cannot save as \"" + name.toStdString() + "\", the first free name: " + bad;
            return false;
        }
        // The session is called what its folder is called, so the name the
        // window shows is the one the Open list shows.
        if (name.toStdString() != n) {
            to_write.settings.name = name.toStdString();
            rename = true;
        }
        file = session_file_path(root, name);
    }
    // A session saved before format 7 has no id: this save gives it one,
    // and the document takes it once the file is written, as with the name.
    if (to_write.settings.session_id.empty()) {
        to_write.settings.session_id = model::new_session_id();
        rename = true;
    }

    // Canonical bytes (session_file.h): the same content is the same bytes,
    // so a save that would write what the file already holds writes nothing
    // and keeps the generation, as session-format.md asks.
    model::WriteInfo info;
    info.written_by = "sonora-ui";
    info.save_generation = save_generation_;
    std::string bytes, error;
    if (!model::serialize(to_write, info, bytes, error, &digests_)) {
        why = "the session could not be saved: " + error;
        sync();
        return false;
    }
    Written w;
    w.file = file;
    w.revision = doc_.revision();
    w.rename = rename;
    w.settings = to_write.settings;
    w.caution = std::move(caution);
    if (file == file_ && bytes == saved_bytes_) {
        w.generation = save_generation_;
        w.bytes = std::move(bytes);
        return finishSave(true, {}, std::move(w), why);
    }
    info.save_generation = save_generation_ + 1;
    if (!model::serialize(to_write, info, bytes, error, &digests_)) {
        why = "the session could not be saved: " + error;
        sync();
        return false;
    }
    w.generation = info.save_generation;
    w.bytes = bytes;

    // The engine writes the sequence files, then the session file, and
    // cleans up (ADR-0015 §3, §6); it answers now (no engine in between) or
    // from a later tick, which the first reports through `why` and the
    // second on its own.
    save_writing_ = true;
    write_wait_.start();
    const uint64_t ticket = ++write_ticket_;
    struct Inline {
        bool inside = true;
        bool answered = false;
        bool saved = false;
        std::string why;
    };
    auto now = std::make_shared<Inline>();
    auto pending = std::make_shared<Written>(std::move(w));
    doc_.save_session(file.toStdString(), std::move(bytes),
                      [this, ticket, now, pending](bool ok, const std::string& message) {
                          if (ticket != write_ticket_) return;
                          std::string why;
                          const bool saved = finishSave(ok, message, std::move(*pending), why);
                          if (now->inside) {
                              now->answered = true;
                              now->saved = saved;
                              now->why = why;
                          } else if (!saved && !why.empty()) {
                              emit refused(q(why));
                          }
                      });
    now->inside = false;
    if (now->answered) {
        why = now->why;
        return now->saved;
    }
    sync();
    return true;
}

bool SessionModel::finishSave(bool ok, const std::string& message, Written w, std::string& why) {
    save_writing_ = false;
    if (!ok) {
        why = "the session could not be saved: " + message;
        sync();
        return false;
    }
    const bool nothing_new = w.file == file_ && w.bytes == saved_bytes_;
    std::string caution = std::move(w.caution);
    if (!message.empty()) caution = caution.empty() ? message : caution + "; " + message;
    save_generation_ = w.generation;
    saved_bytes_ = std::move(w.bytes);
    file_ = w.file;
    // An edit made while the engine wrote is not in the file.
    const bool edited_since = doc_.revision() != w.revision;
    // The file is on disk under the new name and id; the document follows
    // it. Only those two: any other setting changed while the engine wrote
    // stays as it is now. A refusal here would be a model bug, and the file
    // already says the name.
    std::string rename_why;
    model::SetSettings named{ctl_.snapshot().settings};
    named.settings.name = w.settings.name;
    named.settings.session_id = w.settings.session_id;
    if (w.rename && !doc_.submit(named, rename_why)) {
        caution = caution.empty() ? "the window keeps the old name: " + rename_why
                                  : caution + "; the window keeps the old name: " + rename_why;
    }
    saved_revision_ = edited_since ? w.revision : doc_.revision();
    seen_dirty_ = dirty();
    sync();
    emit sessionChanged();
    QString said = nothing_new ? QStringLiteral("Nothing to save: ") + file_ +
                                     QStringLiteral(" is current")
                               : QStringLiteral("Saved ") + file_;
    if (!caution.empty()) said += QStringLiteral(", but ") + q(caution);
    emit notice(said);
    return true;
}

}  // namespace sonora::ui
