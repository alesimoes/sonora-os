// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window on the monitor bus, wired as main() wires it: FakeEngine
// behind ControlAdapter, ReplicaDocument, SessionModel, ShellModel's tick,
// MixModel. Every value is checked where the engine has it (its document,
// or the gain its state block publishes), never in a stand-in that agrees
// with the interface:
//
//   - typed gain and pan: the spellings accepted, and the ones refused with
//     the reason, never clamped quietly;
//   - the strips read the mix by display order, after a reorder too
//     (regression: they read MixState's id order);
//   - a fader ride is heard through ParameterChange per step and saved by
//     one SetMix at release; the bus, and a track the engine lacks, ride
//     through the document instead;
//   - an interrupted ride never leaves the engine and the document apart:
//     a lost link is put right by the engine (PR #71), a step that crossed
//     another interface's reorder by the interface, which otherwise sends
//     no gain of its own accord; the refusal at release stays said, and a
//     ride whose track moved or left, or whose session was replaced, is
//     dropped with the reason;
//   - Alt-click solo and mute set every track; Ctrl-click solo is solo
//     safe, a SetMix the engine's document holds, the track's own solo
//     left alone;
//   - the implicit-mute outline is the engine's rule: a solo-safe track
//     another track's solo does not silence is not outlined;
//   - the QML: what the meters read is written on them, a drag on a fader
//     is a ride, a typed gain keeps its keys from the profile, Alt-click on
//     a solo button, Ctrl-click on one in either window recesses it with a
//     padlock and takes its mute's outline away.
#include <QColor>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "control_adapter.h"
#include "fader_taper.h"
#include "key_router.h"
#include "mix_model.h"
#include "peak_source.h"
#include "register_types.h"
#include "replica_document.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/model/op_codec.h"
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
    std::vector<std::string> refused;
    // False while the connection is down: what the sender sends then is
    // dropped unsent, as SocketEngine drops it before a new Hello.
    bool link = true;
    // ParameterChanges that reached the engine, rides' steps or not.
    int gain_sends = 0;
    struct Sent {
        uint32_t track;
        api::ParamId param;
        api::Gesture gesture;
    };
    std::vector<Sent> sent;
    // The engine's track count as the latest Hello says it.
    uint32_t engine_tracks = 4;

    static api::testing::FakeEngine::Config config() {
        api::testing::FakeEngine::Config c;
        c.tracks = 4;
        return c;
    }

    // `live_tracks`: how many tracks the ride's ParameterChange may reach;
    // -1 for no live path at all.
    explicit Rig(int live_tracks = 4)
        : mixer(session,
                live_tracks < 0 ? MixModel::GainSender()
                                : [this](uint32_t t, api::ParamId p, float g, api::Gesture ge) {
                                      if (!link) return;
                                      engine.send_track_param(t, p, g, ge);
                                      sent.push_back({t, p, ge});
                                      ++gain_sends;
                                  },
                [this]() { return engine_tracks; }, true),
          engine_tracks(live_tracks < 0 ? 0u : static_cast<uint32_t>(live_tracks)) {
        engine.setModelSink([this](const std::vector<api::ModelUpdate>& u,
                                   const std::vector<api::CommandReply>& r) {
            if (doc.receive(u, r)) session.documentUpdated();
        });
        doc.on_refused = [this](const std::string& why) { refused.push_back(why); };
        QObject::connect(&session, &SessionModel::refused,
                         [this](const QString& why) { refused.push_back(why.toStdString()); });
        session.installCommands(router.local());
        router.setSelection([this]() { return session.selectedTrackForCommands(); });
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
    // The gain the engine's state block publishes for engine track `t`.
    float engine_gain(uint32_t t) {
        api::StateFrame f;
        fake.state().frame(f);
        return t < f.tracks.size() ? f.tracks[t].gain : -1.0f;
    }
};

// --- typed values ------------------------------------------------------------

int typed_text() {
    float v = 0.0f;
    std::string why;
    CHECK(parse_gain_text("-6.5", v, why) && near(v, -6.5));
    CHECK(parse_gain_text(" -6.5 dB ", v, why) && near(v, -6.5));
    CHECK(parse_gain_text("+3", v, why) && near(v, 3.0));
    CHECK(parse_gain_text("0", v, why) && v == 0.0f);
    CHECK(parse_gain_text("12", v, why) && near(v, 12.0));
    CHECK(parse_gain_text("-INF", v, why) && v == kFaderSilenceDb);
    CHECK(parse_gain_text("-300", v, why) && v == kFaderSilenceDb);
    // Off the top is refused with the range, never written as +12.
    CHECK(!parse_gain_text("+20", v, why) && why.find("+12 dB") != std::string::npos);
    CHECK(!parse_gain_text("loud", v, why) && why.find("not a gain") != std::string::npos);
    CHECK(!parse_gain_text("", v, why));
    CHECK(!parse_gain_text("-6x", v, why));
    CHECK(!parse_gain_text("nan", v, why));

    CHECK(parse_pan_text("C", v, why) && v == 0.0f);
    CHECK(parse_pan_text("<30", v, why) && near(v, -0.30));
    CHECK(parse_pan_text("30>", v, why) && near(v, 0.30));
    CHECK(parse_pan_text("L 45", v, why) && near(v, -0.45));
    CHECK(parse_pan_text("r100", v, why) && near(v, 1.0));
    CHECK(parse_pan_text("-20", v, why) && near(v, -0.20));
    CHECK(parse_pan_text("0", v, why) && v == 0.0f);
    // Its own spelling reads back.
    CHECK(parse_pan_text(pan_text(-0.62f), v, why) && near(v, -0.62));
    CHECK(!parse_pan_text("<120", v, why) && why.find("100") != std::string::npos);
    CHECK(!parse_pan_text("L-30", v, why));
    CHECK(!parse_pan_text("middle", v, why) && why.find("not a pan") != std::string::npos);

    CHECK(fader_linear_gain(kFaderSilenceDb) == 0.0f);
    CHECK(near(fader_linear_gain(0.0f), 1.0));
    CHECK(near(fader_linear_gain(-6.0f), std::pow(10.0, -6.0 / 20.0)));
    CHECK(fader_linear_gain(12.0f) <= 4.0f);
    std::printf("  typed gain and pan: ok\n");
    return 0;
}

