// SPDX-License-Identifier: GPL-3.0-or-later
// The ratio calculator: length, bars, tempo, percent, and the elastic limits.
#include <cstdio>

#include "sonora/ui_tce/tce_ratio.h"

using namespace sonora::ui::tce;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

int main() {
    CHECK(from_frames(48000, 72000).ok && from_frames(48000, 72000).ratio == 1.5);
    CHECK(from_seconds(48000, 48000, 0.5).ratio == 0.5);
    // Two bars of 4/4 at 120 bpm are 4 s; a 2 s source must double.
    CHECK(from_bars(96000, 48000, 120, 4, 2).ratio == 2.0);
    CHECK(from_tempo(100, 125).ratio == 0.8);  // faster: shorter
    CHECK(from_percent(150).ratio == 1.5);
    CHECK(resulting_length(48000, 1.5) == 72000);
    CHECK(from_percent(25).ok && from_percent(400).ok);
    CHECK(!from_percent(24).ok && !from_percent(401).ok);
    CHECK(!from_frames(48000, 300000).ok && !from_frames(48000, 1000).ok);
    CHECK(!from_tempo(60, 241).ok && !from_tempo(0, 100).ok);
    CHECK(!from_frames(0, 100).ok && !from_percent(0).ok && !from_bars(1, 48000, 0, 4, 1).ok);
    CHECK(!from_percent(500).why.empty());
    return failures == 0 ? 0 : 1;
}
