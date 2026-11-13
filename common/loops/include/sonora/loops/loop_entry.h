#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sonora::loops {

// One catalog item. Plain data, built off the audio thread (ADR-0163).
struct LoopEntry {
    std::string id;          // stable within its source
    std::string name;
    std::string path;        // audio file, relative to the source root
    std::string category;    // e.g. "Drums"
    std::vector<std::string> tags;
    double bpm = 0.0;        // 0 = unknown
    int beats = 0;           // length in beats; 0 = unknown
    std::string key;         // "" = unknown
    std::uint32_t sample_rate = 0;
    std::uint64_t frames = 0;
};

// Empty fields do not filter; bpm_tolerance is a fraction (0.05 = 5 %).
struct LoopQuery {
    std::string text;        // case-insensitive substring of name or tag
    std::string category;
    std::string key;
    double bpm = 0.0;
    double bpm_tolerance = 0.0;
};

}  // namespace sonora::loops
