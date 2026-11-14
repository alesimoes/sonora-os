// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0149: the nudge value's conversion, cycling, clamp at 0, tempo-aware
// musical values and FollowGrid being today's nudge.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "nudge_controller.h"

using namespace sonora;
using namespace sonora::ui;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
    model::SessionSettings s;
    s.sample_rate = 48000;
    s.tempo = {model::TempoEntry{}};

    CHECK(nudge_ms_frames(100, 48000) == 4800);
    CHECK(nudge_ms_frames(10, 48000) == 480);
    CHECK(nudge_ms_frames(1, 48000) == 48);
    CHECK(nudge_ms_frames(1, 100) == 1);  // 0.1 rounds to 0, floored to 1
    CHECK(nudge_value_target(10000, 1, NudgeValue::Ms10, GridValue::Bar, s) == 10480);
    CHECK(nudge_value_target(100, -1, NudgeValue::Ms10, GridValue::Bar, s) == 0);

    for (GridValue g : {GridValue::Bar, GridValue::Beat, GridValue::Sixteenth}) {
        CHECK(nudge_value_target(5000, 1, NudgeValue::FollowGrid, g, s) == nudge_target(5000, 1, g, s));
        CHECK(nudge_value_target(5000, -1, NudgeValue::FollowGrid, g, s) == nudge_target(5000, -1, g, s));
    }
    CHECK(nudge_value_target(5000, 1, NudgeValue::Beat, GridValue::Bar, s) ==
          nudge_target(5000, 1, GridValue::Beat, s));

    s.tempo.push_back(model::TempoEntry{});
    s.tempo[1].at = 96000;
    s.tempo[1].bpm = 60;
    CHECK(nudge_value_target(96000, 1, NudgeValue::Beat, GridValue::Bar, s) ==
          nudge_target(96000, 1, GridValue::Beat, s));

    NudgeController c;
    CHECK(c.value() == NudgeValue::FollowGrid);
    for (int i = 0; i < 7; ++i) c.advance();
    CHECK(c.value() == NudgeValue::Ms1);
    CHECK(std::string(c.label()) == "1 ms");
    c.advance();
    CHECK(c.value() == NudgeValue::FollowGrid);
    c.advance();
    c.reset();
    CHECK(c.value() == NudgeValue::FollowGrid);

    std::printf(failures ? "nudge_value: FAILED\n" : "nudge_value: ok\n");
    return failures ? 1 : 0;
}
