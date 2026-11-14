// SPDX-License-Identifier: GPL-3.0-or-later
//
// The argument of audio.elastic (ADR-0136 §2): stretch one region offline
// into a new file, then repoint the region (one undo step). Written by the
// interface and read by the engine and the fake from this one place. No
// protocol bump: an engine announces kCapabilityElastic, and one without it
// answers the command NotImplemented.
//
//   "<region> <algorithm> ratio=<R>"
//   "<region> <algorithm> warp=<SRC>:<DST>[,<SRC>:<DST>...]"
//   "<region> <algorithm> quantize=<GRID>[:<STRENGTH>]"
//
//   region     the model id of an audio region, decimal, not 0
//   algorithm  polyphonic | rhythmic | monophonic | varispeed
//   ratio      new length / old length, kMinElasticRatio..kMaxElasticRatio
//              (2 is twice as long, half the speed)
//   warp       markers in frames, both sides counted from the region's
//              first frame: SRC a frame of the region as it is, DST where
//              it lands. The region's start is the implicit marker 0:0, so
//              every SRC and DST is > 0, and both sides strictly increase.
//              What follows the last marker keeps its speed.
//   quantize   GRID in beats (0.25 is a sixteenth at 4/4), STRENGTH 1..100
//              percent, 100 when omitted. The engine finds the onsets and
//              reads the tempo map; the result's length is known only then.
//
// Decimals are written with a '.', at most six places, whatever the
// process's locale: the interface (Qt sets LC_NUMERIC from the environment)
// and the engine must read the same number.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sonora::api {

inline constexpr const char* kCapabilityElastic = "audio.elastic";

// The region is held in memory whole while it is stretched, and so is the
// result: ten minutes at 48 kHz each way, which bounds both.
inline constexpr int64_t kMaxElasticFrames = int64_t{48000} * 60 * 10;
inline constexpr double kMinElasticRatio = 0.25;
inline constexpr double kMaxElasticRatio = 4.0;
// Keeps the argument, and the stretcher's key-frame map, bounded.
inline constexpr std::size_t kMaxElasticMarkers = 1024;
// The finest and coarsest quantize grids, in beats (a 64th to four bars).
inline constexpr double kMinElasticGrid = 0.0625;
inline constexpr double kMaxElasticGrid = 16.0;

enum class ElasticAlgorithm : uint8_t { Polyphonic, Rhythmic, Monophonic, Varispeed };
enum class ElasticMode : uint8_t { Ratio, Warp, Quantize };

struct WarpMarker {
    int64_t source = 0;
    int64_t target = 0;
    bool operator==(const WarpMarker& o) const { return source == o.source && target == o.target; }
};

struct ElasticRequest {
    uint64_t region = 0;
    ElasticAlgorithm algorithm = ElasticAlgorithm::Polyphonic;
    ElasticMode mode = ElasticMode::Ratio;
    double ratio = 1.0;               // Ratio
    std::vector<WarpMarker> markers;  // Warp
    double grid_beats = 0.25;         // Quantize
    uint32_t strength = 100;          // Quantize, percent

    bool operator==(const ElasticRequest& o) const {
        return region == o.region && algorithm == o.algorithm && mode == o.mode &&
               ratio == o.ratio && markers == o.markers && grid_beats == o.grid_beats &&
               strength == o.strength;
    }
};

inline const char* elastic_algorithm_name(ElasticAlgorithm a) {
    switch (a) {
        case ElasticAlgorithm::Polyphonic: return "polyphonic";
        case ElasticAlgorithm::Rhythmic: return "rhythmic";
        case ElasticAlgorithm::Monophonic: return "monophonic";
        case ElasticAlgorithm::Varispeed: return "varispeed";
    }
    return "polyphonic";
}

