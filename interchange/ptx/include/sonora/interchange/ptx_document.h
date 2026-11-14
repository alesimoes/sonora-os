// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Neutral description of a Pro Tools session (ADR-0156). Plain data: nothing
// here names libptformat, so the decoder stays behind PtxReader.

#include <cstdint>
#include <string>
#include <vector>

namespace sonora::interchange {

struct PtxAudioSource {
    std::string name;
    std::string path;  // as stored in the file; may be relative or missing on disk
    std::uint64_t length_samples = 0;
};

struct PtxRegion {
    std::uint32_t source_index = 0;    // into PtxDocument::sources
    std::uint64_t start_samples = 0;   // position on the timeline
    std::uint64_t offset_samples = 0;  // into the source
    std::uint64_t length_samples = 0;
    float gain = 1.0f;  // linear
    bool muted = false;
};

struct PtxTrack {
    std::string name;
    std::vector<PtxRegion> regions;  // active playlist only
};

struct PtxDocument {
    std::uint32_t sample_rate = 0;
    std::string version;  // as reported by the decoder, for error messages
    std::vector<PtxAudioSource> sources;
    std::vector<PtxTrack> tracks;
    // Features seen and not carried (plugins, automation, MIDI, inactive
    // playlists); the importer copies them to ConversionReport::dropped.
    std::vector<std::string> unsupported;
};

// Decodes a file into a PtxDocument. Implementations may link third-party
// code; they run only inside sonora-convert.
class PtxReader {
public:
    virtual ~PtxReader() = default;
    virtual bool read(const std::string& path, PtxDocument& out, std::string& error) = 0;
};

}  // namespace sonora::interchange
