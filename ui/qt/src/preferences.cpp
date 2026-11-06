// SPDX-License-Identifier: GPL-3.0-or-later
#include "preferences.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <cstring>

namespace sonora::ui {
namespace {

constexpr int kVersion = 1;

template <typename E, std::size_t N>
bool pick(const QJsonObject& o, const char* key, const E (&all)[N], const char* (*name)(E), E& out,
          std::string& why) {
    const QString want = o.value(QString::fromLatin1(key)).toString();
    if (want.isEmpty()) return true;   // not written: the default stays
    for (E e : all) {
        if (want == QString::fromLatin1(name(e))) {
            out = e;
            return true;
        }
    }
    why = std::string("unknown ") + key + " \"" + want.toStdString() + "\"";
    return false;
}

constexpr TimeFormat kFormats[] = {TimeFormat::BarsBeats, TimeFormat::MinSec, TimeFormat::Timecode,
                                   TimeFormat::Samples};
constexpr GridValue kGrids[] = {GridValue::Bar, GridValue::Beat, GridValue::Eighth,
                                GridValue::Sixteenth};
constexpr EditMode kModes[] = {EditMode::Shuffle, EditMode::Slip, EditMode::Spot, EditMode::Grid};

}  // namespace

QString default_preferences_file() {
    const QString env = qEnvironmentVariable("SONORA_PREFERENCES_FILE");
    if (!env.isEmpty()) return env;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("sonora/preferences.json"));
}

bool load_preferences(const QString& file, Preferences& out, std::string& why) {
    QFile f(file);
    if (!f.exists()) return true;
    if (!f.open(QIODevice::ReadOnly)) {
        why = "cannot read " + file.toStdString() + ": " + f.errorString().toStdString();
        return false;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (doc.isNull() || !doc.isObject()) {
        why = file.toStdString() + " is not a preferences file (" +
              err.errorString().toStdString() + ")";
        return false;
    }
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("version")).toInt(0) != kVersion) {
        why = file.toStdString() + " is version " +
              std::to_string(o.value(QStringLiteral("version")).toInt(0)) +
              "; this one reads version " + std::to_string(kVersion);
        return false;
    }
    Preferences p = out;
    std::string reason;
    if (!pick(o, "counter", kFormats, time_format_name, p.counter, reason) ||
        !pick(o, "grid", kGrids, grid_value_name, p.grid, reason) ||
        !pick(o, "mode", kModes, edit_mode_name, p.mode, reason)) {
        why = file.toStdString() + ": " + reason;
        return false;
    }
    out = p;
    return true;
}

bool save_preferences(const QString& file, const Preferences& prefs, std::string& why) {
    if (!QDir().mkpath(QFileInfo(file).absolutePath())) {
        why = "cannot create the folder of " + file.toStdString();
        return false;
    }
    QJsonObject o;
    o[QStringLiteral("version")] = kVersion;
    o[QStringLiteral("counter")] = QString::fromLatin1(time_format_name(prefs.counter));
    o[QStringLiteral("grid")] = QString::fromLatin1(grid_value_name(prefs.grid));
    o[QStringLiteral("mode")] = QString::fromLatin1(edit_mode_name(prefs.mode));
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly) ||
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented)) < 0 || !f.commit()) {
        why = "cannot write " + file.toStdString() + ": " + f.errorString().toStdString();
        return false;
    }
    return true;
}

}  // namespace sonora::ui
