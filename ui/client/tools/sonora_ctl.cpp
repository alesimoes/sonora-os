// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-ctl: the smallest interface there is, for scripts and for the
// end-to-end check (tools/test-engine-docker.sh). A client on the engine's
// control socket like any other (ADR-0007 §3).
//
//   sonora-ctl [--control-socket PATH] state
//   sonora-ctl [--control-socket PATH] send COMMAND [TRACK]
//   sonora-ctl [--control-socket PATH] take SECONDS
//
// `take` arms every track, records for SECONDS, stops, asks for playback,
// and checks the state block at each step; it prints one RESULT line and
// exits 0 only if every check held.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "sonora/api/state_view.h"
#include "sonora/client/socket_engine.h"

namespace {

using sonora::api::TransportState;
using Clock = std::chrono::steady_clock;

uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     Clock::now().time_since_epoch())
                                     .count());
}

const char* state_name(TransportState s) {
    switch (s) {
        case TransportState::Stopped: return "stopped";
        case TransportState::Rolling: return "rolling";
        case TransportState::Recording: return "recording";
    }
    return "?";
}

// The reply to `seq`, polling for up to two seconds.
bool await_reply(sonora::client::SocketEngine& e, uint64_t seq, sonora::api::CommandReply& out) {
    const auto end = Clock::now() + std::chrono::seconds(2);
    sonora::api::Inbox in;
    while (Clock::now() < end) {
        e.poll(in);
        for (const auto& r : in.replies) {
            if (r.seq == seq) {
                out = r;
                return true;
            }
        }
        in.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

sonora::api::CommandReply run(sonora::client::SocketEngine& e, const std::string& command,
                              int32_t target = -1) {
    sonora::api::CommandRequest r;
    r.command = command;
    r.target = target;
    sonora::api::CommandReply reply;
    if (!await_reply(e, e.send(r), reply)) {
        reply.result = sonora::DispatchResult::Failed;
        reply.message = "no reply within 2 s";
    }
    return reply;
}

void print_state(sonora::client::SocketEngine& e) {
    sonora::api::StateFrame f;
    e.state().frame(f);
    const auto& t = f.transport;
    unsigned armed = 0;
    for (const auto& tr : f.tracks) armed += tr.armed() ? 1 : 0;
    std::printf("STATE transport=%s take=%d playhead=%llu tracks=%zu armed=%u xruns=%u"
                " overruns=%u health=0x%x inputs=%u heartbeat=%llu alive=%d\n",
                state_name(t.state), t.take, static_cast<unsigned long long>(t.playhead),
                f.tracks.size(), armed, t.xruns, t.overruns, t.health_flags,
                t.inputs_connected, static_cast<unsigned long long>(t.heartbeat),
                e.state().engine_alive(now_ns()) ? 1 : 0);
}

template <typename Pred>
bool within(sonora::client::SocketEngine& e, int ms, Pred pred) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    sonora::api::Inbox in;
    while (Clock::now() < end) {
        e.poll(in);
        in.clear();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

int take(sonora::client::SocketEngine& e, double seconds) {
    int failed = 0;
    const auto check = [&failed](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++failed;
    };
    auto& v = e.state();
    const uint32_t tracks = e.hello().track_count;
    check(tracks > 0, "hello names the tracks");
    check(v.engine_alive(now_ns()), "the state block's heartbeat is live");
    const int32_t first_take = v.transport().take;

    check(run(e, "track.arm_all").result == sonora::DispatchResult::Ok, "arm every track");
    check(within(e, 1000, [&]() {
              for (uint32_t i = 0; i < tracks; ++i) {
                  if (!v.track(i).armed()) return false;
              }
              return true;
          }),
          "the block shows every track armed");

    const auto rec = run(e, "transport.record");
    if (rec.result != sonora::DispatchResult::Ok) {
        std::printf("  record: %s\n", rec.message.c_str());
    }
    check(rec.result == sonora::DispatchResult::Ok, "record");
    check(within(e, 1000, [&]() { return v.transport().state == TransportState::Recording; }),
          "the block shows recording");
    const uint64_t start = v.transport().playhead;
    float peak = 0.0f;
    const auto end = Clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    sonora::api::Inbox in;
    while (Clock::now() < end) {
        e.poll(in);
        in.clear();
        for (uint32_t i = 0; i < tracks; ++i) peak = std::max(peak, v.take_peak(i));
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    const auto during = v.transport();
    const uint64_t rate = e.hello().sample_rate;
    const uint64_t rolled = during.playhead - start;
    check(rolled >= static_cast<uint64_t>(seconds * 0.8 * static_cast<double>(rate)),
          "the playhead advanced while recording");
    check(peak > 0.0f, "the meters saw signal");
    check(v.engine_alive(now_ns()), "the heartbeat stayed live");

    check(run(e, "transport.stop").result == sonora::DispatchResult::Ok, "stop");
    check(within(e, 1000, [&]() { return v.transport().state == TransportState::Stopped; }),
          "the block shows stopped");
    check(within(e, 1000, [&]() { return v.transport().take == first_take + 1; }),
          "the take number moved on");
    const auto after = v.transport();
    check(after.xruns == 0 && after.overruns == 0, "no xruns and no overruns");

    // Playback: asked for honestly. The engine has no playback path yet
    // (BACKLOG, DAW 1 track 1 item 3), so it must refuse by name rather
    // than claim to roll.
    const auto play = run(e, "transport.play");
    if (play.result == sonora::DispatchResult::Ok) {
        check(within(e, 1000, [&]() { return v.transport().state == TransportState::Rolling; }),
              "playback rolls");
        run(e, "transport.stop");
    } else {
        check(play.result == sonora::DispatchResult::NotImplemented,
              "playback is refused as not implemented, not failed");
    }

    std::printf("RESULT checks_failed=%d tracks=%u take=%d recorded_frames=%llu peak=%.3f"
                " play=%s\n",
                failed, tracks, first_take, static_cast<unsigned long long>(rolled), peak,
                sonora::dispatch_result_text(play.result));
    return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    sonora::client::SocketEngine::Options opts;
    int i = 1;
    if (i + 1 < argc && std::strcmp(argv[i], "--control-socket") == 0) {
        opts.endpoint = argv[i + 1];
        i += 2;
    }
    if (i >= argc) {
        std::fprintf(stderr,
                     "usage: sonora-ctl [--control-socket PATH] state | send COMMAND [TRACK]"
                     " | take SECONDS\n");
        return 2;
    }
    const std::string verb = argv[i++];
    // A one-shot tool does not reconnect.
    opts.reconnect_period_ns = 0;
    opts.connect_timeout_ms = 5000;
    sonora::client::SocketEngine engine(opts);
    std::string error;
    if (!engine.connect(error)) {
        std::fprintf(stderr, "sonora-ctl: %s\n", error.c_str());
        return 1;
    }
    if (verb == "state") {
        print_state(engine);
        return 0;
    }
    if (verb == "send" && i < argc) {
        const std::string command = argv[i++];
        const int32_t target = i < argc ? std::atoi(argv[i]) - 1 : -1;
        const auto r = run(engine, command, target);
        std::printf("REPLY %s %s\n", sonora::dispatch_result_text(r.result), r.message.c_str());
        return r.result == sonora::DispatchResult::Ok ? 0 : 1;
    }
    if (verb == "take" && i < argc) return take(engine, std::atof(argv[i]));
    std::fprintf(stderr, "sonora-ctl: unknown verb %s\n", verb.c_str());
    return 2;
}
