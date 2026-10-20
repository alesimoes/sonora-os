// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/control_socket_server.h"

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <utility>

namespace sonora {

struct ControlSocketServer::Connection {
    EngineControlServer::ClientId id = 0;
    std::unique_ptr<platform::ControlChannel> channel;
    const platform::SharedRegion* state = nullptr;
    std::size_t max_queued = 0;

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<EngineControlServer::Frame> out;
    std::size_t queued = 0;
    bool closing = false;

    std::thread reader;
    std::thread writer;
    std::atomic<bool> reader_done{false};
    std::atomic<bool> writer_done{false};

    // Any thread. The reader and writer wake, and the peer reads Closed.
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            closing = true;
            out.clear();
            queued = 0;
        }
        wake.notify_all();
        channel->shutdown();
    }

    // The control thread's Outbound: queues, never waits on the peer.
    void enqueue(EngineControlServer::Frame frame) {
        bool overflow = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closing) return;
            if (queued + frame->size() > max_queued) {
                overflow = true;
            } else {
                queued += frame->size();
                out.push_back(std::move(frame));
            }
        }
        if (overflow) {
            std::fprintf(stderr, "sonora: interface %llu stopped reading; disconnecting it\n",
                         static_cast<unsigned long long>(id));
            close();
            return;
        }
        wake.notify_all();
    }

    void write_loop() {
        bool first = true;
        std::string error;
        for (;;) {
            EngineControlServer::Frame frame;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this]() { return closing || !out.empty(); });
                if (closing) break;
                frame = std::move(out.front());
                out.pop_front();
                queued -= frame->size();
            }
            // The Hello's first frame carries the state block, so a client
            // has it before it can ask anything.
            const platform::IoStatus s =
                first ? channel->send_with_region(*frame, *state, -1, error)
                      : channel->send(*frame, -1, error);
            first = false;
            if (s != platform::IoStatus::Ok) {
                if (s == platform::IoStatus::Error) {
                    std::fprintf(stderr, "sonora: interface %llu: %s\n",
                                 static_cast<unsigned long long>(id), error.c_str());
                }
                close();
                break;
            }
        }
        writer_done.store(true, std::memory_order_release);
    }

    bool is_closing() {
        std::lock_guard<std::mutex> lock(mutex);
        return closing;
    }

    void read_loop(EngineControlServer& server) {
        std::string frame, error;
        for (;;) {
            // Backpressure: while the engine has this client's backlog in
            // hand, its frames stay in its socket and its sends block.
            const EngineControlServer::Room room = server.wait_for_room(id, 100);
            if (room == EngineControlServer::Room::Gone) break;
            if (room == EngineControlServer::Room::Full) {
                if (is_closing()) break;
                continue;
            }
            const platform::IoStatus s =
                channel->receive(frame, api::wire::kMaxFrameBytes, nullptr, -1, error);
            if (s == platform::IoStatus::Ok) {
                server.deliver(id, std::move(frame));
                continue;
            }
            if (s == platform::IoStatus::WouldBlock) continue;
            if (s == platform::IoStatus::Error) {
                std::fprintf(stderr, "sonora: interface %llu refused: %s\n",
                             static_cast<unsigned long long>(id), error.c_str());
            }
            break;
        }
        server.detach(id);
        close();
        reader_done.store(true, std::memory_order_release);
    }
};

ControlSocketServer::ControlSocketServer(EngineControlServer& server,
                                         const platform::SharedRegion& state,
                                         std::unique_ptr<platform::ControlListener> listener,
                                         Options options)
    : server_(server), state_(state), listener_(std::move(listener)), opts_(options) {}

ControlSocketServer::~ControlSocketServer() { stop(); }

void ControlSocketServer::start() {
    if (acceptor_.joinable()) return;
    stopping_.store(false, std::memory_order_release);
    acceptor_ = std::thread([this]() { accept_loop(); });
}

void ControlSocketServer::stop() {
    stopping_.store(true, std::memory_order_release);
    if (acceptor_.joinable()) acceptor_.join();
    reap(true);
}

std::size_t ControlSocketServer::connections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t n = 0;
    for (const auto& c : connections_) {
        if (!c->reader_done.load(std::memory_order_acquire)) ++n;
    }
    return n;
}

void ControlSocketServer::accept_loop() {
    while (!stopping_.load(std::memory_order_acquire)) {
        std::string error;
        // Short, so stop() is answered promptly and the dead are reaped.
        std::unique_ptr<platform::ControlChannel> ch = listener_->accept(100, error);
        if (!error.empty()) std::fprintf(stderr, "sonora: control socket: %s\n", error.c_str());
        reap(false);
        if (ch) open(std::move(ch));
    }
}

void ControlSocketServer::open(std::unique_ptr<platform::ControlChannel> channel) {
    if (connections() >= opts_.max_clients) {
        std::fprintf(stderr, "sonora: %zu interfaces are connected already; refusing another\n",
                     opts_.max_clients);
        channel->shutdown();
        return;
    }
    auto c = std::make_shared<Connection>();
    c->channel = std::move(channel);
    c->state = &state_;
    c->max_queued = opts_.max_queued_bytes;
    // The callbacks hold the connection, so it outlives the server's last
    // use of them even when the reaper has let go of it.
    std::weak_ptr<Connection> weak = c;
    c->id = server_.attach(
        [c](EngineControlServer::Frame frame) { c->enqueue(std::move(frame)); },
        [weak](const std::string& why) {
            if (auto conn = weak.lock()) {
                std::fprintf(stderr, "sonora: interface %llu refused: %s\n",
                             static_cast<unsigned long long>(conn->id), why.c_str());
                conn->close();
            }
        });
    accepted_.fetch_add(1, std::memory_order_acq_rel);
    c->writer = std::thread([c]() { c->write_loop(); });
    c->reader = std::thread([this, c]() { c->read_loop(server_); });
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.push_back(std::move(c));
}

void ControlSocketServer::reap(bool all) {
    std::vector<std::shared_ptr<Connection>> done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = connections_.begin(); it != connections_.end();) {
            const auto& c = *it;
            if (all || (c->reader_done.load(std::memory_order_acquire) &&
                        c->writer_done.load(std::memory_order_acquire))) {
                done.push_back(*it);
                it = connections_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& c : done) {
        c->close();
        if (c->reader.joinable()) c->reader.join();
        if (c->writer.joinable()) c->writer.join();
    }
}

}  // namespace sonora
