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

// Lowers the calling thread only: its CPU priority to nice `nice` (a
// thread already nicer stays as it is) and, with `idle_io`, the idle I/O
// class. For a
// background worker whose own file reads and whose children must not
// compete with the session (the plugin scanner). Children it starts
// inherit both. False, with `error`, when the OS refused; the thread keeps
// running at its old priority. Irreversible without privileges: only for a
// thread that stays background for good.
bool lower_current_thread(int nice, bool idle_io, std::string& error);

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
        // Background work (a plugin probe, ADR-0012 amendment of 2026-10-19,
        // changes A and D): a lower CPU priority, and the idle I/O class, so
        // it competes with neither the hosts nor the disk thread. POSIX:
        // nice and, on Linux, IOPRIO_CLASS_IDLE. Never raises a priority;
        // a child of a thread already lowered (lower_current_thread) starts
        // lowered, with no window at the caller's priority.
        int nice = 0;
        bool idle_io = false;
        // A process group of its own (POSIX setpgid(0, 0) at spawn): a
        // Ctrl+C or a signal to the engine's group does not reach it, so
        // only its owner ends it (code-reviewer finding 1 on #95); and
        // terminate() and kill() signal the whole group, so what it forked
        // ends with it.
        bool own_group = false;
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
    // For a poll set, never for I/O: readable (POLLIN) once it has exited,
    // so an owner can sleep until a child dies instead of polling running()
    // (ADR-0012 §4). A Linux pidfd, close-on-exec, valid for this object's
    // life; -1 where the OS has none, and then only running() tells.
    int exit_handle() const;
    // True once it has exited, waiting up to `timeout_ms` (-1: for ever).
    // Sleeps on exit_handle() when there is one; otherwise polls.
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
