// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin_manager_model.h"

#include <QVariantMap>
#include <algorithm>
#include <cctype>
#include <set>

#include "sonora/api/plugin_refusal.h"

namespace sonora::ui {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

QString q(const std::string& s) { return QString::fromStdString(s); }

// What a browser groups by, broadest first, less VST3's catch-all "Fx",
// which every effect carries and so tells nothing (as InsertController).
std::vector<std::string> shown_categories(const api::PluginInfo& p) {
    std::vector<std::string> out;
    for (const std::string& c : p.categories) {
        if (!c.empty() && c != "Fx") out.push_back(c);
    }
    return out;
}

std::string joined(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& s : parts) out += (out.empty() ? "" : " / ") + s;
    return out;
}

QVariantMap to_map(const PluginManagerModel::Row& r) {
    QVariantMap m;
    m.insert(QStringLiteral("key"), q(r.key));
    m.insert(QStringLiteral("index"), r.index);
    m.insert(QStringLiteral("name"), q(r.name));
    m.insert(QStringLiteral("vendor"), q(r.vendor));
    m.insert(QStringLiteral("version"), q(r.version));
    m.insert(QStringLiteral("format"), q(r.format));
    m.insert(QStringLiteral("category"), q(r.category));
    m.insert(QStringLiteral("bridge"), q(r.bridge));
    m.insert(QStringLiteral("usable"), r.usable);
    m.insert(QStringLiteral("blocked"), r.blocked);
    m.insert(QStringLiteral("refusal"), q(r.refusal));
    m.insert(QStringLiteral("favourite"), r.favourite);
    m.insert(QStringLiteral("state"), QString::fromLatin1(r.state()));
    return m;
}

}  // namespace

PluginManagerModel::PluginManagerModel(PluginFavourites& favourites, Engine engine, QObject* parent)
    : QObject(parent), favourites_(favourites), engine_(std::move(engine)) {}

void PluginManagerModel::onCatalog(const api::PluginCatalog& c) {
    catalog_ = c;
    has_catalog_ = true;
    rebuild();
}

void PluginManagerModel::rebuild() {
    all_.clear();
    all_.reserve(catalog_.plugins.size());
    for (std::size_t i = 0; i < catalog_.plugins.size(); ++i) {
        const api::PluginInfo& p = catalog_.plugins[i];
        Row r;
        r.key = plugin_key(p.format, p.id);
        r.index = static_cast<int>(i);
        r.name = p.name.empty() ? p.id : p.name;
        r.vendor = p.vendor;
        r.version = p.version;
        r.format = p.format == api::PluginFormat::Vst3 ? "VST3" : "LV2";
        r.category = joined(shown_categories(p));
        r.bridge = p.bridge == api::PluginBridge::Yabridge ? "Windows (Wine)" : "";
        r.usable = p.usable;
        r.refusal = p.refusal;
        // A usable entry is never "blocked", whatever its words say.
        r.blocked = !p.usable && api::is_blocked_refusal(p.refusal);
        r.favourite = favourites_.contains(r.key);
        all_.push_back(std::move(r));
    }
    std::stable_sort(all_.begin(), all_.end(),
                     [](const Row& a, const Row& b) { return lower(a.name) < lower(b.name); });
    refilter();
}

void PluginManagerModel::refilter() {
    filtered_.clear();
    const std::string needle = lower(search_.toStdString());
    const std::string cat = category_.toStdString();
    for (const Row& r : all_) {
        if (show_ == QLatin1String("favourites") && !r.favourite) continue;
        if (show_ == QLatin1String("unusable") && r.usable) continue;
        if (!cat.empty()) {
            const auto cats = shown_categories(catalog_.plugins[static_cast<std::size_t>(r.index)]);
            if (std::find(cats.begin(), cats.end(), cat) == cats.end()) continue;
        }
        if (!needle.empty() && lower(r.name).find(needle) == std::string::npos &&
            lower(r.vendor).find(needle) == std::string::npos) {
            continue;
        }
        filtered_.push_back(r);
    }
    emit changed();
}

QVariantList PluginManagerModel::rows() const {
    QVariantList out;
    for (const Row& r : filtered_) out << to_map(r);
    return out;
}

QStringList PluginManagerModel::categories() const {
    std::set<std::string> all;
    for (const api::PluginInfo& p : catalog_.plugins) {
        for (const std::string& c : shown_categories(p)) all.insert(c);
    }
    QStringList out;
    for (const std::string& c : all) out << q(c);
    return out;
}

