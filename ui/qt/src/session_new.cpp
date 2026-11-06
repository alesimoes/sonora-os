// SPDX-License-Identifier: GPL-3.0-or-later
//
// File > New Session (Pro Tools Reference Guide, Sessions): a name, where
// it goes, its sample rate and bit depth.
//
// Where: the sessions folder, the one folder the Open panel lists. There is
// no file browser on the appliance, so the dialog names the folder and
// offers no other.
//
// Rate and depth are the engine's, not the session's to choose. The engine
// opens the interface at one rate for its whole run (AudioBackend::
// sample_rate(), fixed from open()) and writes every take at one depth
// (Session's config, --float); a session at any other rate is refused by
// Open, and nothing here can restart the engine at another. So the dialog
// lists the rates Pro Tools offers and the depths and lights only the ones
// this system runs at; the others are shown dimmed with the reason, rather
// than offered and then quietly not used.
//
// The session is made the way Open makes one: the document is reset (a
// Load with no file, which the engine starts in a fresh Untitled folder,
// ADR-0018 §3), and the first Save names its folder. That Save is sent
// once the engine has taken the Load, never before: a Load it refuses
// gives the old session back (openWasRefused), and a Save sent ahead of it
// would have written the empty session beside the one the engine kept.
#include <QDir>
#include <QFileInfo>
#include <QVariantMap>
#include <algorithm>

#include "session_files.h"
#include "session_model.h"
#include "sonora/model/session_file.h"
#include "timeline_document.h"

namespace sonora::ui {

using namespace model;

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

QString rate_text(uint32_t hz) {
    return hz % 1000 == 0 ? QStringLiteral("%1 kHz").arg(hz / 1000)
                          : QStringLiteral("%1 kHz").arg(hz / 1000.0, 0, 'f', 1);
}

QString depth_text(AudioFormat f) {
    return f == AudioFormat::Float32 ? QStringLiteral("32-bit float") : QStringLiteral("24-bit");
}

}  // namespace

// Save As's: beside the engine's folder, which the engine makes the new
// one's Untitled folder beside, else the sessions folder.
QString SessionModel::newSessionRoot() const {
    return engine_folder_.isEmpty() ? sessions_dir_ : QFileInfo(engine_folder_).absolutePath();
}

QVariantMap SessionModel::newSessionChoices() const {
    const SessionSettings& now = ctl_.snapshot().settings;
    QVariantMap out;
    out[QStringLiteral("name")] = unused_session_name(newSessionRoot(), QStringLiteral("Untitled"));
    out[QStringLiteral("where")] = newSessionRoot();

    std::vector<uint32_t> rates = {44100, 48000, 88200, 96000};
    if (std::find(rates.begin(), rates.end(), now.sample_rate) == rates.end()) {
        rates.push_back(now.sample_rate);
        std::sort(rates.begin(), rates.end());
    }
    QVariantList rate_list;
    for (uint32_t r : rates) {
        QVariantMap m;
        m[QStringLiteral("text")] = rate_text(r);
        m[QStringLiteral("on")] = r == now.sample_rate;
        m[QStringLiteral("why")] =
            r == now.sample_rate
                ? QString()
                : QStringLiteral("this system runs at %1: the engine opens the interface at one rate "
                                 "for its whole run, and a session at another rate cannot play here "
                                 "until sample-rate conversion exists")
                      .arg(rate_text(now.sample_rate));
        rate_list << m;
    }
    out[QStringLiteral("rates")] = rate_list;

    QVariantList depths;
    {
        QVariantMap m;
        m[QStringLiteral("text")] = QStringLiteral("16-bit");
        m[QStringLiteral("on")] = false;
        m[QStringLiteral("why")] =
            QStringLiteral("the engine records 24-bit or 32-bit float, never 16-bit");
        depths << m;
    }
    for (AudioFormat f : {AudioFormat::Int24, AudioFormat::Float32}) {
        QVariantMap m;
        m[QStringLiteral("text")] = depth_text(f);
        m[QStringLiteral("on")] = f == now.audio_format;
        m[QStringLiteral("why")] =
            f == now.audio_format
                ? QString()
                : QStringLiteral("this system records %1: the engine writes every take at the "
                                 "depth it was started with")
                      .arg(depth_text(now.audio_format));
        depths << m;
    }
    out[QStringLiteral("depths")] = depths;
    return out;
}

bool SessionModel::makeNewSession(const QString& raw, bool discard, std::string& why) {
    if (overlay_.recording) {
        why = "a take is open; stop recording before making a new session";
        return false;
    }
    if (save_waiting_ || save_writing_ || new_session_save_) {
        why = "a save is still being written; make the new session once it is saved";
        return false;
    }
    if (dirty() && !discard) {
        why = "this session has unsaved edits; save it first, or make the new one with "
              "\"Discard edits\"";
        return false;
    }
    const QString name = raw.trimmed();
    std::string bad = session_name_problem(name);
    if (!bad.empty()) {
        why = "cannot make a session called \"" + name.toStdString() + "\": " + bad;
        return false;
    }
    if (QFileInfo::exists(QDir(newSessionRoot()).filePath(name))) {
        why = "a session called \"" + name.toStdString() + "\" is already in " +
              newSessionRoot().toStdString() + "; open it, or choose another name";
        return false;
    }

    const Snapshot& now = ctl_.snapshot();
    // The engine records into this session's tracks by position, so the new
    // one has as many, as an unsaved recording opened from the Open panel
    // does.
    Snapshot fresh = make_empty_session(static_cast<uint32_t>(now.edit->tracks.size()),
                                        now.settings.sample_rate);
    fresh.settings.name = name.toStdString();
    fresh.settings.session_id = model::new_session_id();
    fresh.settings.audio_format = now.settings.audio_format;
    if (!doc_.reset(std::move(fresh), std::string(), why)) {
        why = "the new session was not made: " + why;
        return false;
    }
    ctl_.document_replaced();
    live_.clear();
    before_open_.valid = true;
    before_open_.opened = QStringLiteral("The new session \"") + name + QStringLiteral("\"");
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
    emit sessionOpened();
    new_session_save_ = true;
    new_session_name_ = name;
    saveNewSession();
    return true;
}

void SessionModel::saveNewSession() {
    if (!new_session_save_ || doc_.opening()) return;
    new_session_save_ = false;
    // Another session was opened over it before the engine answered.
    if (!file_.isEmpty() || q(ctl_.snapshot().settings.name) != new_session_name_) return;
    std::string why;
    if (!saveSession(why)) {
        emit openNotice(QStringLiteral("The new session \"") + new_session_name_ +
                        QStringLiteral("\" is open but was not written: ") + q(why) +
                        QStringLiteral("; save it to keep it"));
    }
}

QString SessionModel::newSession(const QString& name, bool discard) {
    std::string why;
    if (makeNewSession(name, discard, why)) {
        emit notice(QStringLiteral("New session ") + name.trimmed());
        return QString();
    }
    return q(why);
}

}  // namespace sonora::ui
