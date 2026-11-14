// SPDX-License-Identifier: GPL-3.0-or-later
// The render argument round-trips, a name keeps its spaces, ';' and '=', and
// what it cannot read is refused with a reason. insert=<id> (ADR-0087) is
// Process Selection's alone.
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

    // One insert: read for Process Selection, round-tripped with a name that
    // says insert= too, and refused for Freeze, Commit and the command-blind
    // reader an engine without track.render.insert uses.
    o.only_insert = 41;
    o.name = "Vox insert=2";
    const std::string with_insert = format_render_argument(o);
    CHECK(parse_render_argument(kCommandProcessSelection, with_insert, back, why));
    CHECK(back.only_insert == 41 && back.name == "Vox insert=2" && back.end == 96000);
    CHECK(parse_render_argument(kCommandProcessSelection, "track=3 start=0 end=9", back, why) &&
          back.only_insert == 0);
    CHECK(format_render_argument(back).find("insert=") == std::string::npos);
    for (const char* command : {"track.freeze", "track.commit", "track.unfreeze"}) {
        why.clear();
        CHECK(!parse_render_argument(command, "track=3 insert=41", back, why));
        CHECK(why.find("Process Selection") != std::string::npos);
    }
    why.clear();
    CHECK(!parse_render_argument(with_insert, back, why) && !why.empty());
    for (const char* bad : {"track=3 insert=0", "track=3 insert=x", "track=3 insert=-4"}) {
        why.clear();
        CHECK(!parse_render_argument(kCommandProcessSelection, bad, back, why) && !why.empty());
    }
    CHECK(std::string(kCapabilityRenderInsert) == "track.render.insert");
    return 0;
}
