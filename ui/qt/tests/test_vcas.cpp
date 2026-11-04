// SPDX-License-Identifier: GPL-3.0-or-later
//
// VCA masters and folders (ADR-0042) in the interface:
//
//   - making, re-membering and removing a VCA, and a folder, is one
//     operation and one undo step each; a VCA's fader and mute are not an
//     undo step; the refusals say why;
//   - a folder's mute and solo are one Compound over its members;
//   - the commands vca.new, vca.delete, folder.new and folder.dissolve, and
//     a refusal from them is a held notice;
//   - a closed folder is one card and hides its tracks' strips; a session
//     opening puts every folder back open;
//   - the strips, the VCA strip's fader and the folder tag in the real QML.
#include <QColor>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "control_adapter.h"
#include "edit_controller.h"
#include "fader_taper.h"
#include "key_router.h"
#include "mix_model.h"
#include "peak_source.h"
#include "register_types.h"
#include "replica_document.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "theme.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

bool muted(const ModelDocument& d, int row) {
    return d.snapshot().mix.find(d.snapshot().edit->tracks[static_cast<std::size_t>(row)]->id)->mute;
}
bool soloed(const ModelDocument& d, int row) {
    return d.snapshot().mix.find(d.snapshot().edit->tracks[static_cast<std::size_t>(row)]->id)->solo;
}

int controller() {
    ModelDocument doc(make_empty_session(4, 48000));
    EditController c(doc);
    std::string why;

    CHECK(!c.vca_new(9, 1, "", why) && why == "there is no track 10");
    CHECK(!c.vca_new(-1, 1, "", why) && why == "no track is selected");
    CHECK(doc.snapshot().edit->vcas.empty());

    // Tracks 1 and 2: one step, named for the first free number.
    CHECK(c.vca_new(0, 2, "", why));
    const auto& vcas = doc.snapshot().edit->vcas;
    CHECK(vcas.size() == 1 && vcas[0].name == "VCA 1" && vcas[0].members.size() == 2);
    CHECK(!c.vca_new(2, 1, "VCA 1", why) && why.find("already named") != std::string::npos);
    // A track may be on two VCAs.
    CHECK(c.vca_new(1, 2, "Second", why));
    CHECK(doc.snapshot().edit->vcas.size() == 2);
    CHECK(c.undo(why) && doc.snapshot().edit->vcas.size() == 1);
    CHECK(c.redo(why) && doc.snapshot().edit->vcas.size() == 2);

    // The fader and the mute are values, not steps: undo leaves them.
    const std::uint64_t rev = doc.revision();
    CHECK(c.set_vca_gain(0, -9.0f, why));
    CHECK(c.toggle_vca_mute(0, why));
    CHECK(doc.revision() == rev + 2);
    const model::VcaMix* m = doc.snapshot().mix.find_vca(vcas[0].id);
    CHECK(m != nullptr && near(m->gain_db, -9.0) && m->mute);
    CHECK(c.undo(why) && doc.snapshot().edit->vcas.size() == 1);
    CHECK(near(doc.snapshot().mix.find_vca(vcas[0].id)->gain_db, -9.0));
    CHECK(c.redo(why));
    CHECK(!c.set_vca_gain(0, std::nanf(""), why));
    CHECK(!c.set_vca_gain(7, 0.0f, why) && why == "there is no such VCA");
    // The fader's own range: past +12 dB is not written.
    CHECK(c.set_vca_gain(0, 40.0f, why));
    CHECK(near(doc.snapshot().mix.find_vca(vcas[0].id)->gain_db, 12.0));

    // Set: the VCA holds the selected tracks instead.
    CHECK(c.vca_assign(0, 2, 2, why));
    CHECK(doc.snapshot().edit->vcas[0].members.size() == 2);
    CHECK(doc.snapshot().edit->vcas[0].members[0] == doc.snapshot().edit->tracks[2]->id);
    CHECK(!c.vca_assign(0, -1, 1, why) && why.find("select the tracks") != std::string::npos);

    // Deleting a held track leaves the VCA; undo brings it back.
    CHECK(c.delete_track(2, why));
    CHECK(doc.snapshot().edit->vcas[0].members.size() == 1);
    CHECK(c.undo(why));
    CHECK(doc.snapshot().edit->vcas[0].members.size() == 2);
    CHECK(c.vca_remove(0, why) && doc.snapshot().edit->vcas.size() == 1);
    CHECK(!c.vca_remove(5, why));

    // Folders: a track is in one.
    CHECK(!c.folder_new(-1, 1, "", why));
    CHECK(c.folder_new(0, 2, "", why));
    CHECK(doc.snapshot().edit->folders[0].name == "Folder 1");
    CHECK(c.folder_index(0) == 0 && c.folder_index(1) == 0 && c.folder_index(2) == -1);
    CHECK(!c.folder_new(1, 2, "Other", why) && why.find("already in folder \"Folder 1\"") != std::string::npos);
    CHECK(doc.snapshot().edit->folders.size() == 1);
    // Mute all, then release all, as one revision each.
    std::uint64_t r2 = doc.revision();
    CHECK(c.toggle_mute(0, why) && muted(doc, 0) && !muted(doc, 1));
    r2 = doc.revision();
    CHECK(c.folder_toggle(0, false, why) && muted(doc, 0) && muted(doc, 1) && !muted(doc, 2));
    CHECK(doc.revision() == r2 + 1);
    CHECK(c.folder_toggle(1, false, why) && !muted(doc, 0) && !muted(doc, 1));
    CHECK(c.folder_toggle(0, true, why) && soloed(doc, 0) && soloed(doc, 1) && !soloed(doc, 3));
    CHECK(!c.folder_toggle(3, false, why) && why.find("not in a folder") != std::string::npos);
    CHECK(c.folder_dissolve(1, why) && doc.snapshot().edit->folders.empty());
    CHECK(!c.folder_dissolve(1, why));
    CHECK(c.undo(why) && doc.snapshot().edit->folders.size() == 1);
    std::printf("  controller: ok\n");
    return 0;
}

