// SPDX-License-Identifier: GPL-3.0-or-later
// The pitch note list (ADR-0143) round-trips in its shortest form, reads the
// optional fields, keeps gain to 0.01 dB whatever the locale, and refuses
// every malformed or out-of-range list with a reason.
#include <clocale>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "sonora/api/pitch.h"
#include "sonora/api/wire.h"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

using namespace sonora::api;

namespace {

PitchNote note(uint64_t start, uint64_t end, int32_t cents, int32_t formant = 0,
               double gain = 0.0) {
    PitchNote n;
    n.start = start;
    n.end = end;
    n.cents = cents;
    n.formant_cents = formant;
    n.gain_db = gain;
    return n;
}

bool same(const PitchNote& a, const PitchNote& b) {
    return a.start == b.start && a.end == b.end && a.cents == b.cents &&
           a.formant_cents == b.formant_cents && std::fabs(a.gain_db - b.gain_db) < 1e-9;
}

int round_trip() {
    PitchEditSpec spec;
    spec.notes = {note(0, 4800, 100), note(4800, 9600, -250, 30),
                  note(12000, 24000, 2400, -2400, -3.5), note(24000, 24001, -2400, 0, 12.0),
                  note(30000, 31000, 0, 0, -0.05)};
    const std::string text = format_pitch_param(spec);
    CHECK(text ==
          "n=0:4800:100;n=4800:9600:-250:30;n=12000:24000:2400:-2400:-3.5;"
          "n=24000:24001:-2400:0:12;n=30000:31000:0:0:-0.05");
    PitchEditSpec back;
    std::string why;
    CHECK(parse_pitch_param(text, back, why));
    CHECK(back.notes.size() == spec.notes.size());
    for (std::size_t i = 0; i < spec.notes.size(); ++i) CHECK(same(back.notes[i], spec.notes[i]));
    CHECK(format_pitch_param(back) == text);
    return 0;
}

int optional_fields_and_signs() {
    PitchEditSpec spec;
    std::string why;
    CHECK(parse_pitch_param("n=10:20:+50:-0:+1.255;n=20:30:7:0:0.004", spec, why));
    CHECK(spec.notes.size() == 2);
    CHECK(spec.notes[0].cents == 50 && spec.notes[0].formant_cents == 0);
    CHECK(std::fabs(spec.notes[0].gain_db - 1.26) < 1e-9);
    CHECK(spec.notes[1].gain_db == 0.0);
    CHECK(format_pitch_param(spec) == "n=10:20:50:0:1.26;n=20:30:7");
    return 0;
}

int max_notes_fit_the_wire() {
    PitchEditSpec spec;
    for (std::size_t i = 0; i < kMaxPitchNotes; ++i) {
        const uint64_t base = kPitchMaxFrame - 2 * kMaxPitchNotes + 2 * i;
        spec.notes.push_back(note(base, base + 1, -2400, -2400, -47.99));
    }
    std::string why;
    CHECK(validate_pitch_spec(spec, why));
    const std::string text = format_pitch_param(spec);
    CHECK(text.size() <= kMaxPitchParamChars);
    CHECK(kMaxPitchParamChars < wire::kMaxText);
    PitchEditSpec back;
    CHECK(parse_pitch_param(text, back, why));
    CHECK(back.notes.size() == kMaxPitchNotes);
    return 0;
}

int locale_does_not_change_the_wire() {
    // A decimal-comma locale, when the image has one; the test holds either way.
    if (std::setlocale(LC_ALL, "de_DE.UTF-8") == nullptr) std::setlocale(LC_ALL, "C");
    PitchEditSpec spec;
    spec.notes = {note(0, 10, 0, 0, -6.25)};
    const std::string text = format_pitch_param(spec);
    PitchEditSpec back;
    std::string why;
    const bool ok = parse_pitch_param(text, back, why);
    std::setlocale(LC_ALL, "C");
    CHECK(text == "n=0:10:0:0:-6.25");
    CHECK(ok && std::fabs(back.notes[0].gain_db + 6.25) < 1e-9);
    return 0;
}

// Parses `text`, expects a refusal whose reason holds `reason`, and the spec
// left as it was.
int refused(const std::string& text, const std::string& reason) {
    PitchEditSpec spec;
    spec.notes = {note(1, 2, 3)};
    std::string why;
    if (parse_pitch_param(text, spec, why)) {
        std::fprintf(stderr, "accepted: %s\n", text.c_str());
        return 1;
    }
    if (why.find(reason) == std::string::npos) {
        std::fprintf(stderr, "'%s': reason '%s' lacks '%s'\n", text.c_str(), why.c_str(),
                     reason.c_str());
        return 1;
    }
    CHECK(spec.notes.size() == 1 && spec.notes[0].start == 1);
    return 0;
}

int every_refusal_has_its_reason() {
    int bad = 0;
    bad |= refused("", "no notes");
    bad |= refused("n=0:10:0;", "note 2 does not start with n=");
    bad |= refused("x=0:10:0", "note 1 does not start with n=");
    bad |= refused("n=0:10", "needs START:END:CENTS");
    bad |= refused("n=0:10:0:0:0:0", "needs START:END:CENTS");
    bad |= refused("n=-1:10:0", "start '-1' is not a frame number");
    bad |= refused("n=0:1e3:0", "end '1e3' is not a frame number");
    bad |= refused("n=0:281474976710657:0", "is not a frame number");
    bad |= refused("n=0:10:1.5", "shift '1.5' is not a whole number");
    bad |= refused("n=0:10:", "shift '' is not a whole number");
    bad |= refused("n=0:10:0:x", "formant 'x' is not a whole number");
    bad |= refused("n=0:10:0:0:1,5", "gain '1,5' is not a number of dB");
    bad |= refused("n=0:10:0:0:.5", "gain '.5' is not a number of dB");
    bad |= refused("n=0:10:0:0:1.", "gain '1.' is not a number of dB");
    bad |= refused("n=0:10:0:0:1000", "gain '1000' is not a number of dB");
    bad |= refused("n=10:10:0", "note 1 is empty");
    bad |= refused("n=20:10:0", "note 1 is empty");
    bad |= refused("n=0:10:0;n=0:20:0", "note 2 does not start after");
    bad |= refused("n=0:10:0;n=5:20:0", "note 2 overlaps");
    bad |= refused("n=10:20:0;n=0:5:0", "note 2 does not start after");
    bad |= refused("n=0:10:2401", "outside -2400..2400");
    bad |= refused("n=0:10:-2401", "outside -2400..2400");
    bad |= refused("n=0:10:0:2401", "formant 2401 cents is outside");
    bad |= refused("n=0:10:0:0:12.01", "gain is outside -48..12 dB");
    bad |= refused("n=0:10:0:0:-48.01", "gain is outside -48..12 dB");
    bad |= refused("n=0:10:0 n=10:20:0", "is not a whole number");
    bad |= refused(std::string(kMaxPitchParamChars + 1, 'n'), "longer than");
    std::string many;
    for (std::size_t i = 0; i <= kMaxPitchNotes; ++i) {
        if (i != 0) many += ';';
        many += "n=" + std::to_string(i * 2) + ':' + std::to_string(i * 2 + 1) + ":0";
    }
    bad |= refused(many, "65 notes, at most 64");
    return bad;
}

int validate_and_fit() {
    std::string why;
    PitchEditSpec spec;
    CHECK(!validate_pitch_spec(spec, why) && why.find("no notes") != std::string::npos);
    spec.notes = {note(0, 10, 0, 0, std::numeric_limits<double>::quiet_NaN())};
    CHECK(!validate_pitch_spec(spec, why) && why.find("gain is outside") != std::string::npos);
    // A bad gain never formats into a PARAM parse accepts.
    PitchEditSpec back;
    CHECK(!parse_pitch_param(format_pitch_param(spec), back, why));
    spec.notes = {note(0, 10, 0, 0, 1e300)};
    CHECK(!parse_pitch_param(format_pitch_param(spec), back, why));
    spec.notes = {note(0, kPitchMaxFrame + 1, 0)};
    CHECK(!validate_pitch_spec(spec, why) && why.find("past the last frame") != std::string::npos);

    spec.notes = {note(0, 100, 0), note(100, 480, 0)};
    CHECK(pitch_notes_fit(spec, 480, why));
    CHECK(!pitch_notes_fit(spec, 479, why));
    CHECK(why == "pitch: note 2 ends past the region (480 > 479 frames)");
    CHECK(std::string(kCapabilityPitch) == "pitch" && std::string(kPitchVerb) == "pitch");
    return 0;
}

}  // namespace

int main() {
    if (round_trip() != 0) return 1;
    if (optional_fields_and_signs() != 0) return 1;
    if (max_notes_fit_the_wire() != 0) return 1;
    if (locale_does_not_change_the_wire() != 0) return 1;
    if (every_refusal_has_its_reason() != 0) return 1;
    if (validate_and_fit() != 0) return 1;
    std::printf("pitch param: ok\n");
    return 0;
}
