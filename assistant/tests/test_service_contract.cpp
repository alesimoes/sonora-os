// SPDX-License-Identifier: GPL-3.0-or-later
// AssistantService contract (ADR-0226), run against the fake.
#include <cstdio>
#include <cstdlib>

#include "sonora/assistant/fake_assistant_service.h"

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            std::exit(1);                                           \
        }                                                           \
    } while (0)

using namespace sonora::assistant;

static ClaudeResult ask(AssistantService& s, int& calls) {
    ClaudeResult out;
    FeatureBundle b;
    sonora::model::Snapshot snap;
    s.request_claude(b, snap, [&](ClaudeResult r) { ++calls; out = std::move(r); });
    return out;
}

int main() {
    FakeAssistantService s;
    int calls = 0;
    CHECK(!s.settings().enabled);
    CHECK(s.masked_key().empty());
    ClaudeResult r = ask(s, calls);
    CHECK(calls == 1 && r.why == Unavailable::SwitchedOff && !r.has_plan);
    CHECK(s.connections() == 0);

    std::string err;
    CHECK(s.set_enabled(true, err));
    r = ask(s, calls);
    CHECK(calls == 2 && r.why == Unavailable::NoKey && s.connections() == 0);

    CHECK(!s.set_key("", err) && !err.empty());
    CHECK(s.set_key("sk-ant-abcdefgh7f3a", err));
    CHECK(s.masked_key() == "sk-...7f3a");
    ClaudeResult scripted;
    scripted.has_plan = true;
    s.answer(scripted);
    r = ask(s, calls);
    CHECK(r.why == Unavailable::None && r.has_plan && s.connections() == 1);

    s.clear_key();
    CHECK(s.masked_key().empty());
    r = ask(s, calls);
    CHECK(r.why == Unavailable::NoKey && s.connections() == 1);

    s.fail_persist(true);
    CHECK(!s.set_enabled(false, err) && s.settings().enabled);
    std::printf("ok\n");
    return 0;
}
