// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tone-match contract (ADR-0072): a capture profile round-trips
// through its file, the parser refuses a malformed one, a stale profile
// is refused with a message naming it, the distance is level-invariant,
// and the fake matcher answers what it was set up with.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/analyze/tone_fakes.h"
#include "sonora/analyze/tone_match.h"
#include "sonora/analyze/tone_profile.h"

using namespace sonora::analyze;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

const std::string kCardSha = "fc92072f1b9b19552ce3b74a9c8ce0cecb97633a12ae524ac0d93c05800e354e";
const std::string kIrSha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::string replace(std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

void test_round_trip() {
    // Quarter-dB steps: exact in float, so the round trip is exact.
    CaptureProfile p = fake_capture_profile("crunch-head", kCardSha, 0.25);
    p.ir_id = "closed-back-2x12";
    p.ir_card_sha256 = kIrSha;
    std::string text, error;
    check(profile_to_json(p, text, error), "a complete profile is written");
    CaptureProfile back;
    check(parse_profile(text, back, error), "the written profile parses back");
    check(back.id == p.id && back.card_sha256 == p.card_sha256 && back.ir_id == p.ir_id &&
              back.ir_card_sha256 == p.ir_card_sha256 && back.stimulus == p.stimulus,
          "round trip keeps the stamps");
    bool same = back.profile.crest_db == p.profile.crest_db && back.profile.flatness == p.profile.flatness;
    for (int b = 0; b < kToneBands; ++b) same = same && back.profile.bands_db[b] == p.profile.bands_db[b];
    check(same, "round trip keeps the bands, crest and flatness");
    check(back.amp_settings.empty(), "amp settings are not in the file");
    std::string again;
    check(profile_to_json(back, again, error) && again == text, "the file is canonical");

    ToneProfile hole = p.profile;
    hole.valid[30] = false;
    CaptureProfile incomplete = p;
    incomplete.profile = hole;
    check(!complete(hole), "a profile with an invalid band is not complete");
    check(!profile_to_json(incomplete, text, error), "an incomplete profile is not written");
}

void test_parse_refusals() {
    std::string good, error;
    // Tilt 0.25: band 0 is -3.75, which the edits below anchor on.
    check(profile_to_json(fake_capture_profile("crunch-head", kCardSha, 0.25), good, error), "fixture written");
    CaptureProfile out;
    const struct {
        std::string text;
        const char* what;
    } bad[] = {
        {"[]", "not an object"},
        {replace(good, "\"crunch-head\"", "\"Crunch Head\""), "id outside [a-z0-9-]+"},
        {replace(good, kCardSha, "abc"), "short card stamp"},
        {replace(good, "\"ir_card_sha256\": \"\"", "\"ir_card_sha256\": \"" + kIrSha + "\""),
         "IR stamp without an IR id"},
        {replace(good, "\"crest\"", "\"artist\": \"x\",\n  \"crest\""), "unknown key"},
        {replace(good, "\"flatness\": 0.25", "\"flatness\": 1.5"), "flatness outside [0, 1]"},
        {replace(good, "\"crest\": 12", "\"crest\": null"), "crest not a number"},
        {replace(good, "[-3.75, ", "["), "30 bands"},
        {replace(good, "[-3.75, ", "[null, "), "a null band"},
    };
    for (const auto& b : bad) {
        check(b.text != good, b.what);  // the fixture edit applied
        check(!parse_profile(b.text, out, error) && !error.empty(), b.what);
    }
}

void test_stale_refused() {
    std::string error;
    const CaptureProfile fresh = fake_capture_profile("crunch-head", kCardSha);
    check(check_fresh(fresh, kCardSha, "", error), "a fresh profile passes");

    const CaptureProfile stale = fake_stale_capture_profile("crunch-head", kCardSha);
    error.clear();
    check(!check_fresh(stale, kCardSha, "", error), "a changed card makes the profile stale");
    check(error.find("crunch-head") != std::string::npos && error.find("stale") != std::string::npos,
          "the refusal names the capture and says stale");

    CaptureProfile with_ir = fresh;
    with_ir.ir_id = "closed-back-2x12";
    with_ir.ir_card_sha256 = kIrSha;
    error.clear();
    check(!check_fresh(with_ir, kCardSha, kCardSha, error), "a changed IR card makes the profile stale");

    CaptureProfile old_stimulus = fresh;
    old_stimulus.stimulus = "sonora-tone-stimulus-v0";
    error.clear();
    check(!check_fresh(old_stimulus, kCardSha, "", error) && error.find("stimulus") != std::string::npos,
          "another stimulus is stale");
}

void test_distance() {
    const ToneProfile a = fake_tone_profile(0.5);
    ToneProfile louder = a;
    for (double& d : louder.bands_db) d += 7.0;
    check(std::fabs(tone_distance_db(a, louder)) < 1e-12, "overall level is not tone");

    // +3 dB on every core band only: the weighted mean removes part of it,
    // the rest is the distance; the far bands weigh a quarter.
    ToneProfile bump = a;
    for (int b = 0; b < kToneBands; ++b)
        if (tone_band_weight(b) == 1.0) bump.bands_db[b] += 3.0;
    const double d = tone_distance_db(a, bump);
    // 20 core bands at weight 1, 11 at 0.25: mean 60/22.75; RMS of the rest.
    const double mean = 60.0 / 22.75;
    const double expect = std::sqrt((20.0 * (3.0 - mean) * (3.0 - mean) + 2.75 * mean * mean) / 22.75);
    check(std::fabs(d - expect) < 1e-9, "weighted distance as ADR-0072 §4 states it");
    check(tone_valid_core_bands(a) == 20, "20 core bands, 80 Hz to 6.3 kHz");

    ToneProfile none = a;
    none.valid.fill(false);
    check(tone_distance_db(a, none) < 0.0, "no common band: negative distance");
    check(tone_params_valid(ToneMatchParams{}), "the default params are valid");
}

void test_fake_matcher() {
    FakeToneMatcher m;
    ToneSuggestion s;
    s.capture_id = "crunch-head";
    s.distance_db = 2.5;
    m.suggestions = {s};
    std::vector<ToneSuggestion> out = {ToneSuggestion{}, ToneSuggestion{}};
    std::string error;

    check(m.match(fake_tone_profile(), {}, out, error) && out.empty(),
          "an empty library is an empty answer, not an error");
    check(m.match(fake_tone_profile(1.0), {fake_capture_profile("crunch-head", kCardSha)}, out, error) &&
              out.size() == 1 && out[0].capture_id == "crunch-head",
          "the fake returns its suggestions");
    check(m.calls == 2 && m.last_library_size == 1 && m.last_reference.bands_db[0] != 0.0,
          "the fake records its calls");
    m.fail_with = "profile x: incomplete";
    check(!m.match(fake_tone_profile(), {}, out, error) && error == m.fail_with, "the fake fails on demand");
}

}  // namespace

int main() {
    test_round_trip();
    test_parse_refusals();
    test_stale_refused();
    test_distance();
    test_fake_matcher();
    if (g_failures == 0) std::puts("analyze_tone_contract: ok");
    return g_failures == 0 ? 0 : 1;
}
