// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sonora/ringbuffer.h"

namespace sonora {

class Track;

// Where a track's capture window opens or closes, in band with its audio
// (timeline-model.md, "Closing a take while rolling"). `at` counts the
// samples the audio thread has written to that track's capture ring since
// the session was created; the ring is never reset, so the disk thread's
// own count of samples read is the same clock. The audio thread pushes a
// record after the samples before `at`, so the disk thread never sees a
// boundary ahead of its audio. Trivial, so the ring holds it as bytes.
struct CaptureBoundary {
    enum class Kind : uint32_t { Open = 1, Close = 2 };
    uint32_t track;
    Kind kind;
    uint32_t take;
    uint32_t reserved;
    uint64_t at;
    // Open: the timeline frame of the window's first sample, the file's
    // TimeReference.
    uint64_t timeline;
};

// Drains every track's capture ring and splits it into take files at the
// boundary records. Runs at normal priority: it is allowed to block on I/O,
// which is exactly why it is not the audio callback.
class DiskThread {
public:
    // A take's file on one track, closed: what engine/control places in
    // the session, with the length the file has on disk.
    struct ClosedFile {
        int track = 0;
        int take = 0;
        std::string path;
        uint64_t frames = 0;
        std::string failure;  // empty unless the file failed
    };

    DiskThread(std::vector<Track*> tracks, std::size_t chunk_frames,
               RingBuffer<CaptureBoundary>& boundaries);
    ~DiskThread();

    void start();
    // Joins the thread, then drains what is left, on the caller's thread.
    void stop();
    bool running() const { return running_.load(std::memory_order_relaxed); }

    // One pass: every boundary record, every ring. The disk thread's loop;
    // also the caller's, while the thread is not running.
    void drain_once();
    // While the thread is not running: closes every file a slot still
    // holds, at what has been drained. For a take whose close record never
    // came: the transport stopped, and no callback ran after it.
    void close_all();

    // Tests only: called in every pass once the boundary records are taken
    // and before any ring is drained, so a test can land the audio
    // thread's writes exactly there. Set while the thread is not running.
    using PassHook = void (*)(void* context);
    void set_pass_hook(PassHook hook, void* context) {
        hook_ = hook;
        hook_context_ = context;
    }

    // Files closed since the last call, in the order they were closed.
    // Any thread.
    std::vector<ClosedFile> take_closed();

    // Total frames the callback could not hand off, summed across tracks.
    uint32_t total_overruns() const;

private:
    struct TrackState {
        uint64_t consumed = 0;  // samples read from the ring, ever
        // How far this pass may read: the ring's fill, read before the
        // boundary records were.
        uint64_t visible = 0;
        int active = -1;        // the writer slot the ring drains into
        std::deque<CaptureBoundary> pending;
    };

    void run();
    void drain_track(std::size_t i);
    // Reads `n` samples from track i's ring into its active file, or
    // nowhere when no window is open.
    void move(std::size_t i, uint64_t n);
    void apply(std::size_t i, const CaptureBoundary& b);
    void finish(std::size_t i, int slot);

    std::vector<Track*> tracks_;
    std::size_t chunk_frames_;
    RingBuffer<CaptureBoundary>& boundaries_;
    std::vector<TrackState> state_;
    // Owned by whichever thread drains; sized here, never grown.
    std::vector<float> scratch_;
    std::mutex closed_mutex_;
    std::vector<ClosedFile> closed_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    PassHook hook_ = nullptr;
    void* hook_context_ = nullptr;
};

}  // namespace sonora
