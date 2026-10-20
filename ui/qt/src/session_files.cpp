// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_files.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <cstdio>
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

namespace {

constexpr const char* kSequenceDir = "MIDI Sequences";
// A 16 M-entry session is about 100 MiB of sequence files in all (ADR-0015
// §3), so one file past this is not one of ours.
constexpr qint64 kMaxSequenceBytes = qint64{1} << 30;

// The file's bytes, before its rename: its data, not its metadata, is what
// must be on the medium (fsync where there is no fdatasync).
bool sync_data(int fd) {
#if defined(Q_OS_LINUX)
    return ::fdatasync(fd) == 0;
#else
    return sync_fd(fd);
#endif
}

bool rename_over(const QString& from, const QString& to) {
#if defined(Q_OS_UNIX)
    return ::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0;
#else
    QFile::remove(to);
    return QFile::rename(from, to);
#endif
}

// "<64 hex>.msq": a name the saver gave, the only kind cleanup may touch.
bool sequence_file_name(const QString& name, std::string& sha) {
    if (name.size() != 64 + 4 || !name.endsWith(QStringLiteral(".msq"))) return false;
    for (int i = 0; i < 64; ++i) {
        const QChar c = name[i];
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) {
            return false;
        }
    }
    sha = name.left(64).toStdString();
    return true;
}

class FolderSequences : public model::SequenceSource {
public:
    explicit FolderSequences(QString folder) : folder_(std::move(folder)) {}
    bool read(const std::string& file, std::string& bytes, std::string& why) override {
        QFile in(QDir(folder_).filePath(QString::fromStdString(file)));
        if (!in.exists()) {
            why = "the file is not there";
            return false;
        }
        if (!in.open(QIODevice::ReadOnly)) {
            why = "it cannot be read: " + text(in.errorString());
            return false;
        }
        if (in.size() > kMaxSequenceBytes) {
            why = "it is too large to be a sequence file";
            return false;
        }
        const QByteArray data = in.readAll();
        if (in.error() != QFileDevice::NoError) {
            why = "reading it failed: " + text(in.errorString());
            return false;
        }
        bytes.assign(data.constData(), static_cast<std::size_t>(data.size()));
        return true;
    }

private:
    QString folder_;
};

}  // namespace

void MidiSequenceFolder::enter(const QString& session_file) {
    const QString folder = QFileInfo(session_file).absolutePath();
    if (folder == folder_) return;
    folder_ = folder;
    files_.clear();
    const QDir dir(QDir(folder_).filePath(QLatin1String(kSequenceDir)));
    // Interrupted saves: no session file names a ".msq.tmp".
    for (const QString& name : dir.entryList({QStringLiteral("*.msq.tmp")}, QDir::Files)) {
        QFile::remove(dir.filePath(name));
    }
    read_kept();
}

void MidiSequenceFolder::read_kept() {
    const QDir session(folder_);
    QStringList kept;
    for (const QString& name : session.entryList({QStringLiteral("*.sonora")}, QDir::Files)) {
        kept << session.filePath(name);
    }
    const QDir backups(session.filePath(QStringLiteral("Session File Backups")));
    for (const QString& name : backups.entryList({QStringLiteral("*.sonora")}, QDir::Files)) {
        kept << backups.filePath(name);
    }
    for (const QString& path : kept) {
        QFile in(path);
        std::set<std::string> refs;
        std::string why;
        if (!in.open(QIODevice::ReadOnly) || in.size() > kMaxSessionBytes) {
            files_.kept_unreadable(text(path));
            continue;
        }
        const QByteArray data = in.readAll();
        if (!model::sequence_references(
                std::string_view(data.constData(), static_cast<std::size_t>(data.size())), refs,
                why)) {
            files_.kept_unreadable(text(path));
            continue;
        }
        files_.kept(text(path), std::move(refs));
    }
}

void MidiSequenceFolder::open(const QString& session_file, const std::set<std::string>& verified) {
    folder_.clear();
    enter(session_file);
    for (const std::string& sha : verified) files_.wrote(sha);
}

