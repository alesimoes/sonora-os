// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sonora/disk_thread.h"
#include "sonora/process_callback.h"
#include "sonora/rt.h"
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
// process() from its own real-time thread, through ProcessCallback; it never
// sees Session itself. Final because the base has no virtual destructor.
class Session final : public backend::ProcessCallback {
public:
    // The highest take number a session writes. Four digits is far more
    // takes than a session holds, and a bound is what lets the folder scan
    // ignore a stray "Audio 1_20261012.wav" instead of numbering after it.
    static constexpr int kMaxTake = 9999;

    explicit Session(SessionConfig cfg);
    ~Session();

    bool create_dirs();

    // One more than the highest take number among the files in the session's
    // audio folder that are named exactly as start_recording() names them
    // for any track the session could have -- "Audio 12_05.wav" counts even
    // when the session is reopened with 8 tracks -- so a take is never
    // numbered like one already on disk and no stray file can push the
    // count. Can return kMaxTake + 1, which start_recording() refuses with
    // a reason.
    // Reads the directory: never call it from the audio thread.
    int next_free_take() const;

    // Opens a file per armed track and flips the transport to Recording.
    // All or nothing: if any armed track's file cannot be created -- above
    // all because it already exists -- nothing is recorded, the files this
    // call created are removed, and `error`, if given, says which file and
    // why.
    bool start_recording(int take_number, std::string* error = nullptr);
    // Closes every open file and stops the transport. Returns false if the
    // take it ends failed on any track -- a disk error, or the file reaching
    // the 4 GB WAV limit -- and `error`, if given, says which file and why.
    // The files are closed either way, and one that reached the limit is
    // valid up to its last whole frame.
    bool stop(std::string* error = nullptr);
    // While a take runs: empty, or which file failed and why, in the words
    // stop() will use. Writing stops at the failure, so the operator is told
    // now rather than when the take ends. Control thread; allocates.
    std::string take_failure() const;

    // --- real-time safe: called from the audio callback ---
    // block.inputs[i] feeds track i; a null input, or a track past
    // block.input_count, is unconnected. Every output is written silent:
    // nothing is played back yet.
    void process(const backend::AudioBlock& block) noexcept SONORA_RT override;

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