// --- strips by display order -------------------------------------------------

int display_order() {
    // Regression: gains(), pans(), mutes() and solos() walked MixState's
    // entries, keyed by id, so after a reorder each strip showed another
    // track's fader, and a drag moved a third.
    ModelDocument document(make_empty_session(3, 48000));
    SyntheticPeakSource peaks(48000, 120.0);
    SessionModel session(document, peaks, true);
    EditController& c = session.controller();
    std::string why;
    CHECK(c.set_gain(0, -1.0f, why) && c.set_gain(1, -2.0f, why) && c.set_gain(2, -3.0f, why));
    CHECK(c.set_pan(2, 0.5f, why) && c.toggle_solo(2, why));
    std::vector<model::Id> order;
    for (const auto& t : document.snapshot().edit->tracks) order.push_back(t->id);
    std::reverse(order.begin(), order.end());
    CHECK(document.submit(model::ReorderTracks{order}, why));
    session.documentUpdated();
    const QList<qreal> g = session.gains();
    CHECK(g.size() == 3 && near(g[0], -3.0) && near(g[1], -2.0) && near(g[2], -1.0));
    CHECK(near(session.pans()[0], 0.5) && session.solos()[0] && !session.solos()[2]);
    CHECK(near(session.stripGain(0), -3.0) && near(session.stripPan(0), 0.5));
    // A drag on the first strip moves the track shown there.
    session.faderDrag(0, -10.0, 100.0, false);
    const model::TrackMix* moved = document.snapshot().mix.find(order[0]);
    CHECK(moved != nullptr && moved->gain_db > -3.0f);
    CHECK(near(document.snapshot().mix.find(order[2])->gain_db, -1.0));
    std::printf("  strips follow display order: ok\n");
    return 0;
}

// --- rides -------------------------------------------------------------------

int track_ride() {
    Rig r;
    CHECK(r.connect());
    const uint64_t rev = r.doc.revision();
    CHECK(near(r.session.stripGain(1), 0.0) && near(r.engine_gain(1), 1.0));

    r.mixer.beginRide(1);
    CHECK(r.mixer.rideTrack() == 1 && near(r.mixer.rideGain(), 0.0));
    r.mixer.ride(1, -20, 200, false);
    r.mixer.ride(1, -20, 200, false);
    const float up = static_cast<float>(r.mixer.rideGain());
    CHECK(up > 0.0f && up < 12.0f);
    r.settle();
    // Heard: the engine's gain is the ride's. Not saved: the document has
    // not moved, so a ride is not a model change per step.
    CHECK(r.mixer.liveSteps() == 2);
    CHECK(near(r.engine_gain(1), fader_linear_gain(up)));
    CHECK(r.doc.revision() == rev && near(r.session.stripGain(1), 0.0));
    CHECK(r.refused.empty());
    // Fine: a tenth of the motion.
    r.mixer.ride(1, 20, 200, true);
    const float fine = static_cast<float>(r.mixer.rideGain());
    CHECK(fine < up && up - fine < 1.0f);
    // A step past the top of the travel changes nothing and sends nothing.
    r.mixer.ride(1, -10000, 200, false);
    CHECK(near(r.mixer.rideGain(), kFaderMaxDb));
    const uint64_t steps = r.mixer.liveSteps();
    r.mixer.ride(1, -50, 200, false);
    CHECK(r.mixer.liveSteps() == steps);
    // Another strip's motion is not this ride's.
    r.mixer.ride(2, -50, 200, false);
    CHECK(r.mixer.liveSteps() == steps);

    r.mixer.endRide(1);
    CHECK(r.mixer.rideTrack() == MixModel::kNoRide);
    r.settle();
    CHECK(r.doc.revision() == rev + 1);
    CHECK(near(r.session.stripGain(1), kFaderMaxDb));
    CHECK(near(r.engine_gain(1), fader_linear_gain(kFaderMaxDb)));
    CHECK(r.refused.empty());

    // A press and release with no motion saves nothing.
    r.mixer.beginRide(1);
    r.mixer.endRide(1);
    r.settle();
    CHECK(r.doc.revision() == rev + 1);
    std::printf("  a track's ride is heard per step and saved once: ok\n");
    return 0;
}

