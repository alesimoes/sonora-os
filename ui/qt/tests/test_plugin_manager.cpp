// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plug-in manager (ADR-0065) on FakeEngine, wired as main() wires it:
// the catalog through ShellModel's tick and its plugin sinks, commands
// through the engine link. The fake announces plugin.rescan, plugin.block
// and plugin.unblock as planned commands, logs them and answers Ok; what a
// block does to the catalog is scripted as the engine's next catalog, so
// every check reads what the engine sent, never a guess of the UI's.
//
//   - the three kinds of entry: usable, blocked (the prefix), refused;
//   - filters: category, search, favourites, blocked or refused;
//   - block then unblock round-trip, the row changing only on the engine's
//     catalog; a blocked plug-in whose bundle is gone is listed and unblockable;
//   - refusals held in the window: an engine without the commands, a block
//     of a refused entry, an unblock of a usable one;
//   - favourites through a fake store; a failed save keeps nothing; an
//     unreadable file is never overwritten; the file store's round trip;
//   - the QML: the chooser's Manage, star and Favourites filter.
#include <QFile>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "control_adapter.h"
#include "insert_model.h"
#include "key_router.h"
#include "mix_model.h"
#include "peak_source.h"
#include "plugin_favourites.h"
#include "plugin_manager_model.h"
#include "register_types.h"
#include "replica_document.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/api/plugin_refusal.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

namespace {

const std::string kGainKey = "lv2 urn:sonora:fake:gain";
const std::string kRefusedKey = "lv2 urn:sonora:fake:refused";
const std::string kCompKey = "vst3 534F4E4F524146414B45434F4D503031";
const std::string kBlockedWords = std::string(api::kBlockedRefusalPrefix) + ": it is blocked in the plug-in manager";

struct FakeStore : FavouritesStore {
    std::set<std::string> saved;
    int saves = 0;
    bool fail_save = false;
    bool fail_load = false;
    bool load(std::set<std::string>& out, std::string& why) override {
        if (fail_load) {
            why = "the file is not JSON";
            return false;
        }
        out = saved;
        return true;
    }
    bool save(const std::set<std::string>& keys, std::string& why) override {
        if (fail_save) {
            why = "the disk is full";
            return false;
        }
        saved = keys;
        ++saves;
        return true;
    }
};

struct Rig {
    api::testing::FakeEngine fake;
    ControlAdapter engine{fake};
    ReplicaDocument doc{[this](const api::ModelRequest& r) { return engine.submitModel(r); },
                        make_empty_session(4, 48000)};
    SyntheticPeakSource peaks{48000, 120.0};
    SessionModel session{doc, peaks, true};
    KeyRouter router{KeyMap::built_in_default(), api::testing::FakeEngine::engine_commands(),
                     [this](const std::string& c, const CommandContext& ctx) { return engine.send(c, ctx); }};
    ShellModel shell{engine, router};
    InsertModel inserts{doc, session};
    MixModel mixer{session, MixModel::GainSender(), []() { return 0u; }, true};
    FakeStore store;
    PluginFavourites favourites{store};
    std::vector<std::pair<std::string, std::string>> sent;   // {command, argument}
    PluginManagerModel manager;
    QTemporaryDir dir;

    static api::testing::FakeEngine::Config config(bool commands) {
        api::testing::FakeEngine::Config c;
        c.tracks = 4;
        if (commands) c.planned_commands = {"plugin.rescan", "plugin.block", "plugin.unblock"};
        return c;
    }

    explicit Rig(bool commands = true)
        : fake(config(commands)),
          manager(favourites,
                  {[this](const std::string& command, const std::string& argument) {
                       sent.emplace_back(command, argument);
                       CommandContext ctx;
                       ctx.argument = argument;
                       engine.send(command, ctx);
                   },
                   [this](const std::string& command) {
                       const auto& ids = engine.info().implemented_commands;
                       return std::find(ids.begin(), ids.end(), command) != ids.end();
                   }}) {
        session.setSessionsDir(dir.path());
        engine.setModelSink([this](const std::vector<api::ModelUpdate>& u, const std::vector<api::CommandReply>& r) {
            if (doc.receive(u, r)) session.documentUpdated();
        });
        ShellModel::PluginSinks sinks;
        sinks.catalog = [this](const api::PluginCatalog& c) {
            inserts.onCatalog(c);
            manager.onCatalog(c);
        };
        shell.setPluginSinks(std::move(sinks));
        session.installCommands(router.local());
        router.prepare({});
    }

