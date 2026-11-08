// SPDX-License-Identifier: GPL-3.0-or-later
//
// ToneMatcher (ADR-0072 §4): three synthetic profiles rank nearest first, a
// far reference gets no suggestion, a stale profile is refused by id.
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/analyze/tone_fakes.h"
#include "tone_matcher.h"

using namespace sonora::analyze;

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}
const std::string kSha(64, 'a');

std::vector<CaptureProfile> library() {
    return {fake_capture_profile("c-far", kSha, 0.5), fake_capture_profile("c-near", kSha, 0.1),
            fake_capture_profile("c-mid", kSha, 0.25)};
}

void test_ranking() {
    ClassicalToneMatcher m;
    std::vector<ToneSuggestion> out;
    std::string err;
    check(m.match(fake_tone_profile(0.12), library(), out, err), "match ok");
    check(out.size() == 3, "three suggestions");
    if (out.size() == 3) {
        check(out[0].capture_id == "c-near" && out[1].capture_id == "c-mid" && out[2].capture_id == "c-far",
              "nearest first");
        check(out[0].distance_db <= out[1].distance_db && out[1].distance_db <= out[2].distance_db, "ascending");
        check(!out[0].amp_settings.empty(), "card settings copied");
        for (double g : out[2].eq_curve_db) check(g >= -12.0 && g <= 12.0, "eq clamped");
    }
}

void test_no_close() {
    ClassicalToneMatcher m;
    std::vector<ToneSuggestion> out;
    std::string err;
    check(m.match(fake_tone_profile(3.0), library(), out, err), "far reference is still ok (exit 0)");
    check(out.empty(), "no suggestion for a far reference");
    check(m.match(fake_tone_profile(0.1), {}, out, err) && out.empty(), "empty library, empty answer");
}

void test_refusals() {
    ClassicalToneMatcher m;
    std::vector<ToneSuggestion> out;
    std::string err;
    ToneProfile thin = fake_tone_profile(0.1);
    for (int b = 0; b < 20; ++b) thin.valid[b] = false;
    check(!m.match(thin, library(), out, err), "too few valid bands fails");
    auto dup = library();
    dup.push_back(fake_capture_profile("c-near", kSha));
    check(!m.match(fake_tone_profile(0.1), dup, out, err), "duplicate id fails");
}

void test_stale() {
    auto lib = library();
    lib.push_back(fake_stale_capture_profile("c-stale", kSha));
    std::string err;
    auto digest = [](const std::string&, std::string& sha) {
        sha = kSha;
        return true;
    };
    check(!check_library_fresh(lib, digest, err), "stale library refused");
    check(err.find("c-stale") != std::string::npos, "message names the capture id");
    check(check_library_fresh(library(), digest, err), "fresh library passes");
    auto missing = [](const std::string&, std::string&) { return false; };
    check(!check_library_fresh(library(), missing, err), "missing card refused");
}
}  // namespace

int main() {
    test_ranking();
    test_no_close();
    test_refusals();
    test_stale();
    if (g_failures == 0) std::puts("tone matcher: ok");
    return g_failures == 0 ? 0 : 1;
}
