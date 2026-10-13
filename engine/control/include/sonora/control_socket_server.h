// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control server on a socket (ADR-0007 §3, M7): each connection a
// listener accepts becomes one EngineControlServer client. The first
// datagram a client receives, its Hello's first frame, carries the state
// block's region, which the client maps read-only.
//
// No socket or mapping code here: that is common/platform's (ADR-0008 P9).
//
// Each connection has a reader thread, which hands frames to the server,
// and a writer thread, which drains what the server queued for it. So the
// control thread never waits on a client: a client that stops reading only
// fills its own queue, and one whose queue passes the bound is dropped.
// A client that disconnects, crashes, or is refused for what it sent costs
// the engine one connection and nothing else; the take goes on.
//
// Nothing here is on the audio thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sonora/api/wire.h"
#include "sonora/engine_control_server.h"
#include "sonora/platform/ipc.h"

namespace sonora {

class ControlSocketServer {
public:
    struct Options {
        // An interface, a terminal, a control surface, and room for tools.
        std::size_t max_clients = 16;
        // What may wait for one client: a whole Snapshot and change. Past
        // it the client is not reading, and it is dropped.
        std::size_t max_queued_bytes = api::wire::kMaxMessageBytes + (std::size_t{16} << 20);
    };

    // `server` holds its state block in `state`. `listener` is listening
    // already, so a second engine on the same endpoint has been refused
    // before anything was opened. All three outlive this object.
    ControlSocketServer(EngineControlServer& server, const platform::SharedRegion& state,
                        std::unique_ptr<platform::ControlListener> listener, Options options);
    ControlSocketServer(EngineControlServer& server, const platform::SharedRegion& state,
                        std::unique_ptr<platform::ControlListener> listener)
        : ControlSocketServer(server, state, std::move(listener), Options{}) {}
    ~ControlSocketServer();
    ControlSocketServer(const ControlSocketServer&) = delete;
    ControlSocketServer& operator=(const ControlSocketServer&) = delete;

    // Accepts from now on. The EngineControlServer must be running.
    void start();
    // Closes every connection and stops accepting. Idempotent.
    void stop();

    const std::string& endpoint() const { return listener_->endpoint(); }
    std::size_t connections() const;
    // Connections accepted since start.
    uint64_t accepted() const { return accepted_.load(std::memory_order_acquire); }

private:
    struct Connection;

    void accept_loop();
    void open(std::unique_ptr<platform::ControlChannel> channel);
    void reap(bool all);

    EngineControlServer& server_;
    const platform::SharedRegion& state_;
    std::unique_ptr<platform::ControlListener> listener_;
    Options opts_;

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Connection>> connections_;
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> accepted_{0};
    std::thread acceptor_;
};

}  // namespace sonora
