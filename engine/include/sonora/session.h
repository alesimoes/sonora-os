// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sonora/disk_thread.h"
#include "sonora/track.h"
#include "sonora/transport.h"

namespace sonora {

struct SessionConfig {
    std::string name = "Untitled";
    std::string dir;                 // session folder; audio goes in <dir>/Audio Files
    uint32_t sample_rate = 48000;
    int track_count = 8;
    uint16_t channels_per_track = 1;
    WavWriter::Format format = WavWriter::Format::Int24;
    // Seconds of headroom in each capture ring before an overrun is possible.
    double ring_seconds = 4.0;
};

// Owns the tracks, the transport and the disk thread. The audio backend calls
// process() from its own real-time thread.
class Session {
public:
    explicit Session(SessionConfig cfg);
    ~Session();

    bool create_dirs();

    // Opens a file per armed track and flips the transport to Recording.
    bool start_recording(int take_number);
    // Closes every open file and stops the transport.
    void stop();

    // --- real-time safe: called from the audio callback ---
    // inputs[i] is a block of nframes for track i (nullptr if unconnected).
    void process(const float* const* inputs, uint32_t nframes);

    Transport& transport() { return *transport_; }
    const std::vector<std::unique_ptr<Track>>& tracks() const { return tracks_; }
    const SessionConfig& config() const { return cfg_; }
    DiskThread& disk() { return *disk_; }

private:
    SessionConfig cfg_;
    std::unique_ptr<Transport> transport_;
    std::vector<std::unique_ptr<Track>> tracks_;
    std::unique_ptr<DiskThread> disk_;
};

}  // namespace sonora
