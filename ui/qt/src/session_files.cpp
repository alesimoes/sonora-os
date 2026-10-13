// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_files.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sonora::ui {

namespace {

constexpr qint64 kMaxSessionBytes = qint64{256} << 20;  // 60x the spec's large session

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
    if (name.size() > 120) return "a session name is at most 120 characters";
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

bool write_session_file(const QString& file, const std::string& bytes, std::string& why) {
    const QFileInfo info(file);
    const QString folder = info.absolutePath();
    if (!QDir().mkpath(folder)) {
        why = "cannot create the session folder " + text(folder);
        return false;
    }
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
    if (!sync_folder(folder)) {
        why = "saved " + text(file) + ", but the folder could not be synced; a power cut now "
              "could lose the save";
        return false;
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