QString PluginManagerModel::summary() const {
    if (!has_catalog_) return QStringLiteral("The engine has not sent its plug-in catalog.");
    int blocked = 0, refused = 0;
    for (const Row& r : all_) {
        if (r.blocked) ++blocked;
        else if (!r.usable) ++refused;
    }
    QString s = QString::number(all_.size()) + (all_.size() == 1 ? QStringLiteral(" plug-in") : QStringLiteral(" plug-ins"));
    if (blocked > 0) s += QStringLiteral(" · %1 blocked").arg(blocked);
    if (refused > 0) s += QStringLiteral(" · %1 refused").arg(refused);
    return s;
}

void PluginManagerModel::setCategory(const QString& c) {
    if (c == category_) return;
    category_ = c;
    refilter();
}

void PluginManagerModel::setSearch(const QString& s) {
    if (s == search_) return;
    search_ = s;
    refilter();
}

void PluginManagerModel::setShow(const QString& s) {
    if (s == show_) return;
    if (s != QLatin1String("all") && s != QLatin1String("favourites") && s != QLatin1String("unusable")) {
        refuse("there is no \"" + s.toStdString() + "\" view; it is all, favourites or unusable");
        return;
    }
    show_ = s;
    refilter();
}

void PluginManagerModel::openManager() {
    if (open_) return;
    open_ = true;
    emit openChanged();
}

void PluginManagerModel::closeManager() {
    if (!open_) return;
    open_ = false;
    dismissNotice();
    emit openChanged();
}

bool PluginManagerModel::implemented(const char* command) const {
    return engine_.implemented && engine_.implemented(command);
}

const PluginManagerModel::Row* PluginManagerModel::find(const std::string& key) const {
    for (const Row& r : all_) {
        if (r.key == key) return &r;
    }
    return nullptr;
}

bool PluginManagerModel::refuse(const std::string& why) {
    hold(q(why));
    return false;
}

void PluginManagerModel::hold(const QString& why) {
    if (why == notice_) return;
    notice_ = why;
    emit noticeChanged();
}

void PluginManagerModel::dismissNotice() { hold(QString()); }

bool PluginManagerModel::send(const char* command, const std::string& argument) {
    if (!implemented(command) || !engine_.send) {
        return refuse(std::string("this engine does not run ") + command);
    }
    dismissNotice();
    engine_.send(command, argument);
    return true;
}

bool PluginManagerModel::rescan() { return send("plugin.rescan", std::string()); }

bool PluginManagerModel::block(const QString& key) {
    const Row* r = find(key.toStdString());
    if (r == nullptr) return refuse("no such plug-in in the engine's catalog");
    if (r->blocked) return refuse(r->name + " is already blocked");
    // The probe refuses it at every scan already; a block would hide why.
    if (!r->usable) return refuse(r->name + " is refused by its scan, not by you: " + r->refusal);
    return send("plugin.block", r->key);
}

bool PluginManagerModel::unblock(const QString& key) {
    const Row* r = find(key.toStdString());
    if (r == nullptr) return refuse("no such plug-in in the engine's catalog");
    if (!r->blocked) return refuse(r->name + " is not blocked");
    return send("plugin.unblock", r->key);
}

bool PluginManagerModel::toggleFavourite(const QString& key) {
    const std::string k = key.toStdString();
    std::string why;
    if (!favourites_.set(k, !favourites_.contains(k), why)) return refuse(why);
    for (Row& r : all_) {
        if (r.key == k) r.favourite = favourites_.contains(k);
    }
    ++favourites_revision_;
    emit favouritesChanged();
    refilter();
    return true;
}

bool PluginManagerModel::favouriteAt(int index) const {
    if (index < 0 || index >= static_cast<int>(catalog_.plugins.size())) return false;
    const api::PluginInfo& p = catalog_.plugins[static_cast<std::size_t>(index)];
    return favourites_.contains(plugin_key(p.format, p.id));
}

bool PluginManagerModel::toggleFavouriteAt(int index) {
    if (index < 0 || index >= static_cast<int>(catalog_.plugins.size())) {
        return refuse("no such plug-in in the engine's catalog");
    }
    const api::PluginInfo& p = catalog_.plugins[static_cast<std::size_t>(index)];
    return toggleFavourite(q(plugin_key(p.format, p.id)));
}

}  // namespace sonora::ui
