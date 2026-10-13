// SPDX-License-Identifier: GPL-3.0-or-later
//
// FakeEngine against the control API's conformance suite, the same suite
// engine/control runs on InProcessEngine, plus what only a fake can be
// asked to do: be deterministic, script an event, die on cue.
#include "sonora/api/fake_engine.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

#include "sonora/api/conformance.h"

using namespace sonora::api;
using sonora::api::testing::FakeEngine;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {

class FakeRig final : public conformance::Rig {
public:
    const char* name() const override { return "FakeEngine"; }
    std::unique_ptr<EngineControl> make(uint32_t protocol, uint32_t layout) override {
        FakeEngine::Config cfg;
        cfg.tracks = 4;
        cfg.protocol_version = protocol;
        cfg.state_layout_version = layout;
        cfg.clock = [this]() { return now_; };
        return std::make_unique<FakeEngine>(cfg);
    }
    uint64_t now_ns() override { return now_; }
    void wait_ns(uint64_t ns) override { now_ += ns; }
    void kill(EngineControl& e) override { static_cast<FakeEngine&>(e).die(); }
    bool has_signal() const override { return true; }

private:
    uint64_t now_ = 5'000'000'000;
};

void deterministic() {
    uint64_t now = 1'000'000'000;
    FakeEngine::Config cfg;
    cfg.clock = [&now]() { return now; };
    FakeEngine e(cfg);
    std::string error;
    CHECK(e.connect(error));
    CHECK(e.hello().track_count == 18);
    // Channel 10 (index 9) goes over full scale in the first 50 ms of every
    // 7 s. A reader that sampled only its tick's end would miss it; the
    // peak is the maximum over the interval.
    Inbox in;
    now += 30'000'000;
    e.publish();
    now += 40'000'000;
    e.poll(in);
    CHECK(FakeEngine::level_at(9, 0.07) < 1.0f);
    CHECK(e.state().take_peak(9) >= 1.0f);
    now += 16'000'000;
    e.poll(in);
    CHECK(e.state().take_peak(9) < 1.0f);
    CHECK(e.state().track(9).clip_count >= 1);

    // A scripted peak and event.
    e.script_peak(2, 0.75f);
    EngineEvent ev;
    ev.kind = EventKind::Notice;
    ev.text = "recording from Built-in Audio (onboard)";
    e.post_event(ev);
    e.poll(in);
    CHECK(e.state().take_peak(2) == 0.75f);
    CHECK(in.events.size() == 1 && in.events[0].text == ev.text);

    // Death at a time: no publish, no reply after it.
    e.die_at(now + 1);
    now += 2;
    const uint64_t hb = e.state().transport().heartbeat;
    CommandRequest r;
    r.command = "track.arm_all";
    e.send(r);
    in.clear();
    e.poll(in);
    CHECK(in.replies.empty());
    CHECK(e.state().transport().heartbeat == hb);
}

}  // namespace

int main() {
    FakeRig rig;
    const int failed = conformance::run(rig);
    if (failed != 0) {
        std::fprintf(stderr, "%d conformance check(s) failed on FakeEngine\n", failed);
        ++failures;
    }
    deterministic();
    if (failures) return EXIT_FAILURE;
    std::printf("api fake engine: ok\n");
    return EXIT_SUCCESS;
}