struct Rig {
    api::testing::FakeEngine fake{config()};
    ControlAdapter engine{fake};
    ReplicaDocument doc{[this](const api::ModelRequest& r) { return engine.submitModel(r); },
                        make_empty_session(4, 48000)};
    SyntheticPeakSource peaks{48000, 120.0};
    SessionModel session{doc, peaks, true};
    KeyRouter router{KeyMap::built_in_default(), api::testing::FakeEngine::engine_commands(),
                     [this](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     }};
    ShellModel shell{engine, router};
    MixModel mixer;
    std::vector<std::string> held;

    static api::testing::FakeEngine::Config config() {
        api::testing::FakeEngine::Config c;
        c.tracks = 4;
        return c;
    }
    Rig()
        : mixer(session, [](uint32_t, api::ParamId, float, api::Gesture) {},
                []() { return 4u; }, true) {
        engine.setModelSink([this](const std::vector<api::ModelUpdate>& u,
                                   const std::vector<api::CommandReply>& r) {
            if (doc.receive(u, r)) session.documentUpdated();
        });
        QObject::connect(&session, &SessionModel::heldNotice,
                         [this](const QString& why) { held.push_back(why.toStdString()); });
        session.installCommands(router.local());
        router.setSelection([this]() { return session.selectedTrackForCommands(); });
        router.prepare({});
    }
    bool connect() {
        std::string error;
        if (!engine.connect(error)) return false;
        settle();
        return doc.synced();
    }
    void settle() {
        for (int i = 0; i < 8; ++i) shell.tick();
    }
};

int commands_and_structure() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CommandContext none;
    r.session.controller().select_range(0, 0, 0, 3);
    none.argument = "Drums";
    CHECK(r.router.local().dispatch("vca.new", none, why) == DispatchResult::Ok);
    r.settle();
    CHECK(r.doc.snapshot().edit->vcas.size() == 1 && r.doc.snapshot().edit->vcas[0].name == "Drums");
    CHECK(r.session.undoText() == QStringLiteral("Undo VCA Master"));
    // A second of the same name is refused, and the refusal is held.
    r.held.clear();
    CHECK(r.router.local().dispatch("vca.new", none, why) == DispatchResult::Failed);
    CHECK(r.held.size() == 1 && r.held[0].find("already named \"Drums\"") != std::string::npos);
    CHECK(r.doc.snapshot().edit->vcas.size() == 1);
    CHECK(r.mixer.trackVcas() == QStringList({"Drums", "Drums", "Drums", ""}));
    const QVariantMap info = r.mixer.vcas()[0].toMap();
    CHECK(info.value("count").toInt() == 3 && info.value("gainText").toString() == "0.0");

    // A delete by an unknown name is refused, naming it.
    CommandContext del;
    del.argument = "Strings";
    r.held.clear();
    CHECK(r.router.local().dispatch("vca.delete", del, why) == DispatchResult::Failed);
    CHECK(r.held.size() == 1 && r.held[0].find("no VCA named \"Strings\"") != std::string::npos);
    del.argument = "Drums";
    CHECK(r.router.local().dispatch("vca.delete", del, why) == DispatchResult::Ok);
    CHECK(r.doc.snapshot().edit->vcas.empty());

    // A folder over tracks 2 and 3 (rows 1, 2); one already in a folder is refused, held.
    r.session.controller().select_range(1, 0, 0, 2);
    CommandContext f;
    f.argument = "Kit";
    CHECK(r.router.local().dispatch("folder.new", f, why) == DispatchResult::Ok);
    r.settle();
    CHECK(r.mixer.trackFolders() == QList<int>({-1, 0, 0, -1}));
    r.held.clear();
    r.session.controller().select_range(2, 0, 0, 2);
    f.argument = "Overlap";
    CHECK(r.router.local().dispatch("folder.new", f, why) == DispatchResult::Failed);
    CHECK(r.held.size() == 1 && r.held[0].find("already in folder \"Kit\"") != std::string::npos);

    // Open: every track has a place. Closed: the folder is one card where
    // its first track was, and the others have none.
    CHECK(r.mixer.trackSlots() == QList<int>({0, 1, 2, 3}) && r.mixer.slotCount() == 4);
    CHECK(r.mixer.folderCards().isEmpty());
    r.mixer.toggleFolderOpen(0);
    CHECK(r.mixer.trackSlots() == QList<int>({0, -1, -1, 2}) && r.mixer.slotCount() == 3);
    const QVariantList cards = r.mixer.folderCards();
    CHECK(cards.size() == 1 && cards[0].toMap().value("slot").toInt() == 1);
    CHECK(cards[0].toMap().value("count").toInt() == 2 && cards[0].toMap().value("name").toString() == "Kit");
    // Closing is the window's: no document change, no undo step.
    CHECK(r.session.undoText() == QStringLiteral("Undo Folder"));
    r.mixer.toggleFolderOpen(0);
    CHECK(r.mixer.trackSlots() == QList<int>({0, 1, 2, 3}));
    r.mixer.toggleFolderOpen(0);
    // A session opening puts every folder back open.
    emit r.session.sessionOpened();
    CHECK(r.mixer.trackSlots() == QList<int>({0, 1, 2, 3}));
    CHECK(r.mixer.folderCards().isEmpty());

    CommandContext on;
    on.target = 2;
    CHECK(r.router.local().dispatch("folder.dissolve", on, why) == DispatchResult::Ok);
    r.settle();
    CHECK(r.doc.snapshot().edit->folders.empty());
    on.target = 2;
    CHECK(r.router.local().dispatch("folder.dissolve", on, why) == DispatchResult::Failed);
    std::printf("  commands: ok\n");
    return 0;
}

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

