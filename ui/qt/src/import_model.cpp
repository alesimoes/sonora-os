// SPDX-License-Identifier: GPL-3.0-or-later
#include "import_model.h"

#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QVariantMap>

namespace sonora::ui {
namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

QString duration_text(int64_t frames, uint32_t rate) {
    const double s = rate > 0 ? static_cast<double>(frames) / rate : 0.0;
    const int m = static_cast<int>(s / 60.0);
    return QStringLiteral("%1:%2").arg(m).arg(s - m * 60.0, 4, 'f', 1, QLatin1Char('0'));
}

QString rate_text(uint32_t rate) {
    return QString::number(rate / 1000.0, 'g', 4) + QStringLiteral(" kHz");
}

// Audio the browser shows but cannot import, so it says so instead of
// hiding a file the operator knows is there.
bool other_audio(const QString& suffix) {
    static const QStringList kinds = {"aif", "aiff", "aifc", "flac", "mp3", "ogg", "m4a", "wma", "caf", "w64"};
    return kinds.contains(suffix.toLower());
}

}  // namespace

ImportModel::ImportModel(EditController& ctl, Hooks hooks, QObject* parent)
    : QObject(parent), ctl_(ctl), hooks_(std::move(hooks)), importer_(ctl) {
    timer_.setInterval(50);
    connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
}

void ImportModel::setAtInsertion(bool on) {
    if (at_insertion_ == on) return;
    at_insertion_ = on;
    emit optionsChanged();
}

QString ImportModel::status() const {
    if (!busy()) return QString();
    const int n = files_;
    return QStringLiteral("Converting ") + q(importer_.current()) +
           (n > 1 ? QStringLiteral(" (%1 files)").arg(n) : QString());
}

QString ImportModel::summary() const {
    const int n = selectedCount();
    if (n == 0) return QStringLiteral("Choose one or more WAV files.");
    return (n == 1 ? QStringLiteral("1 file") : QStringLiteral("%1 files").arg(n)) +
           QStringLiteral(" -> ") + (n == 1 ? QStringLiteral("a new track") : QStringLiteral("%1 new tracks").arg(n)) +
           (at_insertion_ ? QStringLiteral(" at the edit cursor") : QStringLiteral(" at the session start"));
}