    bool connect() {
        std::string error;
        if (!engine.connect(error)) {
            std::fprintf(stderr, "connect: %s\n", error.c_str());
            return false;
        }
        settle();
        return doc.synced();
    }
    void settle() {
        for (int i = 0; i < 8; ++i) shell.tick();
    }
    const PluginManagerModel::Row* row(const std::string& key) const {
        for (const auto& r : manager.all()) {
            if (r.key == key) return &r;
        }
        return nullptr;
    }
    bool logged(const std::string& command) const {
        for (const auto& c : fake.planned_log()) {
            if (c.command == command) return true;
        }
        return false;
    }
    // The engine after a block of the gain: as it masks a blocked id.
    void engine_blocks_gain(bool blocked) {
        auto c = api::testing::fake_catalog();
        if (blocked) {
            c[0].usable = false;
            c[0].refusal = kBlockedWords;
        }
        fake.script_catalog(std::move(c));
        settle();
    }
};

int kinds_of_entry() {
    Rig r;
    CHECK(r.connect());
    CHECK(r.manager.hasCatalog());
    CHECK(r.manager.all().size() == 7);
    CHECK(r.row(kGainKey)->usable && !r.row(kGainKey)->blocked);
    // Refused by its probe: not blocked, with the probe's words.
    const auto* refused = r.row(kRefusedKey);
    CHECK(!refused->usable && !refused->blocked && std::string(refused->state()) == "refused");
    CHECK(refused->refusal.find("instance-access") != std::string::npos);
    // ADR-0017 Y2: the bridged one carries its badge; the rest none.
    CHECK(r.row(kCompKey)->bridge == "Windows (Wine)" && r.row(kGainKey)->bridge.empty());
    CHECK(r.row(kCompKey)->format == "VST3" && r.row(kCompKey)->category == "Dynamics");
    CHECK(r.manager.summary() == QStringLiteral("7 plug-ins · 1 refused"));
    // By name, whatever the catalog's order.
    CHECK(r.manager.all().front().name == "Fake Cabinet");
    std::printf("  kinds of entry: ok\n");
    return 0;
}

int filters() {
    Rig r;
    CHECK(r.connect());
    CHECK(r.manager.categories().contains(QStringLiteral("Dynamics")));
    CHECK(!r.manager.categories().contains(QStringLiteral("Fx")));
    r.manager.setCategory(QStringLiteral("Dynamics"));
    CHECK(r.manager.filtered().size() == 2);
    r.manager.setCategory(QString());
    r.manager.setSearch(QStringLiteral("COMP"));
    CHECK(r.manager.filtered().size() == 1 && r.manager.filtered()[0].key == kCompKey);
    r.manager.setSearch(QString());
    r.manager.setShow(QStringLiteral("unusable"));
    CHECK(r.manager.filtered().size() == 1 && r.manager.filtered()[0].key == kRefusedKey);
    r.manager.setShow(QStringLiteral("favourites"));
    CHECK(r.manager.filtered().empty());
    CHECK(r.manager.toggleFavourite(QString::fromStdString(kGainKey)));
    CHECK(r.manager.filtered().size() == 1 && r.manager.filtered()[0].key == kGainKey);
    // A view that does not exist is refused, the filter left as it was.
    r.manager.setShow(QStringLiteral("hidden"));
    CHECK(r.manager.show() == QStringLiteral("favourites") && !r.manager.notice().isEmpty());
    std::printf("  filters: ok\n");
    return 0;
}

int block_unblock_round_trip() {
    Rig r;
    CHECK(r.connect());
    CHECK(r.manager.canBlock() && r.manager.canRescan());
    CHECK(r.manager.block(QString::fromStdString(kGainKey)));
    CHECK(r.sent.size() == 1 && r.sent[0].first == "plugin.block" && r.sent[0].second == kGainKey);
    r.settle();
    CHECK(r.logged("plugin.block"));
    // Sent is not done: the row waits for the engine's catalog.
    CHECK(r.row(kGainKey)->usable);
    r.engine_blocks_gain(true);
    CHECK(!r.row(kGainKey)->usable && r.row(kGainKey)->blocked);
    CHECK(std::string(r.row(kGainKey)->state()) == "blocked");
    CHECK(r.manager.summary() == QStringLiteral("7 plug-ins · 1 blocked · 1 refused"));
    r.manager.setShow(QStringLiteral("unusable"));
    CHECK(r.manager.filtered().size() == 2);
    // Blocking it again is refused here, not sent.
    CHECK(!r.manager.block(QString::fromStdString(kGainKey)) && r.sent.size() == 1);
    CHECK(r.manager.unblock(QString::fromStdString(kGainKey)));
    CHECK(r.sent.size() == 2 && r.sent[1].first == "plugin.unblock" && r.sent[1].second == kGainKey);
    CHECK(r.manager.notice().isEmpty());
    r.settle();
    CHECK(r.logged("plugin.unblock"));
    CHECK(r.row(kGainKey)->blocked);
    r.engine_blocks_gain(false);
    CHECK(r.row(kGainKey)->usable && !r.row(kGainKey)->blocked);
    CHECK(r.manager.filtered().size() == 1);
    // Rescan is the engine's.
    CHECK(r.manager.rescan());
    r.settle();
    CHECK(r.logged("plugin.rescan"));
    std::printf("  block then unblock round-trip: ok\n");
    return 0;
}

int blocked_and_gone() {
    Rig r;
    CHECK(r.connect());
    // ADR-0065 §4: on the block list, no bundle provides it any more.
    auto c = api::testing::fake_catalog();
    api::PluginInfo gone;
    gone.id = "urn:elsewhere:gone";
    gone.name = "Gone Delay";
    gone.usable = false;
    gone.refusal = std::string(api::kBlockedRefusalPrefix);
    c.push_back(gone);
    r.fake.script_catalog(std::move(c));
    r.settle();
    const auto* g = r.row("lv2 urn:elsewhere:gone");
    CHECK(g != nullptr && g->blocked);
    CHECK(r.manager.unblock(QStringLiteral("lv2 urn:elsewhere:gone")));
    // A probe reason that merely starts with the same letters is a refusal.
    auto d = api::testing::fake_catalog();
    d[3].refusal = "blocked by your antivirus";
    r.fake.script_catalog(std::move(d));
    r.settle();
    CHECK(!r.row(kRefusedKey)->blocked);
    std::printf("  blocked and gone: ok\n");
    return 0;
}

int refusals_held() {
    Rig r(false);
    CHECK(r.connect());
    CHECK(!r.manager.canBlock() && !r.manager.canRescan());
    CHECK(!r.manager.block(QString::fromStdString(kGainKey)));
    CHECK(r.sent.empty() && r.manager.notice().contains(QStringLiteral("plugin.block")));
    r.manager.dismissNotice();
    CHECK(r.manager.notice().isEmpty());
    CHECK(!r.manager.rescan() && r.sent.empty());

    Rig s;
    CHECK(s.connect());
    CHECK(!s.manager.block(QString::fromStdString(kRefusedKey)));
    CHECK(s.manager.notice().contains(QStringLiteral("refused by its scan")));
    CHECK(!s.manager.unblock(QString::fromStdString(kGainKey)));
    CHECK(s.manager.notice().contains(QStringLiteral("not blocked")));
    CHECK(!s.manager.block(QStringLiteral("lv2 urn:nowhere")));
    CHECK(s.sent.empty());
    // Closing the manager lets go of what it was holding.
    s.manager.openManager();
    s.manager.closeManager();
    CHECK(s.manager.notice().isEmpty());
    std::printf("  refusals held: ok\n");
    return 0;
}

int favourites() {
    Rig r;
    CHECK(r.connect());
    CHECK(r.manager.toggleFavouriteAt(0));   // catalog index 0: the gain
    CHECK(r.store.saved.count(kGainKey) == 1 && r.store.saves == 1);
    CHECK(r.manager.favouriteAt(0) && r.row(kGainKey)->favourite);
    // Never sent to the engine.
    CHECK(r.sent.empty());
    // A save that fails keeps nothing and says so.
    r.store.fail_save = true;
    const int rev = r.manager.favouritesRevision();
    CHECK(!r.manager.toggleFavourite(QString::fromStdString(kGainKey)));
    CHECK(r.manager.favouriteAt(0) && r.manager.favouritesRevision() == rev);
    CHECK(r.manager.notice().contains(QStringLiteral("disk is full")));
    // A favourite not in the catalog is kept, shown nowhere.
    r.store.fail_save = false;
    CHECK(r.manager.toggleFavourite(QStringLiteral("lv2 urn:elsewhere:later")));
    CHECK(r.store.saved.count("lv2 urn:elsewhere:later") == 1);
    r.manager.setShow(QStringLiteral("favourites"));
    CHECK(r.manager.filtered().size() == 1);

    // An unreadable store is never overwritten.
    FakeStore bad;
    bad.fail_load = true;
    PluginFavourites f(bad);
    std::string why;
    CHECK(!f.load(why) && !why.empty());
    CHECK(!f.set(kGainKey, true, why) && bad.saves == 0);
    CHECK(why.find("cannot be read") != std::string::npos);

    // The file store.
    QTemporaryDir dir;
    FileFavouritesStore file(dir.filePath(QStringLiteral("cfg/plugin-favourites.json")));
    std::set<std::string> keys;
    CHECK(file.load(keys, why) && keys.empty());
    CHECK(file.save({kGainKey, kCompKey}, why));
    CHECK(file.load(keys, why) && keys == std::set<std::string>({kGainKey, kCompKey}));
    QFile other(dir.filePath(QStringLiteral("v2.json")));
    CHECK(other.open(QIODevice::WriteOnly));
    other.write("{\"version\": 2, \"favourites\": []}");
    other.close();
    keys = {"keep"};
    CHECK(!FileFavouritesStore(other.fileName()).load(keys, why) && keys.count("keep") == 1);
    QFile junk(dir.filePath(QStringLiteral("junk.json")));
    CHECK(junk.open(QIODevice::WriteOnly));
    junk.write("{\"version\": 1, \"favourites\": [\"gain\"]}");
    junk.close();
    CHECK(!FileFavouritesStore(junk.fileName()).load(keys, why) && why.find("\"gain\"") != std::string::npos);
    std::printf("  favourites: ok\n");
    return 0;
}

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

QPoint centre(QQuickItem* item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

int qml() {
    Rig r;
    CHECK(r.connect());
    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&r.shell)},
                               {QStringLiteral("session"), QVariant::fromValue(&r.session)},
                               {QStringLiteral("inserts"), QVariant::fromValue(&r.inserts)},
                               {QStringLiteral("mixer"), QVariant::fromValue(&r.mixer)}});
    view.rootContext()->setContextProperty(QStringLiteral("pluginManager"), &r.manager);
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    r.session.setWindow(QStringLiteral("mix"));
    view.show();
    CHECK(QTest::qWaitForWindowExposed(&view));
    QQuickItem* root = view.rootObject();

    r.inserts.openBrowser(1, 2);
    QCoreApplication::processEvents();
    QQuickItem* browser = named(root, QStringLiteral("pluginBrowser"));
    CHECK(browser != nullptr && browser->isVisible());
    CHECK(browser->property("entries").toList().size() == 7);
    // Favourites only: none yet, then the one starred.
    QQuickItem* fav = named(root, QStringLiteral("pluginFavourites"));
    CHECK(fav != nullptr && fav->isVisible());
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(fav));
    CHECK(browser->property("favouritesOnly").toBool());
    CHECK(browser->property("entries").toList().empty());
    CHECK(r.manager.toggleFavourite(QString::fromStdString(kCompKey)));
    QCoreApplication::processEvents();
    const QVariantList only = browser->property("entries").toList();
    CHECK(only.size() == 1 && only[0].toMap()[QStringLiteral("name")].toString() == QStringLiteral("Fake Compressor"));

    // Manage opens the manager over the chooser, with every entry.
    QQuickItem* manage = named(root, QStringLiteral("pluginManage"));
    CHECK(manage != nullptr && manage->isVisible());
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(manage));
    CHECK(r.manager.open());
    QQuickItem* mgr = named(root, QStringLiteral("pluginManager"));
    CHECK(mgr != nullptr && mgr->isVisible());
    QQuickItem* list = named(root, QStringLiteral("managerList"));
    CHECK(list != nullptr && list->property("count").toInt() == 7);
    QQuickItem* rescan = named(root, QStringLiteral("managerRescan"));
    CHECK(rescan != nullptr && rescan->property("available").toBool());
    // An engine that runs the commands: no read-only line.
    QQuickItem* readOnly = named(root, QStringLiteral("managerReadOnly"));
    CHECK(readOnly != nullptr && !readOnly->isVisible());
    // Closing the chooser closes the manager with it.
    r.inserts.closeBrowser();
    QCoreApplication::processEvents();
    CHECK(!r.manager.open());
    std::printf("  qml: ok\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    int failed = 0;
    failed += kinds_of_entry();
    failed += filters();
    failed += block_unblock_round_trip();
    failed += blocked_and_gone();
    failed += refusals_held();
    failed += favourites();
    failed += qml();
    if (failed == 0) std::printf("test_plugin_manager: all passed\n");
    return failed == 0 ? 0 : 1;
}
