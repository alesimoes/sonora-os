// SPDX-License-Identifier: GPL-3.0-or-later
//
// The beat contract (ADR-0114): a report round-trips through its JSON, the
// reader refuses another schema number, an unknown key and an invalid
// result, the writer refuses an invalid result, and the fake detector
// answers what it was scripted with.
#include <atomic>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "sonora/analyze/beat_detector.h"
#include "sonora/analyze/beat_fakes.h"

using namespace sonora::analyze;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

std::string replace(std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

std::string written(const BeatResult& r) {
    std::string text, error;
    check(beat_report_to_json(r, text, error), "a valid result is written");
    return text;
}

void test_round_trip() {
    // 120.5 and 0.75 are exact in float, so the round trip is exact.
    BeatResult r = fake_beat_result(120.5, 0.75, 44100, 6.0, 512);
    r.onsets.insert(r.onsets.begin() + 1, r.beats[0] + 100);
    r.stable = false;
    BeatResult back;
    std::string error;
    check(parse_beat_report(written(r), back, error), "the written report parses back");
    check(back.sample_rate == 44100 && back.frames == r.frames, "rate and length survive");
    check(back.onsets == r.onsets && back.beats == r.beats, "positions survive exactly");
    check(back.bpm == 120.5 && back.confidence == 0.75, "bpm and confidence survive");
    check(!back.stable && back.method == BeatMethod::spectral_flux, "stable flag and method survive");
    check(written(r).find("\"pass\": \"beat\"") != std::string::npos, "the report names its pass");
}

void test_schema_refused() {
    const std::string text = written(fake_beat_result());
    const std::string schema = "\"schema\": " + std::to_string(kBeatReportSchema);
    const std::string other = replace(text, schema, "\"schema\": 2");
    check(other != text, "the schema number was found to change");
    BeatResult r;
    std::string error;
    check(!parse_beat_report(other, r, error), "another schema number is refused");
    check(error.find("schema 2") != std::string::npos, "the refusal names the schema");
    check(!parse_beat_report(replace(text, "\"schema\"", "\"schemo\""), r, error), "a missing schema is refused");
}

void test_malformed_refused() {
    const std::string text = written(fake_beat_result());
    BeatResult r;
    std::string error;
    check(!parse_beat_report(replace(text, "\"stable\"", "\"tempo_map\""), r, error), "an unknown key is refused");
    check(!parse_beat_report(replace(text, "\"beat\"", "\"key\""), r, error), "another pass is refused");
    check(!parse_beat_report(replace(text, "\"spectral_flux\"", "\"beat_this\""), r, error),
          "an unknown method is refused");
    check(!parse_beat_report("[]", r, error), "a non-object is refused");

    BeatResult bad = fake_beat_result();
    std::string out;
    bad.confidence = 1.5;
    check(!beat_report_to_json(bad, out, error), "confidence above 1 is not written");
    bad = fake_beat_result();
    bad.bpm = 10.0;
    check(!beat_report_to_json(bad, out, error), "bpm below the searched range is not written");
    bad = fake_beat_result();
    std::swap(bad.beats[0], bad.beats[1]);
    check(!beat_report_to_json(bad, out, error), "unsorted beats are not written");
    bad = fake_beat_result();
    bad.onsets.push_back(bad.frames);
    check(!beat_report_to_json(bad, out, error), "an onset past the end is not written");
}

void test_fake_detector() {
    std::vector<std::vector<float>> planar(1, std::vector<float>(4800, 0.0f));
    MemorySource src(planar, 48000.0);
    FakeBeatDetector det;
    det.result = fake_beat_result(96.0, 0.2);
    RunOptions run;
    BeatResult out;
    std::string error;
    check(det.detect(src, run, out, error), "the fake succeeds");
    check(out.bpm == 96.0 && out.confidence < kBeatMinConfidence, "the fake returns its scripted result");
    check(det.calls == 1 && det.last_frames == 4800, "the fake saw the source");

    det.fail_with = "no periodicity found";
    check(!det.detect(src, run, out, error) && error == "no periodicity found", "the fake fails as scripted");

    det.fail_with.clear();
    std::atomic<bool> cancel{true};
    run.cancel = &cancel;
    check(!det.detect(src, run, out, error) && error == "cancelled", "the fake honours cancel");
}

}  // namespace

int main() {
    test_round_trip();
    test_schema_refused();
    test_malformed_refused();
    test_fake_detector();
    if (g_failures == 0) std::puts("analyze_beat_contract: ok");
    return g_failures == 0 ? 0 : 1;
}