namespace elastic_detail {

inline constexpr int64_t kMicro = 1000000;

// Six decimal places, rounded; the parser below reads it back exactly.
inline std::string format_micro(double value) {
    const double scaled = value * static_cast<double>(kMicro);
    const int64_t m = static_cast<int64_t>(scaled < 0 ? scaled - 0.5 : scaled + 0.5);
    std::string s = std::to_string(m / kMicro);
    int64_t frac = m % kMicro;
    if (frac == 0) return s;
    std::string digits = std::to_string(frac);
    digits.insert(0, 6 - digits.size(), '0');
    while (digits.back() == '0') digits.pop_back();
    return s + "." + digits;
}

// Digits only, at most 18 of them, so the value cannot overflow.
inline bool parse_whole(const std::string& text, int64_t& out) {
    if (text.empty() || text.size() > 18) return false;
    int64_t v = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

// "D[.DDDDDD]", by hand: strtod would read the locale's decimal point.
inline bool parse_decimal(const std::string& text, double& out) {
    const std::size_t dot = text.find('.');
    const std::string whole = text.substr(0, dot);
    int64_t w = 0;
    if (whole.size() > 9 || !parse_whole(whole, w)) return false;
    int64_t frac = 0;
    if (dot != std::string::npos) {
        std::string f = text.substr(dot + 1);
        if (f.empty() || f.size() > 6 || !parse_whole(f, frac)) return false;
        for (std::size_t i = f.size(); i < 6; ++i) frac *= 10;
    }
    out = static_cast<double>(w * kMicro + frac) / static_cast<double>(kMicro);
    return true;
}

inline bool parse_algorithm(const std::string& word, ElasticAlgorithm& out) {
    for (const ElasticAlgorithm a : {ElasticAlgorithm::Polyphonic, ElasticAlgorithm::Rhythmic,
                                     ElasticAlgorithm::Monophonic, ElasticAlgorithm::Varispeed}) {
        if (word == elastic_algorithm_name(a)) {
            out = a;
            return true;
        }
    }
    return false;
}

inline bool parse_markers(const std::string& text, std::vector<WarpMarker>& out,
                          std::string& why) {
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t comma = text.find(',', pos);
        if (comma == std::string::npos) comma = text.size();
        const std::string pair = text.substr(pos, comma - pos);
        const std::size_t colon = pair.find(':');
        WarpMarker m;
        if (colon == std::string::npos || !parse_whole(pair.substr(0, colon), m.source) ||
            !parse_whole(pair.substr(colon + 1), m.target)) {
            why = "audio.elastic: warp marker \"" + pair + "\" is not SRC:DST in frames";
            return false;
        }
        if (out.size() == kMaxElasticMarkers) {
            why = "audio.elastic: more than " + std::to_string(kMaxElasticMarkers) +
                  " warp markers";
            return false;
        }
        out.push_back(m);
        pos = comma + 1;
    }
    return true;
}

// The marker rules that need no region: past the implicit 0:0, both sides
// strictly increasing.
inline bool check_markers(const std::vector<WarpMarker>& markers, std::string& why) {
    WarpMarker last;
    for (const WarpMarker& m : markers) {
        if (m.source <= last.source || m.target <= last.target) {
            why = "warp markers must move forward on both sides: " +
                  std::to_string(m.source) + ":" + std::to_string(m.target) + " follows " +
                  std::to_string(last.source) + ":" + std::to_string(last.target) +
                  " (the region's start is 0:0)";
            return false;
        }
        last = m;
    }
    return true;
}

inline bool parse_quantize(const std::string& text, ElasticRequest& r, std::string& why) {
    const std::size_t colon = text.find(':');
    if (!parse_decimal(text.substr(0, colon), r.grid_beats)) {
        why = "audio.elastic: quantize grid \"" + text.substr(0, colon) +
              "\" is not a number of beats";
        return false;
    }
    if (r.grid_beats < kMinElasticGrid || r.grid_beats > kMaxElasticGrid) {
        why = "a quantize grid of " + format_micro(r.grid_beats) + " beats is outside " +
              format_micro(kMinElasticGrid) + ".." + format_micro(kMaxElasticGrid);
        return false;
    }
    r.strength = 100;
    if (colon == std::string::npos) return true;
    int64_t s = 0;
    const std::string strength = text.substr(colon + 1);
    if (!parse_whole(strength, s)) {
        why = "audio.elastic: quantize strength \"" + strength + "\" is not a percentage";
        return false;
    }
    if (s < 1 || s > 100) {
        why = "a quantize strength of " + strength + "% is outside 1..100; 0 would move nothing";
        return false;
    }
    r.strength = static_cast<uint32_t>(s);
    return true;
}

}  // namespace elastic_detail

inline std::string format_elastic_request(const ElasticRequest& r) {
    std::string s = std::to_string(r.region) + " " + elastic_algorithm_name(r.algorithm) + " ";
    switch (r.mode) {
        case ElasticMode::Ratio: return s + "ratio=" + elastic_detail::format_micro(r.ratio);
        case ElasticMode::Warp:
            s += "warp=";
            for (std::size_t i = 0; i < r.markers.size(); ++i) {
                if (i > 0) s += ",";
                s += std::to_string(r.markers[i].source) + ":" +
                     std::to_string(r.markers[i].target);
            }
            return s;
        case ElasticMode::Quantize:
            return s + "quantize=" + elastic_detail::format_micro(r.grid_beats) + ":" +
                   std::to_string(r.strength);
    }
    return s;
}

