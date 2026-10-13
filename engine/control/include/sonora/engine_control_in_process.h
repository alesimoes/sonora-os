// SPDX-License-Identifier: GPL-3.0-or-later
//
// InProcessEngine: EngineControl over the real Session, in this process
// (ADR-0007 §3, until M7).
//
// The client half of the seam. Requests are encoded and handed to an
// EngineControlServer running its control thread here; what comes back is
// decoded from the server's bytes; the state block is anonymous memory the
// server owns, read through a StateView. So every message crosses the wire
// codec exactly as it will across a socket, and M7 replaces this class with
// SocketEngineClient and changes no interface code.
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "sonora/api/engine_control.h"
#include "sonora/engine_control_server.h"

namespace sonora {

class InProcessEngine final : public api::EngineControl {
public:
    InProcessEngine(Session& session, backend::AudioBackend* audio,
                    EngineControlServer::Options options = {});
    ~InProcessEngine() override;

    // Starts the server, then checks its Hello as a remote client would.
    bool connect(std::string& error) override;
    const api::Hello& hello() const override { return hello_; }
    uint64_t send(const api::CommandRequest& request) override;
    uint64_t send(const api::ParameterChange& change) override;
    uint64_t submit(const api::ModelRequest& request) override;
    void poll(api::Inbox& inbox) override;
    api::StateView& state() override { return view_; }

    // The engine side stops: no more publishes, no more answers. What a
    // crashed engine process looks like from the client, and the first
    // half of an orderly shutdown. Idempotent.
    void shutdown() { server_.stop(); }

    EngineControlServer& server() { return server_; }

private:
    void receive(std::string bytes);

    EngineControlServer server_;
    api::Hello hello_;
    api::StateView view_;
    uint64_t seq_ = 0;
    bool connected_ = false;

    std::mutex mutex_;
    std::deque<std::string> inbound_;   // engine -> client, encoded
};

}  // namespace sonora
