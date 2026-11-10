// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_files.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <string>

#include "sonora/model/template_list.h"

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

// The device names Windows keeps in every folder: "CON", or "CON.sonora",
// opens the console, not a file, so a stick written here and read there
// would lose the session. Matched on the part before the first dot, with
// the spaces Windows ignores trimmed, in any case.
bool windows_reserved(const QString& name) {
    const QString stem = name.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
    static const QStringList fixed = {QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
                                      QStringLiteral("NUL"), QStringLiteral("CONIN$"),
                                      QStringLiteral("CONOUT$")};
    if (fixed.contains(stem)) return true;
    if (stem.size() != 4 || !(stem.startsWith(QLatin1String("COM")) || stem.startsWith(QLatin1String("LPT"))))
        return false;
    const QChar n = stem.at(3);
    // Windows also reserves the superscript digits 1 to 3.
    return (n >= QLatin1Char('0') && n <= QLatin1Char('9')) || n == QChar(0x00B9) || n == QChar(0x00B2) ||
           n == QChar(0x00B3);
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

QString default_sessions_dir() {
    const QString env = qEnvironmentVariable("SONORA_SESSIONS_DIR");
    if (!env.isEmpty()) return env;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation))
        .filePath(QStringLiteral("Sessions"));
}

QString default_templates_dir() {
    const QString env = qEnvironmentVariable("SONORA_TEMPLATES_DIR");
    if (!env.isEmpty()) return env;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("sonora/Session Templates"));
}

std::string session_name_problem(const QString& name) {
    if (name.trimmed().isEmpty()) return "a session needs a name";
    if (name != name.trimmed()) return "a session name cannot start or end with a space";
    if (name.startsWith(QLatin1Char('.'))) return "a session name cannot start with '.'";
    // Windows drops a trailing dot from a folder name, so "Mix." would be
    // read back there as "Mix" while its file is still "Mix..sonora".
    if (name.endsWith(QLatin1Char('.'))) return "a session name cannot end with '.'";
    if (windows_reserved(name)) {
        return "\"" + text(name.section(QLatin1Char('.'), 0, 0).trimmed()) +
               "\" is a device name on Windows, so a stick read there could not open the session";
    }
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

bool is_link(const QString& path) {
    const QFileInfo i(path);
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    if (i.isJunction()) return true;
#endif
    return i.isSymLink();
}

namespace {

// The templates folder on the disk, links left out: a linked template or
// category would let Rename and Delete reach a folder that is not ours.
class DiskTemplateFolder : public model::TemplateFolder {
public:
    std::vector<std::string> subfolders(const std::string& folder) override {
        std::vector<std::string> out;
        const QDir d(QString::fromStdString(folder));
        for (const QString& name : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
            if (is_link(d.filePath(name)) || !session_name_problem(name).empty()) continue;
            out.push_back(text(name));
        }
        return out;
    }
    bool read_file(const std::string& file, std::string& bytes) override {
        const QString path = QString::fromStdString(file);
        if (is_link(path) || !QFileInfo(path).isFile()) return false;
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly) || in.size() > kMaxSessionBytes) return false;
        const QByteArray data = in.readAll();
        if (in.error() != QFileDevice::NoError) return false;
        bytes.assign(data.constData(), static_cast<std::size_t>(data.size()));
        return true;
    }
};

}  // namespace

std::vector<model::TemplateInfo> list_session_templates(const QString& dir) {
    if (dir.isEmpty()) return {};
    DiskTemplateFolder folder;
    return model::list_templates(folder, text(QDir::cleanPath(dir)));
}

