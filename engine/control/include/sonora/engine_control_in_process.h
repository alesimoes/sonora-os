// SPDX-License-Identifier: GPL-3.0-or-later
//
// InProcessEngine: EngineControl over the real Session, in this process.
//
// The client half of the seam. Requests are encoded and handed to an
// EngineControlServer running its control thread here; what comes back is
// decoded from the server's bytes; the state block is anonymous memory the
// server owns, read through a StateView. So every message crosses the wire
// codec exactly as it does across the socket. Since M7 no interface uses
// it (they are separate processes, on ui/client's SocketEngine); it stays
// as the conformance suite's real engine with no transport in the way.
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "sonora/api/engine_control.h"
#include "sonora/api/wire.h"
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
    uint64_t send(const api::PluginParameterChange& change) override;
    uint64_t submit(const api::ModelRequest& request) override;
    void poll(api::Inbox& inbox) override;
    api::StateView& state() override { return view_; }

    // The engine side stops: no more publishes, no more answers. What a
    // crashed engine process looks like from the client, and the first
    // half of an orderly shutdown. Idempotent.
    void shutdown() { server_.stop(); }

    EngineControlServer& server() { return server_; }

private:
    void receive(std::string frame);
    uint64_t deliver(const std::string& message, uint64_t seq);
    // The next whole message from the server, if its frames are here.
    bool next_message(std::string& message);

    EngineControlServer server_;
    api::Hello hello_;
    api::StateView view_;
    uint64_t seq_ = 0;
    bool connected_ = false;

    std::mutex mutex_;
    std::deque<std::string> inbound_;   // engine -> client, frames
    api::wire::Reassembler reassembler_;   // the client thread's
};

}  // namespace sonora
