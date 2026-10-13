// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/engine_control_in_process.h"

#include <utility>
#include <vector>

#include "sonora/api/wire.h"

namespace sonora {

InProcessEngine::InProcessEngine(Session& session, backend::AudioBackend* audio,
                                 EngineControlServer::Options options)
    : server_(session, audio, std::move(options)) {}

// The server's threads call receive(); they are joined before the queue
// they write to goes away.
InProcessEngine::~InProcessEngine() { server_.stop(); }

void InProcessEngine::receive(std::string frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    inbound_.push_back(std::move(frame));
}

bool InProcessEngine::next_message(std::string& message) {
    for (;;) {
        std::string frame;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (inbound_.empty()) return false;
            frame = std::move(inbound_.front());
            inbound_.pop_front();
        }
        std::string error;
        // A frame out of place loses its message; after M7 the socket
        // client closes the connection instead.
        if (reassembler_.add(frame, message, error) ==
            api::wire::Reassembler::Result::Complete) {
            return true;
        }
    }
}

uint64_t InProcessEngine::deliver(const std::string& message, uint64_t seq) {
    std::vector<std::string> frames;
    std::string error;
    if (api::wire::split_frames(message, frames, error)) {
        for (std::string& f : frames) server_.deliver(std::move(f));
    }
    return seq;
}

bool InProcessEngine::connect(std::string& error) {
    if (connected_) return true;
    server_.start([this](std::string frame) { receive(std::move(frame)); });

    // The Hello is the first thing the server sends, before its threads
    // start, so all of its frames are already here.
    std::string bytes;
    next_message(bytes);
    uint32_t protocol = 0, layout = 0;
    if (!api::wire::decode_hello_versions(bytes, protocol, layout, error)) {
        error = "the engine did not say hello: " + error;
        server_.stop();
        return false;
    }
    error = api::wire::version_mismatch(protocol, layout);
    if (!error.empty()) {
        server_.stop();
        return false;
    }
    if (!api::wire::decode(bytes, hello_, error)) {
        error = "the engine's hello does not parse: " + error;
        server_.stop();
        return false;
    }
    if (!view_.attach(server_.state_block(), server_.state_block_bytes(), error)) {
        server_.stop();
        return false;
    }
    connected_ = true;
    return true;
}

uint64_t InProcessEngine::send(const api::CommandRequest& request) {
    api::CommandRequest r = request;
    r.seq = ++seq_;
    std::string bytes;
    api::wire::encode(r, bytes);
    return deliver(bytes, r.seq);
}

uint64_t InProcessEngine::send(const api::ParameterChange& change) {
    api::ParameterChange c = change;
    c.seq = ++seq_;
    std::string bytes;
    api::wire::encode(c, bytes);
    return deliver(bytes, c.seq);
}

uint64_t InProcessEngine::send(const api::PluginParameterChange& change) {
    api::PluginParameterChange c = change;
    c.seq = ++seq_;
    std::string bytes;
    api::wire::encode(c, bytes);
    return deliver(bytes, c.seq);
}

uint64_t InProcessEngine::submit(const api::ModelRequest& request) {
    api::ModelRequest m = request;
    m.seq = ++seq_;
    std::string bytes;
    api::wire::encode(m, bytes);
    return deliver(bytes, m.seq);
}

void InProcessEngine::poll(api::Inbox& inbox) {
    std::string bytes;
    while (next_message(bytes)) {
        std::string error;
        api::wire::decode_into(bytes, inbox, error);
    }
}

}  // namespace sonora
