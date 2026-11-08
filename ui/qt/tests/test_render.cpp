// SPDX-License-Identifier: GPL-3.0-or-later
//
// A meter and the timeline rendered for real: a QQuickWindow on xcb under
// Xvfb, drawn by the scene graph's OpenGL path (llvmpipe here), read back
// with grabWindow(). The other UI tests never render, so without this
// nothing runs MeterItem's or SceneItem's updatePaintNode(). The pixels
// checked are theme.h's tokens; what a meter or a region shows is
// test_meter_model's and test_edit's job, and this only proves that it
// reaches the screen.
#include <QGuiApplication>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTest>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "control_adapter.h"
#include "key_router.h"
#include "meter_item.h"
#include "peak_source.h"
#include "scene_item.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "theme.h"
#include "timeline_document.h"

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

// meter_model.cpp's colours are these tokens.
constexpr QRgb kNoDataBackground = sonora::ui::theme::kMeterNoData;
constexpr QRgb kGreen = sonora::ui::theme::kMeterGreen;
constexpr QRgb kOverOn = sonora::ui::theme::kMeterOverOn;
constexpr QRgb kBlue = 0xff0000ff;  // not a token: a colour no token is

class FlatPeaks final : public PeakSource {
public:
    void peaks(model::Id, uint16_t, model::Frames, double, int count,
               PeakPair* out) const override {
        for (int i = 0; i < count; ++i) out[i] = {-0.5f, 0.5f};
    }
};

// Every sample at one level, so the sample view's line has one known y; or
// still loading, and no line at all (ADR-0096 §2).
class LevelSamples final : public SampleSource {
public:
    explicit LevelSamples(float level) : level_(level) {}
    void read(model::Id, uint16_t, model::Frames, int count, float* out) const override {
        for (int i = 0; i < count; ++i) out[i] = loaded ? level_ : 0.0f;
    }
    bool ready(model::Id) const override { return loaded; }
    bool loaded = true;

private:
    float level_;
};

// One track, one unnamed region of five seconds at the session start.
model::Snapshot one_region() {
    model::Snapshot s = make_empty_session(1, 48000);
    auto pool = std::make_shared<model::SourcePool>();
    model::Source src;
    src.id = 100;
    src.path = "Audio Files/take.wav";
    src.length = 48000 * 60;
    pool->sources.push_back(src);
    s.sources = pool;
    s.next_id = 101;  // every id above is taken
    auto edit = std::make_shared<model::EditState>(*s.edit);
    auto t = std::make_shared<model::TrackEdit>(*edit->tracks[0]);
    auto p = std::make_shared<model::Playlist>(*t->playlists[0]);
    model::Region r;
    r.id = 10;
    r.length = 48000 * 5;
    r.channels = {{100, 0}};
    p->regions = {r};
    t->playlists[0] = p;
    edit->tracks[0] = t;
    s.edit = edit;
    return s;
}

bool is(const QImage& img, int x, int y, QRgb want) {
    const QRgb got = img.pixel(x, y);
    const bool ok = std::abs(qRed(got) - qRed(want)) <= 2 &&
                    std::abs(qGreen(got) - qGreen(want)) <= 2 &&
                    std::abs(qBlue(got) - qBlue(want)) <= 2;
    if (!ok) std::fprintf(stderr, "  pixel (%d,%d) is %08x, want %08x\n", x, y, got, want);
    return ok;
}