int ride_through_the_document() {
    // The bus has no ParamId: each step is a SetMonitorBus, heard as it goes.
    {
        Rig r;
        CHECK(r.connect());
        const uint64_t rev = r.doc.revision();
        r.mixer.beginRide(-1);
        r.mixer.ride(-1, 30, 200, false);
        r.mixer.ride(-1, 30, 200, false);
        CHECK(r.mixer.liveSteps() == 0);
        r.settle();
        CHECK(r.doc.revision() == rev + 2);
        CHECK(near(r.session.busGain(), r.mixer.rideGain()) && r.session.busGain() < 0.0);
        r.mixer.endRide(-1);
        r.settle();
        CHECK(r.doc.revision() == rev + 2);
    }
    // A track past the engine's: no atomic to write, so the document.
    {
        Rig r(2);
        CHECK(r.connect());
        const uint64_t rev = r.doc.revision();
        r.mixer.beginRide(3);
        r.mixer.ride(3, -30, 200, false);
        CHECK(r.mixer.liveSteps() == 0);
        r.settle();
        CHECK(r.doc.revision() == rev + 1 && r.session.stripGain(3) > 0.0f);
        r.mixer.ride(1, -30, 200, false);   // not the ride
        r.mixer.endRide(3);
        r.settle();
        CHECK(r.doc.revision() == rev + 1);
    }
    // No live path at all (no sender): the same.
    {
        Rig r(-1);
        CHECK(r.connect());
        r.mixer.beginRide(0);
        r.mixer.ride(0, -30, 200, false);
        r.settle();
        CHECK(r.mixer.liveSteps() == 0 && r.session.stripGain(0) > 0.0f);
        r.mixer.endRide(0);
    }
    std::printf("  the bus and unreachable tracks ride through the document: ok\n");
    return 0;
}

// --- interrupted rides (PR #61 review, findings 1-3) ---------------------------

void resync(Rig& r) {
    api::ModelRequest q;
    q.action = api::ModelAction::Resync;
    r.engine.submitModel(q);
}

int ride_across_a_lost_link() {
    // Regression: the link went mid-ride. The steps already delivered set
    // the engine's gain; the document refused the release while offline and
    // the restoring change was dropped unsent, so the engine kept the ride's
    // gain with nothing to say so. Since PR #71 the engine puts it back
    // itself: a Resync applies the document's mix, and a client that leaves
    // has the tracks it was the last to ride restored. The interface sends
    // nothing for it.
    Rig r;
    CHECK(r.connect());
    r.mixer.beginRide(1);
    r.mixer.ride(1, -40, 200, false);
    const float up = static_cast<float>(r.mixer.rideGain());
    CHECK(up > 0.0f);
    r.settle();
    CHECK(near(r.engine_gain(1), fader_linear_gain(up)));
    r.link = false;
    r.doc.set_link(false);
    r.mixer.endRide(1);
    r.settle();
    CHECK(near(r.session.stripGain(1), 0.0) && !r.refused.empty());
    CHECK(near(r.engine_gain(1), fader_linear_gain(up)));
    // The next connection brings the engine's document as a Snapshot (a
    // Resync here): the engine is put back to what that document holds.
    r.link = true;
    r.doc.set_link(true);
    const int sends = r.gain_sends;
    resync(r);
    r.settle();
    CHECK(near(r.session.stripGain(1), 0.0));
    for (uint32_t i = 0; i < 4; ++i) CHECK(near(r.engine_gain(i), 1.0));
    CHECK(r.gain_sends == sends);
    std::printf("  a ride cut by a lost link is undone on the next Snapshot: ok\n");
    return 0;
}

int ride_refusal_stays_said() {
    // Regression: endRide said the refusal, then the restoring change's Ok
    // reply cleared the status line on the next tick.
    Rig r;
    CHECK(r.connect());
    QObject::connect(&r.session, &SessionModel::refused, &r.shell, &ShellModel::say);
    r.mixer.beginRide(1);
    r.mixer.ride(1, -40, 200, false);
    r.settle();
    r.doc.set_link(false);   // the document refuses; the fader's path still answers
    r.mixer.endRide(1);
    const QString said = r.shell.statusText();
    CHECK(!said.isEmpty());
    r.settle();
    CHECK(r.shell.statusText() == said);
    CHECK(near(r.engine_gain(1), 1.0));
    // Nor does a ride's every step wipe what the line was saying.
    r.doc.set_link(true);
    r.shell.say(QStringLiteral("a line"));
    r.mixer.beginRide(2);
    r.mixer.ride(2, -20, 200, false);
    r.mixer.ride(2, -20, 200, false);
    r.settle();
    CHECK(r.mixer.liveSteps() >= 3);
    CHECK(r.shell.statusText() == QStringLiteral("a line"));
    r.mixer.endRide(2);
    // A command's Ok still clears it.
    r.shell.invoke(QStringLiteral("transport.play"), -1);
    r.settle();
    CHECK(r.shell.statusText().isEmpty());
    std::printf("  a ride's refusal stays on the status line: ok\n");
    return 0;
}

