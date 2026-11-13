// SPDX-License-Identifier: GPL-3.0-or-later
//
// The real HttpTransport (ADR-0155 §6): one POST, one overall deadline, no
// retry. https needs OpenSSL (SONORA_ASSISTANT_TLS); without it an https
// endpoint fails loudly rather than falling back to plain text. http is for
// loopback tests only.
#pragma once

#include <functional>
#include <string>
#include <utility>

#include "sonora/assistant/http_transport.h"

namespace sonora::assistant {

struct TransportConfig {
    std::string url;        // e.g. https://api.anthropic.com/v1/messages
    int timeout_ms = 30000; // whole call: connect, send and full response
    std::string key_header = "x-api-key";
    std::function<void(const std::string&)> log;  // never receives the key
};

class SocketTransport final : public HttpTransport {
public:
    explicit SocketTransport(TransportConfig cfg) : cfg_(std::move(cfg)) {}
    TransportStatus post(const Request& request, const std::string& key,
                         Response& out) override;

private:
    TransportConfig cfg_;
};

}  // namespace sonora::assistant
