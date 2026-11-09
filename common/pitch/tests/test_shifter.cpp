// SPDX-License-Identifier: GPL-3.0-or-later
//
// PitchShifter and its fake: the call is recorded, the length is kept, and a
// spec that does not fit is refused.
#include <cmath>

#include "sonora/pitch/shifter.h"
#include "synth.h"

using namespace sonora::pitch;
namespace api = sonora::api;

namespace {

api::PitchEditSpec one_note(uint64_t start, uint64_t end, double gain_db) {
    api::PitchNote n;
    n.start = start;
    n.end = end;
    n.cents = 100;
    n.gain_db = gain_db;
    return api::PitchEditSpec{{n}};
}

int test_records_and_keeps_length() {
    FakePitchShifter fake;
    PitchShifter& shifter = fake;  // used through the interface
    const Planar in(2, std::vector<float>(1000, 0.5f));
    Planar out;
    std::string why;
    CHECK(shifter.shift(in, 48000, one_note(100, 200, -6.0206), out, why));
    CHECK(out.size() == 2 && out[0].size() == 1000 && out[1].size() == 1000);
    CHECK(out[0][50] == 0.5f && out[0][250] == 0.5f);
    CHECK(std::fabs(out[1][150] - 0.25f) < 1e-3f);  // -6 dB inside the note
    CHECK(fake.calls().size() == 1);
    CHECK(fake.calls()[0].channels == 2 && fake.calls()[0].frames == 1000);
    CHECK(fake.calls()[0].rate == 48000);
    CHECK(fake.calls()[0].spec.notes.size() == 1 && fake.calls()[0].spec.notes[0].cents == 100);
    return 0;
}

int test_refusals() {
    FakePitchShifter fake;
    const Planar in(1, std::vector<float>(1000, 0.5f));
    Planar out(1, std::vector<float>(3, 9.0f));
    std::string why;
    CHECK(!fake.shift(in, 48000, one_note(100, 1001, 0.0), out, why));  // past the end
    CHECK(why.find("ends past the region") != std::string::npos);
    CHECK(!fake.shift(in, 48000, api::PitchEditSpec{}, out, why));      // no notes
    Planar ragged{std::vector<float>(10), std::vector<float>(11)};
    CHECK(!fake.shift(ragged, 48000, one_note(0, 5, 0.0), out, why));
    CHECK(!fake.shift(Planar{}, 48000, one_note(0, 5, 0.0), out, why));
    CHECK(out.size() == 1 && out[0].size() == 3);  // untouched on refusal
    CHECK(fake.calls().empty());                   // refusals are not recorded as renders
    fake.refuse_with("pitch: the shifter is not built in");
    CHECK(!fake.shift(in, 48000, one_note(0, 5, 0.0), out, why));
    CHECK(why == "pitch: the shifter is not built in");
    return 0;
}

}  // namespace

int main() {
    if (int r = test_records_and_keeps_length()) return r;
    if (int r = test_refusals()) return r;
    std::puts("pitch_shifter ok");
    return 0;
}
