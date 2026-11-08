// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mix and edit groups (ADR-0041) in the interface:
//
//   - grouping the selected tracks is one operation and one undo step, and
//     the refusals say why (two tracks at least, one group per track);
//   - a fader, mute or solo move on a member moves the members its group
//     links as one Compound: one revision, relative or absolute, clamped to
//     the fader, and not at all once the group is suspended or the
//     attribute unlinked;
//   - deleting a member leaves the group, and undo brings it back;
//   - the commands group.new, group.dissolve and group.toggle_suspend;
//   - a ridden fader moves its group's members live through ParameterChange
//     and saves the whole group once at release, wired as main() wires it
//     (FakeEngine behind ControlAdapter, ReplicaDocument);
//   - the strips' group tags and the group buttons in the real QML.
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

constexpr std::uint8_t kMixAttrs =
    model::kGroupVolume | model::kGroupMute | model::kGroupSolo | model::kGroupArm;

float gain(const ModelDocument& d, int row) {
    return d.snapshot().mix.find(d.snapshot().edit->tracks[static_cast<std::size_t>(row)]->id)
        ->gain_db;
}
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

    CHECK(!c.group_tracks(0, 1, "", kMixAttrs, why) && why.find("two or more") != std::string::npos);
    CHECK(!c.group_tracks(3, 1, "", kMixAttrs, why));
    CHECK(!c.group_tracks(9, 2, "", kMixAttrs, why) && why == "there is no track 10");
    CHECK(!c.group_tracks(0, 2, "", 0, why) && why.find("at least one attribute") != std::string::npos);
    CHECK(doc.snapshot().edit->groups.empty());

    // Tracks 1 and 2 (rows 0 and 1): one step, named for the first free number.
    CHECK(c.group_tracks(0, 2, "", kMixAttrs, why));
    CHECK(doc.snapshot().edit->groups.size() == 1 && doc.snapshot().edit->groups[0].name == "Group 1");
    CHECK(c.group_index(0) == 0 && c.group_index(1) == 0 && c.group_index(2) == -1);
    CHECK(c.undo(why) && doc.snapshot().edit->groups.empty());
    CHECK(c.redo(why) && doc.snapshot().edit->groups.size() == 1);
    // One group per track; names are unique.
    CHECK(!c.group_tracks(1, 2, "", kMixAttrs, why) && why.find("already in group \"Group 1\"") != std::string::npos);
    CHECK(!c.group_tracks(2, 2, "Group 1", kMixAttrs, why) && why.find("already named") != std::string::npos);

    // Relative: the same dB on the other member; a track outside is untouched.
    std::uint64_t rev = doc.revision();
    CHECK(c.set_gain(1, -10.0f, why) && near(gain(doc, 0), -10.0));   // t1 was 0, so t0 follows to -10
    CHECK(near(gain(doc, 1), -10.0) && near(gain(doc, 2), 0.0));
    CHECK(doc.revision() == rev + 1);   // the whole group in one revision
    CHECK(c.set_gain(0, -4.0f, why));   // +6 dB on t0: t1 follows by +6
    CHECK(near(gain(doc, 0), -4.0) && near(gain(doc, 1), -4.0));
    // A relative move keeps the members apart: separate them, then move one.
    CHECK(c.toggle_group_suspend(0, why));
    CHECK(c.set_gain(1, -14.0f, why) && near(gain(doc, 0), -4.0) && near(gain(doc, 1), -14.0));
    CHECK(c.toggle_group_suspend(0, why));
    rev = doc.revision();
    CHECK(c.set_gain(0, -1.0f, why));   // +3 dB on t0: t1 -14 -> -11
    CHECK(near(gain(doc, 0), -1.0) && near(gain(doc, 1), -11.0) && doc.revision() == rev + 1);
    // Clamped to the fader's end, never past it: t0 -1 and t1 -11, so the
    // top of t1's fader (+12, 23 dB up) takes t0 only as far as +12.
    CHECK(c.set_gain(1, 12.0f, why));
    CHECK(near(gain(doc, 1), 12.0) && near(gain(doc, 0), 12.0));

    // Absolute: every member to one value.
    CHECK(c.set_group_relative(0, false, why));
    CHECK(c.set_gain(1, -6.0f, why) && near(gain(doc, 0), -6.0) && near(gain(doc, 1), -6.0));
    CHECK(c.set_group_relative(0, true, why));

    // Mute and solo are copied from the track pressed.
    CHECK(c.toggle_mute(0, why) && muted(doc, 0) && muted(doc, 1) && !muted(doc, 2));
    CHECK(c.toggle_mute(1, why) && !muted(doc, 0) && !muted(doc, 1));
    CHECK(c.toggle_solo(1, why) && soloed(doc, 0) && soloed(doc, 1) && !soloed(doc, 2));
    CHECK(c.toggle_solo(0, why) && !soloed(doc, 0) && !soloed(doc, 1));
    // An attribute switched off links nothing; the others still do.
    CHECK(c.toggle_group_attribute(0, model::kGroupMute, why));
    CHECK(c.toggle_mute(0, why) && muted(doc, 0) && !muted(doc, 1));
    CHECK(c.toggle_mute(0, why));
    CHECK(c.toggle_solo(0, why) && soloed(doc, 1));
    CHECK(c.toggle_solo(0, why));
    CHECK(c.toggle_group_attribute(0, model::kGroupMute, why));
    CHECK(!c.toggle_group_attribute(2, model::kGroupMute, why) && why.find("not in a group") != std::string::npos);

    // A deleted member leaves; a group of one goes; undo brings both back.
    CHECK(c.delete_track(1, why));
    CHECK(doc.snapshot().edit->groups.empty());
    CHECK(c.undo(why));
    CHECK(doc.snapshot().edit->groups.size() == 1 && doc.snapshot().edit->groups[0].members.size() == 2);

    // Dissolve: the tracks and their mix stay.
    const float kept = gain(doc, 1);
    CHECK(c.dissolve_group(0, why) && doc.snapshot().edit->groups.empty());
    CHECK(near(gain(doc, 1), kept));
    CHECK(!c.dissolve_group(0, why) && why.find("not in a group") != std::string::npos);
    CHECK(c.set_gain(0, -3.0f, why) && near(gain(doc, 1), kept));
    std::printf("  controller: ok\n");
    return 0;
}

