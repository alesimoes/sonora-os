// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/client/socket_engine.h"

#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include "sonora/platform/ipc.h"

namespace sonora::client {

namespace {

uint64_t steady_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

template <typename T>
void append(std::vector<T>& to, std::vector<T>& from) {
    for (T& x : from) to.push_back(std::move(x));
    from.clear();
}

}  // namespace

SocketEngine::SocketEngine(Options options) : opts_(std::move(options)) {
    if (opts_.endpoint.empty()) opts_.endpoint = platform::default_control_endpoint();
}

SocketEngine::~SocketEngine() = default;

uint64_t SocketEngine::now() const { return opts_.now_ns ? opts_.now_ns() : steady_ns(); }

bool SocketEngine::open() {
    std::string error;
    channel_ = platform::ControlChannel::connect(opts_.endpoint, error);
    if (!channel_) {
        last_error_ = error;
        return false;
    }
    greeted_ = false;
    incoming_region_.reset();
    reassembler_ = api::wire::Reassembler();
    out_.clear();
    queued_ = 0;
    return true;
}

void SocketEngine::drop_connection(const std::string& why) {
    if (channel_) channel_->shutdown();
    channel_.reset();
    greeted_ = false;
    out_.clear();
    queued_ = 0;
    last_error_ = why;
    next_attempt_ns_ = now() + opts_.reconnect_period_ns;
}

void SocketEngine::take(const std::string& message) {
    std::string error;
    if (!greeted_) {
        // The first message on a connection is Hello, and its first frame
        // brought the state block.
        uint32_t protocol = 0, layout = 0;
        if (!api::wire::decode_hello_versions(message, protocol, layout, error)) {
            drop_connection("the engine did not say hello: " + error);
            return;
        }
        error = api::wire::version_mismatch(protocol, layout);
        if (!error.empty()) {
            refused_version_ = true;
            drop_connection(error);
            return;
        }
        api::Hello hello;
        if (!api::wire::decode(message, hello, error)) {
            drop_connection("the engine's hello does not parse: " + error);
            return;
        }
        if (!incoming_region_) {
            drop_connection("the engine's hello came without its state block");
            return;
        }
        if (!view_.attach(incoming_region_->data(), incoming_region_->bytes(), error)) {
            drop_connection(error);
            return;
        }
        region_ = std::move(incoming_region_);
        hello_ = std::move(hello);
        greeted_ = true;
        refused_version_ = false;
        ++greetings_;
        last_error_.clear();
        return;
    }

    // What does not decode is dropped: the engine is the trusted side, and
    // a newer one's message this build cannot read must not end the session.
    api::wire::decode_into(message, held_, error);
}

void SocketEngine::pump(int wait_ms) {
    std::string error;
    while (channel_ && !out_.empty()) {
        const platform::IoStatus s = channel_->send(out_.front(), 0, error);
        if (s == platform::IoStatus::WouldBlock) break;
        if (s != platform::IoStatus::Ok) {
            drop_connection(s == platform::IoStatus::Closed ? "the engine closed the connection"
                                                            : error);
            return;
        }
        queued_ -= out_.front().size();
        out_.pop_front();
    }

    std::string frame, message;
    int wait = wait_ms;
    while (channel_) {
        std::unique_ptr<platform::SharedRegion> region;
        const platform::IoStatus s =
            channel_->receive(frame, api::wire::kMaxFrameBytes, &region, wait, error);
        wait = 0;
        if (s == platform::IoStatus::WouldBlock) return;
        if (s != platform::IoStatus::Ok) {
            drop_connection(s == platform::IoStatus::Closed ? "the engine closed the connection"
                                                            : error);
            return;
        }
        if (region) incoming_region_ = std::move(region);
        switch (reassembler_.add(frame, message, error)) {
            case api::wire::Reassembler::Result::Partial: break;
            case api::wire::Reassembler::Result::Complete: take(message); break;
            case api::wire::Reassembler::Result::Error:
                drop_connection("the engine framed a message wrongly: " + error);
                return;
        }
    }
}

bool SocketEngine::connect(std::string& error) {
    if (greeted_) return true;
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(opts_.connect_timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (!channel_ && !open()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        pump(50);
        if (greeted_) {
            // Whatever followed Hello waits in held_ for the first poll().
            return true;
        }
        if (refused_version_) break;
    }
    if (refused_version_) {
        error = last_error_;
    } else if (channel_) {
        error = "the engine at " + opts_.endpoint + " did not say hello in time";
    } else {
        error = "no engine answered on " + opts_.endpoint +
                (last_error_.empty() ? std::string() : ": " + last_error_);
    }
    channel_.reset();
    return false;
}

uint64_t SocketEngine::queue(const std::string& message, uint64_t seq) {
    // Dropped, unanswered: a dead engine replies to nothing (the conformance
    // suite's heartbeat check), and the interface already says from the
    // heartbeat that it is not answering.
    if (!greeted_) return seq;
    std::vector<std::string> frames;
    std::string error;
    if (!api::wire::split_frames(message, frames, error)) {
        api::CommandReply r;
        r.seq = seq;
        r.result = DispatchResult::Failed;
        r.message = error;
        held_.replies.push_back(std::move(r));
        return seq;
    }
    for (std::string& f : frames) {
        queued_ += f.size();
        out_.push_back(std::move(f));
    }
    if (queued_ > opts_.max_queued_bytes) {
        drop_connection("the engine stopped reading requests");
        return seq;
    }
    pump(0);
    return seq;
}

uint64_t SocketEngine::send(const api::CommandRequest& request) {
    api::CommandRequest r = request;
    r.seq = ++seq_;
    std::string bytes;
    api::wire::encode(r, bytes);
    return queue(bytes, r.seq);
}

uint64_t SocketEngine::send(const api::ParameterChange& change) {
    api::ParameterChange c = change;
    c.seq = ++seq_;
    std::string bytes;
    api::wire::encode(c, bytes);
    return queue(bytes, c.seq);
}

uint64_t SocketEngine::send(const api::PluginParameterChange& change) {
    api::PluginParameterChange c = change;
    c.seq = ++seq_;
    std::string bytes;
    api::wire::encode(c, bytes);
    return queue(bytes, c.seq);
}

uint64_t SocketEngine::submit(const api::ModelRequest& request) {
    api::ModelRequest m = request;
    m.seq = ++seq_;
    std::string bytes;
    api::wire::encode(m, bytes);
    return queue(bytes, m.seq);
}

std::string SocketEngine::link_problem() const {
    if (greeted_) return {};
    if (refused_version_) {
        return last_error_ + ". The interface has stopped reconnecting: retrying cannot fix this";
    }
    return last_error_;
}

void SocketEngine::poll(api::Inbox& inbox) {
    if (channel_) {
        pump(0);
    } else if (!refused_version_ && opts_.reconnect_period_ns != 0 &&
               now() >= next_attempt_ns_) {
        // Connecting to a Unix socket answers at once, listening or not, so
        // this costs the tick nothing; the Hello is read on later ticks.
        next_attempt_ns_ = now() + opts_.reconnect_period_ns;
        if (open()) pump(0);
    }
    append(inbox.replies, held_.replies);
    append(inbox.events, held_.events);
    append(inbox.model, held_.model);
    append(inbox.catalogs, held_.catalogs);
}

}  // namespace sonora::client
