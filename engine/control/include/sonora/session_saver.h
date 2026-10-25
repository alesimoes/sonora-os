// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's saver thread (session-format.md, "Saving"; ADR-0015 §6 and
// its update of 2026-10-25): File > Save's writes, off the document thread,
// paced behind the disk thread's checkpoints while a take is open.
//
// The control thread hands it a save: the session file's path, the bytes a
// client serialised, and the engine's document as it is now (immutable and
// shared, so it is read here while the control thread goes on). The saver
// writes the sequence files those bytes name and then the session file
// (common/session_files), cleans up, and hands back the result, which the
// control thread sends as the client's reply.
//
// Pacing, while a take is open: on ext4 every fdatasync and folder fsync
// forces a journal commit, which under data=ordered also flushes the take
// files' dirty pages, so a save could push the disk thread's own syncs
// into a capture overrun. The saver therefore waits for the disk thread's
// checkpoint counter to move after the save arrived, and only then touches
// the disk: its commits sit just behind a checkpoint, never in front of
// one, and flush only what was written since it. One save per bump, so at
// most one batch per checkpoint period. Cleanup after such a save waits for
// the transport to stop. The disk thread never waits for the saver.
//
// Never on the audio thread. Its own thread may block on the medium for as
// long as the medium takes; nothing else waits for it.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/session_files/session_folder.h"

namespace sonora {

class SessionSaver {
public:
    // The disk thread's checkpoint passes so far (DiskThread::checkpoints()),
    // asked from the saver thread.
    using Checkpoints = std::function<uint32_t()>;
    // Called on the saver thread each time a result is ready to take.
    using Ready = std::function<void()>;

    struct Job {
        uint64_t client = 0;
        uint64_t seq = 0;
        std::string file;
        std::string bytes;
        model::Snapshot document;
    };
    struct Done {
        uint64_t client = 0;
        uint64_t seq = 0;   // 0: no reply, only `notice` (a deferred cleanup's)
        bool ok = false;
        // The reason when not ok; a caution, or empty, when ok.
        std::string message;
        // For every client: what a cleanup could not do.
        std::string notice;
        // The save waited for a checkpoint.
        bool paced = false;
    };

    SessionSaver(Checkpoints checkpoints, Ready ready,
                 std::chrono::milliseconds poll = std::chrono::milliseconds(2));
    ~SessionSaver();
    SessionSaver(const SessionSaver&) = delete;
    SessionSaver& operator=(const SessionSaver&) = delete;

    void start();
    // Finishes the saves already handed over, unpaced, then joins. A
    // deferred cleanup is dropped: the next save's cleanup covers it.
    void stop();

    // --- the control thread ---
    // Whether a take is open and whether the transport is stopped, as the
    // control thread last saw them; set every turn of its loop.
    void set_take_open(bool open);
    void set_transport_stopped(bool stopped);
    void submit(Job job);
    std::vector<Done> take_done();

    // For the tests and the measurement: saves that waited for a
    // checkpoint, and cleanups run.
    uint64_t paced_saves() const { return paced_.load(std::memory_order_acquire); }
    uint64_t cleanups() const { return cleanups_.load(std::memory_order_acquire); }
    // Saves handed over and not yet taken back with take_done().
    std::size_t pending() const;

private:
    void run();
    // Waits, when a take is open, for the checkpoint counter to move.
    // True when it waited.
    bool pace(std::unique_lock<std::mutex>& lock);
    void clean(Done& out);

    Checkpoints checkpoints_;
    Ready ready_;
    std::chrono::milliseconds poll_;
    session_files::SessionFolder folder_;   // the saver thread's alone

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::vector<Done> done_;
    std::size_t in_hand_ = 0;   // submitted, not yet taken back
    bool stopping_ = false;
    bool take_open_ = false;
    bool stopped_ = true;
    bool clean_pending_ = false;
    std::thread thread_;

    std::atomic<uint64_t> paced_{0};
    std::atomic<uint64_t> cleanups_{0};
};

}  // namespace sonora
