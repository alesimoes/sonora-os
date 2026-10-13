// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's side of the control API (ADR-0007 §4.1): where engine/core
// and contracts/api meet, and nowhere else.
//
// It owns the state block and its publisher, the command handlers
// (install_session_commands), and the session document (ADR-0009 §2), and
// runs a control thread that takes requests as encoded bytes, handles them
// in order and answers as encoded bytes. It knows no transport: bytes come
// in through deliver() and go out through the Outbound callback. In process
// that is InProcessEngine's queue; at M7 it is a socket, and the state
// block moves to a memfd. Nothing else changes.
//
// Nothing here runs on the audio thread. The handlers reach it only
// through the atomics Track and Transport already have; the publisher
// reads those atomics on its own thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sonora/api/messages.h"
#include "sonora/api/state_block.h"
#include "sonora/api/wire.h"
#include "sonora/command.h"
#include "sonora/session_commands.h"
#include "sonora/state_publisher.h"

namespace sonora {

class Session;
namespace backend {
class AudioBackend;
}
namespace model {
class Document;
}

class EngineControlServer {
public:
    using Outbound = std::function<void(std::string bytes)>;

    struct Options {
        // 100 Hz: inside the 100 Hz - 1 kHz the measurement found clean,
        // and the meters' ballistics need no more.
        uint64_t publish_period_ns = 10'000'000;
        // Asked before every take (session_commands.h).
        MayRecord may_record;
        // What the wiring layer keeps an eye on (inputs, the device
        // fallback), run on the control thread every housekeeping period.
        // It may call post_event() and the input setters.
        std::function<void(EngineControlServer&)> housekeeping;
        uint64_t housekeeping_period_ns = 1'000'000'000;
        // What Hello announces. Only the conformance suite changes these,
        // to check that a client refuses a mismatch.
        uint32_t protocol_version = api::kProtocolVersion;
        uint32_t state_layout_version = api::kStateLayoutVersion;
    };

    // `audio` may be null: then there are no xrun counts, no fatal check,
    // and buffer_frames is 0. Sizes the state block for the session's
    // tracks, here and once.
    EngineControlServer(Session& session, backend::AudioBackend* audio, Options options);
    ~EngineControlServer();
    EngineControlServer(const EngineControlServer&) = delete;
    EngineControlServer& operator=(const EngineControlServer&) = delete;

    // Sends Hello, then the document's snapshot, then any event posted
    // before start, through `out`; then starts the publisher and the
    // control thread. `out` is called on the control thread from then on.
    void start(Outbound out);
    // Joins both threads. Idempotent. Requests delivered afterwards are
    // dropped: a stopped engine answers nothing.
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    // One frame of a client's request (api/wire.h). Any thread; never
    // waits for the request to be handled. Outbound gets frames too.
    void deliver(std::string frame);

    const void* state_block() const { return block_.data(); }
    std::size_t state_block_bytes() const { return block_.bytes(); }

    // --- the control thread, housekeeping, or before start() ---
    void post_event(const api::EngineEvent& event);
    void set_input_connected(uint32_t track, bool connected) {
        publisher_->set_input_connected(track, connected);
    }
    void set_inputs_connected(uint32_t count, bool none_is_a_problem);

    // Engine-domain commands with a handler, in catalog order.
    const std::vector<std::string>& implemented() const { return implemented_; }

private:
    void run();
    void handle(const std::string& bytes);
    void tick();
    void send(const std::string& bytes);
    void reply(uint64_t seq, DispatchResult result, const std::string& message);

    Session& session_;
    backend::AudioBackend* audio_;
    Options opts_;

    api::StateBlockMemory block_;
    api::StateWriter writer_;
    std::unique_ptr<StatePublisher> publisher_;
    std::unique_ptr<model::Document> document_;

    CommandRegistry registry_;
    std::vector<std::string> implemented_;
    // The take install_session_commands numbers. The control thread's own;
    // the publisher sees it through set_take().
    int take_ = 1;
    bool quit_ = false;   // app.quit's flag; never set, as the engine never runs it

    Outbound out_;
    std::vector<std::string> early_;   // events posted before start()

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> inbound_;   // frames
    api::wire::Reassembler reassembler_;   // the control thread's
    bool stopping_ = false;
    std::atomic<bool> running_{false};
    std::thread thread_;

    uint64_t next_housekeeping_ns_ = 0;
    bool fatal_said_ = false;
    int failed_take_ = 0;
    bool take_failed_shown_ = false;
};

}  // namespace sonora
