// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control API's conformance suite (ADR-0007 §7): what every
// EngineControl must do, run against FakeEngine in contracts/api's ctest
// and against InProcessEngine on drivers/fake in engine/control's, so the
// fake cannot drift from the real engine.
//
// A Rig makes engines and lets time pass. The suite drives them only
// through EngineControl and StateView, as an interface would.
#pragma once

#include <cstdint>
#include <memory>

#include "sonora/api/engine_control.h"

namespace sonora::api::conformance {

class Rig {
public:
    virtual ~Rig() = default;
    virtual const char* name() const = 0;
    // A fresh engine, not yet connected, whose Hello announces these
    // versions. Each call is a new engine with a new session.
    virtual std::unique_ptr<EngineControl> make(uint32_t protocol_version,
                                                uint32_t state_layout_version) = 0;
    // The clock StateView::engine_alive() is judged on.
    virtual uint64_t now_ns() = 0;
    // Lets `ns` of that clock pass: a sleep for a real engine, a step of
    // the injected clock for the fake.
    virtual void wait_ns(uint64_t ns) = 0;
    // The engine stops as a crashed process would: nothing more is
    // published, and nothing answers.
    virtual void kill(EngineControl& engine) = 0;
    // Whether the inputs carry a signal, so a published peak is above zero.
    virtual bool has_signal() const = 0;
};

// Runs every check, prints "ok <check>" or "FAIL <check>: <why>" for each,
// and returns the number that failed.
int run(Rig& rig);

}  // namespace sonora::api::conformance