int ride_follows_its_track() {
    // Regression: a ride was its strip's index. A reorder mid-ride (Ctrl+Z
    // with the mouse down) moved the next steps to another track, and the
    // release saved the ride onto it.
    {
        Rig r;
        CHECK(r.connect());
        std::string why;
        CHECK(r.session.controller().set_gain(1, -6.0f, why));
        r.settle();
        r.mixer.beginRide(1);
        r.mixer.ride(1, -40, 200, false);
        r.settle();
        CHECK(r.engine_gain(1) > 1.0f);
        std::vector<model::Id> order;
        for (const auto& t : r.doc.snapshot().edit->tracks) order.push_back(t->id);
        std::reverse(order.begin(), order.end());
        CHECK(r.doc.submit(model::ReorderTracks{order}, why));
        r.settle();
        CHECK(r.mixer.rideTrack() == MixModel::kNoRide);
        CHECK(!r.refused.empty() && r.refused.back().find("moved") != std::string::npos);
        const uint64_t rev = r.doc.revision();
        const uint64_t steps = r.mixer.liveSteps();
        r.mixer.ride(1, -40, 200, false);
        r.mixer.endRide(1);
        r.settle();
        CHECK(r.doc.revision() == rev && r.mixer.liveSteps() == steps);
        const QList<qreal> g = r.session.gains();
        CHECK(near(g[0], 0.0) && near(g[1], 0.0) && near(g[2], -6.0) && near(g[3], 0.0));
        for (int i = 0; i < 4; ++i) {
            CHECK(near(r.engine_gain(static_cast<uint32_t>(i)),
                       fader_linear_gain(static_cast<float>(g[i]))));
        }
    }
    // The track leaves (an undone AddTrack) mid-ride: dropped, nothing saved.
    {
        // Three live tracks: the ride on the fourth row has no live path,
        // so each step goes to the document, where its undo can drop it.
        Rig r(3);
        CHECK(r.connect());
        std::string why;
        // Room for it on the four engine tracks (ADR-0023 §5).
        CHECK(r.doc.submit(model::RemoveTrack{r.doc.snapshot().edit->tracks.back()->id}, why));
        r.settle();
        model::AddTrack add;
        add.track = r.doc.reserve_id();
        add.name = "Extra";
        add.playlist = r.doc.reserve_id();
        add.playlist_name = "Extra.01";
        add.mix.input = {0};
        CHECK(r.doc.submit(add, why));
        r.settle();
        CHECK(r.session.trackCount() == 4);
        r.mixer.beginRide(3);
        r.mixer.ride(3, -30, 200, false);
        r.settle();
        CHECK(r.session.stripGain(3) > 0.0f);
        CHECK(r.doc.undo(why));
        r.settle();
        CHECK(r.session.trackCount() == 3);
        CHECK(r.mixer.rideTrack() == MixModel::kNoRide);
        CHECK(!r.refused.empty() && r.refused.back().find("gone") != std::string::npos);
        const uint64_t rev = r.doc.revision();
        r.mixer.ride(3, -30, 200, false);
        r.mixer.endRide(3);
        r.settle();
        CHECK(r.doc.revision() == rev);
    }
    // Another session arrives mid-ride (another interface's Open): the
    // release must not write the old ride into it.
    {
        Rig r;
        CHECK(r.connect());
        r.mixer.beginRide(0);
        r.mixer.ride(0, -40, 200, false);
        r.settle();
        CHECK(r.engine_gain(0) > 1.0f);
        std::string why;
        CHECK(r.doc.reset(make_empty_session(4, 48000), why));
        r.settle();
        CHECK(r.mixer.rideTrack() == MixModel::kNoRide);
        CHECK(!r.refused.empty() && r.refused.back().find("session") != std::string::npos);
        const uint64_t rev = r.doc.revision();
        r.mixer.endRide(0);
        r.settle();
        CHECK(r.doc.revision() == rev && near(r.session.stripGain(0), 0.0));
        CHECK(near(r.engine_gain(0), 1.0));
    }
    std::printf("  a ride is its track's, and is dropped when it goes: ok\n");
    return 0;
}

// --- gains sent of the interface's own accord (PR #61, PR #71) ----------------

int no_gain_sent_without_a_ride() {
    // Regression: every new document (the Snapshot that greets a
    // connection, a Resync, another interface's Open) sent every engine
    // track's gain again. Since PR #71 the engine applies the mix on a
    // Resync and on every Open, and on purpose leaves a ride alone when a
    // client attaches mid-ride. The resend overrode another interface's
    // ride, and made this interface the track's last rider, so the engine
    // no longer put the track back if the real rider then left.
    Rig r;
    CHECK(r.connect());
    CHECK(r.gain_sends == 0);
    resync(r);
    r.settle();
    CHECK(r.gain_sends == 0);
    std::string why;
    CHECK(r.doc.reset(make_empty_session(4, 48000), why));
    r.settle();
    CHECK(r.gain_sends == 0);
    for (uint32_t i = 0; i < 4; ++i) CHECK(near(r.engine_gain(i), 1.0));
    std::printf("  a new document sends no gain: ok\n");
    return 0;
}

int ride_step_crossing_another_edit() {
    // What the engine cannot cover: a step sent before this interface has
    // seen another interface's reorder reaches the engine after the engine
    // applied that reorder's mix, and lands on whichever track is now at
    // the ride's index. Nothing on the engine says it is stale; the
    // interface drops the ride when the update arrives and puts that one
    // index back to the document.
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.session.controller().set_gain(1, -6.0f, why));
    r.settle();
    r.mixer.beginRide(1);
    r.mixer.ride(1, -40, 200, false);
    r.settle();
    // Sent as another interface sends it: past this one's replica.
    std::vector<model::Id> order;
    for (const auto& t : r.doc.snapshot().edit->tracks) order.push_back(t->id);
    std::reverse(order.begin(), order.end());
    api::ModelRequest q;
    q.action = api::ModelAction::Apply;
    CHECK(model::encode_operation(model::ReorderTracks{order}, q.op, why));
    r.engine.submitModel(q);
    r.mixer.ride(1, 20, 200, false);   // in flight behind the reorder
    CHECK(r.mixer.rideTrack() == 1);
    const int sends = r.gain_sends;
    r.settle();
    CHECK(r.mixer.rideTrack() == MixModel::kNoRide);
    CHECK(!r.refused.empty() && r.refused.back().find("moved") != std::string::npos);
    CHECK(r.gain_sends == sends + 1);   // that index alone
    const QList<qreal> g = r.session.gains();
    CHECK(near(g[1], 0.0) && near(g[2], -6.0));
    for (int i = 0; i < 4; ++i) {
        CHECK(near(r.engine_gain(static_cast<uint32_t>(i)),
                   fader_linear_gain(static_cast<float>(g[i]))));
    }
    std::printf("  a step crossing another interface's reorder is put back: ok\n");
    return 0;
}

