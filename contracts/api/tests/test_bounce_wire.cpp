// SPDX-License-Identifier: GPL-3.0-or-later
// The bounce argument round-trips, a name keeps its spaces, ';' and '=', and
// what it cannot read is refused with a reason.
#include <cstdio>
#include <limits>

#include "sonora/api/bounce.h"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

using namespace sonora::api;

int main() {
    BounceOptions o;
    o.mode = BounceMode::RealTime;
    o.start = 48000;
    o.end = 96000;
    o.format = BounceFormat::Float32;
    o.normalise = true;
    o.target_lufs = -14.0;
    o.name = "Mix; v=2 final";
    BounceOptions back;
    std::string why;
    CHECK(parse_bounce_argument(format_bounce_argument(o), back, why));
    CHECK(back.mode == BounceMode::RealTime && back.start == 48000 && back.end == 96000);
    CHECK(back.format == BounceFormat::Float32 && back.normalise);
    CHECK(back.target_lufs == -14.0 && back.name == "Mix; v=2 final");

    CHECK(parse_bounce_argument("", back, why));
    CHECK(!back.normalise && back.mode == BounceMode::Offline);  // off by default
    CHECK(parse_bounce_argument("end=9 start=3", back, why) && back.start == 3 && back.end == 9);

    for (const char* bad : {"mode=fast", "start=-1", "start=+1", "start=10 end=10",
                            "normalise=3", "normalise=nan", "normalise=-80", "colour=red",
                            "start=99999999999999999999", "format=mp3", "loose"}) {
        why.clear();
        CHECK(!parse_bounce_argument(bad, back, why) && !why.empty());
    }
    return 0;
}
