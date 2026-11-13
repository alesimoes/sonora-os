// SPDX-License-Identifier: GPL-3.0-or-later
//
// The operator's switch and why the assistant cannot answer (ADR-0155 §1).
// Off by default: a fresh install, a reset or an unreadable settings file
// all leave Claude off, and while it is off no transport is even called.
#pragma once

#include <cstddef>

namespace sonora::assistant {

// Large enough for a 64-track session's features, small enough that a
// request never surprises the operator's bill (ADR-0155 §3).
constexpr std::size_t kDefaultMaxRequestBytes = 64 * 1024;

struct AssistantSettings {
    bool enabled = false;
    std::size_t max_request_bytes = kDefaultMaxRequestBytes;
};

// Why only native suggestions are shown. The UI prints the reason; a
// missing Claude plan never looks like "Claude had nothing to say".
enum class Unavailable {
    None,          // available
    SwitchedOff,   // the operator has not opted in
    NoKey,         // opted in, no key stored
    Offline,       // no route to the API
    BadKey,        // the API refused the key (401/403)
    ApiError,      // any other HTTP or transport failure
    TooLarge,      // the bundle does not fit max_request_bytes
    BadReply,      // the reply did not parse into a plan
};

// Short, operator-facing text for each reason: a static string, never null.
inline const char* unavailable_text(Unavailable why) noexcept {
    switch (why) {
    case Unavailable::None: return "Claude available";
    case Unavailable::SwitchedOff: return "Claude is off (Settings)";
    case Unavailable::NoKey: return "Claude: no API key stored";
    case Unavailable::Offline: return "Claude: offline, native suggestions only";
    case Unavailable::BadKey: return "Claude: the API refused the key";
    case Unavailable::ApiError: return "Claude: API error, native suggestions only";
    case Unavailable::TooLarge: return "Claude: session too large to describe";
    case Unavailable::BadReply: return "Claude: reply refused, native suggestions only";
    }
    return "Claude: unavailable";
}

}  // namespace sonora::assistant
