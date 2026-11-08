// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0077's contract: the metrics behave as mir_eval.melody defines them,
// the fake source stands in for a dataset, and the report carries numbers
// and the dataset's identity, never audio.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/native/tuner_eval.h"

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            return 1;                                                              \
        }                                                                          \
    } while (0)

namespace {

using namespace sonora::native::tuner_eval;

bool near(double a, double b) { return std::fabs(a - b) < 1e-12; }

// 10 voiced frames at 220 Hz, then 5 unvoiced, on a 10 ms grid.
std::vector<PitchFrame> reference_track() {
    std::vector<PitchFrame> t;
    for (int i = 0; i < 15; ++i) {
        const bool voiced = i < 10;
        t.push_back({i * 0.01, voiced ? 220.0 : 0.0, voiced});
    }
    return t;
}

int perfect_estimate_scores_one() {
    FakeAnnotationSource src;
    src.add("vocadito/1", reference_track());
    const auto ref = src.load("vocadito/1");
    CHECK(ref.size() == 15);
    const EvalReport rep = score_detector(ref, ref, 50.0);
    CHECK(near(rep.rpa, 1.0));
    CHECK(near(rep.rca, 1.0));
    CHECK(near(rep.voicing_recall, 1.0));
    CHECK(near(rep.voicing_false_alarm, 0.0));
    CHECK(std::isnan(rep.in_scale_fraction));
    CHECK(rep.frames == 15);
    return 0;
}

int octave_error_counts_for_chroma_only() {
    const auto ref = reference_track();
    auto est = ref;
    est[0].hz = 440.0;   // one octave up
    est[1].hz = 110.0;   // one octave down
    est[2].hz = 220.0 * std::pow(2.0, 60.0 / 1200.0);  // 60 cents off: neither
    const EvalReport rep = score_detector(ref, est, 50.0);
    CHECK(near(rep.rpa, 7.0 / 10.0));
    CHECK(near(rep.rca, 9.0 / 10.0));
    return 0;
}

int unvoiced_reference_frames_are_not_in_rpa() {
    const auto ref = reference_track();
    auto est = ref;
    // Wild pitches and voiced flags on the reference's unvoiced frames
    // change the false-alarm rate, never RPA or RCA.
    for (std::size_t i = 10; i < est.size(); ++i) est[i] = {est[i].time_s, 1000.0, true};
    const EvalReport rep = score_detector(ref, est, 50.0);
    CHECK(near(rep.rpa, 1.0));
    CHECK(near(rep.rca, 1.0));
    CHECK(near(rep.voicing_false_alarm, 1.0));
    return 0;
}

int pitch_scored_whatever_the_voicing_flag() {
    const auto ref = reference_track();
    auto est = ref;
    est[3].voiced = false;  // right pitch, called unvoiced: mir_eval's raw metrics count it
    est[4] = {est[4].time_s, 0.0, false};  // no pitch at all: a miss
    const EvalReport rep = score_detector(ref, est, 50.0);
    CHECK(near(rep.rpa, 9.0 / 10.0));
    CHECK(near(rep.voicing_recall, 8.0 / 10.0));
    return 0;
}

int missing_source_is_empty_not_scored() {
    FakeAnnotationSource src;
    const auto ref = src.load("absent");
    CHECK(ref.empty());
    const EvalReport rep = score_detector(ref, ref, 50.0);
    CHECK(rep.frames == 0);
    CHECK(std::isnan(rep.rpa));
    return 0;
}

int json_has_numbers_and_identity_not_audio() {
    EvalReport rep = score_detector(reference_track(), reference_track(), 50.0);
    const std::string j = render_json(rep, "vocadito", "CC BY 4.0", "zenodo-5578259 \"v1\"");
    CHECK(j.find("\"dataset\": \"vocadito\"") != std::string::npos);
    CHECK(j.find("\"licence\": \"CC BY 4.0\"") != std::string::npos);
    CHECK(j.find("\"version\": \"zenodo-5578259 \\\"v1\\\"\"") != std::string::npos);
    CHECK(j.find("\"rpa\": 1,") != std::string::npos);
    CHECK(j.find("\"in_scale_fraction\": null") != std::string::npos);
    CHECK(j.find("\"frames\": 15") != std::string::npos);
    for (const char* banned : {"audio", "samples", "wav", "pcm", "["}) {
        CHECK(j.find(banned) == std::string::npos);
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += perfect_estimate_scores_one();
    failed += octave_error_counts_for_chroma_only();
    failed += unvoiced_reference_frames_are_not_in_rpa();
    failed += pitch_scored_whatever_the_voicing_flag();
    failed += missing_source_is_empty_not_scored();
    failed += json_has_numbers_and_identity_not_audio();
    if (failed == 0) std::printf("tuner eval contract: 6 cases passed\n");
    return failed == 0 ? 0 : 1;
}