int live_path_follows_the_engine() {
    // The engine's track count is the latest Hello's (PR #67 review): a
    // restarted engine with more tracks gives those tracks a live path.
    Rig r(2);
    CHECK(r.connect());
    r.mixer.beginRide(3);
    r.mixer.ride(3, -30, 200, false);
    CHECK(r.mixer.liveSteps() == 0);
    r.mixer.endRide(3);
    r.settle();
    r.engine_tracks = 4;
    r.mixer.beginRide(3);
    r.mixer.ride(3, 30, 200, false);
    CHECK(r.mixer.liveSteps() == 1);
    r.mixer.endRide(3);
    r.settle();
    CHECK(near(r.engine_gain(3), fader_linear_gain(r.session.stripGain(3))));
    std::printf("  the live path follows the engine's track count: ok\n");
    return 0;
}

// --- typed values and modifier clicks, through the engine ---------------------

int typed_values() {
    Rig r;
    CHECK(r.connect());
    CHECK(r.mixer.setGainText(1, QStringLiteral("-6.5 dB")));
    CHECK(r.mixer.setGainText(-1, QStringLiteral("-3")));
    CHECK(r.mixer.setPanText(2, QStringLiteral("L30")));
    r.settle();
    CHECK(near(r.session.stripGain(1), -6.5) && near(r.session.busGain(), -3.0));
    CHECK(near(r.session.stripPan(2), -0.30));
    CHECK(r.refused.empty());
    const uint64_t rev = r.doc.revision();
    CHECK(!r.mixer.setGainText(1, QStringLiteral("+20")));
    CHECK(!r.refused.empty() && r.refused.back().find("+12 dB") != std::string::npos);
    CHECK(!r.mixer.setPanText(2, QStringLiteral("hard left")));
    CHECK(r.refused.back().find("not a pan") != std::string::npos);
    r.settle();
    CHECK(r.doc.revision() == rev && near(r.session.stripGain(1), -6.5));
    std::printf("  typed values reach the engine's document: ok\n");
    return 0;
}

int modifier_clicks() {
    Rig r;
    CHECK(r.connect());
    // A plain click is the command's, sent by QML.
    CHECK(!r.mixer.soloClick(0, 0) && !r.mixer.muteClick(0, 0));
    // Alt-click solo: every track to the clicked one's new state.
    CHECK(r.mixer.soloClick(2, Qt::AltModifier));
    r.settle();
    CHECK(r.session.solos() == QList<bool>({true, true, true, true}));
    CHECK(r.mixer.soloClick(0, Qt::AltModifier));
    r.settle();
    CHECK(r.session.solos() == QList<bool>({false, false, false, false}));
    // From a mixed state, to the clicked track's new one.
    std::string why;
    CHECK(r.session.controller().toggle_mute(1, why));
    r.settle();
    CHECK(r.mixer.muteClick(0, Qt::AltModifier));
    r.settle();
    CHECK(r.session.mutes() == QList<bool>({true, true, true, true}));
    CHECK(r.mixer.muteClick(1, Qt::AltModifier));
    r.settle();
    CHECK(r.session.mutes() == QList<bool>({false, false, false, false}));
    CHECK(r.refused.empty());
    // Ctrl-click solo is solo safe: one SetMix, held by the engine's own
    // document, and the track's solo left as it was.
    const auto engine_mix = [&r](int track) {
        const model::Snapshot& s = r.fake.document();
        return *s.mix.find(s.edit->tracks[static_cast<std::size_t>(track)]->id);
    };
    const uint64_t rev = r.doc.revision();
    CHECK(r.mixer.soloClick(3, Qt::ControlModifier));
    r.settle();
    CHECK(r.refused.empty());
    CHECK(r.doc.revision() == rev + 1);
    CHECK(engine_mix(3).solo_safe && !engine_mix(3).solo);
    CHECK(r.session.soloSafes() == QList<bool>({false, false, false, true}));
    CHECK(r.session.solos() == QList<bool>({false, false, false, false}));
    // Ctrl+Alt is solo safe, never a solo of every track.
    CHECK(r.mixer.soloClick(2, Qt::ControlModifier | Qt::AltModifier));
    r.settle();
    CHECK(engine_mix(2).solo_safe && !engine_mix(2).solo);
    CHECK(r.session.solos() == QList<bool>({false, false, false, false}));
    // And again: off. A soloed track keeps its solo through both.
    CHECK(r.session.controller().toggle_solo(3, why));
    r.settle();
    CHECK(r.mixer.soloClick(3, Qt::ControlModifier));
    CHECK(r.mixer.soloClick(2, Qt::ControlModifier));
    r.settle();
    CHECK(!engine_mix(3).solo_safe && engine_mix(3).solo && !engine_mix(2).solo_safe);
    CHECK(r.session.soloSafes() == QList<bool>({false, false, false, false}));
    // A track that is not there is refused by name, and nothing is sent.
    const uint64_t before = r.doc.revision();
    CHECK(r.mixer.soloClick(7, Qt::ControlModifier));
    CHECK(!r.refused.empty() && r.refused.back().find("no track 8") != std::string::npos);
    r.settle();
    CHECK(r.doc.revision() == before);
    std::printf("  Alt-click all, Ctrl-click solo safe through SetMix: ok\n");
    return 0;
}

