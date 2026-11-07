// SPDX-License-Identifier: GPL-3.0-or-later
//
// The key pass (ADR-0066 §1, §4): tonic and mode of a whole file. The
// classical detector (Krumhansl-Schmuckler on chroma) is always built and
// is the default; a model-backed one (keynet) runs only when asked for by
// name, and its result names it. Offline, in sonora-analyze only.
#pragma once

#include <string>
#include <string_view>

#include "sonora/analyze/mix_analysis.h"

namespace sonora::analyze {

enum class KeyMode { major, minor };

// The method that produced a figure; written to the report as is.
enum class KeyMethod { krumhansl, keynet };

struct KeyResult {
    int tonic = 0;  // pitch class, 0 = C ... 11 = B
    KeyMode mode = KeyMode::major;
    double confidence = 0.0;  // 0 to 1
    KeyMethod method = KeyMethod::krumhansl;
};

class KeyDetector {
public:
    virtual ~KeyDetector() = default;
    // Reads the source in blocks; stops between blocks when
    // `run.cancel` is set (false, error "cancelled"). False with `error`
    // on any failure, including an unavailable model: never a silent
    // fallback to another method (ADR-0066 §1).
    virtual bool detect(AudioSource& source, const RunOptions& run, KeyResult& out, std::string& error) = 0;
};

inline const char* key_mode_name(KeyMode m) noexcept { return m == KeyMode::minor ? "minor" : "major"; }

inline bool parse_key_mode(std::string_view s, KeyMode& out) noexcept {
    if (s == "major") { out = KeyMode::major; return true; }
    if (s == "minor") { out = KeyMode::minor; return true; }
    return false;
}

inline const char* key_method_name(KeyMethod m) noexcept { return m == KeyMethod::keynet ? "keynet" : "krumhansl"; }

inline bool parse_key_method(std::string_view s, KeyMethod& out) noexcept {
    if (s == "krumhansl") { out = KeyMethod::krumhansl; return true; }
    if (s == "keynet") { out = KeyMethod::keynet; return true; }
    return false;
}

// Sharps throughout, as the report writes them; null outside 0..11.
inline const char* tonic_name(int pitch_class) noexcept {
    static const char* const kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return pitch_class >= 0 && pitch_class < 12 ? kNames[pitch_class] : nullptr;
}

inline bool key_result_valid(const KeyResult& r) noexcept {
    return r.tonic >= 0 && r.tonic < 12 && r.confidence >= 0.0 && r.confidence <= 1.0;
}

}  // namespace sonora::analyze
