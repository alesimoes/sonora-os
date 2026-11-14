// SPDX-License-Identifier: GPL-3.0-or-later
//
// A scripted HttpTransport for tests (ADR-0155 §6, ADR-0007 §7). It opens
// nothing; it records what it was asked to send so a test can prove the
// switch-off path never reached it and the key never reached a body.
#pragma once

#include <cstddef>
#include <string>
#include <utility>

#include "sonora/assistant/http_transport.h"

namespace sonora::assistant {

class FakeTransport final : public HttpTransport {
public:
    // Every call answers with this until changed.
    void reply(int http_status, std::string body) {
        status_ = TransportStatus::Ok;
        http_status_ = http_status;
        body_ = std::move(body);
    }
    // Every call fails with `status` (Offline, Timeout, Failed).
    void fail(TransportStatus status) {
        status_ = status;
        http_status_ = 0;
        body_.clear();
    }

    TransportStatus post(const Request& request, const std::string& key,
                         Response& out) override {
        ++calls_;
        last_body_ = request.body;
        last_key_ = key;
        out.status = status_;
        out.http_status = status_ == TransportStatus::Ok ? http_status_ : 0;
        out.body = status_ == TransportStatus::Ok ? body_ : std::string();
        return status_;
    }

    std::size_t calls() const { return calls_; }
    const std::string& last_body() const { return last_body_; }
    const std::string& last_key() const { return last_key_; }

private:
    TransportStatus status_ = TransportStatus::Offline;
    int http_status_ = 0;
    std::string body_;
    std::size_t calls_ = 0;
    std::string last_body_;
    std::string last_key_;
};

}  // namespace sonora::assistant