int commands() {
    ModelDocument doc(make_empty_session(4, 48000));
    SyntheticPeakSource peaks(48000, 120.0);
    SessionModel session(doc, peaks, true);
    CommandRegistry local;
    session.installCommands(local);
    std::string why;
    CommandContext none;

    // Nothing selected: refused by name.
    CHECK(local.dispatch("group.new", none, why) == DispatchResult::Failed);
    session.controller().select_range(1, 0, 0, 2);
    none.argument = "Drums";
    CHECK(local.dispatch("group.new", none, why) == DispatchResult::Ok);
    CHECK(doc.snapshot().edit->groups.size() == 1 && doc.snapshot().edit->groups[0].name == "Drums");
    CHECK(session.undoText() == QStringLiteral("Undo Group Tracks"));
    CommandContext on;
    on.target = 2;
    CHECK(local.dispatch("group.toggle_suspend", on, why) == DispatchResult::Ok);
    CHECK(!doc.snapshot().edit->groups[0].active);
    CHECK(local.dispatch("group.toggle_suspend", on, why) == DispatchResult::Ok);
    on.target = 0;
    CHECK(local.dispatch("group.dissolve", on, why) == DispatchResult::Failed);
    CHECK(why.find("not in a group") != std::string::npos);
    on.target = 1;
    CHECK(local.dispatch("group.dissolve", on, why) == DispatchResult::Ok);
    CHECK(doc.snapshot().edit->groups.empty());
    CHECK(session.undoText() == QStringLiteral("Undo Ungroup Tracks"));
    std::printf("  commands: ok\n");
    return 0;
}

// --- a ride through the engine ----------------------------------------------

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
    struct Sent {
        uint32_t track;
        api::ParamId param;
        api::Gesture gesture;
        float value;
    };
    std::vector<Sent> sent;
    std::vector<std::string> refused;

    static api::testing::FakeEngine::Config config() {
        api::testing::FakeEngine::Config c;
        c.tracks = 4;
        return c;
    }
    Rig()
        : mixer(session,
                [this](uint32_t t, api::ParamId p, float g, api::Gesture ge) {
                    engine.send_track_param(t, p, g, ge);
                    sent.push_back({t, p, ge, g});
                },
                []() { return 4u; }, true) {
        engine.setModelSink([this](const std::vector<api::ModelUpdate>& u,
                                   const std::vector<api::CommandReply>& r) {
            if (doc.receive(u, r)) session.documentUpdated();
        });
        QObject::connect(&session, &SessionModel::refused,
                         [this](const QString& why) { refused.push_back(why.toStdString()); });
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
    float engine_gain(uint32_t t) {
        api::StateFrame f;
        fake.state().frame(f);
        return t < f.tracks.size() ? f.tracks[t].gain : -1.0f;
    }
    float doc_gain(int row) { return session.stripGain(row); }
};