bool MidiSequenceFolder::write(const QString& session_file, const model::Snapshot& s,
                               std::string& why) {
    enter(session_file);
    std::vector<model::NamedSequence> todo;
    for (model::NamedSequence& n : model::named_sequences(s, digests_)) {
        if (n.data->missing || files_.on_disk(n.ref.sha256)) continue;
        todo.push_back(std::move(n));
    }
    if (todo.empty()) return true;
    const QString dir = QDir(folder_).filePath(QLatin1String(kSequenceDir));
    const bool created_folder = !QFileInfo::exists(folder_);
    const bool created = !QFileInfo::exists(dir);
    if (!QDir().mkpath(dir)) {
        why = "cannot create " + text(dir);
        return false;
    }
    // A new folder's entry is only durable once its parent is synced.
    if ((created_folder && !sync_folder(QFileInfo(folder_).absolutePath())) ||
        (created && !sync_folder(folder_))) {
        why = "cannot sync the session folder after creating " + text(dir);
        return false;
    }
    std::vector<std::string> renamed;
    for (model::NamedSequence& n : todo) {
        if (n.encoded.empty()) n.encoded = model::encode_sequence(*n.data);
        const QString target = QDir(dir).filePath(QString::fromStdString(n.ref.sha256) +
                                                  QStringLiteral(".msq"));
        const QString tmp = target + QStringLiteral(".tmp");
        QFile out(tmp);
        const auto size = static_cast<qint64>(n.encoded.size());
        // The name is the hash: a file torn by a crash must never carry it.
        const bool ok = out.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                        out.write(n.encoded.data(), size) == size && out.flush() &&
                        sync_data(out.handle());
        const std::string error = text(out.errorString());
        out.close();
        if (!ok || !rename_over(tmp, target)) {
            QFile::remove(tmp);
            why = "writing " + text(target) + " failed" + (ok ? std::string() : ": " + error);
            return false;
        }
        renamed.push_back(n.ref.sha256);
    }
    // One sync for the batch, before any session file names what it holds.
    if (!sync_folder(dir)) {
        why = "cannot sync " + text(dir) + " after writing its sequence files";
        return false;
    }
    for (const std::string& sha : renamed) files_.wrote(sha);
    return true;
}

void MidiSequenceFolder::saved(const QString& session_file, const std::string& bytes) {
    enter(session_file);
    std::set<std::string> refs;
    std::string why;
    if (model::sequence_references(bytes, refs, why)) {
        files_.kept(text(QFileInfo(session_file).absoluteFilePath()), std::move(refs));
    } else {
        files_.kept_unreadable(text(QFileInfo(session_file).absoluteFilePath()));
    }
    // A save may have kept a pre-migration copy, which is a kept file too.
    read_kept();
}

int MidiSequenceFolder::clean(std::string& caution) {
    if (folder_.isEmpty()) return 0;
    const std::vector<std::string> unreadable = files_.unreadable();
    if (!unreadable.empty()) {
        caution = "no sequence file is cleaned up while " + unreadable.front() +
                  " cannot be read: what it names is unknown";
        return 0;
    }
    const QDir dir(QDir(folder_).filePath(QLatin1String(kSequenceDir)));
    std::vector<std::string> present;
    for (const QString& name : dir.entryList({QStringLiteral("*.msq")}, QDir::Files)) {
        std::string sha;
        if (sequence_file_name(name, sha)) present.push_back(sha);
    }
    int gone = 0;
    for (const std::string& sha : files_.deletable(present)) {
        // A crash here leaves an extra file, never a missing one.
        if (QFile::remove(dir.filePath(QString::fromStdString(sha) + QStringLiteral(".msq")))) {
            files_.deleted(sha);
            ++gone;
        }
    }
    return gone;
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
    FolderSequences sequences(QFileInfo(file).absolutePath());
    if (!model::deserialize(bytes, out, error, &sequences)) {
        why = text(QFileInfo(file).fileName()) + ": " + error.text();
        return false;
    }
    return true;
}

}  // namespace sonora::ui