QPoint centre(QQuickItem* item) {
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

int qml() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.session.controller().vca_new(0, 2, "Drums", why));
    CHECK(r.session.controller().folder_new(2, 2, "Kit", why));
    r.settle();

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&r.shell)},
                               {QStringLiteral("session"), QVariant::fromValue(&r.session)},
                               {QStringLiteral("mixer"), QVariant::fromValue(&r.mixer)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    r.session.setWindow(QStringLiteral("mix"));
    view.show();
    CHECK(QTest::qWaitForWindowExposed(&view));
    QQuickItem* root = view.rootObject();
    CHECK(root != nullptr);

    // The tags: the VCA's name on the tracks it holds, none elsewhere; the
    // folder's on its members.
    QQuickItem* tag0 = named(root, QStringLiteral("vcaTag0"));
    QQuickItem* tag3 = named(root, QStringLiteral("vcaTag3"));
    CHECK(tag0 != nullptr && tag3 != nullptr);
    CHECK(tag0->isVisible() && !tag3->isVisible());
    QQuickItem* ftag = named(root, QStringLiteral("folderTag2"));
    CHECK(ftag != nullptr && ftag->isVisible());

    // The VCA strip: mute and the fader are mix values of the document.
    QQuickItem* mute = named(root, QStringLiteral("vcaMute0"));
    CHECK(mute != nullptr);
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, centre(mute));
    r.settle();
    CHECK(r.doc.snapshot().mix.find_vca(r.doc.snapshot().edit->vcas[0].id)->mute);
    QQuickItem* fader = named(root, QStringLiteral("vcaFader0"));
    CHECK(fader != nullptr);
    const QPoint p = centre(fader);
    QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, p);
    QTest::mouseMove(&view, p + QPoint(0, 40));
    QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, p + QPoint(0, 40));
    r.settle();
    CHECK(r.doc.snapshot().mix.find_vca(r.doc.snapshot().edit->vcas[0].id)->gain_db < 0.0f);
    QQuickItem* set = named(root, QStringLiteral("vcaSet0"));
    CHECK(set != nullptr);

    // The folder tag closes the folder into a card; the card opens it.
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, centre(ftag));
    QTest::qWait(50);
    QQuickItem* card = named(root, QStringLiteral("folderCard0"));
    CHECK(card != nullptr && card->isVisible());
    CHECK(!named(root, QStringLiteral("folderTag2"))->isVisible());
    QQuickItem* open = named(root, QStringLiteral("folderOpen0"));
    CHECK(open != nullptr);
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, centre(open));
    QTest::qWait(50);
    CHECK(named(root, QStringLiteral("folderTag2"))->isVisible());
    CHECK(r.mixer.folderCards().isEmpty());

    // + VCA makes one of the selected tracks.
    r.held.clear();
    r.session.controller().select_range(3, 0, 0, 1);
    QQuickItem* newVca = named(root, QStringLiteral("newVca"));
    CHECK(newVca != nullptr);
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, centre(newVca));
    r.settle();
    CHECK(r.doc.snapshot().edit->vcas.size() == 2);
    std::printf("  qml: ok\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (controller() != 0) return 1;
    if (commands_and_structure() != 0) return 1;
    if (qml() != 0) return 1;
    std::printf("ui vcas: all checks passed\n");
    return 0;
}
