// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the strips' meters read: post-fader peaks per side, the Pre/Post
// toggle and its latch, a non-finite peak, and one engine read per channel
// side per tick however many meters show it. The aux strip's NO DATA is in
// test_sends.cpp, where the aux strip exists.
#include <QGuiApplication>
#include <string>
#include <cmath>
#include <cstdio>
#include <limits>

#include "key_router.h"
#include "meter_item.h"
#include "recording_adapter.h"
#include "shell_model.h"

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
struct Rig {
    KeyMap map;
    test::RecordingAdapter engine{4};
    KeyRouter router{map, engine.info().implemented_commands,
                     [](const std::string&, const CommandContext&) { return 0ull; }};
    ShellModel shell{engine, router};
    MeterItem left, right, mono;
    Rig() {
        left.setChannel(0);
        left.setSide(0);
        right.setChannel(0);
        right.setSide(1);
        mono.setChannel(1);  // side -1: the louder side
        for (MeterItem* m : {&left, &right, &mono}) shell.attachMeter(m);
    }
};
}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    // Post peaks reach the strip, side by side; the mono meter shows the louder.
    {
        Rig r;
        r.shell.setMeterPoint(MeterPoint::Post);
        r.engine.post_peaks[0] = {0.5f, 0.05f};
        r.engine.post_peaks[1] = {0.1f, 0.4f};
        r.engine.peaks[0] = 0.9f;  // the input must not be what a post meter shows
        r.shell.tick();
        CHECK(r.left.ballistics().has_data && r.right.ballistics().has_data);
        CHECK(std::fabs(r.left.ballistics().bar_db - peak_to_dbfs(0.5f)) < 0.01f);
        CHECK(std::fabs(r.right.ballistics().bar_db - peak_to_dbfs(0.05f)) < 0.01f);
        CHECK(std::fabs(r.mono.ballistics().bar_db - peak_to_dbfs(0.4f)) < 0.01f);
    }

    // One read per channel side per tick, three meters or not; the input is
    // not read at all while the point is Post.
    {
        Rig r;
        r.shell.setMeterPoint(MeterPoint::Post);
        r.shell.tick();
        // ch0 sides 0 and 1 (shared by the left, right meters), ch1 both
        // sides for the mono meter.
        CHECK(r.engine.post_peak_reads == 4);
        CHECK(r.engine.peak_reads == 0);
        r.shell.tick();
        CHECK(r.engine.post_peak_reads == 8);
    }

    // The toggle switches the source, and the latch and the fall belong to
    // the point they were read at.
    {
        Rig r;
        r.shell.setMeterPoint(MeterPoint::Post);
        r.engine.post_peaks[0] = {1.0f, 0.1f};
        r.shell.tick();
        CHECK(r.left.ballistics().over);
        r.shell.setMeterPoint(MeterPoint::Pre);
        CHECK(!r.left.ballistics().over && !r.left.ballistics().has_data);
        r.engine.peaks[0] = 0.25f;
        r.engine.post_peaks[0] = {0.9f, 0.9f};
        r.shell.tick();
        CHECK(std::fabs(r.left.ballistics().bar_db - peak_to_dbfs(0.25f)) < 0.01f);
        CHECK(!r.left.ballistics().over);
        CHECK(r.engine.peak_reads == 2);  // ch0 once for both meters, ch1 once
        CHECK(r.engine.post_peak_reads == 4);  // the Post tick above, none since
        // Same point again: nothing is reset.
        const float bar = r.left.ballistics().bar_db;
        r.shell.setMeterPoint(MeterPoint::Pre);
        CHECK(r.left.ballistics().bar_db == bar);
    }

    // A peak past full scale latches a clip; one that is not a number does
    // not, and shows no level.
    {
        Rig r;
        r.shell.setMeterPoint(MeterPoint::Post);
        r.engine.post_peaks[0] = {std::numeric_limits<float>::infinity(), 0.0f};
        r.shell.tick();
        CHECK(r.left.ballistics().over);
        CHECK(!r.right.ballistics().over);
        r.engine.post_peaks[1] = {std::numeric_limits<float>::quiet_NaN(), 0.0f};
        r.shell.tick();
        CHECK(!r.mono.ballistics().over);
    }

    std::printf("ui strip metering: all checks passed\n");
    return 0;
}
