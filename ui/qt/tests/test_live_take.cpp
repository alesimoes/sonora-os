// SPDX-License-Identifier: GPL-3.0-or-later
//
// The take being recorded, drawn growing on each armed track's lane as Pro
// Tools draws it: where it starts and how far it reaches (a plain take, a
// punch, a loop take's passes), the overview from the tracks' input peaks,
// the record colour, which tracks get one, and when it goes (the engine's
// region arrives, the engine dies, a new take starts). Through ShellModel's
// tick on a RecordingAdapter, so the peak reads are counted. Nothing is
// rendered.
#include <QGuiApplication>
#include <cmath>
#include <cstdio>

#include "key_router.h"
#include "live_take.h"
#include "peak_source.h"
#include "recording_adapter.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "theme.h"
#include "timeline_document.h"
#include "timeline_scene.h"

using namespace sonora;
using namespace sonora::ui;
using model::Frames;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

constexpr Frames B = LiveTake::kBucket;

const LiveTake::Lane* lane_of(const LiveTake& t, int row) {
    for (const auto& l : t.lanes()) {
        if (l.row == row) return &l;
    }
    return nullptr;
}

float peak_at(const LiveTake& t, const LiveTake::Lane& l, Frames at) {
    PeakPair p;
    t.peaks(l, at, 1.0, 1, &p);
    return p.max;
}

int unit() {
    const std::vector<bool> armed{true, false, true};
    const std::vector<bool> audio{true, true, true};
    model::Transport plain;

    // --- a plain take starts where the playhead stood at the reading
    // before it, and reaches the playhead; unarmed tracks get nothing ---
    {
        LiveTake t;
        t.tick(false, 10 * B, armed, audio, {0.9f, 0.9f, 0.9f}, plain);
        CHECK(t.empty());
        t.tick(true, 12 * B, armed, audio, {0.5f, 0.9f, 0.25f}, plain);
        CHECK(t.lanes().size() == 2 && t.recording());
        const auto* a = lane_of(t, 0);
        const auto* c = lane_of(t, 2);
        CHECK(a && c && !lane_of(t, 1));
        CHECK(a->start == 10 * B && a->end == 12 * B);
        CHECK(peak_at(t, *a, 10 * B) == 0.5f && peak_at(t, *c, 11 * B) == 0.25f);
        t.tick(true, 14 * B, armed, audio, {0.75f, 0.0f, 0.0f}, plain);
        a = lane_of(t, 0);
        CHECK(a->end == 14 * B);
        // Each reading lands on the stretch the playhead covered since the
        // one before.
        CHECK(peak_at(t, *a, 11 * B) == 0.5f && peak_at(t, *a, 13 * B) == 0.75f);
        CHECK(peak_at(t, *a, 14 * B) == 0.0f);   // not recorded yet
        // Over several buckets a column is their maximum, both sides.
        PeakPair p;
        t.peaks(*a, 10 * B, 4.0 * B, 1, &p);
        CHECK(p.max == 0.75f && p.min == -0.75f);

        // --- stopped: kept for the engine's region, then gone ---
        for (int i = 0; i < LiveTake::kHoldTicks; ++i) {
            t.tick(false, 14 * B, armed, audio, {0, 0, 0}, plain);
        }
        CHECK(!t.empty() && !t.recording());
        t.tick(false, 14 * B, armed, audio, {0, 0, 0}, plain);
        CHECK(t.empty());

        // --- a new take does not inherit the last one's lanes ---
        // The transport located from 14 B to 30 B: the take starts at 30 B,
        // and the peak read then is from before it.
        t.tick(true, 30 * B, {false, true, false}, audio, {0, 0.5f, 0}, plain);
        CHECK(t.empty() && t.recording());
        t.tick(true, 31 * B, {false, true, false}, audio, {0, 0.5f, 0}, plain);
        CHECK(t.lanes().size() == 1 && lane_of(t, 1));
        CHECK(lane_of(t, 1)->start == 30 * B);
    }

    // --- only an audio track takes a recording ---
    {
        LiveTake t;
        t.tick(false, 0, armed, {true, true, false}, {0, 0, 0}, plain);
        t.tick(true, 2 * B, armed, {true, true, false}, {0.5f, 0.5f, 0.5f}, plain);
        CHECK(t.lanes().size() == 1 && lane_of(t, 0));
    }

    // --- a punch: nothing before the punch-in, nothing past the out ---
    {
        model::Transport punch;
        punch.punch = {{100 * B, 200 * B}};
        LiveTake t;
        t.tick(false, 50 * B, armed, audio, {0, 0, 0}, punch);
        t.tick(true, 52 * B, armed, audio, {0.5f, 0, 0.5f}, punch);   // pre-roll
        t.tick(true, 99 * B, armed, audio, {0.5f, 0, 0.5f}, punch);
        CHECK(t.empty() && t.recording());
        t.tick(true, 101 * B, armed, audio, {0.5f, 0, 0.5f}, punch);
        CHECK(lane_of(t, 0) && lane_of(t, 0)->start == 100 * B && lane_of(t, 0)->end == 101 * B);
        t.tick(true, 230 * B, armed, audio, {0.5f, 0, 0.5f}, punch);   // post-roll
        CHECK(lane_of(t, 0)->end == 200 * B);
    }

    // --- loop record: each pass starts again at A ---
    {
        model::Transport loop;
        loop.loop = {{100 * B, 140 * B}};
        loop.loop_record = true;
        LiveTake t;
        t.tick(false, 96 * B, armed, audio, {0, 0, 0}, loop);
        t.tick(true, 98 * B, armed, audio, {0.5f, 0, 0}, loop);   // pre-roll
        CHECK(t.empty());
        t.tick(true, 110 * B, armed, audio, {0.5f, 0, 0}, loop);
        CHECK(lane_of(t, 0)->start == 100 * B && lane_of(t, 0)->end == 110 * B);
        t.tick(true, 138 * B, armed, audio, {0.5f, 0, 0}, loop);
        CHECK(lane_of(t, 0)->end == 138 * B);
        t.tick(true, 104 * B, armed, audio, {0.25f, 0, 0}, loop);   // wrapped
        const auto* a = lane_of(t, 0);
        CHECK(a->start == 100 * B && a->end == 104 * B);
        CHECK(peak_at(t, *a, 101 * B) == 0.25f);
        // Loop playback alone is not loop record: a straight take.
        model::Transport play_loop = loop;
        play_loop.loop_record = false;
        LiveTake u;
        u.tick(false, 90 * B, armed, audio, {0, 0, 0}, play_loop);
        u.tick(true, 92 * B, armed, audio, {0.5f, 0, 0}, play_loop);
        CHECK(lane_of(u, 0)->start == 90 * B);
    }
    return 0;
}

