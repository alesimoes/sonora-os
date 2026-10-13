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
#include <cstdio>
#include <cstdlib>

#include "meter_item.h"
#include "peak_source.h"
#include "scene_item.h"
#include "session_model.h"
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

    std::printf("ui render: all checks passed\n");
    return 0;
}
