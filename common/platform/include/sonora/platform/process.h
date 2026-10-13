// SPDX-License-Identifier: GPL-3.0-or-later
//
// Child processes: the engine starting its terminal interface, and tests
// that run the engine and a client as separate processes (ADR-0007 §3).
// Not for the audio thread.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace sonora::platform {

// The directory this executable was started from, or empty if unknown.
std::string executable_dir();

class ChildProcess {
public:
    struct Impl;

    struct Options {
        // Empty: inherit this process's stream.
        std::string stdout_path;
        std::string stderr_path;
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
