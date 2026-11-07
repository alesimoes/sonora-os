// SPDX-License-Identifier: GPL-3.0-or-later
// ADR-0065: the blocked-by-operator marker on PluginInfo::refusal is fixed
// words, and the predicate tells a block from a probe's refusal.
#include <cstdio>
#include <string>

#include "sonora/api/messages.h"
#include "sonora/api/plugin_refusal.h"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

using namespace sonora::api;

// Usable in a constant expression, so a UI can switch on it at no cost.
static_assert(is_blocked_refusal(kBlockedRefusalPrefix), "the prefix alone is a block");

int main() {
    // The wire words themselves: a client built against another revision
    // must read the same marker.
    CHECK(kBlockedRefusalPrefix == "blocked by you");

    CHECK(is_blocked_refusal("blocked by you"));
    CHECK(is_blocked_refusal("blocked by you: it is blocked in the plug-in manager"));
    CHECK(is_blocked_refusal(std::string(kBlockedRefusalPrefix) + ": its bundle is gone"));

    // A probe's refusals, and near misses, are not a block.
    CHECK(!is_blocked_refusal(""));
    CHECK(!is_blocked_refusal("it crashed while being probed"));
    CHECK(!is_blocked_refusal("it reports 2 outputs and wrote 8"));
    CHECK(!is_blocked_refusal("blocked by yours"));
    CHECK(!is_blocked_refusal("blocked by yo"));
    CHECK(!is_blocked_refusal("Blocked by you"));
    CHECK(!is_blocked_refusal(" blocked by you"));
    CHECK(!is_blocked_refusal("Comp cannot be used: blocked by you"));

    // No protocol bump: the marker rides on the existing field.
    PluginInfo p;
    p.usable = false;
    p.refusal = std::string(kBlockedRefusalPrefix) + ": it is blocked in the plug-in manager";
    CHECK(!p.usable && is_blocked_refusal(p.refusal));

    std::puts("plugin_refusal: ok");
    return 0;
}
