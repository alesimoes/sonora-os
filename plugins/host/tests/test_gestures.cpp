// SPDX-License-Identifier: GPL-3.0-or-later
// The host's gesture rules (contracts/plugin host_control.h, "Parameter
// touches"), without a plugin. Regression for the rt-engineer review of
// PR #51, M3:
//  - a lost touch must not make GestureEnd stale: it carries the value the
//    plugin reports at the end, not the last touch the host queued;
//  - a lost end must not leave a gesture open for ever (and SetInsertParams
//    unrecorded): after any drop the host ends every open gesture with the
//    plugin's current value, a late end of it is ignored, and a later touch
//    starts a new gesture.
#include "control.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace sonora::plugin_host;
namespace pc = sonora::plugin::control;

namespace {

int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++failures;                                                  \
        }                                                                \
    } while (0)

struct Seen {
    pc::Type type;
    uint32_t param;
    float value;
};

struct Wire {
    std::vector<Seen> seen;
    void operator()(const std::string& b) {
        pc::Type t;
        std::string e;
        if (!pc::peek_type(b, t)) return;
        if (t == pc::Type::GestureBegin) {
            pc::GestureBegin m;
            if (pc::decode(b, m, e)) seen.push_back({t, m.param, 0.f});
        } else if (t == pc::Type::ParamTouched) {
            pc::ParamTouched m;
            if (pc::decode(b, m, e)) seen.push_back({t, m.param, m.value});
        } else if (t == pc::Type::GestureEnd) {
            pc::GestureEnd m;
            if (pc::decode(b, m, e)) seen.push_back({t, m.param, m.value});
        }
    }
};

TouchQueue::Event ev(TouchKind k, uint32_t param, float v) { return {0, k, param, v}; }

constexpr uint64_t kMs = 1000000ull;

void a_lost_touch_does_not_make_the_end_stale() {
    GestureTracker g;
    Wire w;
    auto send = [&](const std::string& b) { w(b); };
    g.apply(ev(TouchKind::Begin, 3, 0.1f), 100 * kMs, send);
    g.apply(ev(TouchKind::Perform, 3, 0.25f), 100 * kMs, send);
    // The Perform to 0.5 was dropped; endEdit reports the controller's 0.5.
    g.apply(ev(TouchKind::End, 3, 0.5f), 200 * kMs, send);
    CHECK(w.seen.size() == 3);
    CHECK(w.seen.size() == 3 && w.seen[2].type == pc::Type::GestureEnd && w.seen[2].value == 0.5f);
    CHECK(!g.any_open());
}

void a_lost_end_does_not_leave_a_gesture_open() {
    GestureTracker g;
    Wire w;
    auto send = [&](const std::string& b) { w(b); };
    g.apply(ev(TouchKind::Begin, 3, 0.1f), 100 * kMs, send);
    g.apply(ev(TouchKind::Perform, 3, 0.3f), 100 * kMs, send);
    g.apply(ev(TouchKind::Begin, 4, 0.7f), 100 * kMs, send);
    // The End of param 3 was dropped. The host saw the drop: it ends every
    // open gesture with what the plugin says now.
    CHECK(g.any_open());
    g.end_all(
        [](uint32_t, uint32_t param, float& v) {
            if (param != 3) return false;  // the format cannot say: the last touch
            v = 0.4f;
            return true;
        },
        send);
    CHECK(!g.any_open());
    // Begin 3, touched 0.3, begin 4, end 3 at 0.4, end 4 at its last (0.7).
    CHECK(w.seen.size() == 5);
    if (w.seen.size() == 5) {
        CHECK(w.seen[3].type == pc::Type::GestureEnd && w.seen[3].param == 3 && w.seen[3].value == 0.4f);
        CHECK(w.seen[4].type == pc::Type::GestureEnd && w.seen[4].param == 4 && w.seen[4].value == 0.7f);
    }
    // The end that arrives late is ignored; a later touch is a new gesture.
    g.apply(ev(TouchKind::End, 3, 0.4f), 300 * kMs, send);
    CHECK(w.seen.size() == 5);
    g.apply(ev(TouchKind::Perform, 3, 0.6f), 400 * kMs, send);
    CHECK(w.seen.size() == 8 && w.seen[5].type == pc::Type::GestureBegin && w.seen[6].value == 0.6f &&
          w.seen[7].type == pc::Type::GestureEnd && w.seen[7].value == 0.6f);
    CHECK(!g.any_open());
}

void touches_are_coalesced_and_flushed() {
    GestureTracker g;
    Wire w;
    auto send = [&](const std::string& b) { w(b); };
    g.apply(ev(TouchKind::Begin, 1, 0.f), 0, send);
    g.apply(ev(TouchKind::Perform, 1, 0.1f), 1000 * kMs, send);  // sent
    g.apply(ev(TouchKind::Perform, 1, 0.2f), 1002 * kMs, send);  // coalesced
    g.apply(ev(TouchKind::Perform, 1, 0.3f), 1004 * kMs, send);  // coalesced
    CHECK(w.seen.size() == 2);
    const uint64_t due = g.flush(1005 * kMs, send);
    CHECK(due == 1000 * kMs + uint64_t(pc::kTouchIntervalMs) * kMs);
    CHECK(g.flush(due, send) == 0);
    CHECK(w.seen.size() == 3 && w.seen[2].type == pc::Type::ParamTouched && w.seen[2].value == 0.3f);
}

}  // namespace

int main() {
    a_lost_touch_does_not_make_the_end_stale();
    a_lost_end_does_not_leave_a_gesture_open();
    touches_are_coalesced_and_flushed();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("gestures: all checks passed\n");
    return 0;
}