// The outline under a strip's mute says another track's solo silences it,
// so it follows the engine's rule (Session::process()): soloed tracks, a
// solo-safe one and every track with no solo anywhere are not outlined.
int implicit_mutes() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.session.implicitMutes() == QList<bool>({false, false, false, false}));
    // Solo safe alone silences nothing and outlines nothing.
    CHECK(r.mixer.soloClick(1, Qt::ControlModifier));
    r.settle();
    CHECK(r.session.implicitMutes() == QList<bool>({false, false, false, false}));
    CHECK(r.session.controller().toggle_solo(0, why));
    r.settle();
    CHECK(r.session.implicitMutes() == QList<bool>({false, false, true, true}));
    // Its own mute still applies, and is the mute button lit, not an outline.
    CHECK(r.session.controller().toggle_mute(1, why));
    r.settle();
    CHECK(r.session.mutes()[1] && !r.session.implicitMutes()[1]);
    // Solo safe off: the solo silences it again.
    CHECK(r.mixer.soloClick(1, Qt::ControlModifier));
    r.settle();
    CHECK(r.session.implicitMutes() == QList<bool>({false, true, true, true}));
    CHECK(r.refused.empty());
    std::printf("  implicit-mute outline skips solo-safe tracks: ok\n");
    return 0;
}

// --- the QML -----------------------------------------------------------------

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
    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&r.shell)},
                               {QStringLiteral("session"), QVariant::fromValue(&r.session)},
                               {QStringLiteral("mixer"), QVariant::fromValue(&r.mixer)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    r.shell.routeKeysFrom(&view);
    r.session.setWindow(QStringLiteral("mix"));
    view.show();
    view.requestActivate();
    CHECK(QTest::qWaitForWindowExposed(&view));
    CHECK(QTest::qWaitForWindowActive(&view));
    QQuickItem* root = view.rootObject();

    // What each meter reads is written on it, and the line above says it.
    QQuickItem* cap = named(root, QStringLiteral("meterCaption0"));
    CHECK(cap != nullptr && cap->property("text").toString() == QStringLiteral("IN"));
    QQuickItem* bus = named(root, QStringLiteral("busMeterCaption"));
    CHECK(bus != nullptr && bus->property("text").toString() == QStringLiteral("OUT"));
    QQuickItem* note = named(root, QStringLiteral("mixNote"));
    CHECK(note != nullptr && note->property("text").toString() == r.mixer.note());
    CHECK(r.mixer.note().contains(QStringLiteral("input")));

    // A drag on a fader is a ride: heard, the cap following it, saved at
    // release.
    QQuickItem* fader = named(root, QStringLiteral("fader0"));
    CHECK(fader != nullptr);
    const uint64_t rev = r.doc.revision();
    const QPoint from = centre(fader);
    QTest::mousePress(&view, Qt::LeftButton, {}, from);
    for (int i = 1; i <= 4; ++i) QTest::mouseMove(&view, from - QPoint(0, 10 * i));
    CHECK(r.mixer.rideTrack() == 0 && r.mixer.rideGain() > 0.0);
    CHECK(r.mixer.liveSteps() >= 3);
    r.settle();
    CHECK(r.doc.revision() == rev);
    QTest::mouseRelease(&view, Qt::LeftButton, {}, from - QPoint(0, 40));
    CHECK(r.mixer.rideTrack() == MixModel::kNoRide);
    r.settle();
    CHECK(r.doc.revision() == rev + 1 && r.session.stripGain(0) > 0.0f);

    // Click the readout and type: the keys go to the field, not the
    // profile ("s" would solo), and Return sets the gain.
    QQuickItem* readout = named(root, QStringLiteral("gainValue1"));
    CHECK(readout != nullptr);
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(readout));
    CHECK(r.shell.textEntry());
    const QList<bool> solos = r.session.solos();
    QTest::keyClick(&view, Qt::Key_A, Qt::ControlModifier);
    for (const char c : std::string("-9s")) QTest::keyClick(&view, c);
    CHECK(r.session.solos() == solos);
    QTest::keyClick(&view, Qt::Key_Backspace);
    QTest::keyClick(&view, Qt::Key_Return);
    CHECK(!r.shell.textEntry());
    r.settle();
    CHECK(near(r.session.stripGain(1), -9.0));
    // Text that is not a gain keeps the field open, and Escape leaves it.
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(readout));
    for (const char c : std::string("99")) QTest::keyClick(&view, c);
    QTest::keyClick(&view, Qt::Key_Return);
    CHECK(r.shell.textEntry() && r.refused.back().find("+12 dB") != std::string::npos);
    QTest::keyClick(&view, Qt::Key_Escape);
    CHECK(!r.shell.textEntry());
    r.settle();
    CHECK(near(r.session.stripGain(1), -9.0));

    // Alt-click on a solo button solos every track.
    QQuickItem* solo = named(root, QStringLiteral("solo2"));
    CHECK(solo != nullptr);
    QTest::mouseClick(&view, Qt::LeftButton, Qt::AltModifier, centre(solo));
    r.settle();
    CHECK(r.session.solos() == QList<bool>({true, true, true, true}));
    // A plain click is the command: one track.
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(solo));
    r.settle();
    CHECK(r.session.solos() == QList<bool>({true, true, false, true}));

    r.refused.clear();  // the typed "99" above
    // Ctrl-click: solo safe. The button is recessed with its padlock, its
    // mute loses the outline the other solos gave it, and the flag is the
    // engine's document's.
    const auto outlined = [&root](int track) {
        QQuickItem* m = named(root, QStringLiteral("mute%1").arg(track));
        QObject* border = m != nullptr ? m->property("border").value<QObject*>() : nullptr;
        return border != nullptr &&
               border->property("color").value<QColor>() == QColor::fromRgba(theme::kMute);
    };
    const auto engine_safe = [&r](int track) {
        const model::Snapshot& s = r.fake.document();
        return s.mix.find(s.edit->tracks[static_cast<std::size_t>(track)]->id)->solo_safe;
    };
    QQuickItem* padlock = named(root, QStringLiteral("soloSafe2"));
    CHECK(padlock != nullptr && !padlock->isVisible());
    CHECK(!solo->property("recessed").toBool());
    CHECK(outlined(2));
    QTest::mouseClick(&view, Qt::LeftButton, Qt::ControlModifier, centre(solo));
    r.settle();
    CHECK(engine_safe(2));
    CHECK(r.session.solos() == QList<bool>({true, true, false, true}));
    CHECK(padlock->isVisible() && solo->property("recessed").toBool());
    CHECK(!outlined(2));
    // The Edit window's solo button shows it, and its Ctrl-click is solo
    // safe too, never a solo.
    r.session.setWindow(QStringLiteral("edit"));
    QTest::qWait(50);
    QQuickItem* edit_lock2 = named(root, QStringLiteral("editSoloSafe2"));
    CHECK(edit_lock2 != nullptr && edit_lock2->isVisible());
    // Track 2 unsoloed first, so a Ctrl-click that soloed would show.
    std::string why;
    CHECK(r.session.controller().toggle_solo(1, why));
    r.settle();
    CHECK(r.session.solos() == QList<bool>({true, false, false, true}));
    QQuickItem* edit_solo = named(root, QStringLiteral("editSolo1"));
    QQuickItem* edit_lock1 = named(root, QStringLiteral("editSoloSafe1"));
    CHECK(edit_solo != nullptr && edit_solo->isVisible());
    CHECK(edit_lock1 != nullptr && !edit_lock1->isVisible());
    QTest::mouseClick(&view, Qt::LeftButton, Qt::ControlModifier, centre(edit_solo));
    r.settle();
    CHECK(engine_safe(1));
    CHECK(r.session.soloSafes() == QList<bool>({false, true, true, false}));
    CHECK(r.session.solos() == QList<bool>({true, false, false, true}));
    CHECK(edit_lock1->isVisible() && edit_solo->property("recessed").toBool());
    r.session.setWindow(QStringLiteral("mix"));
    QTest::qWait(50);
    // Ctrl-click again in the Mix window: off, and the outline is back.
    QTest::mouseClick(&view, Qt::LeftButton, Qt::ControlModifier, centre(solo));
    r.settle();
    CHECK(!engine_safe(2));
    CHECK(!padlock->isVisible() && !solo->property("recessed").toBool() && outlined(2));
    // The I button, Pro Tools' TrackInput: Input mode on and off, in the
    // engine's document, lit in the Mix window and the Edit window alike.
    const auto engine_input = [&r](int track) {
        const model::Snapshot& s = r.fake.document();
        return s.mix.find(s.edit->tracks[static_cast<std::size_t>(track)]->id)->monitor ==
               model::MonitorMode::Input;
    };
    QQuickItem* input = named(root, QStringLiteral("input2"));
    CHECK(input != nullptr && !input->property("active").toBool());
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(input));
    r.settle();
    CHECK(engine_input(2) && input->property("active").toBool());
    CHECK(r.session.inputMonitors() == QList<bool>({false, false, true, false}));
    r.session.setWindow(QStringLiteral("edit"));
    QTest::qWait(50);
    QQuickItem* edit_input2 = named(root, QStringLiteral("editInput2"));
    QQuickItem* edit_input1 = named(root, QStringLiteral("editInput1"));
    CHECK(edit_input2 != nullptr && edit_input2->property("active").toBool());
    CHECK(edit_input1 != nullptr && !edit_input1->property("active").toBool());
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(edit_input1));
    r.settle();
    CHECK(engine_input(1) && edit_input1->property("active").toBool());
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(edit_input2));
    r.settle();
    CHECK(!engine_input(2) && !edit_input2->property("active").toBool());
    r.session.setWindow(QStringLiteral("mix"));
    QTest::qWait(50);
    CHECK(!input->property("active").toBool());
    CHECK(r.session.inputMonitors() == QList<bool>({false, true, false, false}));
    CHECK(r.refused.empty());
    std::printf("  qml: ok\n");
    return 0;
}

