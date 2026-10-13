// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_files.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <string>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sonora::ui {

namespace {

constexpr qint64 kMaxSessionBytes = qint64{256} << 20;  // 60x the spec's large session

// A session file name is "<Name>.sonora", and QSaveFile writes it first as
// "<Name>.sonora.XXXXXX" beside it; both must fit ext4's NAME_MAX, which is
// 255 bytes, not characters, so the name is limited in UTF-8 bytes.
constexpr int kNameMaxBytes = 255;
constexpr int kSuffixBytes = 7;     // ".sonora"
constexpr int kTempSuffixBytes = 7; // QSaveFile's ".XXXXXX"
constexpr int kMaxSessionNameBytes = kNameMaxBytes - kSuffixBytes - kTempSuffixBytes;

std::string text(const QString& s) { return s.toStdString(); }

bool sync_fd(int fd) {
#if defined(Q_OS_UNIX)
    return ::fsync(fd) == 0;
#else
    (void)fd;
    return true;
#endif
}

// The rename is only durable once the folder holding it is synced
// (session-format.md, "Saving", step 4).
bool sync_folder(const QString& dir) {
#if defined(Q_OS_UNIX)
    const int fd = ::open(QFile::encodeName(dir).constData(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
#else
    (void)dir;
    return true;
#endif
}

// A temporary file, written, flushed and synced, renamed over `file`. The
// QSaveFile is gone when this returns, and with it any temporary file.
bool replace_file(const QString& file, const std::string& bytes, std::string& why) {
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly)) {
        why = "cannot write " + text(file) + ": " + text(out.errorString());
        return false;
    }
    const auto size = static_cast<qint64>(bytes.size());
    if (out.write(bytes.data(), size) != size || !out.flush() || !sync_fd(out.handle())) {
        why = "writing " + text(file) + " failed: " + text(out.errorString());
        out.cancelWriting();
        return false;
    }
    if (!out.commit()) {
        why = "saving " + text(file) + " failed: " + text(out.errorString());
        return false;
    }
    return true;
}

// session-format.md, "Versioning": before the first save over a file of an
// older format version, the original is copied to "Session File
// Backups/<Name>.v<old>.sonora" and never rotated out, so the build it came
// from (an A/B rollback) can still open it. A save that cannot keep it does
// not overwrite it either.
bool keep_older_version(const QString& file, std::string& why) {
    QFile in(file);
    if (!in.exists()) return true;
    if (!in.open(QIODevice::ReadOnly)) {
        why = "cannot read " + text(file) + " to keep a copy of it: " + text(in.errorString());
        return false;
    }
    if (in.size() > kMaxSessionBytes) return true;
    const QByteArray old = in.readAll();
    std::int64_t version = 0;
    if (!model::file_format_version(std::string_view(old.constData(), static_cast<std::size_t>(old.size())),
                                    version) ||
        version == model::kFormatVersion) {
        return true;
    }
    // Forward compatibility: a newer build's file is never overwritten by an
    // older one, which cannot even read what it would destroy.
    if (version > model::kFormatVersion) {
        why = text(file) + " is a session file of version " + std::to_string(version) +
              ", newer than the version " + std::to_string(model::kFormatVersion) +
              " this build writes; it is left as it is. Save under another name";
        return false;
    }
    const QFileInfo info(file);
    const QString backups = QDir(info.absolutePath()).filePath(QStringLiteral("Session File Backups"));
    const QString copy = QDir(backups).filePath(info.completeBaseName() + QStringLiteral(".v") +
                                                QString::number(version) + QStringLiteral(".sonora"));
    // The first save over it kept it; that copy is the one to keep.
    if (QFileInfo::exists(copy)) return true;
    const bool created = !QFileInfo::exists(backups);
    if (!QDir().mkpath(backups)) {
        why = "cannot create " + text(backups) + " to keep the version " + std::to_string(version) +
              " file";
        return false;
    }
    if (created && !sync_folder(info.absolutePath())) {
        why = "cannot sync the session folder after creating " + text(backups);
        return false;
    }
    if (!replace_file(copy, std::string(old.constData(), static_cast<std::size_t>(old.size())), why)) {
        return false;
    }
    if (!sync_folder(backups)) {
        why = "cannot sync " + text(backups) + " after keeping the version " +
              std::to_string(version) + " file";
        return false;
    }
    return true;
}

}  // namespace