QVariantList ImportModel::places() const {
    QVariantList out;
    const auto add = [&](const QString& name, const QString& path) {
        if (path.isEmpty() || !QFileInfo(path).isDir()) return;
        QVariantMap m;
        m["name"] = name;
        m["path"] = QDir::cleanPath(path);
        out.push_back(m);
    };
    const QString session = hooks_.session_folder ? hooks_.session_folder() : QString();
    if (!session.isEmpty()) add(QStringLiteral("Session audio"), session + QStringLiteral("/Audio Files"));
    add(QStringLiteral("Home"), QDir::homePath());
    if (hooks_.sessions_dir) add(QStringLiteral("Sessions"), hooks_.sessions_dir());
    // USB sticks and cards: where audio comes from on a machine with no
    // network and no desktop.
    for (const QString& base : {QStringLiteral("/media"), QStringLiteral("/run/media")}) {
        for (const QFileInfo& f : QDir(base).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (base == QStringLiteral("/run/media")) {
                for (const QFileInfo& g : QDir(f.absoluteFilePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
                    add(g.fileName(), g.absoluteFilePath());
            } else {
                add(f.fileName(), f.absoluteFilePath());
            }
        }
    }
    return out;
}

void ImportModel::show() {
    setRefusal(QString());
    chosen_.clear();
    order_.clear();
    const QString session = hooks_.session_folder ? hooks_.session_folder() : QString();
    QString start = session.isEmpty() ? QDir::homePath() : session + QStringLiteral("/Audio Files");
    if (!QFileInfo(start).isDir()) start = session.isEmpty() ? QDir::homePath() : session;
    folder_ = QDir::cleanPath(start);
    list();
    emit folderChanged();
}

void ImportModel::open(const QString& path) {
    if (busy() || !QFileInfo(path).isDir()) return;
    folder_ = QDir::cleanPath(path);
    list();
    emit folderChanged();
}

void ImportModel::up() {
    QDir d(folder_);
    if (busy() || !d.cdUp()) return;
    open(d.absolutePath());
}

void ImportModel::toggle(const QString& path) {
    if (busy()) return;
    // Only a file the list shows as importable can be chosen.
    bool usable = chosen_.count(path) > 0;
    for (const QVariant& v : entries_) {
        const QVariantMap m = v.toMap();
        if (m.value("path").toString() == path) usable = !m.value("dir").toBool() && m.value("usable").toBool();
    }
    if (!usable) return;
    if (chosen_.erase(path) == 0) {
        chosen_.insert(path);
        order_.push_back(path);
    } else {
        order_.removeAll(path);
    }
    list();
}

void ImportModel::clearSelection() {
    if (busy()) return;
    chosen_.clear();
    order_.clear();
    list();
}

void ImportModel::list() {
    entries_.clear();
    const uint32_t rate = ctl_.snapshot().settings.sample_rate;
    const QDir dir(folder_);
    for (const QFileInfo& f : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase)) {
        QVariantMap m;
        m["name"] = f.fileName();
        m["path"] = f.absoluteFilePath();
        m["dir"] = true;
        m["usable"] = true;
        m["chosen"] = false;
        m["detail"] = QString();
        entries_.push_back(m);
    }
    for (const QFileInfo& f : dir.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase)) {
        const QString suffix = f.suffix();
        const bool wav = suffix.compare("wav", Qt::CaseInsensitive) == 0 ||
                         suffix.compare("wave", Qt::CaseInsensitive) == 0 ||
                         suffix.compare("bwf", Qt::CaseInsensitive) == 0;
        if (!wav && !other_audio(suffix)) continue;
        QVariantMap m;
        m["name"] = f.fileName();
        m["path"] = f.absoluteFilePath();
        m["dir"] = false;
        m["chosen"] = chosen_.count(f.absoluteFilePath()) > 0;
        QString detail;
        bool usable = false;
        if (!wav) {
            detail = QStringLiteral("Cannot import yet: only WAV files");
        } else {
            AudioInfo info;
            std::string why;
            if (!probe_audio(f.absoluteFilePath().toStdString(), info, why)) {
                detail = QStringLiteral("Cannot import: ") + q(why);
            } else if (info.channels > kImportMaxChannels) {
                detail = QStringLiteral("Cannot import: %1 channels; a track is mono or stereo").arg(info.channels);
            } else {
                usable = true;
                detail = (info.channels == 1 ? QStringLiteral("mono") : QStringLiteral("stereo")) +
                         QStringLiteral("  ·  ") + rate_text(info.sample_rate) +
                         QStringLiteral("  ·  ") + duration_text(info.frames, info.sample_rate);
                if (info.sample_rate != rate) {
                    detail += QStringLiteral("  ·  converted to ") + rate_text(rate);
                }
            }
        }
        m["usable"] = usable;
        m["detail"] = detail;
        entries_.push_back(m);
    }
    emit entriesChanged();
}

void ImportModel::setRefusal(const QString& why) {
    if (refusal_ == why) return;
    refusal_ = why;
    emit refusalChanged();
}

bool ImportModel::startImport() {
    setRefusal(QString());
    if (hooks_.recording && hooks_.recording()) {
        setRefusal(QStringLiteral("Stop recording first: converting files while a take is written risks dropouts in it."));
        return false;
    }
    std::vector<std::string> files;
    for (const QString& f : order_) files.push_back(f.toStdString());
    std::string why;
    const QString folder = hooks_.session_folder ? hooks_.session_folder() : QString();
    if (!importer_.start(files, folder.toStdString(), at_insertion_, why)) {
        setRefusal(q(why));
        return false;
    }
    files_ = static_cast<int>(files.size());
    emit busyChanged();
    emit progressChanged();
    timer_.start();
    return true;
}

void ImportModel::cancelImport() { importer_.cancel(); }

void ImportModel::poll() {
    using S = AudioImporter::State;
    const S s = importer_.state();
    if (s == S::Running) {
        emit progressChanged();
        return;
    }
    timer_.stop();
    std::string why;
    if (s == S::Done) {
        const QString folder = hooks_.session_folder ? hooks_.session_folder() : QString();
        if (importer_.commit(folder.toStdString(), why)) {
            chosen_.clear();
            order_.clear();
            list();
            emit busyChanged();
            emit progressChanged();
            emit imported(files_);
            return;
        }
        setRefusal(q(why));
        emit heldNotice(q(why));
    } else if (s == S::Failed) {
        setRefusal(q(importer_.failure()));
        emit heldNotice(q(importer_.failure()));
    } else if (s == S::Cancelled) {
        setRefusal(QStringLiteral("Import cancelled; nothing was added to the session."));
    }
    importer_.reset();
    emit busyChanged();
    emit progressChanged();
}

bool ImportModel::finishNow() {
    while (importer_.state() == AudioImporter::State::Running) QThread::msleep(5);
    poll();
    return refusal_.isEmpty();
}

}  // namespace sonora::ui