// --- automation: the hand's gesture and the mode selector --------------------

int automation_gestures() {
    using api::Gesture;
    using api::ParamId;
    Rig r;
    CHECK(r.connect());
    // Press, moves, release: Touch before the first move, Release after the
    // last, so Touch and Latch know when the hand is on the fader.
    r.mixer.beginRide(1);
    CHECK(r.sent.size() == 1 && r.sent[0].gesture == Gesture::Touch &&
          r.sent[0].param == ParamId::Gain && r.sent[0].track == 1);
    r.mixer.ride(1, -20, 200, false);
    r.mixer.ride(1, -20, 200, false);
    CHECK(r.sent.size() == 3 && r.sent[1].gesture == Gesture::Move &&
          r.sent[2].gesture == Gesture::Move);
    r.mixer.endRide(1);
    r.settle();
    CHECK(r.sent.size() == 4 && r.sent[3].gesture == Gesture::Release && r.sent[3].track == 1);
    CHECK(r.refused.empty());
    // A press with no motion is still a touch and a release: Touch mode
    // holds the value under the hand even when it does not move.
    r.sent.clear();
    r.mixer.beginRide(2);
    r.mixer.endRide(2);
    CHECK(r.sent.size() == 2 && r.sent[0].gesture == Gesture::Touch &&
          r.sent[1].gesture == Gesture::Release);
    // A strip the engine does not have has no live path, so no gesture.
    r.sent.clear();
    r.engine_tracks = 2;
    r.mixer.beginRide(3);
    r.mixer.endRide(3);
    CHECK(r.sent.empty());
    r.engine_tracks = 4;
    // The pan: Touch, a Move per step on the Pan parameter, Release.
    r.mixer.beginPan(0);
    r.mixer.panDrag(0, 10, 100, false);
    r.mixer.endPan(0);
    CHECK(r.sent.size() == 3 && r.sent[0].gesture == Gesture::Touch &&
          r.sent[1].gesture == Gesture::Move && r.sent[2].gesture == Gesture::Release);
    for (const auto& x : r.sent) CHECK(x.param == ParamId::Pan && x.track == 0);
    // A drag the hand never began (a keyboard or surface nudge) is no gesture.
    r.sent.clear();
    r.mixer.panDrag(0, 10, 100, false);
    r.mixer.endPan(0);
    CHECK(r.sent.empty());
    // A hold that loses its Release leaves the track touched (PR #187): every
    // way out of a hold ends with the Release to the index that was Touched.
    {
        // A second press does not strand the first.
        r.sent.clear();
        r.mixer.beginPan(0);
        r.mixer.beginPan(1);
        CHECK(r.sent.size() == 3 && r.sent[1].gesture == Gesture::Release && r.sent[1].track == 0 &&
              r.sent[2].gesture == Gesture::Touch && r.sent[2].track == 1);
        r.mixer.endPan(1);
        r.sent.clear();
        r.mixer.beginRide(0);
        r.mixer.beginRide(1);
        CHECK(r.sent.size() == 3 && r.sent[1].gesture == Gesture::Release && r.sent[1].track == 0 &&
              r.sent[2].gesture == Gesture::Touch && r.sent[2].track == 1);
        r.mixer.endRide(1);
    }
    {
        // The session is replaced under the hand.
        Rig q;
        CHECK(q.connect());
        q.mixer.beginPan(2);
        q.sent.clear();
        std::string why;
        CHECK(q.doc.reset(make_empty_session(4, 48000), why));
        q.settle();
        CHECK(q.sent.size() == 1 && q.sent[0].gesture == Gesture::Release && q.sent[0].track == 2);
        q.mixer.endPan(2);
        CHECK(q.sent.size() == 1);
    }
    {
        // The track is removed under the hand: its index gets the Release,
        // and nothing more goes at the lift.
        Rig q;
        CHECK(q.connect());
        std::string why;
        q.mixer.beginPan(1);
        q.sent.clear();
        CHECK(q.doc.submit(model::RemoveTrack{q.doc.snapshot().edit->tracks[1]->id}, why));
        q.settle();
        CHECK(q.sent.size() == 1 && q.sent[0].gesture == Gesture::Release &&
              q.sent[0].param == ParamId::Pan && q.sent[0].track == 1);
        q.mixer.endPan(1);
        CHECK(q.sent.size() == 1);
    }
    std::printf("  automation gestures: ok\n");
    return 0;
}

