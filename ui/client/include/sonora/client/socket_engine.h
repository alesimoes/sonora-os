// SPDX-License-Identifier: GPL-3.0-or-later
//
// SocketEngine: EngineControl over the engine's control socket and its
// read-only state block (ADR-0007 §3, M7). The interfaces' side of the
// process split; what InProcessEngine was to them before.
//
// It is single-threaded, on the interface's own tick. connect() may wait
// (for an engine that is still starting); poll() and the sends never do:
// requests queue and go out as the socket takes them, and what the engine
// sent is read without waiting.
//
// When the connection goes (an engine that crashed or restarted, a socket
// closed for any reason) the StateView keeps reading the last block, whose
// heartbeat stands still, so the interface shows "no data" as ADR-0007 §3
// asks. poll() then reconnects on its own. The engine greets every new
// connection with Hello and a Snapshot of the document and its undo window,
// which is the resync: a Replica fed by this class undoes and redoes in step
// with the engine after it. A request sent while there is no connection is
// dropped and never answered, as by a dead engine; the stopped heartbeat
// is what the interface shows. So a request without a reply when the
// connection goes never gets one: an interface that keeps requests in
// flight watches connected() and greetings() and drops them then
// (ControlAdapter::watchConnection, ReplicaDocument::set_link), and never
// replays them on the new connection's Snapshot.
//
// No socket or mapping code here: common/platform has it (ADR-0008 P9).
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "sonora/api/engine_control.h"
#include "sonora/api/wire.h"

namespace sonora::platform {
class ControlChannel;
class SharedRegion;
}  // namespace sonora::platform

namespace sonora::client {

class SocketEngine final : public api::EngineControl {
public:
    struct Options {
        // Empty: platform::default_control_endpoint().
        std::string endpoint;
        // How long connect() keeps trying an engine that is not listening
        // yet, and waits for its Hello.
        int connect_timeout_ms = 3000;
        // Between reconnection attempts in poll(). 0: never reconnect.
        uint64_t reconnect_period_ns = 250'000'000;
        // What may wait to be sent before the engine is taken as not
        // reading and the connection is dropped.
        std::size_t max_queued_bytes = std::size_t{8} << 20;
        // The clock reconnection is paced on; steady_clock by default.
        std::function<uint64_t()> now_ns;
    };

    explicit SocketEngine(Options options);
    ~SocketEngine() override;

    // Fails, with a reason to show the operator, when no engine answers
    // within the timeout or it speaks another version.
    bool connect(std::string& error) override;
    const api::Hello& hello() const override { return hello_; }
    uint64_t send(const api::CommandRequest& request) override;
    uint64_t send(const api::ParameterChange& change) override;
    uint64_t send(const api::PluginParameterChange& change) override;
    uint64_t submit(const api::ModelRequest& request) override;
    void poll(api::Inbox& inbox) override;
    api::StateView& state() override { return view_; }

    // A greeted connection now.
    bool connected() const { return greeted_; }
    // Hellos received: 1 after connect(), one more per reconnection.
    uint64_t greetings() const { return greetings_; }
    // Why the last connection ended or the last attempt failed.
    const std::string& last_error() const { return last_error_; }
    // The last engine to greet spoke another protocol or state layout
    // version. poll() then stops reconnecting: the engine at the endpoint is
    // still that build, and retrying every period cannot change it. Only a
    // call to connect() tries again.
    bool refused_version() const { return refused_version_; }
    // What to tell the operator about the link: empty while greeted; else
    // last_error(), and for a version refusal that retrying has stopped.
    std::string link_problem() const;
    const std::string& endpoint() const { return opts_.endpoint; }

    // Drops the connection as a crash of the engine's side would look. The
    // next poll() reconnects. For tests, and an interface that wants a
    // fresh start.
    void drop_connection(const std::string& why);

private:
    bool open();
    // Sends what is queued and reads what has arrived, waiting up to
    // `wait_ms` for the first datagram.
    void pump(int wait_ms);
    void take(const std::string& message);
    uint64_t queue(const std::string& message, uint64_t seq);
    uint64_t now() const;

    Options opts_;
    std::unique_ptr<platform::ControlChannel> channel_;
    // The region of the connection being greeted, then the one view_ reads.
    // The old one stays mapped until a new Hello replaces it, so the view
    // never points at unmapped memory.
    std::unique_ptr<platform::SharedRegion> incoming_region_;
    std::unique_ptr<platform::SharedRegion> region_;
    api::wire::Reassembler reassembler_;
    std::deque<std::string> out_;   // frames
    std::size_t queued_ = 0;

    api::Hello hello_;
    api::StateView view_;
    api::Inbox held_;   // arrived, not yet handed to poll()
    uint64_t seq_ = 0;
    bool greeted_ = false;
    bool refused_version_ = false;
    uint64_t greetings_ = 0;
    uint64_t next_attempt_ns_ = 0;
    std::string last_error_;
};

}  // namespace sonora::client
