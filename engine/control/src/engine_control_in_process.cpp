// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/engine_control_in_process.h"

#include <utility>

#include "sonora/api/wire.h"

namespace sonora {

InProcessEngine::InProcessEngine(Session& session, backend::AudioBackend* audio,
                                 EngineControlServer::Options options)
    : server_(session, audio, std::move(options)) {}

// The server's threads call receive(); they are joined before the queue
// they write to goes away.
InProcessEngine::~InProcessEngine() { server_.stop(); }

void InProcessEngine::receive(std::string bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    inbound_.push_back(std::move(bytes));
}

bool InProcessEngine::connect(std::string& error) {
    if (connected_) return true;
    server_.start([this](std::string bytes) { receive(std::move(bytes)); });

    // The Hello is the first thing the server sends, before its threads
    // start, so it is already here.
    std::string bytes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!inbound_.empty()) {
            bytes = std::move(inbound_.front());
            inbound_.pop_front();
        }
    }
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
    server_.deliver(std::move(bytes));
    return r.seq;
}

uint64_t InProcessEngine::send(const api::ParameterChange& change) {
    api::ParameterChange c = change;
    c.seq = ++seq_;
    std::string bytes;
    api::wire::encode(c, bytes);
    server_.deliver(std::move(bytes));
    return c.seq;
}

uint64_t InProcessEngine::submit(const api::ModelRequest& request) {
    api::ModelRequest m = request;
    m.seq = ++seq_;
    std::string bytes;
    api::wire::encode(m, bytes);
    server_.deliver(std::move(bytes));
    return m.seq;
}

void InProcessEngine::poll(api::Inbox& inbox) {
    std::deque<std::string> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(inbound_);
    }
    for (const std::string& bytes : batch) {
        api::wire::Type type{};
        std::string error;
        if (!api::wire::peek_type(bytes, type)) continue;
        if (type == api::wire::Type::CommandReply) {
            api::CommandReply r;
            if (api::wire::decode(bytes, r, error)) inbox.replies.push_back(std::move(r));
        } else if (type == api::wire::Type::EngineEvent) {
            api::EngineEvent e;
            if (api::wire::decode(bytes, e, error)) inbox.events.push_back(std::move(e));
        } else if (type == api::wire::Type::ModelUpdate) {
            api::ModelUpdate m;
            if (api::wire::decode(bytes, m, error)) inbox.model.push_back(std::move(m));
        }
    }
}

}  // namespace sonora
