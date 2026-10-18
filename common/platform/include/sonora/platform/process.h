// SPDX-License-Identifier: GPL-3.0-or-later
//
// Child processes: the engine starting its terminal interface, and tests
// that run the engine and a client as separate processes (ADR-0007 §3).
// Not for the audio thread.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sonora::platform {

class SharedRegion;

// The directory this executable was started from, or empty if unknown.
std::string executable_dir();

// This process's id, as a child is told its parent's (a plugin host's
// --engine-pid).
int64_t current_process_id();

class ChildProcess {
public:
    struct Impl;

    // A region the child finds at descriptor `child_fd`, mapped by it as it
    // chooses. `writable` passes the read-write descriptor (only a region
    // from create_shared_writable has one); otherwise the read-only one.
    struct PassedRegion {
        int child_fd = -1;
        const SharedRegion* region = nullptr;
        bool writable = false;
    };

    struct Options {
        // Empty: inherit this process's stream.
        std::string stdout_path;
        std::string stderr_path;
        // Each at its child_fd, 3 or above; nothing else is inherited.
        std::vector<PassedRegion> regions;
        // The child's end of a ControlChannel::pair (close-on-exec here),
        // each placed at its child_fd beside the regions. Every child_fd,
        // region or handle, is 3 to 63 and used once; spawn() refuses
        // otherwise. The caller keeps its own copy and closes it after
        // spawn (close_handle).
        struct PassedHandle {
            int child_fd = -1;
            int handle = -1;
        };
        std::vector<PassedHandle> handles;
    };

    // argv[0] is the program's path. Fails with a reason, never silently.
    static std::unique_ptr<ChildProcess> spawn(const std::vector<std::string>& argv,
                                               const Options& options, std::string& error);

    explicit ChildProcess(std::unique_ptr<Impl> impl);
    // A child still running is killed and reaped: none outlives its owner.
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Reaps it if it has exited; never waits.
    bool running();
    // Its process id, as it would say in a Hello (host_control.h).
    int64_t pid() const;
    // True once it has exited, waiting up to `timeout_ms` (-1: for ever).
    bool wait(int timeout_ms);
    // Asks it to leave (SIGTERM).
    void terminate();
    // Ends it at once, as a crash would (SIGKILL).
    void kill();

    // Valid once running() is false.
    bool exited_normally() const;
    int exit_code() const;       // when exited_normally()
    int signal_number() const;   // when not

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace sonora::platform
