// SPDX-License-Identifier: GPL-3.0-or-later
//
// The backend conformance suite (ADR-0007 §7, M3). Every guarantee the
// AudioBackend contract states, as a check any backend can be run through:
// drivers/fake in ctest on every build, drivers/jack against PipeWire in
// tools/test-engine-docker.sh. Running both through one suite is what keeps
// the fake from drifting away from the backend it stands in for.
//
// Platform-neutral like the contract: standard C++17 only. What a backend
// cannot do from inside the process (break its server) the Rig does, and a
// fault the Rig cannot cause here is reported as skipped, never as passed.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "sonora/audio_backend.h"

namespace sonora::conformance {

enum class Fault {
    Xrun,          // frames lost: not fatal, counted, frame_time jumps
    ClockStep,     // the server's counter steps back: not fatal, counted
    BufferChange,  // fatal, refused before process()
    RateChange,    // fatal, refused before process()
    Stall,         // no period for over a second: fatal
    Unplug,        // the device feeding the inputs goes: fatal
    ServerLoss,    // fatal; last, since a rig may not get its server back
};

const char* fault_name(Fault f);

// What the suite needs from whoever runs a backend through it.
class Rig {
public:
    virtual ~Rig() = default;

    // A fresh, unopened backend. Every check gets its own.
    virtual std::unique_ptr<backend::AudioBackend> make() = 0;
    // The rate the server runs at, which open() must accept.
    virtual uint32_t rate() = 0;
    // Lets about `periods` server periods go by on an active backend.
    virtual void pass(backend::AudioBackend& backend, uint32_t periods) = 0;
    // True when pass() runs exactly `periods` periods before returning, as
    // drivers/fake's run_blocks() does. Only then can the suite check what
    // one given period did, such as the first at a new rate being refused.
    virtual bool exact() const = 0;

    enum class Injected {
        Caused,       // the server now has the fault
        Unsupported,  // this rig has no way to cause it: skipped
        // The rig asked and the server refused: a server that pins its
        // quantum and rate (audio-stack/pipewire-sonora.conf) never runs
        // the period the check is about. Skipped with held_reason(), after
        // checking the backend carried on unharmed.
        Held,
    };
    // Causes `fault` on `backend`, which is active and fed by
    // unplug_device() when the fault is Unplug.
    virtual Injected inject(backend::AudioBackend& backend, Fault fault) = 0;
    // Why the server refused `fault`, for the skip line.
    virtual std::string held_reason(Fault) { return "the server refused the change"; }
    // Undoes what inject() did to the server. Called once a fatal fault has
    // been seen, before the suite checks that no process() follows, so a
    // server that resumes cannot slip a period past the check.
    virtual void restore(backend::AudioBackend&, Fault) {}
    // The capture device inject(Unplug) removes; empty if none.
    virtual std::string unplug_device() = 0;
};

struct Result {
    int passed = 0;
    int failed = 0;
    int skipped = 0;
};

// One fault check on its own: "" when it passes, "SKIP: why", or the
// failure. run() reports each of these; a test of the suite itself calls it.
std::string check_fault(Rig& rig, Fault fault);

// Runs every check against `rig`, one line per check to `out`:
// "ok <name>", "FAIL <name>: <why>", "SKIP <name>: <why>".
Result run(Rig& rig, const std::string& rig_name, std::FILE* out);

}  // namespace sonora::conformance
