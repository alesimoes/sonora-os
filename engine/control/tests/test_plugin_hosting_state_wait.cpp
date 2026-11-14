// SPDX-License-Identifier: GPL-3.0-or-later
//
// GetState requests in flight (plugin_hosting_state.h, ADR-0151 §1 and §4),
// on a fake clock: a save that may copy plugin files waits
// kStateReplyTimeoutMs for its State, one that copies nothing
// kReplyTimeoutMs; copying saves go one host at a time, each deadline from
// when its host begins on it, so a host queued behind a 50 s copy is not
// killed for the wait. No host, no socket, no sleep.
#include <cstdint>
#include <cstdio>
#include <string>

#include "sonora/plugin/host_control.h"
#include "sonora/plugin_hosting_state.h"

using namespace sonora;
namespace pc = plugin::control;

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            return 1;                                                           \
        }                                                                       \
    } while (0)

namespace {

constexpr uint64_t kT0 = 1000;

// A request as PluginHosting builds it from its GetState.
PluginStateRequests::Request ask(uint64_t flush, model::Id track, bool copies) {
    pc::GetState g;
    g.save_dir = "/home/op/Sessions/Take Five";
    g.flags = copies ? 0u : pc::kGetStateNoCopy;
    PluginStateRequests::Request r;
    r.flush = flush;
    r.insert = 100 + track;
    r.track = track;
    r.where = "insert on track " + std::to_string(track);
    r.frame = "frame " + std::to_string(track);
    r.timeout_ms = pc::state_reply_timeout_ms(g);
    r.copies = !g.save_dir.empty() && !(g.flags & pc::kGetStateNoCopy);
    return r;
}

bool late_track(const PluginStateRequests& q, uint64_t now, model::Id track, int timeout_ms) {
    const auto late = q.late(now);
    return late.size() == 1 && late[0].track == track && late[0].timeout_ms == timeout_ms;
}

// The bug this guards: the reply wait was kReplyTimeoutMs (5 s), so a
// sampler copying a 3 GiB library was killed half way.
int test_copying_state_waits_60_s() {
    PluginStateRequests q;
    q.add(1, ask(1, 7, true));
    const auto due = q.due(kT0);
    CHECK(due.size() == 1 && due[0].first == 1 && due[0].second == "frame 7");
    CHECK(q.late(kT0 + uint64_t(pc::kReplyTimeoutMs)).empty());
    CHECK(q.late(kT0 + uint64_t(pc::kStateReplyTimeoutMs) - 1).empty());
    CHECK(late_track(q, kT0 + uint64_t(pc::kStateReplyTimeoutMs), 7, pc::kStateReplyTimeoutMs));
    CHECK(pc::kStateReplyTimeoutMs == 60000);
    return 0;
}

int test_no_copy_state_waits_5_s() {
    PluginStateRequests q;
    q.add(1, ask(1, 7, false));
    CHECK(q.due(kT0).size() == 1);
    CHECK(q.late(kT0 + uint64_t(pc::kReplyTimeoutMs) - 1).empty());
    CHECK(late_track(q, kT0 + uint64_t(pc::kReplyTimeoutMs), 7, pc::kReplyTimeoutMs));
    return 0;
}

// Two hosts saving at once would each see the same free space and each spend
// the 512 MiB margin: the second is sent only when the first has answered,
// and its 60 s start then.
int test_copying_saves_go_one_host_at_a_time() {
    PluginStateRequests q;
    q.add(1, ask(1, 7, true));
    q.add(2, ask(1, 8, true));
    auto due = q.due(kT0);
    CHECK(due.size() == 1 && due[0].first == 1);
    CHECK(q.due(kT0 + 10).empty());
    CHECK(q.flush_pending(1));
    // Track 8's GetState is not out: its clock has not started.
    const uint64_t answer_at = kT0 + 50000;
    CHECK(q.late(answer_at).empty());
    PluginStateRequests::Request r;
    CHECK(!q.answered(2, 8, answer_at, r));  // never sent, so not an answer
    CHECK(!q.answered(1, 8, answer_at, r));  // another host's request
    CHECK(q.answered(1, 7, answer_at, r) && r.track == 7);
    due = q.due(answer_at);
    CHECK(due.size() == 1 && due[0].first == 2 && due[0].second == "frame 8");
    CHECK(q.late(answer_at + uint64_t(pc::kStateReplyTimeoutMs) - 1).empty());
    CHECK(late_track(q, answer_at + uint64_t(pc::kStateReplyTimeoutMs), 8, pc::kStateReplyTimeoutMs));
    CHECK(q.answered(2, 8, answer_at + 1, r));
    CHECK(q.empty() && !q.flush_pending(1));
    return 0;
}

// A save during a take copies nothing: it is never held behind a copy.
int test_no_copy_is_not_serialised() {
    PluginStateRequests q;
    q.add(1, ask(1, 7, true));
    q.add(2, ask(1, 8, true));
    q.add(3, ask(2, 9, false));
    const auto due = q.due(kT0);
    CHECK(due.size() == 2 && due[0].first == 1 && due[1].first == 3);
    return 0;
}

// One host serves its GetStates in order: the second's deadline starts when
// the first is answered, not when both were sent.
int test_same_host_deadline_starts_in_turn() {
    PluginStateRequests q;
    q.add(1, ask(1, 7, false));
    q.add(2, ask(1, 7, false));
    CHECK(q.due(kT0).size() == 2);
    const uint64_t answer_at = kT0 + 4000;
    PluginStateRequests::Request r;
    CHECK(q.answered(1, 7, answer_at, r));
    CHECK(q.late(kT0 + uint64_t(pc::kReplyTimeoutMs)).empty());
    CHECK(late_track(q, answer_at + uint64_t(pc::kReplyTimeoutMs), 7, pc::kReplyTimeoutMs));
    return 0;
}

// A host that dies (or is killed for its deadline) fails its requests, sent
// or queued, and frees the copying turn for the next host.
int test_host_gone_frees_the_turn() {
    PluginStateRequests q;
    q.add(1, ask(1, 7, true));
    q.add(2, ask(1, 7, true));
    q.add(3, ask(1, 8, true));
    CHECK(q.due(kT0).size() == 1);
    const uint64_t kill_at = kT0 + uint64_t(pc::kStateReplyTimeoutMs);
    CHECK(late_track(q, kill_at, 7, pc::kStateReplyTimeoutMs));
    const auto gone = q.remove_track(7);
    CHECK(gone.size() == 2 && gone[0].flush == 1);
    const auto due = q.due(kill_at);
    CHECK(due.size() == 1 && due[0].first == 3);
    CHECK(late_track(q, kill_at + uint64_t(pc::kStateReplyTimeoutMs), 8, pc::kStateReplyTimeoutMs));
    return 0;
}

// A GetState that could not be sent is removed; the next on its host starts
// its clock then, and the next copying host gets its turn.
int test_send_failure_arms_the_next() {
    PluginStateRequests q;
    q.add(1, ask(1, 7, false));
    q.add(2, ask(1, 7, false));
    CHECK(q.due(kT0).size() == 2);
    const PluginStateRequests::Request r = q.remove(1, kT0 + 3000);
    CHECK(r.track == 7);
    CHECK(q.late(kT0 + uint64_t(pc::kReplyTimeoutMs)).empty());
    CHECK(late_track(q, kT0 + 3000 + uint64_t(pc::kReplyTimeoutMs), 7, pc::kReplyTimeoutMs));

    PluginStateRequests c;
    c.add(1, ask(1, 7, true));
    c.add(2, ask(1, 8, true));
    CHECK(c.due(kT0).size() == 1);
    c.remove(1, kT0);
    const auto due = c.due(kT0);
    CHECK(due.size() == 1 && due[0].first == 2);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_copying_state_waits_60_s();
    failed += test_no_copy_state_waits_5_s();
    failed += test_copying_saves_go_one_host_at_a_time();
    failed += test_no_copy_is_not_serialised();
    failed += test_same_host_deadline_starts_in_turn();
    failed += test_host_gone_frees_the_turn();
    failed += test_send_failure_arms_the_next();
    std::printf("%s\n", failed == 0 ? "plugin_hosting_state_wait: all passed" : "plugin_hosting_state_wait: FAILED");
    return failed == 0 ? 0 : 1;
}
