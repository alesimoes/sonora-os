// SPDX-License-Identifier: GPL-3.0-or-later
//
// ToneMatcher (ADR-0072 §4): three synthetic profiles rank nearest first, a
// far reference gets no suggestion, a stale profile is refused by id.
#include <cmath>
#include <cstdio>
#include <utility>
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

// The threshold is exact: a candidate at distance d is suggested when
// max_distance_db is just above d and dropped when just below.
void test_threshold() {
    const ToneProfile ref = fake_tone_profile(0.12);
    const auto lib = std::vector<CaptureProfile>{fake_capture_profile("c-one", kSha, 0.5)};
    const double d = tone_distance_db(ref, lib[0].profile);
    check(d > 0.02, "fixture distance is positive");
    std::vector<ToneSuggestion> out;
    std::string err;
    ToneMatchParams p;
    p.max_distance_db = d + 0.01;
    check(ClassicalToneMatcher(p).match(ref, lib, out, err) && out.size() == 1, "just inside is suggested");
    if (out.size() == 1) check(std::fabs(out[0].distance_db - d) < 1e-9, "reported distance is the metric's");
    p.max_distance_db = d - 0.01;
    check(ClassicalToneMatcher(p).match(ref, lib, out, err) && out.empty(), "just outside is dropped");
    p.max_distance_db = 100.0;
    p.max_candidates = 2;
    check(ClassicalToneMatcher(p).match(ref, library(), out, err) && out.size() == 2, "max_candidates caps the list");
}

// A reference 45 dB off at the ends must be clamped to +/- max_gain_db, and
// the clamp must actually bind (a vacuous bound would pass an unclamped fit).
double peak_residual(double max_gain_db, bool& ok) {
    ToneMatchParams p;
    p.max_distance_db = 1000.0;
    p.max_gain_db = max_gain_db;
    std::vector<ToneSuggestion> out;
    std::string err;
    ok = ClassicalToneMatcher(p).match(fake_tone_profile(3.0), {fake_capture_profile("c-flat", kSha, 0.0)}, out, err) &&
         out.size() == 1;
    double peak = 0.0;
    if (ok)
        for (double g : out[0].eq_curve_db) peak = std::fmax(peak, std::fabs(g));
    return peak;
}

void test_residual_clamp() {
    bool ok = false;
    double peak = peak_residual(12.0, ok);
    check(ok, "wide threshold match ok");
    check(peak <= 12.0 + 1e-9, "residual never exceeds max_gain_db");
    check(peak >= 12.0 - 1e-6, "residual reaches the clamp");
    peak = peak_residual(6.0, ok);
    check(ok && peak <= 6.0 + 1e-9 && peak >= 6.0 - 1e-6, "clamp follows the parameter");
}

// Same input, same result; equal distances break by capture id, not by
// library order.
void test_deterministic() {
    ClassicalToneMatcher m;
    std::vector<ToneSuggestion> a, b;
    std::string err;
    check(m.match(fake_tone_profile(0.12), library(), a, err) && m.match(fake_tone_profile(0.12), library(), b, err),
          "two runs ok");
    check(a.size() == b.size(), "same count");
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        check(a[i].capture_id == b[i].capture_id && a[i].distance_db == b[i].distance_db &&
                  a[i].eq_curve_db == b[i].eq_curve_db,
              "identical suggestion");
    std::vector<CaptureProfile> tie{fake_capture_profile("c-b", kSha, 0.2), fake_capture_profile("c-a", kSha, 0.2)};
    check(m.match(fake_tone_profile(0.1), tie, a, err) && a.size() == 2 && a[0].capture_id == "c-a", "tie by id");
    std::swap(tie[0], tie[1]);
    check(m.match(fake_tone_profile(0.1), tie, a, err) && a.size() == 2 && a[0].capture_id == "c-a",
          "tie independent of library order");
}

void test_stale_stimulus() {
    CaptureProfile c = fake_capture_profile("c-old", kSha);
    c.stimulus = "sonora-tone-stimulus-v0";
    std::string err;
    check(!check_fresh(c, kSha, "", err) && err.find("c-old") != std::string::npos, "other stimulus is stale");
    check(check_fresh(fake_capture_profile("c-new", kSha), kSha, "", err), "current stimulus is fresh");
    std::vector<ToneSuggestion> out;
    check(!ClassicalToneMatcher().match(fake_tone_profile(0.1), {c}, out, err), "matcher refuses a foreign stimulus");
}

void test_empty_library_replaces_out() {
    std::vector<ToneSuggestion> out{ToneSuggestion{}};
    std::string err;
    check(ClassicalToneMatcher().match(fake_tone_profile(0.1), {}, out, err) && out.empty(), "out is replaced");
}
}  // namespace

int main() {
    test_threshold();
    test_residual_clamp();
    test_deterministic();
    test_stale_stimulus();
    test_empty_library_replaces_out();
    test_ranking();
    test_no_close();
    test_refusals();
    test_stale();
    if (g_failures == 0) std::puts("tone matcher: ok");
    return g_failures == 0 ? 0 : 1;
}