// The overlay: on each recording lane a region in the record colour, its
// overview as tall as the track's peak, the red playhead over it, and the
// other lanes untouched.
int drawing() {
    LiveTake t;
    const std::vector<bool> armed{true, false, true};
    const std::vector<bool> audio{true, true, true};
    model::Transport plain;
    t.tick(false, 0, armed, audio, {0, 0, 0}, plain);
    for (int i = 1; i <= 100; ++i) t.tick(true, i * 1200, armed, audio, {0.5f, 1.0f, 1.0f}, plain);

    ViewState v;
    v.width = 1000;
    v.height = 400;
    v.fpp = 1200;
    v.track_height = 80;
    std::vector<SceneRect> out;
    TransportOverlay rec{100 * 1200, true, true};
    build_timeline_overlay(v, Selection{}, rec, out, &t);

    const uint32_t body = theme::region_body(theme::kRecord);
    const uint32_t band = theme::region_name_band(theme::kRecord);
    const uint32_t wave = theme::region_wave(theme::kRecord);
    // Track 1 (row 0): band at the top, waveform through the middle at half
    // height (peak 0.5), body above and below it.
    const SceneRect* r = rect_at(out, 50, 4);
    CHECK(r && r->argb == band);
    const double band_h = theme::kRegionNameBand;
    const double wy = 2 + band_h + 1, wh = 80 - 4 - band_h - 2;
    const float mid = static_cast<float>(wy + wh / 2);
    r = rect_at(out, 50, mid);
    CHECK(r && r->argb == wave);
    r = rect_at(out, 50, mid - static_cast<float>(wh * 0.25) + 1.5f);
    CHECK(r && r->argb == wave);
    r = rect_at(out, 50, mid - static_cast<float>(wh * 0.25) - 1.5f);
    CHECK(r && r->argb == body);
    // Track 3 (row 2) at full scale; track 2 (row 1), not armed, nothing.
    r = rect_at(out, 50, static_cast<float>(160 + wy + 1));
    CHECK(r && r->argb == wave);
    CHECK(rect_at(out, 50, 120) == nullptr);
    // Past the playhead, nothing; at it, the record-coloured playhead.
    CHECK(rect_at(out, 110, mid) == nullptr);
    r = rect_at(out, 100, mid);
    CHECK(r && r->argb == theme::kRecord);
    // The overview follows the vertical zoom.
    v.wave_scale = 2.0f;
    build_timeline_overlay(v, Selection{}, rec, out, &t);
    r = rect_at(out, 50, static_cast<float>(wy + 1));
    CHECK(r && r->argb == wave);
    // Scrolled past the take: none of it drawn, only the playhead.
    v.scroll = 200 * 1200.0;
    build_timeline_overlay(v, Selection{}, TransportOverlay{250 * 1200, true, true}, out, &t);
    CHECK(out.size() == 1 && out[0].argb == theme::kRecord);
    return 0;
}

