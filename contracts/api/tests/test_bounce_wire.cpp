// SPDX-License-Identifier: GPL-3.0-or-later
// The bounce argument round-trips, a name keeps its ';' and '=', and what it
// cannot read is refused with a reason.
#include <cstdio>

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
    o.name = "Mix; v=2";
    BounceOptions back;
    std::string why;
    CHECK(parse_bounce_argument(format_bounce_argument(o), back, why));
    CHECK(back.mode == BounceMode::RealTime && back.start == 48000 && back.end == 96000);
    CHECK(back.format == BounceFormat::Float32 && back.normalise);
    CHECK(back.target_lufs == -14.0 && back.name == "Mix; v=2");

    CHECK(parse_bounce_argument("", back, why));
    CHECK(!back.normalise && back.mode == BounceMode::Offline);  // off by default
    CHECK(!parse_bounce_argument("mode=fast", back, why) && !why.empty());
    CHECK(!parse_bounce_argument("start=-1", back, why));
    CHECK(!parse_bounce_argument("start=10;end=10", back, why));
    CHECK(!parse_bounce_argument("normalise=3", back, why));
    CHECK(!parse_bounce_argument("colour=red", back, why));
    return 0;
}
