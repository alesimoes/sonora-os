// SPDX-License-Identifier: GPL-3.0-or-later
//
// The gate in front of the transport (ADR-0155 §1, §6): Claude's plan when
// the operator opted in and everything worked, the native planner's plan
// otherwise, with the reason and a source label so the UI never passes one
// off as the other. Control thread only; suggest() blocks for the transport.
#pragma once

#include <string>

#include "sonora/assistant/feature_bundle.h"
#include "sonora/assistant/http_transport.h"
#include "sonora/assistant/key_store.h"
#include "sonora/assistant/settings.h"
#include "sonora/model/plan.h"

namespace sonora::assistant {

enum class Source { Native, Claude };

inline const char* source_label(Source s) noexcept {
    return s == Source::Claude ? "Claude" : "Native";
}

struct Suggestion {
    Source source = Source::Native;
    Unavailable why = Unavailable::None;   // why Native, when it is
    model::Plan plan;
    // Details behind `why` (the parser's refusal, the size): shown held.
    std::string detail;
    // The native planner's own refusal, if it had nothing either.
    std::string native_error;
    bool has_plan = false;
};

class AssistantCore {
public:
    // All references outlive the core. `settings` is read on every call, so
    // flipping the switch takes effect at once.
    AssistantCore(const AssistantSettings& settings, const KeyStore& keys,
                  HttpTransport& transport, model::PlanSource& native)
        : settings_(settings), keys_(keys), transport_(transport), native_(native) {}

    Suggestion suggest(const FeatureBundle& bundle, const model::Snapshot& at);

private:
    Unavailable ask_claude(const FeatureBundle& bundle, const model::Snapshot& at,
                           model::Plan& plan, std::string& detail);

    const AssistantSettings& settings_;
    const KeyStore& keys_;
    HttpTransport& transport_;
    model::PlanSource& native_;
};

}  // namespace sonora::assistant
