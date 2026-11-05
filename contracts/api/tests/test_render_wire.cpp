// SPDX-License-Identifier: GPL-3.0-or-later
// The render argument round-trips, a name keeps its spaces, ';' and '=', and
// what it cannot read is refused with a reason.
#include <cstdio>

#include "sonora/api/render.h"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

using namespace sonora::api;

int main() {
    RenderOptions o;
    o.track = 12;
    o.start = 48000;
    o.end = 96000;
    o.tail_ms = 1500;
    o.name = "Vox; v=2 final";
    RenderOptions back;
    std::string why;
    CHECK(parse_render_argument(format_render_argument(o), back, why));
    CHECK(back.track == 12 && back.start == 48000 && back.end == 96000);
    CHECK(back.tail_ms == 1500 && back.name == "Vox; v=2 final");

    // The tail is the command's own unless asked for; 0 is a real answer.
    CHECK(parse_render_argument("track=3", back, why) && back.tail_ms == -1 && back.end == 0);
    CHECK(parse_render_argument("tail=0 track=3", back, why) && back.tail_ms == 0);
    CHECK(parse_render_argument("end=9 start=3 track=3", back, why) && back.start == 3 &&
          back.end == 9);

    for (const char* bad : {"", "start=1 end=5", "track=0", "track=x", "track=-1", "track=+1",
                            "track=3 start=10 end=10", "track=3 tail=60001", "track=3 colour=red",
                            "track=99999999999999999999", "loose"}) {
        why.clear();
        CHECK(!parse_render_argument(bad, back, why) && !why.empty());
    }
    return 0;
}
