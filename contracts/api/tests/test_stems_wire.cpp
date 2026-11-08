// SPDX-License-Identifier: GPL-3.0-or-later
// The stems argument round-trips over the bounce argument, a name keeps its
// spaces and '=', an empty track list means every track, and each thing it
// cannot accept is refused with a reason.
#include <cstdio>
#include <string>

#include "sonora/api/stems.h"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

using namespace sonora::api;

namespace {

int round_trip() {
    StemsOptions o;
    o.by = StemsBy::Track;
    o.tracks = {7, 3, 12};
    o.base.start = 48000;
    o.base.end = 96000;
    o.base.file = BounceFile::Flac;
    o.base.format = BounceFormat::Int16;
    o.base.sample_rate = 44100;
    o.base.dither = BounceDither::Shaped;
    o.base.name = "Final mix by=output tracks=1 v=2";
    StemsOptions back;
    std::string why;
    CHECK(parse_stems_argument(format_stems_argument(o), back, why));
    CHECK(back.by == StemsBy::Track && back.tracks == o.tracks);
    CHECK(back.base.mode == BounceMode::Offline);
    CHECK(back.base.start == 48000 && back.base.end == 96000);
    CHECK(back.base.file == BounceFile::Flac && back.base.format == BounceFormat::Int16);
    CHECK(back.base.sample_rate == 44100 && back.base.dither == BounceDither::Shaped);
    CHECK(back.base.name == "Final mix by=output tracks=1 v=2");

    StemsOptions out;
    out.by = StemsBy::Output;
    out.base.normalise = true;
    out.base.target_lufs = -14.0;
    CHECK(parse_stems_argument(format_stems_argument(out), back, why));
    CHECK(back.by == StemsBy::Output && back.tracks.empty());
    CHECK(back.base.normalise && back.base.target_lufs == -14.0 && back.base.name.empty());
    return 0;
}

int defaults_and_order() {
    StemsOptions back;
    std::string why;
    // Nothing at all: by track, every track, the bounce defaults.
    CHECK(parse_stems_argument("", back, why));
    CHECK(back.by == StemsBy::Track && back.tracks.empty());
    CHECK(back.base.file == BounceFile::Bwf && back.base.mode == BounceMode::Offline);
    // An empty tracks= is every track, like an absent one.
    CHECK(parse_stems_argument("tracks= by=track", back, why));
    CHECK(back.tracks.empty());
    // Our words may sit anywhere before the name, among the bounce's.
    CHECK(parse_stems_argument("file=wav tracks=2,1 format=float32 by=track name=A  B", back, why));
    CHECK(back.base.file == BounceFile::Wav && back.base.format == BounceFormat::Float32);
    CHECK(back.tracks.size() == 2 && back.tracks[0] == 2 && back.tracks[1] == 1);
    CHECK(back.base.name == "A  B");
    // After name= nothing is a word: by= and tracks= there are the name's.
    CHECK(parse_stems_argument("by=output name=x by=track tracks=9", back, why));
    CHECK(back.by == StemsBy::Output && back.tracks.empty());
    CHECK(back.base.name == "x by=track tracks=9");
    CHECK(parse_stems_argument("mode=offline", back, why));
    return 0;
}

int refusals() {
    const char* const bad[] = {
        "by=bus",                       // unknown grouping
        "by=",                          // empty grouping
        "by=track by=output",           // repeated
        "tracks=1 tracks=2",            // repeated
        "tracks=1,,2",                  // empty item
        "tracks=1,",                    // trailing comma
        "tracks=0",                     // no track 0
        "tracks=-1",                    // not a number
        "tracks=+1",
        "tracks=a",
        "tracks=1234567890123456789",   // past 18 digits
        "tracks=4,5,4",                 // a track twice
        "by=output tracks=1",           // tracks= is for by=track
        "mode=realtime",                // stems are offline only
        "mode=realtime by=track",
        "mode=live",                    // the bounce parser's own refusals
        "start=10 end=10",
        "file=flac format=float32",
        "loud=yes",
        "stray",
    };
    for (const char* arg : bad) {
        StemsOptions back;
        back.by = StemsBy::Output;
        std::string why;
        if (parse_stems_argument(arg, back, why) || why.empty()) {
            std::fprintf(stderr, "FAIL: \"%s\" was accepted or gave no reason\n", arg);
            return 1;
        }
        // A refusal leaves the caller's options alone.
        CHECK(back.by == StemsBy::Output);
    }
    std::string why;
    StemsOptions back;
    CHECK(!parse_stems_argument("mode=realtime", back, why));
    CHECK(why.find("offline") != std::string::npos);
    // A refusal from the bounce parser names the command that was sent.
    CHECK(!parse_stems_argument("loud=yes", back, why));
    CHECK(why.find("mix.stems:") == 0);
    return 0;
}

int event_layout() {
    CHECK(std::string(kCapabilityStems) == "mix.stems");
    CHECK(std::string(kCapabilityStems) != kCapabilityBounce);
    CHECK(stems_folder("Mix") == "Bounced Files/Mix Stems");
    CHECK(stems_folder("A = B") == "Bounced Files/A = B Stems");
    return 0;
}

}  // namespace

int main() {
    if (round_trip() != 0) return 1;
    if (defaults_and_order() != 0) return 1;
    if (refusals() != 0) return 1;
    if (event_layout() != 0) return 1;
    std::printf("stems_wire: ok\n");
    return 0;
}
