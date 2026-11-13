// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Neutral AAF document (ADR-0154): what the importer needs from an AAF file,
// as plain structs. The only code that sees libaaf types is an AafReader
// implementation; everything else maps from this.

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sonora::interchange {

// Exact rational: video-rate edit rates (25/1, 30000/1001) must not go
// through a double.
struct AafRational {
    std::int64_t num = 0, den = 1;
};

struct AafFade {
    std::int64_t length = 0;  // edit units of the slot
};

struct AafSourceClip {
    std::string mob_id;  // the source mob's MobID (UMID), empty if none
    std::int64_t start = 0, length = 0;  // edit units
    std::optional<double> gain_db;
    std::optional<AafFade> fade_in, fade_out;
};
struct AafFiller {
    std::int64_t length = 0;
};
struct AafTransition {
    std::int64_t length = 0, cut_point = 0;
};
using AafComponent = std::variant<AafSourceClip, AafFiller, AafTransition>;

struct AafTimecode {
    std::int64_t start = 0;  // frames
    int fps = 0;
    bool drop = false;
};
struct AafMarker {
    std::int64_t position = 0;  // edit units
    std::string name;
};

struct AafSlot {
    std::string name;
    AafRational edit_rate{48000, 1};
    std::vector<AafComponent> clips;  // one Sequence, in order
    std::vector<AafMarker> markers;
};

struct AafMob {
    std::string mob_id, name;
    bool is_source = false;  // source mob (media) versus composition mob
    std::string media_path;  // essence file for a source mob, if external
    std::vector<AafSlot> slots;
};

struct AafDocument {
    std::vector<AafMob> mobs;
    std::optional<AafTimecode> timecode;
    // Content the reader saw and cannot express here (automation, effects
    // other than gain, video); the importer forwards it to the report.
    std::vector<std::string> unmapped;
};

class AafReader {
public:
    virtual ~AafReader() = default;
    virtual bool read(const std::string& path, AafDocument& out, std::string& error) = 0;
};

}  // namespace sonora::interchange
