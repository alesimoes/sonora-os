// SPDX-License-Identifier: GPL-3.0-or-later
#include "track_views.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <iterator>

namespace sonora::ui {

namespace {

bool is_plain(const TrackView& v) { return v.height == TrackHeight::Medium && !v.hidden; }

}  // namespace

std::string TrackViews::default_path() {
    const QString env = qEnvironmentVariable("SONORA_TRACK_VIEWS_FILE");
    if (!env.isEmpty()) return env.toStdString();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("sonora/track-views.json"))
        .toStdString();
}

bool TrackViews::load(std::string& why) {
    views_.clear();
    if (path_.empty()) return true;
    QFile f(QString::fromStdString(path_));
    if (!f.exists()) return true;
    if (!f.open(QIODevice::ReadOnly)) {
        why = "cannot read " + path_ + ": " + f.errorString().toStdString();
        return false;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (doc.isNull() || !doc.isObject() || !doc.object().value(QStringLiteral("sessions")).isObject()) {
        why = path_ + " is not a track views file" +
              (doc.isNull() ? ": " + err.errorString().toStdString() : std::string()) +
              "; every track is shown at its usual height";
        return false;
    }
    const QJsonObject sessions = doc.object().value(QStringLiteral("sessions")).toObject();
    for (auto s = sessions.begin(); s != sessions.end(); ++s) {
        const QJsonObject tracks = s.value().toObject();
        for (auto t = tracks.begin(); t != tracks.end(); ++t) {
            bool ok = false;
            const uint32_t track = t.key().toUInt(&ok);
            const QJsonObject o = t.value().toObject();
            TrackView v;
            if (!ok || !parse_track_height(o.value(QStringLiteral("height")).toString().toStdString(),
                                           v.height)) {
                continue;
            }
            v.hidden = o.value(QStringLiteral("hidden")).toBool(false);
            if (!is_plain(v)) views_[s.key().toStdString()][track] = v;
        }
    }
    return true;
}

TrackView TrackViews::get(const std::string& session_id, uint32_t track) const {
    const auto s = views_.find(session_id);
    if (s == views_.end()) return {};
    const auto t = s->second.find(track);
    return t == s->second.end() ? TrackView{} : t->second;
}

void TrackViews::set(const std::string& session_id, uint32_t track, const TrackView& v) {
    if (is_plain(v)) {
        auto s = views_.find(session_id);
        if (s != views_.end()) {
            s->second.erase(track);
            if (s->second.empty()) views_.erase(s);
        }
    } else {
        views_[session_id][track] = v;
    }
    save();
}

uint32_t TrackViews::show_all(const std::string& session_id) {
    uint32_t n = 0;
    const auto s = views_.find(session_id);
    if (s == views_.end()) return 0;
    for (auto it = s->second.begin(); it != s->second.end();) {
        if (!it->second.hidden) {
            ++it;
            continue;
        }
        ++n;
        it->second.hidden = false;
        it = is_plain(it->second) ? s->second.erase(it) : std::next(it);
    }
    if (s->second.empty()) views_.erase(s);
    if (n > 0) save();
    return n;
}

void TrackViews::forget(const std::string& session_id) {
    views_.erase(session_id);
    save();
}

uint32_t TrackViews::customised(const std::string& session_id) const {
    const auto s = views_.find(session_id);
    return s == views_.end() ? 0 : static_cast<uint32_t>(s->second.size());
}

void TrackViews::save() {
    save_error_.clear();
    if (path_.empty()) return;
    QJsonObject sessions;
    for (const auto& [id, tracks] : views_) {
        QJsonObject o;
        for (const auto& [track, v] : tracks) {
            o.insert(QString::number(track),
                     QJsonObject{{QStringLiteral("height"),
                                  QString::fromLatin1(track_height_name(v.height))},
                                 {QStringLiteral("hidden"), v.hidden}});
        }
        sessions.insert(QString::fromStdString(id), o);
    }
    const QString path = QString::fromStdString(path_);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        save_error_ = "cannot create the folder for " + path_;
        return;
    }
    QSaveFile f(path);
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                       {QStringLiteral("sessions"), sessions}})
                                 .toJson(QJsonDocument::Indented);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) < 0 || !f.commit()) {
        save_error_ = "cannot write " + path_ + ": " + f.errorString().toStdString();
    }
}

}  // namespace sonora::ui
