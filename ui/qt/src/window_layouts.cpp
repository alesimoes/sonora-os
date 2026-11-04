// SPDX-License-Identifier: GPL-3.0-or-later
#include "window_layouts.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace sonora::ui {

QString default_layouts_file() {
    const QString env = qEnvironmentVariable("SONORA_WINDOW_LAYOUTS_FILE");
    if (!env.isEmpty()) return env;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("sonora/window-layouts.json"));
}

const QStringList& WindowLayouts::ids() {
    static const QStringList list{QStringLiteral("transport"), QStringLiteral("undo_history"),
                                  QStringLiteral("memory_locations"), QStringLiteral("latency")};
    return list;
}

bool WindowLayouts::known(const QString& id) { return ids().contains(id); }

void WindowLayouts::setView(const QString& view) {
    if (view == QStringLiteral("edit") || view == QStringLiteral("mix")) current_.view = view;
}

void WindowLayouts::setOpen(const QString& id, bool open) {
    if (known(id)) current_.windows[id].open = open;
}

void WindowLayouts::place(const QString& id, int x, int y) {
    if (!known(id)) return;
    FloatingWindow& w = current_.windows[id];
    w.placed = true;
    w.x = x;
    w.y = y;
}

bool WindowLayouts::stored(int slot) const { return slots_.count(slot) != 0; }

bool WindowLayouts::store(int slot, QString& why) {
    if (slot < 1 || slot > kSlots) {
        why = QStringLiteral("window configurations are numbered 1 to %1").arg(kSlots);
        return false;
    }
    slots_[slot] = current_;
    return true;
}

bool WindowLayouts::recall(int slot, QString& why) {
    if (slot < 1 || slot > kSlots) {
        why = QStringLiteral("window configurations are numbered 1 to %1").arg(kSlots);
        return false;
    }
    const auto it = slots_.find(slot);
    if (it == slots_.end()) {
        why = QStringLiteral("window configuration %1 is empty; store one there first").arg(slot);
        return false;
    }
    current_.view = it->second.view;
    for (const QString& id : ids()) {
        const auto w = it->second.windows.find(id);
        FloatingWindow& now = current_.windows[id];
        // A window the slot did not mention was not open when it was stored.
        if (w == it->second.windows.end()) {
            now.open = false;
            continue;
        }
        now.open = w->second.open;
        if (w->second.placed) {
            now.placed = true;
            now.x = w->second.x;
            now.y = w->second.y;
        }
    }
    return true;
}

QString WindowLayouts::summary(int slot) const {
    const auto it = slots_.find(slot);
    if (it == slots_.end()) return QStringLiteral("empty");
    int open = 0;
    for (const auto& kv : it->second.windows) open += kv.second.open ? 1 : 0;
    const QString view = it->second.view == QStringLiteral("mix") ? QStringLiteral("Mix")
                                                                   : QStringLiteral("Edit");
    return QStringLiteral("%1, %2 floating").arg(view).arg(open);
}

QByteArray WindowLayouts::toJson() const {
    QJsonObject slots;
    for (const auto& s : slots_) {
        QJsonObject windows;
        for (const auto& kv : s.second.windows) {
            QJsonObject w;
            w.insert(QStringLiteral("open"), kv.second.open);
            if (kv.second.placed) {
                w.insert(QStringLiteral("x"), kv.second.x);
                w.insert(QStringLiteral("y"), kv.second.y);
            }
            windows.insert(kv.first, w);
        }
        QJsonObject slot;
        slot.insert(QStringLiteral("view"), s.second.view);
        slot.insert(QStringLiteral("windows"), windows);
        slots.insert(QString::number(s.first), slot);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("configurations"), slots);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool WindowLayouts::fromJson(const QByteArray& bytes, QString& why) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (doc.isNull() || !doc.isObject()) {
        why = QStringLiteral("not a window configurations file: ") +
              (doc.isNull() ? err.errorString() : QStringLiteral("not an object"));
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("version")).toInt(0) != 1) {
        why = QStringLiteral("window configurations file is of a version this build does not know");
        return false;
    }
    std::map<int, WindowLayout> loaded;
    const QJsonObject slots = root.value(QStringLiteral("configurations")).toObject();
    for (auto it = slots.begin(); it != slots.end(); ++it) {
        bool ok = false;
        const int n = it.key().toInt(&ok);
        if (!ok || n < 1 || n > kSlots) continue;
        const QJsonObject slot = it.value().toObject();
        WindowLayout layout;
        if (slot.value(QStringLiteral("view")).toString() == QStringLiteral("mix")) {
            layout.view = QStringLiteral("mix");
        }
        const QJsonObject windows = slot.value(QStringLiteral("windows")).toObject();
        for (auto w = windows.begin(); w != windows.end(); ++w) {
            if (!known(w.key())) continue;
            const QJsonObject o = w.value().toObject();
            FloatingWindow fw;
            fw.open = o.value(QStringLiteral("open")).toBool(false);
            if (o.contains(QStringLiteral("x")) && o.contains(QStringLiteral("y"))) {
                fw.placed = true;
                fw.x = o.value(QStringLiteral("x")).toInt();
                fw.y = o.value(QStringLiteral("y")).toInt();
            }
            layout.windows[w.key()] = fw;
        }
        loaded[n] = std::move(layout);
    }
    slots_ = std::move(loaded);
    return true;
}

bool WindowLayouts::load(const QString& path, QString& why) {
    QFile f(path);
    if (!f.exists()) return true;
    if (!f.open(QIODevice::ReadOnly)) {
        why = QStringLiteral("cannot read ") + path + QStringLiteral(": ") + f.errorString();
        return false;
    }
    if (!fromJson(f.readAll(), why)) {
        why += QStringLiteral(" (") + path + QStringLiteral(")");
        return false;
    }
    return true;
}

bool WindowLayouts::save(const QString& path, QString& why) const {
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

}  // namespace sonora::ui
