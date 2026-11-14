// SPDX-License-Identifier: GPL-3.0-or-later
#include "assistant_core.h"

#include "sonora/assistant/reply_parser.h"
#include "sonora/assistant/request.h"

namespace sonora::assistant {

Unavailable AssistantCore::ask_claude(const FeatureBundle& bundle, const model::Snapshot& at,
                                      model::Plan& plan, std::string& detail) {
    // Checked before anything is built or sent: off means no connection.
    if (!settings_.enabled) return Unavailable::SwitchedOff;
    std::string key;
    if (!keys_.load(key) || key.empty()) return Unavailable::NoKey;

    Request req;
    if (!build_request(bundle, settings_.max_request_bytes, req, detail))
        return Unavailable::TooLarge;

    Response resp;
    switch (transport_.post(req, key, resp)) {
    case TransportStatus::Ok: break;
    case TransportStatus::Offline:
    case TransportStatus::Timeout: return Unavailable::Offline;
    case TransportStatus::Failed: return Unavailable::ApiError;
    }
    if (resp.http_status == 401 || resp.http_status == 403) return Unavailable::BadKey;
    if (resp.http_status < 200 || resp.http_status >= 300) {
        detail = "HTTP " + std::to_string(resp.http_status);
        return Unavailable::ApiError;
    }
    if (!parse_reply(resp.body, bundle, at, plan, detail)) return Unavailable::BadReply;
    return Unavailable::None;
}

Suggestion AssistantCore::suggest(const FeatureBundle& bundle, const model::Snapshot& at) {
    Suggestion s;
    s.why = ask_claude(bundle, at, s.plan, s.detail);
    if (s.why == Unavailable::None) {
        s.source = Source::Claude;
        s.has_plan = true;
        return s;
    }
    s.plan = model::Plan{};
    s.source = Source::Native;
    s.has_plan = native_.propose(at, s.plan, s.native_error);
    return s;
}

}  // namespace sonora::assistant
