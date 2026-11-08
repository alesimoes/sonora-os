// SPDX-License-Identifier: GPL-3.0-or-later
// How a client tells "blocked by you" from "refused by its probe" in a
// catalog entry (ADR-0065). The operator's block list and a failed probe
// both reach the client as PluginInfo::usable == false with a reason in
// PluginInfo::refusal; a blocked plug-in's reason starts with this prefix.
// A prefix rather than a PluginInfo field, because a field is a protocol
// bump for one bool, and rather than a client-side copy of the block list,
// which drifts from the engine's file.
#pragma once

#include <string_view>

namespace sonora::api {

// The first words of PluginInfo::refusal for a plug-in the operator blocked.
// Followed by nothing or by ": " and the engine's longer words. Wire-visible:
// changing it is a protocol change, so it does not change.
inline constexpr std::string_view kBlockedRefusalPrefix = "blocked by you";

// True when `refusal` (a PluginInfo::refusal) is the operator's block, not a
// probe's verdict. The prefix must end the text or be followed by ':', so a
// probe reason that merely begins with the same letters is not taken for a
// block. Applies to the catalog entry's refusal only: an insert refusal
// wraps it in the plug-in's name, and the client finds the entry by id.
constexpr bool is_blocked_refusal(std::string_view refusal) noexcept {
    if (refusal.substr(0, kBlockedRefusalPrefix.size()) != kBlockedRefusalPrefix) return false;
    return refusal.size() == kBlockedRefusalPrefix.size() || refusal[kBlockedRefusalPrefix.size()] == ':';
}

}  // namespace sonora::api
