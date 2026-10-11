// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/disk_thread.h"

#include <chrono>
#include <vector>

#include "sonora/track.h"

namespace sonora {

DiskThread::DiskThread(std::vector<Track*> tracks, std::size_t chunk_frames)
    : tracks_(std::move(tracks)), chunk_frames_(chunk_frames) {}

DiskThread::~DiskThread() { stop(); }

void DiskThread::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&DiskThread::run, this);
}

void DiskThread::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    // One final pass so nothing still sitting in a ring is lost.
    drain_once();
}

uint32_t DiskThread::total_overruns() const {
    uint32_t sum = 0;
    for (const Track* t : tracks_) sum += t->overruns();
    return sum;
}

void DiskThread::run() {
    // Buffer sized for the widest track we may encounter.
    std::vector<float> scratch;
    while (running_.load(std::memory_order_relaxed)) {
        drain_once();
        // Wake often enough that the rings never approach full, but not so
        // often that we spin. Rings hold seconds; this polls every 10 ms.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void DiskThread::drain_once() {
    std::vector<float> scratch;
    for (Track* t : tracks_) {
        if (!t->writer().is_open()) continue;
        RingBuffer<float>& ring = t->capture_ring();
        const std::size_t ch = t->channels();

        while (true) {
            const std::size_t avail_samples = ring.read_space();
            // Only move whole frames so the file never desynchronises.
            const std::size_t avail_frames = avail_samples / ch;
            if (avail_frames == 0) break;

            const std::size_t frames = std::min(avail_frames, chunk_frames_);
            const std::size_t samples = frames * ch;
            if (scratch.size() < samples) scratch.resize(samples);

            ring.read(scratch.data(), samples);
            t->writer().write(scratch.data(), frames);
        }
    }
}

}  // namespace sonora
