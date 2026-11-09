// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a memory location recalls besides its position (ADR-0137): the Edit
// window's zoom, left edge, track height and hidden tracks, with a Zoom and a
// Track layout toggle per marker. Interface state, never the session's: it
// fits the display it was made on, so it is kept per machine in a file of
// its own, keyed by session id and marker id, and no model, operation or
// session format carries it.
//
// Qt Core only, no display: the tests drive it directly.
#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sonora::ui {

struct MarkerView {
    double fpp = 0.0;          // frames per pixel
    double start = 0.0;        // frame at the left edge
    double track_height = 0.0; // px
    std::vector<std::uint64_t> hidden;  // track ids
    bool recall_zoom = true;
    bool recall_tracks = true;
};

// $SONORA_MARKER_VIEWS_FILE, else $XDG_CONFIG_HOME/sonora/marker-views.json.
inline QString default_marker_views_file() {
    const QString env = qEnvironmentVariable("SONORA_MARKER_VIEWS_FILE");
    if (!env.isEmpty()) return env;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("sonora/marker-views.json"));
}

class MarkerViews {
public:
    const MarkerView* find(const std::string& session, std::uint64_t marker) const {
        const auto s = views_.find(session);
        if (s == views_.end()) return nullptr;
        const auto m = s->second.find(marker);
        return m == s->second.end() ? nullptr : &m->second;
    }

    // Replaces the captured view; the two toggles keep what the operator set.
    void store(const std::string& session, std::uint64_t marker, MarkerView v) {
        if (const MarkerView* old = find(session, marker)) {
            v.recall_zoom = old->recall_zoom;
            v.recall_tracks = old->recall_tracks;
        }
        views_[session][marker] = std::move(v);
    }

    bool clear(const std::string& session, std::uint64_t marker) {
        const auto s = views_.find(session);
        if (s == views_.end() || s->second.erase(marker) == 0) return false;
        if (s->second.empty()) views_.erase(s);
        return true;
    }

    // False when the marker has no stored view: a toggle on nothing lies.
    bool setRecallZoom(const std::string& session, std::uint64_t marker, bool on) {
        MarkerView* v = mutableFind(session, marker);
        if (v == nullptr) return false;
        v->recall_zoom = on;
        return true;
    }
    bool setRecallTracks(const std::string& session, std::uint64_t marker, bool on) {
        MarkerView* v = mutableFind(session, marker);
        if (v == nullptr) return false;
        v->recall_tracks = on;
        return true;
    }

    // Drops the entries of `session` whose marker is no longer in `live`.
    // Returns how many went.
    int prune(const std::string& session, const std::set<std::uint64_t>& live) {
        const auto s = views_.find(session);
        if (s == views_.end()) return 0;
        int dropped = 0;
        for (auto it = s->second.begin(); it != s->second.end();) {
            if (live.count(it->first) == 0) {
                it = s->second.erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
        if (s->second.empty()) views_.erase(s);
        return dropped;
    }

    QByteArray toJson() const {
        QJsonObject sessions;
        for (const auto& [sid, markers] : views_) {
            QJsonObject ms;
            for (const auto& [id, v] : markers) {
                QJsonArray hidden;
                for (std::uint64_t t : v.hidden) hidden.push_back(static_cast<double>(t));
                QJsonObject o;
                o.insert(QStringLiteral("fpp"), v.fpp);
                o.insert(QStringLiteral("start"), v.start);
                o.insert(QStringLiteral("track_height"), v.track_height);
                o.insert(QStringLiteral("hidden"), hidden);
                o.insert(QStringLiteral("recall_zoom"), v.recall_zoom);
                o.insert(QStringLiteral("recall_tracks"), v.recall_tracks);
                ms.insert(QString::number(id), o);
            }
            sessions.insert(QString::fromStdString(sid), ms);
        }
        QJsonObject root;
        root.insert(QStringLiteral("version"), 1);
        root.insert(QStringLiteral("sessions"), sessions);
        return QJsonDocument(root).toJson(QJsonDocument::Indented);
    }

    // Replaces the store. Anything that does not parse leaves it as it was.
    bool fromJson(const QByteArray& bytes, QString& why) {
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
        if (doc.isNull() || !doc.isObject()) {
            why = QStringLiteral("not a JSON object");
            return false;
        }
        const QJsonValue sessions = doc.object().value(QStringLiteral("sessions"));
        if (!sessions.isObject() || doc.object().value(QStringLiteral("version")).toInt() != 1) {
            why = QStringLiteral("unknown layout of the file");
            return false;
        }
        std::map<std::string, std::map<std::uint64_t, MarkerView>> next;
        for (const QString& sid : sessions.toObject().keys()) {
            const QJsonValue ms = sessions.toObject().value(sid);
            if (!ms.isObject()) {
                why = QStringLiteral("session ") + sid + QStringLiteral(" is not an object");
                return false;
            }
            for (const QString& key : ms.toObject().keys()) {
                bool ok = false;
                const qulonglong id = key.toULongLong(&ok);
                const QJsonValue o = ms.toObject().value(key);
                if (!ok || !o.isObject()) {
                    why = QStringLiteral("marker ") + key + QStringLiteral(" is malformed");
                    return false;
                }
                const QJsonObject j = o.toObject();
                MarkerView v;
                v.fpp = j.value(QStringLiteral("fpp")).toDouble();
                v.start = j.value(QStringLiteral("start")).toDouble();
                v.track_height = j.value(QStringLiteral("track_height")).toDouble();
                v.recall_zoom = j.value(QStringLiteral("recall_zoom")).toBool(true);
                v.recall_tracks = j.value(QStringLiteral("recall_tracks")).toBool(true);
                for (const QJsonValue& t : j.value(QStringLiteral("hidden")).toArray()) {
                    v.hidden.push_back(static_cast<std::uint64_t>(t.toDouble()));
                }
                next[sid.toStdString()][id] = std::move(v);
            }
        }
        views_ = std::move(next);
        return true;
    }

    // A missing file is not a fault. A damaged one is set aside as `.bad`,
    // so the next save does not overwrite what the operator may want to
    // inspect, and the store starts empty: false, with `why`, either way.
    bool load(const QString& path, QString& why) {
        QFile f(path);
        if (!f.exists()) return true;
        if (!f.open(QIODevice::ReadOnly)) {
            why = QStringLiteral("cannot read ") + path + QStringLiteral(": ") + f.errorString();
            return false;
        }
        const QByteArray bytes = f.readAll();
        f.close();
        if (fromJson(bytes, why)) return true;
        views_.clear();
        const QString bad = path + QStringLiteral(".bad");
        QFile::remove(bad);
        const bool moved = QFile::rename(path, bad);
        why += QStringLiteral(" (") + path +
               (moved ? QStringLiteral(", kept as ") + bad : QStringLiteral(", could not be moved aside")) +
               QStringLiteral(")");
        return false;
    }

    bool save(const QString& path, QString& why) const {
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            why = QStringLiteral("cannot create the folder for ") + path;
            return false;
        }
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(toJson()) < 0 || !f.commit()) {
            why = QStringLiteral("cannot write ") + path + QStringLiteral(": ") + f.errorString();
            return false;
        }
        return true;
    }

private:
    MarkerView* mutableFind(const std::string& session, std::uint64_t marker) {
        return const_cast<MarkerView*>(static_cast<const MarkerViews*>(this)->find(session, marker));
    }

    std::map<std::string, std::map<std::uint64_t, MarkerView>> views_;
};

}  // namespace sonora::ui