int automation_mode_selector() {
    Rig r;
    CHECK(r.connect());
    CHECK(r.session.automationModes() == QStringList({"Read", "Read", "Read", "Read"}));
    const uint64_t rev = r.doc.revision();
    CHECK(r.mixer.setAutomationMode(1, "Touch"));
    r.settle();
    CHECK(r.doc.revision() == rev + 1 && r.refused.empty());
    CHECK(r.session.automationModes() == QStringList({"Read", "Touch", "Read", "Read"}));
    for (const char* m : {"Latch", "Write", "Off", "Read"}) {
        CHECK(r.mixer.setAutomationMode(1, m));
        r.settle();
        CHECK(r.session.automationModes()[1] == m);
    }
    // The mode already set is no edit, no undo step.
    const uint64_t same = r.doc.revision();
    CHECK(r.mixer.setAutomationMode(1, "Read"));
    r.settle();
    CHECK(r.doc.revision() == same);
    // Trim is refused by name and nothing is sent.
    CHECK(!r.mixer.setAutomationMode(1, "Trim"));
    CHECK(!r.refused.empty() && r.refused.back().find("Trim") != std::string::npos);
    r.settle();
    CHECK(r.doc.revision() == same);
    CHECK(!r.mixer.setAutomationMode(9, "Write"));
    CHECK(!r.mixer.setAutomationMode(1, "Banana"));
    // The lane header's label follows the strip.
    CHECK(r.mixer.setAutomationMode(2, "Write"));
    r.settle();
    CHECK(r.session.automationModes()[2] == "Write");
    std::printf("  automation mode selector: ok\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (typed_text() != 0) return 1;
    if (display_order() != 0) return 1;
    if (track_ride() != 0) return 1;
    if (ride_through_the_document() != 0) return 1;
    if (ride_across_a_lost_link() != 0) return 1;
    if (ride_refusal_stays_said() != 0) return 1;
    if (ride_follows_its_track() != 0) return 1;
    if (no_gain_sent_without_a_ride() != 0) return 1;
    if (ride_step_crossing_another_edit() != 0) return 1;
    if (live_path_follows_the_engine() != 0) return 1;
    if (automation_gestures() != 0) return 1;
    if (automation_mode_selector() != 0) return 1;
    if (typed_values() != 0) return 1;
    if (modifier_clicks() != 0) return 1;
    if (implicit_mutes() != 0) return 1;
    if (qml() != 0) return 1;
    std::printf("ui mix: all checks passed\n");
    return 0;
}