int ride_moves_the_group() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.session.controller().group_tracks(0, 3, "Kit", kMixAttrs, why));
    r.settle();
    CHECK(r.refused.empty());
    CHECK(r.mixer.trackGroups() == QList<int>({0, 0, 0, -1}));
    CHECK(r.mixer.groupNames() == QStringList{"Kit"});
    CHECK(r.mixer.groupSummaries()[0] == QStringLiteral("Volume (Relative), Mute, Solo, Arm"));
    // Track 2 starts apart from the others.
    CHECK(r.session.controller().toggle_group_suspend(0, why));
    r.settle();
    CHECK(r.session.controller().set_gain(1, -6.0f, why));
    r.settle();
    CHECK(r.session.controller().toggle_group_suspend(0, why));
    r.settle();
    CHECK(near(r.doc_gain(0), 0.0) && near(r.doc_gain(1), -6.0));
    const std::uint64_t rev = r.doc.revision();
    r.sent.clear();

    r.mixer.beginRide(0);
    // The hand lands on the leader and on every peer: a Touch each.
    int touches = 0;
    for (const auto& s : r.sent) touches += s.gesture == api::Gesture::Touch ? 1 : 0;
    CHECK(touches == 3);
    r.mixer.ride(0, -20, 200, false);
    const float up = static_cast<float>(r.mixer.rideGain());
    CHECK(up > 0.0f);
    r.settle();
    // Heard on the three tracks of the group, and not on the fourth; the
    // document has not moved yet.
    CHECK(near(r.engine_gain(0), fader_linear_gain(up)));
    CHECK(near(r.engine_gain(1), fader_linear_gain(-6.0f + up), 1e-3));
    CHECK(near(r.engine_gain(2), fader_linear_gain(up)));
    CHECK(near(r.engine_gain(3), 1.0));
    CHECK(r.doc.revision() == rev);
    r.mixer.endRide(0);
    r.settle();
    // Saved once for the whole group, and the engine and document agree.
    CHECK(r.doc.revision() == rev + 1);
    CHECK(near(r.doc_gain(0), up, 1e-3) && near(r.doc_gain(1), -6.0f + up, 1e-3) &&
          near(r.doc_gain(2), up, 1e-3) && near(r.doc_gain(3), 0.0));
    CHECK(near(r.engine_gain(1), fader_linear_gain(-6.0f + up), 1e-3));
    int releases = 0;
    for (const auto& s : r.sent) releases += s.gesture == api::Gesture::Release ? 1 : 0;
    CHECK(releases >= 3);
    CHECK(r.refused.empty());

    // A suspended group rides alone.
    CHECK(r.session.controller().toggle_group_suspend(0, why));
    r.settle();
    CHECK(!r.doc.snapshot().edit->groups[0].active);
    r.sent.clear();
    r.mixer.beginRide(0);
    CHECK(r.sent.size() == 1);
    r.mixer.endRide(0);
    std::printf("  ride: ok\n");
    return 0;
}

namespace {
// By the visual tree: findChild would miss items the QML parents elsewhere.
QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}
}  // namespace

int qml() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.session.controller().group_tracks(1, 2, "Kit", kMixAttrs, why));
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
    // The tag reads the group on its members, and is empty elsewhere.
    QQuickItem* tag1 = named(root, QStringLiteral("groupTag1"));
    QQuickItem* tag0 = named(root, QStringLiteral("groupTag0"));
    CHECK(tag1 != nullptr && tag0 != nullptr);
    CHECK(tag1->property("color").value<QColor>().alpha() > 0);
    CHECK(tag0->property("color").value<QColor>().alpha() == 0);
    // The buttons act on the selected track's group.
    r.session.selectTrack(1);
    QQuickItem* suspend = named(root, QStringLiteral("groupSuspend"));
    CHECK(suspend != nullptr);
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                      suspend->mapToScene(QPointF(suspend->width() / 2, suspend->height() / 2)).toPoint());
    r.settle();
    CHECK(!r.doc.snapshot().edit->groups[0].active);
    QQuickItem* ungroup = named(root, QStringLiteral("groupDissolve"));
    CHECK(ungroup != nullptr);
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                      ungroup->mapToScene(QPointF(ungroup->width() / 2, ungroup->height() / 2)).toPoint());
    r.settle();
    CHECK(r.doc.snapshot().edit->groups.empty());
    std::printf("  qml: ok\n");
    return 0;
}

// The 65th group is refused with a reason and changes nothing; a group move
// is one undo step that returns every member.
int limit_and_undo() {
    ModelDocument doc(make_empty_session(2 * static_cast<int>(model::kMaxGroups) + 2, 48000));
    EditController c(doc);
    std::string why;
    for (int g = 0; g < static_cast<int>(model::kMaxGroups); ++g) {
        CHECK(c.group_tracks(2 * g, 2, "", kMixAttrs, why));
    }
    CHECK(doc.snapshot().edit->groups.size() == model::kMaxGroups);
    const std::uint64_t rev = doc.revision();
    CHECK(!c.group_tracks(2 * static_cast<int>(model::kMaxGroups), 2, "", kMixAttrs, why));
    CHECK(why.find("at most 64 groups") != std::string::npos);
    CHECK(doc.snapshot().edit->groups.size() == model::kMaxGroups && doc.revision() == rev);

    ModelDocument d2(make_empty_session(3, 48000));
    EditController c2(d2);
    CHECK(c2.group_tracks(0, 2, "", kMixAttrs, why));
    CHECK(c2.set_gain(0, -9.0f, why) && near(gain(d2, 0), -9.0) && near(gain(d2, 1), -9.0));
    CHECK(c2.undo(why));
    CHECK(near(gain(d2, 0), 0.0) && near(gain(d2, 1), 0.0));
    CHECK(d2.snapshot().edit->groups.size() == 1);   // the group itself is a step further back
    std::printf("  limit and undo: ok\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (controller() != 0) return 1;
    if (limit_and_undo() != 0) return 1;
    if (commands() != 0) return 1;
    if (ride_moves_the_group() != 0) return 1;
    if (qml() != 0) return 1;
    std::printf("ui groups: all checks passed\n");
    return 0;
}
