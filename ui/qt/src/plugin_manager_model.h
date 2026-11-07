// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plug-in manager (BACKLOG plugins.host.manager, ADR-0065): every plug-in
// the engine's catalog lists, usable or not, with why not. Opened from the
// Mix window's plugin chooser. Three kinds of entry:
//
//   - usable: it can go on an insert; it can be blocked;
//   - blocked by the operator (api::is_blocked_refusal on its refusal): it
//     can be unblocked, even when its bundle is gone (ADR-0065 §4);
//   - refused by its probe: shown with the probe's words, nothing to undo.
//
// The block list is the engine's: Block, Unblock and Rescan are engine
// commands (plugin.block, plugin.unblock, plugin.rescan) through the same
// path a key takes, and what they changed is seen only when the engine's
// next catalog arrives. Nothing here guesses the result ahead of it, so a
// refused block never shows as done. Favourites are the interface's own
// (PluginFavourites) and never reach the engine.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <functional>
#include <string>
#include <vector>

#include "plugin_favourites.h"
#include "sonora/api/messages.h"

namespace sonora::ui {

class PluginManagerModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool open READ open NOTIFY openChanged)
    Q_PROPERTY(bool hasCatalog READ hasCatalog NOTIFY changed)
    // Every entry, before the filters: 0 is "the engine found none".
    Q_PROPERTY(int catalogSize READ catalogSize NOTIFY changed)
    // [{key, index, name, vendor, version, format, category, bridge, usable,
    // blocked, refusal, favourite, state ("usable", "blocked", "refused")}],
    // the filters applied, by name.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(QStringList categories READ categories NOTIFY changed)
    Q_PROPERTY(QString category READ category WRITE setCategory NOTIFY changed)
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY changed)
    // "all", "favourites" or "unusable" (blocked and refused).
    Q_PROPERTY(QString show READ show WRITE setShow NOTIFY changed)
    // "7 plug-ins · 1 blocked · 1 refused", over the whole catalog.
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    // Whether this engine runs the commands; a button it cannot run is
    // drawn unavailable rather than left looking live.
    Q_PROPERTY(bool canRescan READ canRescan NOTIFY changed)
    Q_PROPERTY(bool canBlock READ canBlock NOTIFY changed)
    // A refusal held in the window until dismissed (a favourite not saved,
    // a command this engine does not run): not the status line's 2.5 s.
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    // Bumped when a favourite changes, for QML bindings over favouriteAt().
    Q_PROPERTY(int favouritesRevision READ favouritesRevision NOTIFY favouritesChanged)

public:
    // How the manager reaches the engine: `send` runs an Engine-domain
    // command with its argument (main() hands ShellModel::invokeWith, so a
    // refusal reaches the status line as a key's would); `implemented`
    // says whether this engine announced it.
    struct Engine {
        std::function<void(const std::string& command, const std::string& argument)> send;
        std::function<bool(const std::string& command)> implemented;
    };

    struct Row {
        std::string key;   // plugin_key(): "lv2 <uri>", "vst3 <class id>"
        int index = -1;    // in the catalog as received
        std::string name, vendor, version, format, category, bridge, refusal;
        bool usable = true;
        bool blocked = false;
        bool favourite = false;
        const char* state() const { return usable ? "usable" : blocked ? "blocked" : "refused"; }
    };

    PluginManagerModel(PluginFavourites& favourites, Engine engine, QObject* parent = nullptr);

    // From ShellModel's tick, beside InsertModel::onCatalog.
    void onCatalog(const api::PluginCatalog& c);

    // The rows QML gets, for tests without QVariant.
    const std::vector<Row>& filtered() const { return filtered_; }
    const std::vector<Row>& all() const { return all_; }

    bool open() const { return open_; }
    bool hasCatalog() const { return has_catalog_; }
    int catalogSize() const { return static_cast<int>(all_.size()); }
    QVariantList rows() const;
    QStringList categories() const;
    QString category() const { return category_; }
    QString search() const { return search_; }
    QString show() const { return show_; }
    QString summary() const;
    bool canRescan() const { return implemented("plugin.rescan"); }
    bool canBlock() const { return implemented("plugin.block") && implemented("plugin.unblock"); }
    QString notice() const { return notice_; }
    int favouritesRevision() const { return favourites_revision_; }

    void setCategory(const QString& c);
    void setSearch(const QString& s);
    void setShow(const QString& s);

    Q_INVOKABLE void openManager();
    Q_INVOKABLE void closeManager();
    // False, with the reason held in `notice`, when the command was not
    // sent: this engine does not run it, or no such entry, or the entry is
    // not in a state the command applies to.
    Q_INVOKABLE bool rescan();
    Q_INVOKABLE bool block(const QString& key);
    Q_INVOKABLE bool unblock(const QString& key);
    Q_INVOKABLE bool toggleFavourite(const QString& key);
    // For the plugin chooser, which names a plug-in by its catalog index.
    Q_INVOKABLE bool favouriteAt(int index) const;
    Q_INVOKABLE bool toggleFavouriteAt(int index);
    Q_INVOKABLE void dismissNotice();
    // A refusal to hold, from outside (the favourites file unreadable at start).
    void hold(const QString& why);

signals:
    void openChanged();
    void changed();
    void noticeChanged();
    void favouritesChanged();

private:
    bool implemented(const char* command) const;
    void rebuild();     // all_ from the catalog and the favourites
    void refilter();    // filtered_ from all_ and the filters
    const Row* find(const std::string& key) const;
    bool refuse(const std::string& why);
    bool send(const char* command, const std::string& argument);

    PluginFavourites& favourites_;
    Engine engine_;
    api::PluginCatalog catalog_;
    bool has_catalog_ = false;
    bool open_ = false;
    std::vector<Row> all_, filtered_;
    QString category_, search_;
    QString show_ = QStringLiteral("all");
    QString notice_;
    int favourites_revision_ = 0;
};

}  // namespace sonora::ui
