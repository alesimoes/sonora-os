// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <thread>
#include <vector>

namespace sonora {

class Track;

// Drains every armed track's capture ring into its file. Runs at normal
// priority: it is allowed to block on I/O, which is exactly why it is not the
// audio callback.
class DiskThread {
public:
    DiskThread(std::vector<Track*> tracks, std::size_t chunk_frames);
    ~DiskThread();

    void start();
    void stop();

    // Total frames the callback could not hand off, summed across tracks.
    uint32_t total_overruns() const;

private:
    void run();
    void drain_once();

    std::vector<Track*> tracks_;
    std::size_t chunk_frames_;
    // Owned by the disk thread only. Grown on first use, then reused, so
    // the 10 ms drain loop does not allocate on every wake.
    std::vector<float> scratch_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace sonora