// Through ShellModel's tick, as the window is fed.
int wiring() {
    KeyMap map = KeyMap::built_in_default();
    test::RecordingAdapter engine(4, api::testing::FakeEngine::engine_commands());
    ModelDocument document(make_demo_session(4, 48000));
    SyntheticPeakSource peak_source(48000, 120.0);
    SessionModel session(document, peak_source, true);
    KeyRouter router(map, engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    uint64_t now = 1;
    ShellModel shell(engine, router);
    shell.setClock([&now]() { return now; });
    shell.setTransportSink([&session](const TransportSnapshot& t, bool alive) {
        session.onTransport(t, alive);
    });
    shell.setLiveTakeSink([&session](const EngineFrame& f, const std::vector<float>& p,
                                     bool alive) { session.onLiveTake(f, p, alive); });
    session.controller().set_viewport(1000, 400);

    engine.armed = {true, false, true, false};
    engine.snapshot.playhead_frames = 48000;
    engine.peaks = {0.5f, 0.9f, 0.25f, 0.9f};
    shell.tick();
    // Stopped: nothing drawn, and no peak read for it (no meter is shown).
    CHECK(session.liveTake().empty() && engine.peak_reads == 0);

    // Recording: both armed tracks' peaks read every tick with no meter on
    // screen, the others' not.
    engine.snapshot.state = TransportMode::Recording;
    engine.snapshot.playhead_frames = 48800;
    shell.tick();
    CHECK(engine.peak_reads == 2);
    const LiveTake& live = session.liveTake();
    CHECK(live.lanes().size() == 2 && lane_of(live, 0) && lane_of(live, 2));
    CHECK(lane_of(live, 0)->start == 48000 && lane_of(live, 0)->end == 48800);
    CHECK(peak_at(live, *lane_of(live, 0), 48400) == 0.5f);
    CHECK(peak_at(live, *lane_of(live, 2), 48400) == 0.25f);
    std::vector<SceneRect> out;
    session.buildOverlay(out);
    CHECK(out.size() > 2 && out.back().argb == theme::kRecord);

    // A stale read is no reading: nothing read, nothing moved.
    engine.snapshot.stale = true;
    engine.snapshot.playhead_frames = 49600;
    engine.peaks = {0.75f, 0.9f, 0.75f, 0.9f};
    shell.tick();
    CHECK(engine.peak_reads == 2 && lane_of(live, 0)->end == 48800);
    engine.snapshot.stale = false;
    shell.tick();
    CHECK(engine.peak_reads == 4 && lane_of(live, 0)->end == 49600);
    CHECK(peak_at(live, *lane_of(live, 0), 49500) == 0.75f);

    // Stopped: held until the session's document says what the take left.
    engine.snapshot.state = TransportMode::Stopped;
    shell.tick();
    CHECK(!live.empty() && !live.recording());
    session.documentUpdated();
    CHECK(live.empty());

    // A count-off records nothing yet.
    engine.snapshot.state = TransportMode::Recording;
    engine.snapshot.counting_off = true;
    shell.tick();
    CHECK(live.empty());
    engine.snapshot.counting_off = false;
    engine.snapshot.playhead_frames = 50400;
    shell.tick();
    CHECK(!live.empty());
    // An edit arriving mid-take leaves the take drawn.
    session.documentUpdated();
    CHECK(!live.empty());

    // The engine gone: nothing is being recorded that the window can see.
    engine.alive = false;
    shell.tick();
    CHECK(live.empty());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (int r = unit()) return r;
    if (int r = drawing()) return r;
    if (int r = wiring()) return r;
    std::printf("live take: ok\n");
    return 0;
}
