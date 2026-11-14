// SPDX-License-Identifier: GPL-3.0-or-later
//
// The beat pass (ADR-0114): onsets, beats, a global tempo, a confidence and
// a stability flag for one whole file. Offline, in sonora-analyze only
// (ADR-0011 §3.4): everything here allocates and none of it is reachable
// from a `process`. The report's schema is the contract, so its writer and
// reader are here, inline, like tone_profile.h's.
//
// A detection that fails is an error, never a guessed tempo. One that
// succeeds with a low confidence is reported as such; the interface refuses
// to apply it below kBeatMinConfidence.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sonora/analyze/mix_analysis.h"
#include "sonora/json/json.h"

namespace sonora::analyze {

// Bumped with every change to the report's keys or their meaning; a reader
// refuses any other number, so the interface never half-reads a report.
constexpr int kBeatReportSchema = 1;

// Below this the interface refuses to apply a result (ADR-0114).
constexpr double kBeatMinConfidence = 0.3;

// The tempo range the detector searches; a report outside it is malformed.
constexpr double kBeatMinBpm = 30.0;
constexpr double kBeatMaxBpm = 300.0;

// The method that produced the figures; written to the report as is.
// Classical first (ADR-0066 §1): a model-backed tracker would be a new
// value here, opt-in by name.
enum class BeatMethod { spectral_flux };

struct BeatResult {
    std::uint32_t sample_rate = 0;  // of the source; every position is at it
    std::int64_t frames = 0;        // length of the source
    // Source frames, strictly increasing, each in [0, frames).
    std::vector<std::int64_t> onsets;
    std::vector<std::int64_t> beats;
    double bpm = 0.0;         // global, in [kBeatMinBpm, kBeatMaxBpm]
    double confidence = 0.0;  // 0 to 1
    // True when the beat period holds (no tempo drift worth a map of more
    // than one entry); the interface offers "follow" only when false.
    bool stable = false;
    BeatMethod method = BeatMethod::spectral_flux;
};

class BeatDetector {
public:
    virtual ~BeatDetector() = default;
    // Reads the source in blocks; stops between blocks when `run.cancel`
    // is set (false, error "cancelled"). False with `error` on any failure
    // (too short, silent, no periodicity found): never a guessed tempo.
    virtual bool detect(AudioSource& source, const RunOptions& run, BeatResult& out, std::string& error) = 0;
};

inline const char* beat_method_name(BeatMethod) noexcept { return "spectral_flux"; }

inline bool parse_beat_method(std::string_view s, BeatMethod& out) noexcept {
    if (s == "spectral_flux") {
        out = BeatMethod::spectral_flux;
        return true;
    }
    return false;
}

namespace beat_detail {

inline bool positions_valid(const std::vector<std::int64_t>& v, std::int64_t frames) noexcept {
    std::int64_t prev = -1;
    for (std::int64_t f : v) {
        if (f <= prev || f >= frames) return false;
        prev = f;
    }
    return true;
}

inline json::Value positions_json(const std::vector<std::int64_t>& v) {
    json::Value a = json::Value::array();
    for (std::int64_t f : v) a.push(json::Value::integer(f));
    return a;
}

inline bool read_positions(const json::Value& root, const char* key, std::vector<std::int64_t>& out,
                           std::string& error) {
    const json::Value* a = root.find(key);
    if (!a || !a->is_array()) {
        error = std::string("beat report: '") + key + "' missing or not an array";
        return false;
    }
    out.clear();
    out.reserve(a->items().size());
    for (const json::Value& v : a->items()) {
        std::int64_t f = 0;
        if (!v.is_number() || !v.to_int64(f)) {
            error = std::string("beat report: '") + key + "' holds a value that is not an integer";
            return false;
        }
        out.push_back(f);
    }
    return true;
}

inline bool read_double(const json::Value& root, const char* key, double& out, std::string& error) {
    const json::Value* v = root.find(key);
    if (!v || !v->is_number() || !v->to_double(out) || !std::isfinite(out)) {
        error = std::string("beat report: '") + key + "' missing or not a finite number";
        return false;
    }
    return true;
}

inline bool read_int(const json::Value& root, const char* key, std::int64_t& out, std::string& error) {
    const json::Value* v = root.find(key);
    if (!v || !v->is_number() || !v->to_int64(out)) {
        error = std::string("beat report: '") + key + "' missing or not an integer";
        return false;
    }
    return true;
}

}  // namespace beat_detail

// Sample rate set, positions sorted and inside the source, bpm in the
// searched range, confidence in [0, 1]. A detector returns only valid
// results; the writer refuses anything else.
inline bool beat_result_valid(const BeatResult& r) noexcept {
    return r.sample_rate > 0 && r.frames >= 0 && beat_detail::positions_valid(r.onsets, r.frames) &&
           beat_detail::positions_valid(r.beats, r.frames) && std::isfinite(r.bpm) && r.bpm >= kBeatMinBpm &&
           r.bpm <= kBeatMaxBpm && std::isfinite(r.confidence) && r.confidence >= 0.0 && r.confidence <= 1.0;
}

// The report, keys in this order: pass, schema, method, sample_rate,
// frames, bpm, confidence, stable, onsets, beats. Positions are integers so
// they round-trip exactly; bpm and confidence go through a float, as every
// sonora-analyze report's figures do.
inline bool beat_report_to_json(const BeatResult& r, std::string& out, std::string& error) {
    using json::Value;
    if (!beat_result_valid(r)) {
        error = "beat report: invalid result, not written";
        return false;
    }
    Value root = Value::object();
    root.set("pass", Value::string("beat"));
    root.set("schema", Value::integer(kBeatReportSchema));
    root.set("method", Value::string(beat_method_name(r.method)));
    root.set("sample_rate", Value::integer(r.sample_rate));
    root.set("frames", Value::integer(r.frames));
    root.set("bpm", Value::number_float(static_cast<float>(r.bpm)));
    root.set("confidence", Value::number_float(static_cast<float>(r.confidence)));
    root.set("stable", Value::boolean(r.stable));
    root.set("onsets", beat_detail::positions_json(r.onsets));
    root.set("beats", beat_detail::positions_json(r.beats));
    return json::write(root, out, error);
}

// Strict: every key present and typed, no unknown key, the schema exactly
// kBeatReportSchema, and the result valid. A refusal names the reason.
inline bool parse_beat_report(std::string_view text, BeatResult& out, std::string& error) {
    using json::Value;
    using namespace beat_detail;
    Value root;
    json::Error jerr;
    if (!json::parse(text, root, jerr)) {
        error = "beat report: " + jerr.message + " at line " + std::to_string(jerr.line);
        return false;
    }
    if (!root.is_object()) {
        error = "beat report: not an object";
        return false;
    }
    std::int64_t schema = 0;
    if (!read_int(root, "schema", schema, error)) return false;
    if (schema != kBeatReportSchema) {
        error = "beat report: schema " + std::to_string(schema) + " is not supported (expected " +
                std::to_string(kBeatReportSchema) + ")";
        return false;
    }
    static const char* const kKeys[] = {"pass",       "schema", "method", "sample_rate", "frames",
                                        "bpm",        "confidence", "stable", "onsets", "beats"};
    for (const auto& m : root.members()) {
        bool known = false;
        for (const char* k : kKeys) known = known || m.first == k;
        if (!known) {
            error = "beat report: unknown key '" + m.first + "'";
            return false;
        }
    }
    const Value* pass = root.find("pass");
    if (!pass || !pass->is_string() || pass->text() != "beat") {
        error = "beat report: 'pass' is not \"beat\"";
        return false;
    }
    BeatResult r;
    const Value* method = root.find("method");
    if (!method || !method->is_string() || !parse_beat_method(method->text(), r.method)) {
        error = "beat report: 'method' missing or unknown";
        return false;
    }
    const Value* stable = root.find("stable");
    if (!stable || !stable->is_bool()) {
        error = "beat report: 'stable' missing or not a boolean";
        return false;
    }
    r.stable = stable->as_bool();
    std::int64_t rate = 0;
    if (!read_int(root, "sample_rate", rate, error) || !read_int(root, "frames", r.frames, error) ||
        !read_double(root, "bpm", r.bpm, error) || !read_double(root, "confidence", r.confidence, error) ||
        !read_positions(root, "onsets", r.onsets, error) || !read_positions(root, "beats", r.beats, error))
        return false;
    if (rate <= 0 || rate > 0xFFFFFFFFll) {
        error = "beat report: 'sample_rate' out of range";
        return false;
    }
    r.sample_rate = static_cast<std::uint32_t>(rate);
    if (!beat_result_valid(r)) {
        error = "beat report: positions unsorted or outside the source, or bpm/confidence out of range";
        return false;
    }
    out = std::move(r);
    return true;
}

}  // namespace sonora::analyze