QString default_sessions_dir() {
    const QString env = qEnvironmentVariable("SONORA_SESSIONS_DIR");
    if (!env.isEmpty()) return env;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation))
        .filePath(QStringLiteral("Sessions"));
}

std::string session_name_problem(const QString& name) {
    if (name.trimmed().isEmpty()) return "a session needs a name";
    if (name != name.trimmed()) return "a session name cannot start or end with a space";
    if (name.startsWith(QLatin1Char('.'))) return "a session name cannot start with '.'";
    if (name.toUtf8().size() > kMaxSessionNameBytes) {
        return "a session name is at most " + std::to_string(kMaxSessionNameBytes) +
               " bytes as UTF-8 (" + std::to_string(kMaxSessionNameBytes / 3) +
               " characters of Chinese or Japanese)";
    }
    for (const QChar c : name) {
        if (c.unicode() < 0x20 || c == QLatin1Char('/') || c == QLatin1Char('\\') ||
            c == QLatin1Char(':')) {
            return "a session name cannot contain '/', '\\', ':' or a control character";
        }
    }
    return {};
}

QString session_file_path(const QString& dir, const QString& name) {
    return QDir(QDir(dir).filePath(name)).filePath(name + QStringLiteral(".sonora"));
}

QString unused_session_name(const QString& dir, const QString& name) {
    const QDir d(dir);
    if (!d.exists(name)) return name;
    for (int n = 2;; ++n) {
        const QString candidate = name + QLatin1Char(' ') + QString::number(n);
        if (!d.exists(candidate)) return candidate;
    }
}

std::vector<SessionEntry> list_sessions(const QString& dir) {
    std::vector<SessionEntry> out;
    const QDir d(dir);
    for (const QString& name : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString file = session_file_path(dir, name);
        if (QFileInfo(file).isFile()) out.push_back({name, file});
    }
    return out;
}

bool write_session_file(const QString& file, const std::string& bytes, std::string& why,
                        std::string& caution) {
    caution.clear();
    const QString folder = QFileInfo(file).absolutePath();
    const bool created = !QFileInfo::exists(folder);
    if (!QDir().mkpath(folder)) {
        why = "cannot create the session folder " + text(folder);
        return false;
    }
    bool ok = true;
    // A new folder's own entry is only durable once its parent is synced.
    if (created && !sync_folder(QFileInfo(folder).absolutePath())) {
        why = "cannot sync the sessions folder after creating " + text(folder);
        ok = false;
    }
    if (ok) ok = keep_older_version(file, why);
    if (ok) ok = replace_file(file, bytes, why);
    if (!ok) {
        // Empty by now: replace_file's QSaveFile took its temporary file with it.
        if (created) QDir().rmdir(folder);
        return false;
    }
    // The rename has landed: the file holds the new session, so this is a
    // save. Reporting it as a failure would make a first save retry into
    // "<Name> 2" next to the file it just wrote.
    if (!sync_folder(folder)) {
        caution = "the folder could not be synced; a power cut now could lose this save";
    }
    return true;
}

bool read_session_file(const QString& file, std::string& bytes, model::Loaded& out,
                       std::string& why) {
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly)) {
        why = "cannot open " + text(file) + ": " + text(in.errorString());
        return false;
    }
    if (in.size() > kMaxSessionBytes) {
        why = text(file) + " is too large to be a session file";
        return false;
    }
    const QByteArray data = in.readAll();
    if (in.error() != QFileDevice::NoError) {
        why = "reading " + text(file) + " failed: " + text(in.errorString());
        return false;
    }
    bytes.assign(data.constData(), static_cast<std::size_t>(data.size()));
    model::LoadError error;
    if (!model::deserialize(bytes, out, error)) {
        why = text(QFileInfo(file).fileName()) + ": " + error.text();
        return false;
    }
    return true;
}

}  // namespace sonora::ui
