// SPDX-License-Identifier: GPL-3.0-or-later
//
// How the UI drives the Claude layer (ADR-0226). The UI never sees a
// transport or a key store: it reads settings, flips the switch, stores or
// forgets the key, and asks for a plan with a completion callback so the UI
// thread never blocks on the network. Nothing here is real-time code.
#pragma once

#include <functional>
#include <string>

#include "sonora/assistant/feature_bundle.h"
#include "sonora/assistant/settings.h"
#include "sonora/model/plan.h"

namespace sonora::assistant {

// What a request produced. `why == None` means the plan is Claude's and
// `has_plan` is true; anything else means "show native suggestions" and
// `why` is the reason to print (unavailable_text), `detail` the held notice.
struct ClaudeResult {
    Unavailable why = Unavailable::None;
    bool has_plan = false;
    model::Plan plan;
    std::string detail;
};

class AssistantService {
public:
    // Called once per request, on the service's own thread or, for a refusal
    // that needs no network, synchronously before request_claude returns.
    // The UI posts the result to its own thread; it never waits here.
    using Completion = std::function<void(ClaudeResult)>;

    virtual ~AssistantService() = default;

    virtual AssistantSettings settings() const = 0;
    // False, with `error`, when the choice cannot be persisted: the switch
    // then stays as it was, so the screen never shows a state that is not real.
    virtual bool set_enabled(bool enabled, std::string& error) = 0;
    virtual bool set_key(const std::string& key, std::string& error) = 0;
    virtual void clear_key() = 0;
    // "sk-...7f3a" or empty; never the key.
    virtual std::string masked_key() const = 0;

    // Never blocks. While off (SwitchedOff) or with no key (NoKey) the
    // completion runs at once and no connection is opened. `at` is the
    // snapshot the bundle was taken from (reply_parser.h).
    virtual void request_claude(const FeatureBundle& bundle, const model::Snapshot& at,
                                Completion done) = 0;
};

}  // namespace sonora::assistant
