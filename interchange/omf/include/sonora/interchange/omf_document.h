// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Neutral OMF document (ADR-0160): what the importer needs from an OMF
// interchange file, as plain structs. Only an OmfReader implementation sees
// the file's bytes; everything else maps from this. Read-only: no export.

#include <cstdint>
#include <string>
#include <vector>

namespace sonora::interchange {

struct OmfMediaSource {
    std::string name;
    std::string path;         // external media; empty when embedded
    bool embedded = false;    // AIFC/WAVE bytes live inside the file
    std::int64_t length = 0;  // samples
};

struct OmfClip {
    int source = -1;                 // index into OmfDocument::sources
    std::int64_t start = 0;          // on the timeline, samples
    std::int64_t source_offset = 0;  // samples into the source
    std::int64_t length = 0;         // samples
    double gain_db = 0.0;
    bool mute = false;
    std::int64_t fade_in = 0, fade_out = 0;  // samples
};

struct OmfTrack {
    std::string name;
    std::vector<OmfClip> clips;
};

struct OmfDocument {
    std::int64_t sample_rate = 0;
    std::vector<OmfMediaSource> sources;
    std::vector<OmfTrack> tracks;
    // Effects, automation, MIDI and the like the reader saw and did not model.
    std::vector<std::string> unsupported;
};

class OmfReader {
public:
    virtual ~OmfReader() = default;
    // False with a message naming the problem, e.g. the unsupported version.
    virtual bool read(const std::string& path, OmfDocument& out, std::string& error) = 0;
};

}  // namespace sonora::interchange
