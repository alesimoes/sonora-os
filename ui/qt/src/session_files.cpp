// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_files.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <string>

namespace sonora::ui {

namespace {

constexpr qint64 kMaxSessionBytes = qint64{256} << 20;  // 60x the spec's large session

// A session file name is "<Name>.sonora", and the engine writes it first as
// "<Name>.sonora.tmp" beside it; both must fit ext4's NAME_MAX, which is
// 255 bytes, not characters, so the name is limited in UTF-8 bytes. The
// limit is the one the interface's own saver had, so no name it accepted
// becomes one that cannot be saved.
constexpr int kNameMaxBytes = 255;
constexpr int kSuffixBytes = 7;     // ".sonora"
constexpr int kTempSuffixBytes = 7;
constexpr int kMaxSessionNameBytes = kNameMaxBytes - kSuffixBytes - kTempSuffixBytes;

constexpr const char* kBackupsDir = "Session File Backups";
// A 16 M-entry session is about 100 MiB of sequence files in all (ADR-0015
// §3), so one file past this is not one of ours.
constexpr qint64 kMaxSequenceBytes = qint64{1} << 30;

std::string text(const QString& s) { return s.toStdString(); }

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

std::vector<RecordingEntry> list_unsaved_recordings(const QString& dir) {
    std::vector<RecordingEntry> out;
    const QDir d(dir);
    for (const QString& name : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QDir folder(d.filePath(name));
        if (!folder.entryList({QStringLiteral("*.sonora")}, QDir::Files).isEmpty()) continue;
        const QDir audio(folder.filePath(QStringLiteral("Audio Files")));
        if (audio.entryList({QStringLiteral("*.wav")}, QDir::Files).isEmpty()) continue;
        out.push_back({name, folder.absolutePath()});
    }
    return out;
}

// Sequence files live in the session folder whichever kept file names them.
QString session_folder_of(const QString& file) {
    const QString dir = QFileInfo(file).absolutePath();
    if (QFileInfo(dir).fileName() == QLatin1String(kBackupsDir)) {
        return QFileInfo(dir).absolutePath();
    }
    return dir;
}

QString autosave_file_of(const QString& file) {
    const QString folder = session_folder_of(file);
    return QDir(folder).filePath(QLatin1String(kBackupsDir) + QLatin1Char('/') +
                                 QFileInfo(folder).fileName() + QStringLiteral(".autosave.sonora"));
}

bool newer_autosave(const QString& file, uint64_t generation, QString& autosave) {
    autosave = autosave_file_of(file);
    if (QFileInfo(autosave).absoluteFilePath() == QFileInfo(file).absoluteFilePath()) return false;
    QFile in(autosave);
    if (!in.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = in.readAll();
    uint64_t theirs = 0;
    return model::file_save_generation(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())),
                                       theirs) &&
           theirs > generation;
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
    FolderSequences sequences(session_folder_of(file));
    if (!model::deserialize(bytes, out, error, &sequences)) {
        why = text(QFileInfo(file).fileName()) + ": " + error.text();
        return false;
    }
    return true;
}

}  // namespace sonora::ui