bool holds_audio(const QString& folder) {
    if (QFileInfo::exists(QDir(folder).filePath(QStringLiteral("Audio Files")))) return true;
    // Without QDir::CaseSensitive the filters match "Take.WAV" too.
    QDirIterator it(folder,
                    {QStringLiteral("*.wav"), QStringLiteral("*.bwf"), QStringLiteral("*.flac"),
                     QStringLiteral("*.aif"), QStringLiteral("*.aiff"), QStringLiteral("*.rf64")},
                    QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    return it.hasNext();
}

namespace {

// A read-only file cannot be deleted on Windows, and a read-only folder's
// entries cannot be on Linux; the template is ours to delete either way.
void make_writable(const QString& path, bool dir) {
    QFileDevice::Permissions p = QFile::permissions(path) | QFileDevice::WriteOwner | QFileDevice::ReadOwner;
    if (dir) p |= QFileDevice::ExeOwner;
    QFile::setPermissions(path, p);
}

// A link to a folder is a directory entry on Windows, removed with rmdir.
bool remove_link(const QString& path) { return QFile::remove(path) || QDir().rmdir(path); }

// Removes `path` without following a link, stopping at the first entry
// that stays; `left` names it.
bool remove_without_following(const QString& path, QString& left) {
    // Re-read here rather than trusted from the listing above: a folder
    // swapped for a link after it was listed is removed as a link.
    if (is_link(path)) {
        if (remove_link(path)) return true;
        left = path;
        return false;
    }
    make_writable(path, true);
    const QDir d(path);
    for (const QString& name :
         d.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        const QString p = d.filePath(name);
        bool gone = false;
        if (is_link(p)) {
            gone = remove_link(p);
        } else if (QFileInfo(p).isDir()) {
            if (!remove_without_following(p, left)) return false;
            gone = true;
        } else {
            make_writable(p, false);
            gone = QFile::remove(p);
        }
        if (!gone) {
            left = p;
            return false;
        }
    }
    if (QDir().rmdir(path)) return true;
    left = path;
    return false;
}

}  // namespace

bool remove_template_folder(const QString& folder, std::string& why) {
    const QFileInfo info(folder);
    QDir parent = info.absoluteDir();
    const QString name = info.fileName();
    QString staged = QStringLiteral(".") + name + QStringLiteral(".deleting");
    for (int n = 2; parent.exists(staged); ++n) {
        staged = QStringLiteral(".") + name + QStringLiteral(".deleting ") + QString::number(n);
    }
    // A read-only folder cannot be moved on every filesystem; a link is
    // moved as it is, its target's permissions untouched.
    if (!is_link(folder)) make_writable(folder, true);
    if (!parent.rename(name, staged)) {
        why = "could not delete " + text(folder) + ": it could not be moved aside, so nothing was removed";
        return false;
    }
    QString left;
    if (!remove_without_following(parent.filePath(staged), left)) {
        why = "could not delete " + text(left) + "; the template is no longer listed and what is left of it is in " +
              text(parent.filePath(staged));
        return false;
    }
    return true;
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
    const QFileInfo info(file);
    const QString suffix = QStringLiteral(".autosave.sonora");
    const bool in_backups = QFileInfo(info.absolutePath()).fileName() == QLatin1String(kBackupsDir);
    if (in_backups && info.fileName().endsWith(suffix) && info.fileName().size() > suffix.size()) {
        return info.absoluteFilePath();
    }
    QString stem = info.completeBaseName();
    if (in_backups) {
        // "Mix.20270115-080000[-n].sonora" is a rotated backup of "Mix".
        static const QRegularExpression rotated(QStringLiteral(R"(^(.+)\.\d{8}-\d{6}(-[2-9]\d{0,5}|-1\d{1,5})?$)"));
        const QRegularExpressionMatch m = rotated.match(stem);
        if (m.hasMatch()) stem = m.captured(1);
    }
    return QDir(folder).filePath(QLatin1String(kBackupsDir) + QLatin1Char('/') + stem + suffix);
}

// Before per-session-file autosaves: one per folder, named after it.
static QString legacy_autosave_file_of(const QString& file) {
    const QString folder = session_folder_of(file);
    return QDir(folder).filePath(QLatin1String(kBackupsDir) + QLatin1Char('/') +
                                 QFileInfo(folder).fileName() + QStringLiteral(".autosave.sonora"));
}

bool newer_autosave(const QString& file, uint64_t generation, QString& autosave) {
    // The newest of the two names wins; the older is read for compatibility.
    uint64_t best = generation;
    bool found = false;
    for (const QString& candidate : {autosave_file_of(file), legacy_autosave_file_of(file)}) {
        if (QFileInfo(candidate).absoluteFilePath() == QFileInfo(file).absoluteFilePath()) continue;
        QFile in(candidate);
        if (!in.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = in.readAll();
        uint64_t theirs = 0;
        if (model::file_save_generation(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())),
                                        theirs) &&
            theirs > best) {
            best = theirs;
            autosave = candidate;
            found = true;
        }
    }
    if (!found) autosave = autosave_file_of(file);
    return found;
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