// Counts the scene graph's calls, so a test can tell "redrawn" from "the
// old frame happens to look right".
class CountingMeter : public MeterItem {
public:
    using MeterItem::MeterItem;
    int paints = 0;

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* d) override {
        ++paints;
        return MeterItem::updatePaintNode(old, d);
    }
};

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    QQuickWindow window;
    window.setColor(QColor::fromRgb(kBlue));
    window.resize(600, 300);
    auto* meter = new CountingMeter(window.contentItem());
    meter->setPosition(QPointF(0, 0));
    meter->setSize(QSizeF(40, 100));
    window.show();
    CHECK(QTest::qWaitForWindowExposed(&window));
    // The software adaptation never builds the meter's geometry node, by
    // design; a pass there would prove nothing.
    CHECK(window.rendererInterface()->graphicsApi() != QSGRendererInterface::Software);
    std::printf("ui render: graphics API %d, platform %s\n",
                static_cast<int>(window.rendererInterface()->graphicsApi()),
                qPrintable(QGuiApplication::platformName()));

    // Full scale: the over band lit at the top, green at the bottom of the
    // bar, and nothing drawn outside the item.
    meter->feed(1.0f, 0.0f);
    QImage img = window.grabWindow();
    CHECK(!img.isNull());
    CHECK(img.width() == 600 && img.height() == 300);
    CHECK(is(img, 5, 1, kOverOn));
    CHECK(is(img, 5, 97, kGreen));
    CHECK(is(img, 5, 150, kBlue));
    CHECK(is(img, 70, 50, kBlue));

    // The engine goes away: the no-data face, and the over still latched.
    meter->setNoData();
    img = window.grabWindow();
    CHECK(is(img, 5, 50, kNoDataBackground));
    CHECK(is(img, 5, 1, kOverOn));

    // Resized while the engine is dead, so no feed() asks for a redraw: the
    // meter must still be drawn at its new size.
    const int paints = meter->paints;
    meter->setSize(QSizeF(40, 200));
    img = window.grabWindow();
    std::printf("ui render: updatePaintNode calls %d before the resize, %d after\n", paints,
                meter->paints);
    CHECK(meter->paints > paints);
    CHECK(is(img, 5, 150, kNoDataBackground));
    CHECK(is(img, 5, 250, kBlue));

    // --- the timeline: a region, its waveform, the cursor and the playhead ---
    ModelDocument doc(one_region());
    FlatPeaks peaks;
    SessionModel session(doc, peaks, false);
    auto* timeline = new TimelineItem(window.contentItem());
    timeline->setPosition(QPointF(100, 0));
    timeline->setSize(QSizeF(400, 100));
    timeline->setSession(&session);
    CHECK(session.controller().view().width == 400);
    img = window.grabWindow();
    using namespace sonora::ui::theme;
    const uint32_t palette = region_colour(0);
    // 40 px a second: the region covers x 100..300 of the window.
    CHECK(is(img, 150, 8, region_name_band(palette)));      // the name band
    CHECK(is(img, 150, 59, region_body(palette)));          // the body, under the wave
    CHECK(is(img, 150, 39, region_wave(palette)));          // the waveform's middle
    CHECK(is(img, 250, 39, region_wave(palette)));
    CHECK(is(img, 350, 30, kLaneA));                        // the lane past the region
    CHECK(is(img, 150, 80, kBlue));                       // below the only lane
    CHECK(is(img, 100, 90, kAccent));                       // the insertion cursor at 0

    // Recording: the playhead in the record colour, 2.5 s in.
    TransportSnapshot t;
    t.state = TransportMode::Recording;
    t.playhead_frames = 48000 * 5 / 2;
    session.onTransport(t, true);
    img = window.grabWindow();
    CHECK(is(img, 200, 90, kRecord));
    CHECK(is(img, 100, 90, kGridBeat));                   // the cursor gives way to the bar line

    // --- more rectangles than one geometry node can hold ---
    // A dozen tracks of regions draw about 14000 rectangles, past the 65535
    // vertices a node is drawn up to; the last track's waveform must still
    // reach the screen, as the first one's does.
    {
        QQuickWindow big;
        big.setColor(QColor::fromRgb(kBlue));
        big.resize(1000, 800);
        ModelDocument many(make_demo_session(12, 48000));
        FlatPeaks flat;
        SessionModel crowded(many, flat, false);
        auto* tall = new TimelineItem(big.contentItem());
        tall->setPosition(QPointF(0, 0));
        tall->setSize(QSizeF(1000, 800));
        tall->setSession(&crowded);
        big.show();
        CHECK(QTest::qWaitForWindowExposed(&big));
        const QImage shot = big.grabWindow();
        const double row = crowded.controller().view().track_height;
        // Mid-waveform of a track's first region: its lane + inset + name
        // band + the middle of the area under it.
        const auto wave_y = [&](int track) { return static_cast<int>(track * row + 39); };
        CHECK(is(shot, 300, wave_y(0), region_wave(region_colour(0))));
        // Track 12 wears the colour it was given when it was added.
        CHECK(is(shot, 300, wave_y(11), region_wave(region_colour(model::auto_colour(11)))));
    }

    // --- the sample view (ADR-0096): the samples, not the overview ---
    {
        QQuickWindow w;
        w.setColor(QColor::fromRgb(kBlue));
        w.resize(400, 100);
        ModelDocument d(one_region());
        FlatPeaks flat;  // -0.5..0.5: the overview would fill the lane's middle
        LevelSamples half(0.5f);
        SessionModel s(d, flat, false);
        s.controller().set_sample_source(&half);
        auto* tl = new TimelineItem(w.contentItem());
        tl->setPosition(QPointF(0, 0));
        tl->setSize(QSizeF(400, 100));
        tl->setSession(&s);
        s.controller().zoom_by(1.0 / s.controller().view().fpp, 0);  // a frame a pixel
        s.setViewport(400, 100);
        CHECK(s.sampleView());
        w.show();
        CHECK(QTest::qWaitForWindowExposed(&w));
        const SampleLane lane = sample_lane(0, s.controller().view().track_height, 1, 0);
        const int line_y = static_cast<int>(std::floor(lane.mid() - 0.5 * lane.height / 2));
        const int under = static_cast<int>(std::floor(lane.mid() + lane.height / 8));
        QImage shot = w.grabWindow();
        CHECK(is(shot, 200, line_y, region_wave(palette)));  // the samples' line at 0.5
        CHECK(is(shot, 200, under, region_body(palette)));   // no overview below it
        // Still loading: no line drawn over zeros that are not the audio.
        half.loaded = false;
        s.peaksReady();
        shot = w.grabWindow();
        CHECK(is(shot, 200, line_y, region_body(palette)));
        // Coarser than 4 frames a pixel: the overview again.
        half.loaded = true;
        s.controller().zoom_by(8.0, 0);
        s.setViewport(400, 100);
        CHECK(!s.sampleView());
        shot = w.grabWindow();
        CHECK(is(shot, 200, under, region_wave(palette)));
    }

    // --- a refused sample.edit is a held notice (ADR-0052 §5, ADR-0096 §5) ---
    // The panel's command and a Pencil stroke both end in sample.edit, which
    // FakeEngine refuses (it has no audio files): each refusal is held under
    // the window until dismissed, not left to the status line that clears.
    {
        sonora::api::testing::FakeEngine::Config cfg;
        cfg.tracks = 1;
        sonora::api::testing::FakeEngine fake(cfg);
        ControlAdapter engine(fake);
        std::string err;
        CHECK(engine.connect(err));
        sonora::KeyMap map;
        ModelDocument d(one_region());
        FlatPeaks flat;
        LevelSamples half(0.5f);
        SessionModel s(d, flat, false);
        s.controller().set_sample_source(&half);
        KeyRouter router(map, engine.info().implemented_commands,
                         [&](const std::string& c, const sonora::CommandContext& ctx) {
                             return engine.send(c, ctx);
                         });
        s.installCommands(router.local());
        ShellModel shell(engine, router);
        s.setCommandSender([&shell](const QString& command, const QString& argument) {
            shell.invokeWith(command, -1, argument);
        });
        shell.tick();
        const auto base = shell.notices().size();
        const auto answered = [&](qsizetype want) {
            // The fake answers on a following tick; a bounded number of them.
            for (int i = 0; i < 20 && shell.notices().size() < want; ++i) shell.tick();
            return shell.notices().size() == want;
        };
        auto& ctl = s.controller();
        ctl.set_viewport(400, 100);
        ctl.zoom_by(1.0 / ctl.view().fpp, 0);
        // A range across the clip's end: nothing for a sample edit to change.
        ctl.select_range(0, 48000 * 5 - 100, 48000 * 5 + 100);
        CHECK(!s.sampleEditable());
        ctl.select_range(0, 100, 300);
        CHECK(s.sampleEditable());
        shell.invoke(QStringLiteral("sample.reverse"), -1);
        CHECK(answered(base + 1));
        CHECK(shell.notices().last().startsWith(QStringLiteral("Sample edit refused: ")));
        // The Pencil's stroke: one sample.edit draw, refused and held the same way.
        ctl.clear_selection();
        ctl.set_tool(Tool::Pencil);
        const SampleLane lane = sample_lane(0, ctl.view().track_height, 1, 0);
        s.press(100, lane.mid());
        s.drag(120, lane.mid() - lane.height / 4);
        s.release(120, lane.mid() - lane.height / 4);
        CHECK(answered(base + 2));
        CHECK(shell.notices().last().startsWith(QStringLiteral("Sample edit refused: ")));
        std::printf("ui render: held: %s\n", qPrintable(shell.notices().last()));
    }

    std::printf("ui render: all checks passed\n");
    return 0;
}
