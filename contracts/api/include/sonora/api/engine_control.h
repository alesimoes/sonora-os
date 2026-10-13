// SPDX-License-Identifier: GPL-3.0-or-later
//
// The seam every interface codes against (ADR-0007 §4.1).
//
// Implementations:
//   - InProcessEngine (engine/control): the real engine on a thread in this
//     process, every message through the wire codec, the state block in
//     anonymous memory. Until M7.
//   - SocketEngineClient (ui/client, M7): the same over a socket and a
//     read-only memfd. Swapping it in changes no interface code.
//   - FakeEngine (contracts/api/testing): deterministic, owned by the
//     provider, held to the same conformance suite as InProcessEngine.
//
// Every call is made from one client thread (the UI's tick, the terminal
// loop) and none of them waits on the engine.
#pragma once

#include <cstdint>
#include <string>

#include "sonora/api/messages.h"
#include "sonora/api/state_view.h"

namespace sonora::api {

class EngineControl {
public:
    virtual ~EngineControl() = default;

    // Fails, with a reason to show the operator, when the engine cannot be
    // reached or speaks another protocol or state layout version. Nothing
    // else may be called after it fails.
    virtual bool connect(std::string& error) = 0;
    virtual const Hello& hello() const = 0;

    // Each queues a request and returns its seq, which the reply carries.
    // Seqs start at 1 and increase across all three kinds of request.
    virtual uint64_t send(const CommandRequest& request) = 0;
    virtual uint64_t send(const ParameterChange& change) = 0;
    virtual uint64_t submit(const ModelRequest& request) = 0;

    // Appends whatever has arrived since the last call. Called from the
    // client's own tick; never blocks.
    virtual void poll(Inbox& inbox) = 0;

    virtual StateView& state() = 0;
};

}  // namespace sonora::api