// False, with `why` the operator can be shown, for anything but the forms
// above, or a value out of its range. What needs the region itself (its
// length) is validate_elastic_request's.
inline bool parse_elastic_request(const std::string& text, ElasticRequest& out,
                                  std::string& why) {
    std::vector<std::string> words;
    for (std::size_t pos = 0; pos < text.size();) {
        if (text[pos] == ' ') {
            ++pos;
            continue;
        }
        std::size_t stop = text.find(' ', pos);
        if (stop == std::string::npos) stop = text.size();
        words.push_back(text.substr(pos, stop - pos));
        pos = stop;
    }
    if (words.size() != 3) {
        why = "audio.elastic needs REGION ALGORITHM and one of ratio=, warp= or quantize=";
        return false;
    }
    ElasticRequest r;
    int64_t region = 0;
    if (!elastic_detail::parse_whole(words[0], region) || region == 0) {
        why = "audio.elastic: \"" + words[0] + "\" is no region id";
        return false;
    }
    r.region = static_cast<uint64_t>(region);
    if (!elastic_detail::parse_algorithm(words[1], r.algorithm)) {
        why = "audio.elastic: unknown algorithm \"" + words[1] +
              "\" (polyphonic, rhythmic, monophonic or varispeed)";
        return false;
    }
    const std::string& mode = words[2];
    const std::size_t eq = mode.find('=');
    const std::string key = mode.substr(0, eq);
    const std::string value = eq == std::string::npos ? std::string() : mode.substr(eq + 1);
    if (eq == std::string::npos || value.empty()) {
        why = "audio.elastic: \"" + mode + "\" is not ratio=, warp= or quantize= with a value";
        return false;
    }
    if (key == "ratio") {
        r.mode = ElasticMode::Ratio;
        if (!elastic_detail::parse_decimal(value, r.ratio)) {
            why = "audio.elastic: ratio \"" + value + "\" is not a number";
            return false;
        }
        if (r.ratio < kMinElasticRatio || r.ratio > kMaxElasticRatio) {
            why = "a stretch ratio of " + value + " is outside " +
                  elastic_detail::format_micro(kMinElasticRatio) + ".." +
                  elastic_detail::format_micro(kMaxElasticRatio);
            return false;
        }
    } else if (key == "warp") {
        r.mode = ElasticMode::Warp;
        if (!elastic_detail::parse_markers(value, r.markers, why)) return false;
        if (!elastic_detail::check_markers(r.markers, why)) return false;
    } else if (key == "quantize") {
        r.mode = ElasticMode::Quantize;
        if (!elastic_detail::parse_quantize(value, r, why)) return false;
    } else {
        why = "audio.elastic: unknown mode \"" + key + "\" (ratio, warp or quantize)";
        return false;
    }
    out = std::move(r);
    return true;
}

// The result's length in frames for a region of `region_frames`, or -1 for
// quantize, whose length the onsets decide.
inline int64_t elastic_output_frames(const ElasticRequest& r, int64_t region_frames) {
    switch (r.mode) {
        case ElasticMode::Ratio: {
            const double out = static_cast<double>(region_frames) * r.ratio;
            return static_cast<int64_t>(out + 0.5);
        }
        case ElasticMode::Warp: {
            if (r.markers.empty()) return region_frames;
            const WarpMarker& last = r.markers.back();
            return last.target + (region_frames - last.source);
        }
        case ElasticMode::Quantize: return -1;
    }
    return -1;
}

// What the engine (and the fake) check once the region is found: a parsed
// request against the region's length. The engine adds what only it knows
// (overlap with the next region, a take open for writing).
inline bool validate_elastic_request(const ElasticRequest& r, int64_t region_frames,
                                     std::string& why) {
    if (region_frames <= 0) {
        why = "region " + std::to_string(r.region) + " is empty; nothing to stretch";
        return false;
    }
    if (region_frames > kMaxElasticFrames) {
        why = "region " + std::to_string(r.region) + " is " + std::to_string(region_frames) +
              " frames long, over the " + std::to_string(kMaxElasticFrames) +
              " an elastic render holds in memory; split it first";
        return false;
    }
    if (r.mode == ElasticMode::Ratio &&
        (r.ratio < kMinElasticRatio || r.ratio > kMaxElasticRatio)) {
        why = "a stretch ratio of " + elastic_detail::format_micro(r.ratio) + " is outside " +
              elastic_detail::format_micro(kMinElasticRatio) + ".." +
              elastic_detail::format_micro(kMaxElasticRatio);
        return false;
    }
    if (r.mode == ElasticMode::Warp) {
        if (r.markers.empty()) {
            why = "audio.elastic: warp= needs at least one marker";
            return false;
        }
        if (!elastic_detail::check_markers(r.markers, why)) return false;
        for (const WarpMarker& m : r.markers) {
            if (m.source > region_frames) {
                why = "warp marker " + std::to_string(m.source) + ":" +
                      std::to_string(m.target) + " is past the region's end (" +
                      std::to_string(region_frames) + " frames)";
                return false;
            }
        }
    }
    const int64_t out = elastic_output_frames(r, region_frames);
    if (out > kMaxElasticFrames) {
        why = "the stretched region would be " + std::to_string(out) + " frames long, over the " +
              std::to_string(kMaxElasticFrames) + " an elastic render holds in memory";
        return false;
    }
    return true;
}

}  // namespace sonora::api
