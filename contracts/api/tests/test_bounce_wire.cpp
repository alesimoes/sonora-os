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

    // Protocol 61: the file, depth, rate and dither.
    BounceOptions e;
    e.file = BounceFile::Flac;
    e.format = BounceFormat::Int16;
    e.sample_rate = 44100;
    e.dither = BounceDither::Shaped;
    e.name = "For CD rate=1";
    CHECK(parse_bounce_argument(format_bounce_argument(e), back, why));
    CHECK(back.file == BounceFile::Flac && back.format == BounceFormat::Int16);
    CHECK(back.sample_rate == 44100 && back.dither == BounceDither::Shaped);
    CHECK(back.name == "For CD rate=1");
    CHECK(parse_bounce_argument("", back, why));
    CHECK(back.file == BounceFile::Bwf && back.sample_rate == 0 && back.dither == BounceDither::Auto);
    for (const char* bad : {"file=mp3", "rate=100", "rate=500000", "rate=x", "dither=loud",
                            "file=flac format=float32", "format=float32 file=aiff"}) {
        why.clear();
        CHECK(!parse_bounce_argument(bad, back, why) && !why.empty());
    }
    CHECK(parse_bounce_argument("file=wav format=float32 rate=96000", back, why));

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
