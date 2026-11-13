// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one network seam of the Claude layer (ADR-0155 §6). Everything above
// it is pure and testable; the real implementation (assistant/src/net) is
// the only code that opens a socket, and it is constructed only when the
// operator has switched Claude on. Never called from the audio thread: a
// call blocks for up to the transport's timeout.
#pragma once

#include <string>

#include "sonora/assistant/request.h"

namespace sonora::assistant {

enum class TransportStatus {
    Ok,          // a response arrived; see Response::http_status
    Offline,     // no route, DNS failure, connection refused
    Timeout,     // connected but no complete response in time
    Failed,      // TLS or any other transport error
};

struct Response {
    TransportStatus status = TransportStatus::Failed;
    int http_status = 0;   // meaningful only when status is Ok
    std::string body;
};

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    // Sends `request` to the configured endpoint with `key` as the
    // credential header. The key is a separate argument so no layer above
    // can put it in a body; an implementation never logs it. Returns
    // Response::status; `out` is always filled.
    virtual TransportStatus post(const Request& request, const std::string& key,
                                 Response& out) = 0;
};

}  // namespace sonora::assistant
