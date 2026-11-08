// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin_favourites.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "preferences.h"

namespace sonora::ui {
namespace {

constexpr int kVersion = 1;

bool is_key(const std::string& k) {
    const auto space = k.find(' ');
    if (space == std::string::npos || space + 1 >= k.size()) return false;
    const std::string format = k.substr(0, space);
    return (format == "lv2" || format == "vst3") && k.find(' ', space + 1) == std::string::npos;
}

}  // namespace

std::string plugin_key(api::PluginFormat format, const std::string& id) {
    return (format == api::PluginFormat::Vst3 ? "vst3 " : "lv2 ") + id;
}

QString default_favourites_file() {
    const QString env = qEnvironmentVariable("SONORA_PLUGIN_FAVOURITES_FILE");
    if (!env.isEmpty()) return env;
    return QFileInfo(default_preferences_file()).dir().filePath(QStringLiteral("plugin-favourites.json"));
}

bool FileFavouritesStore::load(std::set<std::string>& out, std::string& why) {
    QFile f(file_);
    if (!f.exists()) {
        out.clear();
        return true;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        why = "cannot read " + file_.toStdString() + ": " + f.errorString().toStdString();
        return false;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (doc.isNull() || !doc.isObject()) {
        why = file_.toStdString() + " is not a favourites file (" + err.errorString().toStdString() + ")";
        return false;
    }
    const QJsonObject o = doc.object();
    const int version = o.value(QStringLiteral("version")).toInt(0);
    if (version != kVersion) {
        why = file_.toStdString() + " is version " + std::to_string(version) + "; this one reads version " +
              std::to_string(kVersion);
        return false;
    }
    std::set<std::string> keys;
    for (const QJsonValue& v : o.value(QStringLiteral("favourites")).toArray()) {
        const std::string k = v.toString().toStdString();
        if (!is_key(k)) {
            why = file_.toStdString() + ": \"" + k + "\" is not \"lv2 <uri>\" or \"vst3 <class id>\"";
            return false;
        }
        keys.insert(k);
    }
    out = std::move(keys);
    return true;
}

bool FileFavouritesStore::save(const std::set<std::string>& keys, std::string& why) {
    if (!QDir().mkpath(QFileInfo(file_).absolutePath())) {
        why = "cannot create the folder of " + file_.toStdString();
        return false;
    }
    QJsonArray list;
    for (const std::string& k : keys) list.append(QString::fromStdString(k));
    QJsonObject o;
    o[QStringLiteral("version")] = kVersion;
    o[QStringLiteral("favourites")] = list;
    QSaveFile f(file_);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(o).toJson(QJsonDocument::Indented)) < 0 ||
        !f.commit()) {
        why = "cannot write " + file_.toStdString() + ": " + f.errorString().toStdString();
        return false;
    }
    return true;
}

bool PluginFavourites::load(std::string& why) {
    if (store_.load(keys_, why)) {
        unreadable_.clear();
        return true;
    }
    unreadable_ = why;
    return false;
}

bool PluginFavourites::set(const std::string& key, bool favourite, std::string& why) {
    if (!unreadable_.empty()) {
        why = "favourites are not saved while their file cannot be read (" + unreadable_ + ")";
        return false;
    }
    if (contains(key) == favourite) return true;
    std::set<std::string> next = keys_;
    if (favourite) next.insert(key);
    else next.erase(key);
    if (!store_.save(next, why)) return false;
    keys_ = std::move(next);
    return true;
}

}  // namespace sonora::ui
