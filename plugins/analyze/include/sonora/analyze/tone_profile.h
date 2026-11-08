// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reference-tone matching (ADR-0072): the spectral fingerprint of a sound
// and the precomputed profile of each capture. Matching is by spectrum,
// never by name: nothing here holds or produces an artist, band or product
// name. The schema of a capture profile file is the contract, so its
// parser and writer are here, inline, like model_manifest.h's.
//
// Offline only (sonora-analyze, ADR-0011 §3.4): everything here allocates
// and none of it is reachable from a `process`.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sonora/analyze/timbre_match.h"
#include "sonora/json/json.h"

namespace sonora::analyze {

// The third-octave bands of the timbre pass (ISO 266, 20 Hz to 20 kHz), so
// the residual curve feeds match_timbre's EQ fit unchanged.
constexpr int kToneBands = kTimbreBands;

// The fixed synthetic stimulus a capture is rendered with to build its
// profile (ADR-0072 §3). Generated in code, no third-party audio. A profile
// made with another stimulus is not comparable and is refused; changing
// the stimulus means a new id here and regenerating every profile.
constexpr const char* kToneStimulusId = "sonora-tone-stimulus-v1";

// Where a capture's profile sits, relative to the repository root. The
// files are derived artefacts owned by the amp-and-cab track's tooling.
constexpr const char* kToneProfileDir = "plugins/native/ampcab/captures/profiles";

// The long-term spectral fingerprint of a sound.
struct ToneProfile {
    // Band level in dB relative to the mean of the valid bands. A band is
    // invalid when its edge is beyond Nyquist or it held no frames above
    // the timbre pass's silence gate; a capture profile has none.
    std::array<double, kToneBands> bands_db{};
    std::array<bool, kToneBands> valid{};
    // Peak-to-RMS ratio of the whole signal, dB (>= 0).
    double crest_db = 0.0;
    // Spectral flatness (geometric over arithmetic mean of power) of the
    // bands at and above 2 kHz, 0 (tonal) to 1 (white).
    double flatness = 0.0;
};

// A capture's profile: a derived artefact, one JSON per capture at
// <kToneProfileDir>/<id>.json, stamped with the SHA-256 of the cards it
// was made from. A changed card makes the profile stale (check_fresh).
struct CaptureProfile {
    std::string id;           // the capture card's id (ADR-0069)
    std::string card_sha256;  // SHA-256 of brain/models/<id>.md's bytes
    // The cabinet IR the render went through; empty when the capture
    // already includes a cabinet (a capture taken at the microphone).
    std::string ir_id;
    std::string ir_card_sha256;  // empty exactly when ir_id is
    std::string stimulus;        // kToneStimulusId when current
    ToneProfile profile;
    // NOT in the file: the amp settings, copied from the card by the
    // loader through parse_card, in card order. The matcher copies them
    // into a suggestion; the card stays the one source.
    std::vector<std::pair<std::string, std::string>> amp_settings;
};

namespace tone_detail {

inline bool is_hex64(std::string_view s) noexcept {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// ADR-0069's id grammar: ^[a-z0-9-]+$.
inline bool is_card_id(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}

inline bool read_string(const json::Value& o, const char* key, std::string& out, std::string& error) {
    const json::Value* v = o.find(key);
    if (!v || !v->is_string()) {
        error = std::string("profile: '") + key + "' missing or not a string";
        return false;
    }
    out = v->text();
    return true;
}

inline bool read_number(const json::Value& o, const char* key, double& out, std::string& error) {
    const json::Value* v = o.find(key);
    if (!v || !v->is_number() || !v->to_double(out) || !std::isfinite(out)) {
        error = std::string("profile: '") + key + "' missing or not a finite number";
        return false;
    }
    return true;
}

inline json::Value number(double v) { return json::Value::number_float(static_cast<float>(v)); }

}  // namespace tone_detail

// Every band valid and finite, crest finite and >= 0, flatness in [0, 1].
// A capture profile must be complete; a reference need not be.
inline bool complete(const ToneProfile& p) noexcept {
    for (int b = 0; b < kToneBands; ++b)
        if (!p.valid[b] || !std::isfinite(p.bands_db[b])) return false;
    return std::isfinite(p.crest_db) && p.crest_db >= 0.0 && std::isfinite(p.flatness) && p.flatness >= 0.0 &&
           p.flatness <= 1.0;
}

// Reads one profile file. Strict: every field present and typed, the ids
// in ADR-0069's grammar, the stamps 64 lower-case hex digits, exactly 31
// bands, and the profile complete. Unknown keys are refused, so a schema
// change amends ADR-0072 and this header together. amp_settings is left
// empty (it is not in the file).
inline bool parse_profile(std::string_view text, CaptureProfile& out, std::string& error) {
    using json::Value;
    using namespace tone_detail;
    Value root;
    json::Error jerr;
    if (!json::parse(text, root, jerr)) {
        error = "profile: " + jerr.message + " at line " + std::to_string(jerr.line);
        return false;
    }
    if (!root.is_object()) {
        error = "profile: not an object";
        return false;
    }
    static const char* const kKeys[] = {"id", "card_sha256", "ir_id", "ir_card_sha256", "stimulus",
                                        "bands",  "crest",       "flatness"};
    for (const auto& m : root.members()) {
        bool known = false;
        for (const char* k : kKeys) known = known || m.first == k;
        if (!known) {
            error = "profile: unknown key '" + m.first + "'";
            return false;
        }
    }
    CaptureProfile p;
    if (!read_string(root, "id", p.id, error) || !read_string(root, "card_sha256", p.card_sha256, error) ||
        !read_string(root, "ir_id", p.ir_id, error) || !read_string(root, "ir_card_sha256", p.ir_card_sha256, error) ||
        !read_string(root, "stimulus", p.stimulus, error) || !read_number(root, "crest", p.profile.crest_db, error) ||
        !read_number(root, "flatness", p.profile.flatness, error))
        return false;
    if (!is_card_id(p.id)) {
        error = "profile: id '" + p.id + "' is not [a-z0-9-]+";
        return false;
    }
    if (!is_hex64(p.card_sha256)) {
        error = "profile " + p.id + ": card_sha256 is not 64 lower-case hex digits";
        return false;
    }
    if (p.ir_id.empty() != p.ir_card_sha256.empty() || (!p.ir_id.empty() && !is_card_id(p.ir_id)) ||
        (!p.ir_card_sha256.empty() && !is_hex64(p.ir_card_sha256))) {
        error = "profile " + p.id + ": ir_id and ir_card_sha256 must both be empty or both valid";
        return false;
    }
    const Value* bands = root.find("bands");
    if (!bands || !bands->is_array() || bands->items().size() != static_cast<std::size_t>(kToneBands)) {
        error = "profile " + p.id + ": 'bands' must be an array of " + std::to_string(kToneBands) + " numbers";
        return false;
    }
    for (int b = 0; b < kToneBands; ++b) {
        const Value& v = bands->items()[static_cast<std::size_t>(b)];
        double d = 0.0;
        if (!v.is_number() || !v.to_double(d) || !std::isfinite(d)) {
            error = "profile " + p.id + ": band " + std::to_string(b) + " is not a finite number";
            return false;
        }
        p.profile.bands_db[b] = d;
        p.profile.valid[b] = true;
    }
    if (!complete(p.profile)) {
        error = "profile " + p.id + ": incomplete (crest < 0 or flatness outside [0, 1])";
        return false;
    }
    out = std::move(p);
    return true;
}

// The canonical file, keys in the order parse_profile lists them. Refuses
// an incomplete profile, so a written file always parses back.
inline bool profile_to_json(const CaptureProfile& p, std::string& out, std::string& error) {
    using json::Value;
    using tone_detail::number;
    if (!complete(p.profile)) {
        error = "profile " + p.id + ": incomplete, not written";
        return false;
    }
    Value root = Value::object();
    root.set("id", Value::string(p.id));
    root.set("card_sha256", Value::string(p.card_sha256));
    root.set("ir_id", Value::string(p.ir_id));
    root.set("ir_card_sha256", Value::string(p.ir_card_sha256));
    root.set("stimulus", Value::string(p.stimulus));
    Value bands = Value::array();
    for (int b = 0; b < kToneBands; ++b) bands.push(number(p.profile.bands_db[b]));
    root.set("bands", std::move(bands));
    root.set("crest", number(p.profile.crest_db));
    root.set("flatness", number(p.profile.flatness));
    return json::write(root, out, error);
}

// The staleness rule (ADR-0072 §3): the profile's stamps must equal the
// SHA-256 of the cards as they are now, and its stimulus the current one.
// False with `error` naming the capture otherwise; the caller fails the
// pass, it never skips the profile or falls back to another.
inline bool check_fresh(const CaptureProfile& p, std::string_view card_sha256, std::string_view ir_card_sha256,
                        std::string& error) {
    if (p.card_sha256 != card_sha256) {
        error = "profile " + p.id + " is stale: its card changed (regenerate the profile)";
        return false;
    }
    if (p.ir_card_sha256 != ir_card_sha256) {
        error = "profile " + p.id + " is stale: the card of IR '" + p.ir_id + "' changed (regenerate the profile)";
        return false;
    }
    if (p.stimulus != kToneStimulusId) {
        error = "profile " + p.id + " is stale: made with stimulus '" + p.stimulus + "', expected '" +
                kToneStimulusId + "'";
        return false;
    }
    return true;
}

// The bands as a timbre curve, for match_timbre's EQ fit.
inline TimbreCurve to_timbre_curve(const ToneProfile& p) {
    TimbreCurve c;
    c.gain_db = p.bands_db;
    c.valid = p.valid;
    return c;
}

}  // namespace sonora::analyze
