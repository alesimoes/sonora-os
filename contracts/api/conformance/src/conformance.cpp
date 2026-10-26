// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/api/conformance.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sonora/api/state_block.h"
#include "sonora/api/wire.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"
#include "sonora/model/session_file.h"

namespace sonora::api::conformance {

namespace {

constexpr uint64_t kStepNs = 2'000'000;
// Long enough for a real engine's control thread and publisher under a
// sanitizer; a fake answers on the first poll.
constexpr int kMaxSteps = 2500;

#define EXPECT(cond, why)                                         \
    do {                                                          \
        if (!(cond)) {                                            \
            std::fprintf(stderr, "    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return fail(why);                                     \
        }                                                         \
    } while (0)

class Check {
public:
    Check(Rig& rig, const char* name) : rig_(rig), name_(name) {}

    int fail(const std::string& why) {
        std::fprintf(stderr, "FAIL %s [%s]: %s\n", name_, rig_.name(), why.c_str());
        return 1;
    }
    int ok() {
        std::printf("ok   %s [%s]\n", name_, rig_.name());
        return 0;
    }
    int skip(const char* why) {
        std::printf("SKIP %s [%s]: %s\n", name_, rig_.name(), why);
        return 0;
    }

protected:
    Rig& rig_;
    const char* name_;
};

bool mentions(const std::string& text, const char* needle) {
    if (text.find(needle) != std::string::npos) return true;
    std::fprintf(stderr, "    expected \"%s\" in: %s\n", needle, text.c_str());
    return false;
}

// An engine, connected, and everything it has said so far.
struct Session {
    std::unique_ptr<EngineControl> engine;
    Inbox inbox;
    model::Replica replica;
    std::string error;

    bool open(Rig& rig) {
        engine = rig.make(kProtocolVersion, kStateLayoutVersion);
        return engine && engine->connect(error);
    }

    // Polls until `pred` holds, letting time pass between polls.
    bool until(Rig& rig, const std::function<bool()>& pred) {
        for (int i = 0; i < kMaxSteps; ++i) {
            engine->poll(inbox);
            feed_replica();
            if (pred()) return true;
            rig.wait_ns(kStepNs);
        }
        engine->poll(inbox);
        feed_replica();
        return pred();
    }

    const CommandReply* reply(uint64_t seq) const {
        for (const auto& r : inbox.replies) {
            if (r.seq == seq) return &r;
        }
        return nullptr;
    }

    bool await(Rig& rig, uint64_t seq, CommandReply& out) {
        if (!until(rig, [&]() { return reply(seq) != nullptr; })) return false;
        out = *reply(seq);
        return true;
    }

    // Waits until a publish has begun and ended since this call, so what
    // the state block says reflects every reply already received. A read
    // straight after a reply can still show the publish before it, and a
    // check that something did not change would pass on stale state.
    bool next_publish(Rig& rig) {
        const uint64_t hb = engine->state().transport().heartbeat;
        return until(rig, [&]() { return engine->state().transport().heartbeat >= hb + 2; });
    }

    const EngineEvent* last_event(EventKind kind) const {
        for (auto it = inbox.events.rbegin(); it != inbox.events.rend(); ++it) {
            if (it->kind == kind) return &*it;
        }
        return nullptr;
    }
    std::size_t count_events(EventKind kind) const {
        std::size_t n = 0;
        for (const auto& e : inbox.events) n += e.kind == kind ? 1 : 0;
        return n;
    }

    uint64_t command(const std::string& id, int32_t target = -1, const std::string& argument = {}) {
        CommandRequest r;
        r.command = id;
        r.target = target;
        r.argument = argument;
        return engine->send(r);
    }

    // Every model update goes to the replica as it arrives, the way an
    // interface's tick would hand it on.
    static_assert(static_cast<int>(model::ReplicationUpdate::Kind::SnapshotPart) ==
                          static_cast<int>(ModelUpdateKind::SnapshotPart),
                  "the model's update kinds and the wire's are listed in the same order");
    void feed_replica() {
        for (; fed_ < inbox.model.size(); ++fed_) {
            const ModelUpdate& m = inbox.model[fed_];
            model::ReplicationUpdate u;
            // The two enums list the same kinds in the same order.
            u.kind = static_cast<model::ReplicationUpdate::Kind>(m.kind);
            u.part_count = m.part_count;
            u.part_bytes = m.part_bytes;
            u.revision = m.revision;
            u.undo_floor = m.undo_floor;
            u.next_id = m.next_id;
            u.bytes = m.bytes;
            u.label = m.label;
            std::string why;
            if (!replica.receive(u, why)) replica_error = why;
        }
    }

    std::size_t fed_ = 0;
    std::string replica_error;
};

int check_hello(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            const Hello& h = s.engine->hello();
            EXPECT(h.protocol_version == kProtocolVersion, "hello names another protocol");
            EXPECT(h.state_layout_version == kStateLayoutVersion, "hello names another layout");
            EXPECT(h.sample_rate > 0, "no sample rate");
            EXPECT(h.track_count > 0, "no tracks");
            EXPECT(h.track_names.size() == h.track_count, "a name per track");
            EXPECT(s.engine->state().attached(), "the state block is not attached");
            EXPECT(s.engine->state().track_count() == h.track_count,
                   "the state block and Hello disagree on the track count");
            for (const std::string& id : h.implemented_commands) {
                EXPECT(is_engine_command(id), id + " is announced but is not an Engine command");
            }
            for (const char* id : {"transport.record", "transport.stop",
                                   "transport.toggle_record", "track.toggle_arm"}) {
                EXPECT(std::find(h.implemented_commands.begin(), h.implemented_commands.end(),
                                 id) != h.implemented_commands.end(),
                       std::string(id) + " is not announced");
            }
            return ok();
        }
    };
    return C(rig, "hello").go();
}

int check_version_mismatch(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            std::string error;
            auto newer = rig_.make(kProtocolVersion + 1, kStateLayoutVersion);
            EXPECT(!newer->connect(error), "a newer protocol was accepted");
            EXPECT(mentions(error, std::to_string(kProtocolVersion + 1).c_str()) &&
                       mentions(error, "protocol"),
                   "the refusal does not name the versions");
            // An engine from before the last bump: a version-3 engine sends
            // no Compound, but a version-4 interface would send it one.
            error.clear();
            auto older = rig_.make(kProtocolVersion - 1, kStateLayoutVersion);
            EXPECT(!older->connect(error), "an older protocol was accepted");
            EXPECT(mentions(error, std::to_string(kProtocolVersion - 1).c_str()) &&
                       mentions(error, std::to_string(kProtocolVersion).c_str()),
                   "the refusal of an older engine does not name both versions");
            error.clear();
            auto layout = rig_.make(kProtocolVersion, kStateLayoutVersion + 1);
            EXPECT(!layout->connect(error), "another state layout was accepted");
            EXPECT(mentions(error, "layout"), "the refusal does not name the layout");
            return ok();
        }
    };
    return C(rig, "version mismatch refused").go();
}

int check_dispatch_results(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            // Seqs rise across every kind of request.
            const uint64_t a = s.command("no.such.command");
            const uint64_t b = s.command("tool.smart");
            std::string unhandled;
            for (const auto& spec : command_catalog()) {
                const auto& impl = s.engine->hello().implemented_commands;
                if (spec.domain == CommandDomain::Engine &&
                    std::find(impl.begin(), impl.end(), spec.id) == impl.end()) {
                    unhandled = spec.id;
                    break;
                }
            }
            const uint64_t c = unhandled.empty() ? 0 : s.command(unhandled);
            ParameterChange p;
            p.track = 0;
            p.value = 1.0f;
            const uint64_t d = s.engine->send(p);
            EXPECT(a >= 1 && b > a && d > b && (c == 0 || (c > b && d > c)),
                   "seqs do not rise");
            CommandReply r;
            EXPECT(s.await(rig_, a, r), "no reply to an unknown command");
            EXPECT(r.result == DispatchResult::UnknownCommand, "unknown command not refused");
            EXPECT(s.await(rig_, b, r), "no reply to an interface command");
            EXPECT(r.result == DispatchResult::NotImplemented,
                   "the engine ran an Interface command");
            if (c != 0) {
                EXPECT(s.await(rig_, c, r), "no reply to an unhandled command");
                EXPECT(r.result == DispatchResult::NotImplemented,
                       unhandled + " has no handler and was not NotImplemented");
            }
            EXPECT(s.await(rig_, d, r), "no reply to a parameter change");
            // In the order they were sent.
            std::vector<uint64_t> order;
            for (const auto& x : s.inbox.replies) order.push_back(x.seq);
            EXPECT(std::is_sorted(order.begin(), order.end()), "replies out of order");
            return ok();
        }
    };
    return C(rig, "dispatch results and seqs").go();
}

int check_arming(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            const uint32_t n = s.engine->hello().track_count;
            StateView& v = s.engine->state();
            CommandReply r;
            EXPECT(s.await(rig_, s.command("track.toggle_arm"), r), "no reply");
            EXPECT(r.result == DispatchResult::Failed && mentions(r.message, "no track given"),
                   "an arm without a target guessed a track");
            EXPECT(s.await(rig_, s.command("track.toggle_arm", static_cast<int32_t>(n)), r),
                   "no reply");
            EXPECT(r.result == DispatchResult::Failed &&
                       mentions(r.message, ("there is no track " + std::to_string(n + 1)).c_str()),
                   "an arm past the last track was not refused by name");
            EXPECT(s.await(rig_, s.command("track.toggle_arm", 0), r), "no reply");
            EXPECT(r.result == DispatchResult::Ok, "arming track 1 failed: " + r.message);
            EXPECT(s.until(rig_, [&]() { return v.track(0).armed(); }),
                   "the state block never showed track 1 armed");
            EXPECT(!v.track(1).armed() || n < 2, "arming track 1 armed track 2");
            EXPECT(s.await(rig_, s.command("track.arm_all"), r) && r.result == DispatchResult::Ok,
                   "arm_all failed");
            EXPECT(s.until(rig_, [&]() {
                       for (uint32_t i = 0; i < n; ++i) if (!v.track(i).armed()) return false;
                       return true;
                   }),
                   "arm_all did not arm every track");
            EXPECT(s.await(rig_, s.command("track.disarm_all"), r) &&
                       r.result == DispatchResult::Ok,
                   "disarm_all failed");
            EXPECT(s.until(rig_, [&]() {
                       for (uint32_t i = 0; i < n; ++i) if (v.track(i).armed()) return false;
                       return true;
                   }),
                   "disarm_all left a track armed");
            return ok();
        }
    };
    return C(rig, "arming").go();
}

int check_take(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            StateView& v = s.engine->state();
            CommandReply r;
            EXPECT(s.await(rig_, s.command("transport.record"), r), "no reply");
            EXPECT(r.result == DispatchResult::Failed && mentions(r.message, "no track is armed"),
                   "a take with nothing armed was not refused by name");

            EXPECT(s.await(rig_, s.command("track.toggle_arm", 0), r) &&
                       r.result == DispatchResult::Ok,
                   "arming failed");
            EXPECT(s.until(rig_, [&]() { return !v.transport().stale; }), "state is stale");
            const int32_t take = v.transport().take;
            EXPECT(take >= 1, "no take number");
            EXPECT(s.await(rig_, s.command("transport.record"), r), "no reply");
            EXPECT(r.result == DispatchResult::Ok, "record failed: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       return v.transport().state == TransportState::Recording &&
                              v.transport().playhead > 0;
                   }),
                   "the state block never showed a take running");
            EXPECT(v.transport().take == take, "the open take is not the one announced");

            // A second record press leaves the take alone.
            EXPECT(s.await(rig_, s.command("transport.record"), r) &&
                       r.result == DispatchResult::Ok,
                   "a second record press failed");
            EXPECT(s.next_publish(rig_), "nothing published after the second press");
            EXPECT(v.transport().state == TransportState::Recording &&
                       v.transport().take == take,
                   "the second press stopped the take or opened another");

            for (const char* id : {"track.toggle_arm", "track.arm_all", "track.disarm_all"}) {
                EXPECT(s.await(rig_, s.command(id, 0), r), "no reply");
                EXPECT(r.result == DispatchResult::Failed && mentions(r.message, "during a take"),
                       std::string(id) + " was not refused during a take");
            }
            EXPECT(s.next_publish(rig_), "nothing published after the refusals");
            EXPECT(v.track(0).armed(), "a refused disarm disarmed track 1");

            EXPECT(s.await(rig_, s.command("transport.stop"), r), "no reply");
            EXPECT(r.result == DispatchResult::Ok, "stop failed: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       const TransportSnapshot t = v.transport();
                       return t.state == TransportState::Stopped && t.take == take + 1;
                   }),
                   "after stop, the next take number was not announced");
            EXPECT(s.await(rig_, s.command("transport.stop"), r) &&
                       r.result == DispatchResult::Ok,
                   "stop while stopped failed");
            return ok();
        }
    };
    return C(rig, "a take").go();
}

// A roll (ADR-0009, update "playback on the M4/M7 control server", item 4):
// play is answered at once and rolls once the window is resident, the
// playhead advances, stop holds it where it is, return to zero moves it to
// 0, and none of it uses a take number.
int check_play(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            StateView& v = s.engine->state();
            const auto& impl = s.engine->hello().implemented_commands;
            for (const char* id :
                 {"transport.play", "transport.toggle_play", "transport.return_to_zero"}) {
                EXPECT(std::find(impl.begin(), impl.end(), id) != impl.end(),
                       std::string(id) + " is not announced");
            }
            EXPECT(s.until(rig_, [&]() { return !v.transport().stale; }), "state is stale");
            const int32_t take = v.transport().take;
            const uint64_t from = v.transport().playhead;
            CommandReply r;
            EXPECT(s.await(rig_, s.command("transport.play"), r), "no reply to play");
            EXPECT(r.result == DispatchResult::Ok, "play failed: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       const TransportSnapshot t = v.transport();
                       return t.state == TransportState::Rolling && t.playhead > from;
                   }),
                   "the state block never showed a roll");
            const uint64_t rolled = v.transport().playhead;
            EXPECT(s.until(rig_, [&]() { return v.transport().playhead > rolled; }),
                   "the playhead did not advance while rolling");

            EXPECT(s.await(rig_, s.command("transport.stop"), r) &&
                       r.result == DispatchResult::Ok,
                   "stop failed: " + r.message);
            EXPECT(s.until(rig_, [&]() { return v.transport().state == TransportState::Stopped; }),
                   "the roll did not stop");
            EXPECT(s.next_publish(rig_), "nothing published after the stop");
            const uint64_t held = v.transport().playhead;
            EXPECT(s.next_publish(rig_), "nothing published while stopped");
            EXPECT(v.transport().playhead == held, "the playhead moved while stopped");
            EXPECT(v.transport().take == take, "a roll used a take number");

            EXPECT(s.await(rig_, s.command("transport.return_to_zero"), r) &&
                       r.result == DispatchResult::Ok,
                   "return to zero failed: " + r.message);
            EXPECT(s.until(rig_, [&]() { return v.transport().playhead == 0; }),
                   "return to zero did not move the playhead to 0");

            EXPECT(s.await(rig_, s.command("transport.toggle_play"), r) &&
                       r.result == DispatchResult::Ok,
                   "toggle_play failed: " + r.message);
            EXPECT(s.until(rig_, [&]() { return v.transport().state == TransportState::Rolling; }),
                   "toggle_play did not roll");
            EXPECT(s.await(rig_, s.command("transport.toggle_play"), r) &&
                       r.result == DispatchResult::Ok,
                   "the second toggle_play failed: " + r.message);
            EXPECT(s.until(rig_, [&]() { return v.transport().state == TransportState::Stopped; }),
                   "the second toggle_play did not stop the roll");

            // Space is toggle_play in both shipped keymaps and the only key
            // there that stops a take.
            EXPECT(s.await(rig_, s.command("track.toggle_arm", 0), r) &&
                       r.result == DispatchResult::Ok,
                   "arming failed: " + r.message);
            EXPECT(s.await(rig_, s.command("transport.record"), r) &&
                       r.result == DispatchResult::Ok,
                   "record failed: " + r.message);
            EXPECT(s.until(rig_,
                           [&]() { return v.transport().state == TransportState::Recording; }),
                   "the state block never showed the take running");
            EXPECT(s.await(rig_, s.command("transport.toggle_play"), r) &&
                       r.result == DispatchResult::Ok,
                   "toggle_play during a take failed: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       const TransportSnapshot t = v.transport();
                       return t.state == TransportState::Stopped && t.take == take + 1;
                   }),
                   "toggle_play did not close the take and announce the next number");
            EXPECT(s.await(rig_, s.command("track.toggle_arm", 0), r) &&
                       r.result == DispatchResult::Ok,
                   "disarming failed: " + r.message);
            return ok();
        }
    };
    return C(rig, "a roll").go();
}

// The click and the count-off (ADR-0020): both toggles are announced, each
// press is answered Ok and flips its flag on the state block, and neither
// touches the transport or the take number.
int check_click(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            StateView& v = s.engine->state();
            const auto& impl = s.engine->hello().implemented_commands;
            for (const char* id : {"transport.toggle_click", "transport.toggle_count_off"}) {
                EXPECT(std::find(impl.begin(), impl.end(), id) != impl.end(),
                       std::string(id) + " is not announced");
            }
            EXPECT(s.until(rig_, [&]() { return !v.transport().stale; }), "state is stale");
            const TransportSnapshot before = v.transport();
            EXPECT((before.transport_flags & (kTransportClick | kTransportCountOff)) == 0,
                   "the click or the count-off is on before anyone turned it on");
            for (const auto& toggle :
                 {std::make_pair("transport.toggle_click", kTransportClick),
                  std::make_pair("transport.toggle_count_off", kTransportCountOff)}) {
                const char* const id = toggle.first;
                const uint32_t flag = toggle.second;
                for (const bool on : {true, false}) {
                    CommandReply r;
                    EXPECT(s.await(rig_, s.command(id), r) && r.result == DispatchResult::Ok,
                           std::string(id) + " failed: " + r.message);
                    EXPECT(s.until(rig_,
                                   [&]() {
                                       return ((v.transport().transport_flags & flag) != 0) == on;
                                   }),
                           std::string(id) + " did not turn its flag " + (on ? "on" : "off"));
                }
            }
            const TransportSnapshot after = v.transport();
            EXPECT(after.state == TransportState::Stopped && after.take == before.take,
                   "a toggle moved the transport or the take number");
            return ok();
        }
    };
    return C(rig, "the click and the count-off").go();
}

// Loop playback (transport.toggle_loop, ADR-0016 update "the loop on
// switch"): announced, off in a new session, and a session setting, so the
// replica sees each press before its reply. Turned on with the selection as
// its argument, the selection becomes the range; turned off, the range
// stays, and the next press turns it on over that range. A selection that
// is not a range is refused by name and changes nothing.
int check_loop_toggle(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            const auto& impl = s.engine->hello().implemented_commands;
            EXPECT(std::find(impl.begin(), impl.end(), "transport.toggle_loop") != impl.end(),
                   "transport.toggle_loop is not announced");
            EXPECT(s.until(rig_, [&]() { return s.replica.synced(); }), "no snapshot");
            const auto transport = [&]() { return s.replica.current().settings.transport; };
            EXPECT(!transport().loop_playback, "loop playback is on in a new session");
            const int32_t take = s.engine->state().transport().take;
            CommandReply r;

            EXPECT(s.await(rig_, s.command("transport.toggle_loop", -1, "48000-96000"), r) &&
                       r.result == DispatchResult::Ok,
                   "toggle_loop with a selection failed: " + r.message);
            EXPECT(transport().loop_playback, "toggle_loop did not turn loop playback on");
            EXPECT(transport().loop[0] == 48000 && transport().loop[1] == 96000,
                   "the selection did not become the loop range");

            EXPECT(s.await(rig_, s.command("transport.toggle_loop"), r) &&
                       r.result == DispatchResult::Ok,
                   "toggle_loop off failed: " + r.message);
            EXPECT(!transport().loop_playback, "toggle_loop did not turn loop playback off");
            EXPECT(transport().loop[0] == 48000 && transport().loop[1] == 96000,
                   "turning loop playback off lost the range");

            EXPECT(s.await(rig_, s.command("transport.toggle_loop", -1, "96000-48000"), r),
                   "no reply to a backwards selection");
            EXPECT(r.result == DispatchResult::Failed && mentions(r.message, "A before B"),
                   "a backwards selection was not refused by name: " + r.message);
            EXPECT(!transport().loop_playback && transport().loop[0] == 48000,
                   "a refused toggle changed the document");

            EXPECT(s.await(rig_, s.command("transport.toggle_loop"), r) &&
                       r.result == DispatchResult::Ok,
                   "toggle_loop on again failed: " + r.message);
            EXPECT(transport().loop_playback && transport().loop[0] == 48000 &&
                       transport().loop[1] == 96000,
                   "toggle_loop with no selection did not loop the range it kept");
            EXPECT(s.await(rig_, s.command("transport.toggle_loop"), r) &&
                       r.result == DispatchResult::Ok,
                   "the last toggle_loop failed: " + r.message);
            EXPECT(s.engine->state().transport().state == TransportState::Stopped &&
                       s.engine->state().transport().take == take,
                   "a toggle moved the transport or the take number");
            return ok();
        }
    };
    return C(rig, "loop playback on and off").go();
}

// transport.locate (marker recall): stopped, the playhead moves to the
// frame and stays; rolling, it jumps there and the roll goes on; a frame
// that is not one, and any locate during a take, is refused by name.
int check_locate(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            StateView& v = s.engine->state();
            const auto& impl = s.engine->hello().implemented_commands;
            EXPECT(std::find(impl.begin(), impl.end(), "transport.locate") != impl.end(),
                   "transport.locate is not announced");
            EXPECT(s.until(rig_, [&]() { return !v.transport().stale; }), "state is stale");
            const uint64_t rate = s.engine->hello().sample_rate;
            const int32_t take = v.transport().take;
            CommandReply r;

            const uint64_t stopped_at = 3ull * rate + 17;
            EXPECT(s.await(rig_, s.command("transport.locate", -1, std::to_string(stopped_at)), r) &&
                       r.result == DispatchResult::Ok,
                   "locate while stopped failed: " + r.message);
            EXPECT(s.until(rig_, [&]() { return v.transport().playhead == stopped_at; }),
                   "locate did not move the playhead to the frame");
            EXPECT(s.next_publish(rig_), "nothing published after the locate");
            EXPECT(v.transport().state == TransportState::Stopped &&
                       v.transport().playhead == stopped_at,
                   "a locate while stopped started the transport or did not hold");

            EXPECT(s.await(rig_, s.command("transport.play"), r) &&
                       r.result == DispatchResult::Ok,
                   "play failed: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       const TransportSnapshot t = v.transport();
                       return t.state == TransportState::Rolling && t.playhead > stopped_at;
                   }),
                   "the roll did not start from the located frame");
            const uint64_t rolling_at = 60ull * rate;
            EXPECT(s.await(rig_, s.command("transport.locate", -1, std::to_string(rolling_at)), r) &&
                       r.result == DispatchResult::Ok,
                   "locate while rolling failed: " + r.message);
            EXPECT(s.until(rig_, [&]() { return v.transport().playhead >= rolling_at; }),
                   "locate while rolling did not jump to the frame");
            EXPECT(v.transport().state == TransportState::Rolling, "locate while rolling stopped the roll");
            const uint64_t jumped = v.transport().playhead;
            EXPECT(s.until(rig_, [&]() { return v.transport().playhead > jumped; }),
                   "the roll did not go on after the locate");
            EXPECT(s.await(rig_, s.command("transport.stop"), r) &&
                       r.result == DispatchResult::Ok,
                   "stop failed: " + r.message);
            EXPECT(s.until(rig_, [&]() { return v.transport().state == TransportState::Stopped; }),
                   "the roll did not stop");

            for (const char* bad : {"-1", "", "abc", "281474976710657"}) {
                EXPECT(s.await(rig_, s.command("transport.locate", -1, bad), r) &&
                           r.result == DispatchResult::Failed && !r.message.empty(),
                       std::string("locate to \"") + bad + "\" was not refused by name");
            }

            EXPECT(s.await(rig_, s.command("track.toggle_arm", 0), r) &&
                       r.result == DispatchResult::Ok,
                   "arming failed: " + r.message);
            EXPECT(s.await(rig_, s.command("transport.record"), r) &&
                       r.result == DispatchResult::Ok,
                   "record failed: " + r.message);
            EXPECT(s.until(rig_,
                           [&]() { return v.transport().state == TransportState::Recording; }),
                   "the state block never showed the take running");
            EXPECT(s.await(rig_, s.command("transport.locate", -1, "0"), r) &&
                       r.result == DispatchResult::Failed &&
                       r.message.find("take") != std::string::npos,
                   "locate during a take was not refused by name: " + r.message);
            EXPECT(v.transport().state == TransportState::Recording, "a refused locate ended the take");
            EXPECT(s.await(rig_, s.command("transport.stop"), r) &&
                       r.result == DispatchResult::Ok,
                   "stop failed: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       const TransportSnapshot t = v.transport();
                       return t.state == TransportState::Stopped && t.take == take + 1;
                   }),
                   "the take did not close");
            EXPECT(s.await(rig_, s.command("track.toggle_arm", 0), r) &&
                       r.result == DispatchResult::Ok,
                   "disarming failed: " + r.message);
            EXPECT(s.await(rig_, s.command("transport.return_to_zero"), r) &&
                       r.result == DispatchResult::Ok,
                   "return to zero failed: " + r.message);
            return ok();
        }
    };
    return C(rig, "locate").go();
}

// transport.punch, on the fly: while rolling it punches in, and during the
// take it punches out with the transport still rolling, the next take
// number announced; a second punch in opens that take.
int check_punch_on_the_fly(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            StateView& v = s.engine->state();
            const auto& impl = s.engine->hello().implemented_commands;
            EXPECT(std::find(impl.begin(), impl.end(), "transport.punch") != impl.end(),
                   "transport.punch is not announced");
            CommandReply r;
            EXPECT(s.await(rig_, s.command("track.toggle_arm", 0), r) &&
                       r.result == DispatchResult::Ok,
                   "arming failed: " + r.message);
            EXPECT(s.until(rig_, [&]() { return !v.transport().stale; }), "state is stale");
            const int32_t take = v.transport().take;
            EXPECT(s.await(rig_, s.command("transport.play"), r) &&
                       r.result == DispatchResult::Ok,
                   "play failed: " + r.message);
            EXPECT(s.until(rig_, [&]() { return v.transport().state == TransportState::Rolling; }),
                   "the state block never showed a roll");

            EXPECT(s.await(rig_, s.command("transport.punch"), r), "no reply to the punch in");
            EXPECT(r.result == DispatchResult::Ok, "punch in failed: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       const TransportSnapshot t = v.transport();
                       return t.state == TransportState::Recording && t.take == take;
                   }),
                   "the punch in did not start the announced take");

            EXPECT(s.await(rig_, s.command("transport.punch"), r), "no reply to the punch out");
            EXPECT(r.result == DispatchResult::Ok, "punch out failed: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       const TransportSnapshot t = v.transport();
                       return t.state == TransportState::Rolling && t.take == take + 1;
                   }),
                   "the punch out stopped the transport or did not announce the next take");
            const uint64_t rolled = v.transport().playhead;
            EXPECT(s.until(rig_, [&]() { return v.transport().playhead > rolled; }),
                   "the playhead did not advance after the punch out");

            EXPECT(s.await(rig_, s.command("transport.punch"), r) &&
                       r.result == DispatchResult::Ok,
                   "the second punch in failed: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       const TransportSnapshot t = v.transport();
                       return t.state == TransportState::Recording && t.take == take + 1;
                   }),
                   "the second punch in did not open the next take");

            EXPECT(s.await(rig_, s.command("transport.stop"), r) &&
                       r.result == DispatchResult::Ok,
                   "stop failed: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       const TransportSnapshot t = v.transport();
                       return t.state == TransportState::Stopped && t.take == take + 2;
                   }),
                   "stop did not close the second take");

            // A punch range set (PUNCH lit) keeps the take inside it: past
            // B the punch is refused by name and the roll goes on.
            EXPECT(s.until(rig_, [&]() { return s.replica.synced(); }), "no snapshot");
            const auto set_punch = [&](model::Frames a, model::Frames b) {
                model::SetSettings set;
                set.settings = s.replica.current().settings;
                set.settings.transport.punch = {{a, b}};
                ModelRequest m;
                std::string error;
                CommandReply sr;
                return model::encode_operation(set, m.op, error) &&
                       s.await(rig_, s.engine->submit(m), sr) && sr.result == DispatchResult::Ok;
            };
            EXPECT(set_punch(1, 2), "the punch range was refused");
            EXPECT(s.await(rig_, s.command("transport.play"), r) &&
                       r.result == DispatchResult::Ok,
                   "play failed: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       const TransportSnapshot t = v.transport();
                       return t.state == TransportState::Rolling && t.playhead > 2;
                   }),
                   "the state block never showed a roll past the range");
            EXPECT(s.await(rig_, s.command("transport.punch"), r), "no reply to the late punch");
            EXPECT(r.result == DispatchResult::Failed && mentions(r.message, "before the playhead"),
                   "a punch past the range was not refused by name: " + r.message);
            EXPECT(s.next_publish(rig_), "nothing published after the refusal");
            EXPECT(v.transport().state == TransportState::Rolling && v.transport().take == take + 2,
                   "the refused punch stopped the roll or used a take number");
            EXPECT(s.await(rig_, s.command("transport.stop"), r) &&
                       r.result == DispatchResult::Ok,
                   "stop failed: " + r.message);
            EXPECT(set_punch(0, 0), "clearing the punch range was refused");
            EXPECT(s.await(rig_, s.command("track.toggle_arm", 0), r) &&
                       r.result == DispatchResult::Ok,
                   "disarming failed: " + r.message);
            return ok();
        }
    };
    return C(rig, "punch on the fly").go();
}

int check_parameters(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            StateView& v = s.engine->state();
            ParameterChange p;
            p.track = 0;
            p.param = ParamId::Gain;
            p.value = 0.5f;
            CommandReply r;
            EXPECT(s.await(rig_, s.engine->send(p), r) && r.result == DispatchResult::Ok,
                   "a gain change failed: " + r.message);
            EXPECT(s.until(rig_, [&]() { return v.track(0).gain == 0.5f; }),
                   "the state block never showed the gain");
            p.track = s.engine->hello().track_count;
            EXPECT(s.await(rig_, s.engine->send(p), r) && r.result == DispatchResult::Failed,
                   "a gain on a track that does not exist was accepted");
            p.track = 0;
            p.value = std::numeric_limits<float>::quiet_NaN();
            EXPECT(s.await(rig_, s.engine->send(p), r) && r.result == DispatchResult::Failed,
                   "a NaN gain was accepted");
            EXPECT(s.next_publish(rig_), "nothing published after the refusals");
            EXPECT(v.track(0).gain == 0.5f, "a refused gain changed the track");
            return ok();
        }
    };
    return C(rig, "parameter changes").go();
}

// The document's mix is what the faders hold: a SetMix moves the track's
// gain and mute, a ride sent as a ParameterChange outlives an unrelated
// edit, and a Snapshot (here a Resync) puts the document's values back,
// so a ride a lost link stranded does not stay (ADR-0009 §4 item 3). An
// engine track the document no longer has keeps its fader and mute.
int check_mix(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            EXPECT(s.until(rig_, [&]() { return s.replica.synced(); }),
                   "no snapshot after connect: " + s.replica_error);
            StateView& v = s.engine->state();
            const model::TrackMix* before =
                s.replica.current().mix.find(s.replica.current().edit->tracks[0]->id);
            EXPECT(before != nullptr, "track 1 has no mix");
            model::SetMix set{*before};
            set.mix.gain_db = -6.0f;
            set.mix.mute = true;
            const float doc_gain = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
            const auto at_doc = [&]() {
                const TrackSnapshot t = v.track(0);
                return std::fabs(t.gain - doc_gain) < 1e-5f && t.muted();
            };
            ModelRequest m;
            m.action = ModelAction::Apply;
            std::string error;
            EXPECT(model::encode_operation(set, m.op, error), error);
            CommandReply r;
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "SetMix was refused: " + r.message);
            EXPECT(s.until(rig_, at_doc), "the state block does not show the document's mix");

            ParameterChange p;
            p.track = 0;
            p.value = 0.5f;
            EXPECT(s.await(rig_, s.engine->send(p), r) && r.result == DispatchResult::Ok,
                   "a gain change failed: " + r.message);
            EXPECT(s.until(rig_, [&]() { return v.track(0).gain == 0.5f; }),
                   "the state block never showed the ride");
            model::AddMarker add;
            add.marker.id = s.replica.current().next_id;
            add.marker.name = "Ride";
            EXPECT(model::encode_operation(add, m.op, error), error);
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "a marker was refused: " + r.message);
            EXPECT(s.next_publish(rig_), "nothing published after the marker");
            EXPECT(v.track(0).gain == 0.5f, "an unrelated edit snapped the ride back");

            m.action = ModelAction::Resync;
            m.op.clear();
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "resync failed");
            EXPECT(s.until(rig_, at_doc), "a resync left the ride's gain in place");

            // A session without the last track: that engine track keeps its
            // fader and mute, since until the plan swap the old plan still
            // plays it there, and a muted track must not come up at unity.
            const uint32_t last = s.engine->hello().track_count - 1;
            const model::Id last_id = s.replica.current().edit->tracks[last]->id;
            model::SetMix quiet{*s.replica.current().mix.find(last_id)};
            quiet.mix.gain_db = -6.0f;
            quiet.mix.mute = true;
            m.action = ModelAction::Apply;
            EXPECT(model::encode_operation(quiet, m.op, error), error);
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "SetMix was refused: " + r.message);
            const auto last_quiet = [&]() {
                const TrackSnapshot t = v.track(last);
                return std::fabs(t.gain - doc_gain) < 1e-5f && t.muted();
            };
            EXPECT(s.until(rig_, last_quiet), "the last track's mix was not applied");
            model::Snapshot fewer = s.replica.current();
            auto edit = std::make_shared<model::EditState>(*fewer.edit);
            edit->tracks.pop_back();
            fewer.edit = edit;
            model::WriteInfo info;
            info.written_by = "conformance";
            EXPECT(model::serialize(fewer, info, m.op, error), error);
            m.op = wire::encode_load("", m.op);
            m.action = ModelAction::Load;
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "a session one track short was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       return s.replica.synced() &&
                              s.replica.current().edit->tracks.size() == last;
                   }),
                   "no snapshot of the shorter session: " + s.replica_error);
            EXPECT(s.next_publish(rig_), "nothing published after the load");
            EXPECT(last_quiet(), "a track the session no longer has lost its mute or fader");
            return ok();
        }
    };
    return C(rig, "the mix follows the document").go();
}

// Pro Tools' TrackInput: a track the document puts in Input mode shows it
// in the state block, which is what the engine does with it, and back on
// Auto it does not. A session loaded with a track in Input mode keeps it,
// in the replica and on the engine.
int check_input_monitor(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            EXPECT(s.until(rig_, [&]() { return s.replica.synced(); }),
                   "no snapshot after connect: " + s.replica_error);
            StateView& v = s.engine->state();
            const model::Id id = s.replica.current().edit->tracks[0]->id;
            const model::TrackMix* before = s.replica.current().mix.find(id);
            EXPECT(before != nullptr, "track 1 has no mix");
            EXPECT(before->monitor == model::MonitorMode::Auto, "track 1 does not start on Auto");
            EXPECT(s.next_publish(rig_), "nothing published after connect");
            EXPECT(!v.track(0).input_monitor(), "an Auto track is shown in Input mode");

            model::SetMix set{*before};
            set.mix.monitor = model::MonitorMode::Input;
            ModelRequest m;
            m.action = ModelAction::Apply;
            std::string error;
            EXPECT(model::encode_operation(set, m.op, error), error);
            CommandReply r;
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "SetMix to Input was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() { return v.track(0).input_monitor(); }),
                   "the state block does not show track 1 in Input mode");
            EXPECT(!v.track(1).input_monitor(), "track 2 followed track 1 into Input mode");

            set.mix.monitor = model::MonitorMode::Auto;
            EXPECT(model::encode_operation(set, m.op, error), error);
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "SetMix back to Auto was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() { return !v.track(0).input_monitor(); }),
                   "track 1 stayed in Input mode back on Auto");

            model::Snapshot saved = s.replica.current();
            for (auto& t : saved.mix.tracks) {
                if (t.track == id) t.monitor = model::MonitorMode::Input;
            }
            model::WriteInfo info;
            info.written_by = "conformance";
            EXPECT(model::serialize(saved, info, m.op, error), error);
            m.op = wire::encode_load("", m.op);
            m.action = ModelAction::Load;
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "a session with a track in Input mode was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       const model::TrackMix* t = s.replica.current().mix.find(id);
                       return s.replica.synced() && t != nullptr &&
                              t->monitor == model::MonitorMode::Input;
                   }),
                   "the loaded session lost track 1's Input mode: " + s.replica_error);
            EXPECT(s.until(rig_, [&]() { return v.track(0).input_monitor(); }),
                   "the engine does not monitor track 1's input after the load");

            m.action = ModelAction::Apply;
            EXPECT(model::encode_operation(set, m.op, error), error);
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "SetMix back to Auto was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() { return !v.track(0).input_monitor(); }),
                   "track 1 stayed in Input mode after the load");
            return ok();
        }
    };
    return C(rig, "input monitor mode").go();
}

// ADR-0009 update 2026-10-14: several operations as one. One reply, one
// update, one undo step; a member refused halfway refuses the whole of it,
// naming the member, and sends nothing.
int check_compound(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            EXPECT(s.until(rig_, [&]() { return s.replica.synced(); }),
                   "no snapshot after connect: " + s.replica_error);
            const model::Snapshot before = s.replica.current();
            const model::Id track = before.edit->tracks[0]->id;
            const std::string name = before.edit->tracks[0]->name;

            model::Compound c;
            c.label = "Paste";
            c.ops = {model::AddMarker{model::Marker{before.next_id, 4800, "A", 0}},
                     model::AddMarker{model::Marker{before.next_id + 1, 9600, "B", 0}},
                     model::SetTrackName{track, "Lead"}};
            ModelRequest m;
            m.action = ModelAction::Apply;
            std::string error;
            EXPECT(model::encode_operation(model::Operation{c}, m.op, error), error);
            const std::size_t updates = s.inbox.model.size();
            CommandReply r;
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "a valid compound was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() { return s.inbox.model.size() == updates + 1; }),
                   "no update for the compound: " + s.replica_error);
            EXPECT(s.inbox.model.back().kind == ModelUpdateKind::Op, "not one Op update");
            EXPECT(s.replica.current().revision == before.revision + 1,
                   "a compound is not one revision");
            EXPECT(s.replica.current().edit->markers->markers.size() == 2 &&
                       s.replica.current().edit->tracks[0]->name == "Lead",
                   "the replica does not hold every member");

            // One undo takes the whole of it back.
            m.action = ModelAction::Undo;
            m.op.clear();
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "undo failed");
            EXPECT(s.until(rig_, [&]() { return s.inbox.model.size() == updates + 2; }),
                   "no update for the undo");
            EXPECT(s.inbox.model.back().kind == ModelUpdateKind::Undo &&
                       s.inbox.model.back().label == "Paste",
                   "the undo does not name the compound it undid");
            EXPECT(s.replica.current().edit->markers->markers.empty() &&
                       s.replica.current().edit->tracks[0]->name == name,
                   "one undo did not take the compound back");

            // Refused at its second member: nothing applied, nothing sent.
            const model::Snapshot here = s.replica.current();
            model::Compound bad;
            bad.ops = {model::SetTrackName{track, "Half"}, model::RemoveMarker{999999}};
            m.action = ModelAction::Apply;
            EXPECT(model::encode_operation(model::Operation{bad}, m.op, error), error);
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Failed,
                   "a compound with a bad member was accepted");
            EXPECT(mentions(r.message, "operation 2 of 2 (RemoveMarker)"),
                   "the refusal does not name the member");
            EXPECT(s.next_publish(rig_), "nothing published after the refusal");
            EXPECT(s.inbox.model.size() == updates + 2, "a refused compound sent an update");
            EXPECT(s.replica.current() == here, "a refused compound changed the replica");
            return ok();
        }
    };
    return C(rig, "compound operations").go();
}

int check_model(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            // The replica starts from the whole document, unasked.
            EXPECT(s.until(rig_, [&]() { return s.replica.synced(); }),
                   "no snapshot after connect: " + s.replica_error);
            EXPECT(s.replica.current().edit->tracks.size() == s.engine->hello().track_count,
                   "the document's tracks are not the engine's");
            const uint64_t rev0 = s.replica.current().revision;

            model::AddMarker add;
            add.marker.id = s.replica.current().next_id;
            add.marker.position = 48000;
            add.marker.name = "Verse";
            std::string bytes, error;
            EXPECT(model::encode_operation(add, bytes, error), error);
            ModelRequest m;
            m.action = ModelAction::Apply;
            m.op = bytes;
            CommandReply r;
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "a valid operation was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() { return s.replica.current().revision == rev0 + 1; }),
                   "no update for the operation: " + s.replica_error);
            EXPECT(s.replica.current().edit->markers->markers.size() == 1, "no marker");
            const ModelUpdate& op = s.inbox.model.back();
            EXPECT(op.kind == ModelUpdateKind::Op && op.undo_floor <= op.revision,
                   "the update is not an Op with its floor");

            m.action = ModelAction::Undo;
            m.op.clear();
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "undo failed");
            EXPECT(s.until(rig_, [&]() { return s.replica.current().revision == rev0 + 2; }),
                   "no update for the undo");
            EXPECT(s.inbox.model.back().kind == ModelUpdateKind::Undo, "not an Undo");
            EXPECT(s.replica.current().edit->markers->markers.empty(), "undo left the marker");
            m.action = ModelAction::Redo;
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "redo failed");
            EXPECT(s.until(rig_, [&]() { return s.replica.current().revision == rev0 + 3; }),
                   "no update for the redo");
            EXPECT(s.replica.current().edit->markers->markers.size() == 1, "redo lost it");

            // Refusals change nothing and send no update.
            const std::size_t updates = s.inbox.model.size();
            m.action = ModelAction::Apply;
            m.op = "{";
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Failed,
                   "bytes that are not an operation were accepted");
            EXPECT(mentions(r.message, "operation"), "the refusal does not say why");
            model::RemoveMarker gone;
            gone.marker = 999999;
            EXPECT(model::encode_operation(gone, m.op, error), error);
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Failed,
                   "an operation on nothing was accepted");
            EXPECT(!r.message.empty(), "the refusal does not say why");
            m.action = ModelAction::Redo;
            m.op.clear();
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Failed,
                   "a redo with nothing to redo was accepted");
            EXPECT(s.inbox.model.size() == updates, "a refusal sent an update");

            m.action = ModelAction::Resync;
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "resync failed");
            EXPECT(s.until(rig_, [&]() { return s.inbox.model.size() == updates + 1; }),
                   "no snapshot for the resync");
            EXPECT(s.inbox.model.back().kind == ModelUpdateKind::Snapshot, "not a Snapshot");
            EXPECT(s.replica.synced() && s.replica.current().revision == rev0 + 3,
                   "the resync moved the revision");

            // The snapshot carries the undo window: undo and redo after it
            // go on applying with no further snapshot.
            EXPECT(s.replica.can_undo(), "the resynced replica has no undo");
            const ModelAction moves[] = {ModelAction::Undo, ModelAction::Redo, ModelAction::Undo};
            uint64_t rev = rev0 + 3;
            for (const ModelAction a : moves) {
                m.action = a;
                EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                       "undo or redo after the resync failed: " + r.message);
                ++rev;
                EXPECT(s.until(rig_, [&]() { return s.replica.current().revision == rev; }),
                       "the replica lost track after the resync: " + s.replica_error);
            }
            EXPECT(s.replica_error.empty(), "the replica refused an update: " + s.replica_error);
            EXPECT(s.replica.synced() && s.replica.current().edit->markers->markers.empty(),
                   "undo after the resync did not remove the marker");
            EXPECT(s.inbox.model.size() == updates + 4, "a further snapshot was needed");
            return ok();
        }
    };
    return C(rig, "model operations, undo, redo, resync").go();
}

// File > Open on the engine's document: a session file's bytes replace it,
// every replica is sent the Snapshot, and undo does not reach back into the
// session before. A file that does not load, or one at another rate, is
// refused with a reason and changes nothing.
int check_load(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            EXPECT(s.until(rig_, [&]() { return s.replica.synced(); }), "no snapshot");
            model::AddMarker add;
            add.marker.id = s.replica.current().next_id;
            add.marker.position = 1000;
            add.marker.name = "Before";
            ModelRequest m;
            std::string error;
            EXPECT(model::encode_operation(add, m.op, error), error);
            CommandReply r;
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "the marker was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() { return s.replica.can_undo(); }), "no undo step");

            // Another session: the engine's own, renamed, marker and all.
            model::Snapshot other = s.replica.current();
            other.settings.name = "Opened";
            std::string bytes;
            model::WriteInfo info;
            info.written_by = "conformance";
            EXPECT(model::serialize(other, info, bytes, error), error);
            const std::size_t updates = s.inbox.model.size();
            m.action = ModelAction::Load;
            m.op = wire::encode_load("", bytes);
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "a valid session was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() { return s.inbox.model.size() > updates; }),
                   "no snapshot after the load");
            EXPECT(s.inbox.model.back().kind == ModelUpdateKind::Snapshot, "not a Snapshot");
            EXPECT(s.replica.synced() && s.replica.current().settings.name == "Opened",
                   "the replica is not the opened session: " + s.replica_error);
            EXPECT(s.replica.current().edit->markers->markers.size() == 1,
                   "the opened session lost its marker");
            EXPECT(!s.replica.can_undo(), "undo reaches into the session before");

            // Refusals.
            const std::size_t after = s.inbox.model.size();
            m.op = wire::encode_load("", "{\"not\": \"a session\"}");
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Failed,
                   "bytes that are not a session were loaded");
            EXPECT(mentions(r.message, "session"), "the refusal does not say why");
            other.settings.sample_rate = s.engine->hello().sample_rate == 44100 ? 48000 : 44100;
            EXPECT(model::serialize(other, info, m.op, error), error);
            m.op = wire::encode_load("", m.op);
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Failed,
                   "a session at another rate was loaded");
            EXPECT(mentions(r.message, "Hz"), "the refusal does not name the rates");
            EXPECT(s.inbox.model.size() == after, "a refusal sent an update");
            EXPECT(s.replica.current().settings.name == "Opened", "a refusal changed the document");
            return ok();
        }
    };
    return C(rig, "load another session").go();
}

// ADR-0015 §4, protocol 10: a session with MIDI opened as a Load in parts,
// its sequences' chunks ahead in LoadPart requests, a sequence across
// several parts where it does not fit one, and sent back to every
// replica as a Snapshot in parts: header, the chunk table's parts, the
// window, with nothing in between. A part that does not decode abandons the
// Load, which is then refused with its reason and changes nothing.
int check_load_in_parts(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            EXPECT(s.until(rig_, [&]() { return s.replica.synced(); }), "no snapshot");
            model::History h(s.replica.current());
            model::AddTrack t;
            t.track = h.current().next_id;
            t.playlist = t.track + 1;
            t.name = "Keys";
            t.playlist_name = "Keys.01";
            t.kind = model::TrackKind::Midi;
            t.channels = 0;
            std::string error;
            EXPECT(h.apply(t, error), error);
            for (int k = 0; k < 3; ++k) {
                model::MidiLists l;
                for (int i = 0; i < 400; ++i) {
                    l.events.push_back(model::MidiEvent{i * 24000, 0xB0, 1,
                                                        static_cast<uint8_t>((i + k) & 0x7F)});
                }
                model::MidiRegion r;
                r.id = h.current().next_id;
                r.name = "Keys_0" + std::to_string(k);
                r.position = k * 40 * model::kTicksPerQuarter;
                r.length = 20 * model::kTicksPerQuarter;
                r.data = std::make_shared<const model::MidiSequence>(l);
                EXPECT(h.apply(model::PlaceMidiRegion{t.playlist, r, 0, {}}, error), error);
            }
            model::Snapshot other = h.current();
            other.settings.name = "Opened in parts";
            model::WriteInfo info;
            info.written_by = "conformance";
            std::string session;
            std::vector<std::string> parts;
            EXPECT(model::encode_load_parts(other, info, session, parts, error, nullptr, 1500),
                   error);
            EXPECT(parts.size() > 3, "expected the sequences split across parts");

            const std::size_t updates = s.inbox.model.size();
            ModelRequest m;
            CommandReply r;
            for (const std::string& p : parts) {
                m.action = ModelAction::LoadPart;
                m.op = p;
                EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                       "a part was refused: " + r.message);
            }
            EXPECT(s.inbox.model.size() == updates, "a part sent an update");
            m.action = ModelAction::Load;
            m.op = wire::encode_load("", session);
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "the session was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       return s.replica.synced() &&
                              s.replica.current().settings.name == "Opened in parts";
                   }),
                   "the replica is not the opened session: " + s.replica_error);
            EXPECT(s.inbox.model.size() >= updates + 3 &&
                       s.inbox.model[updates].kind == ModelUpdateKind::SnapshotHeader,
                   "the Snapshot did not begin with its header");
            const ModelUpdate& head = s.inbox.model[updates];
            EXPECT(s.inbox.model.size() == updates + head.part_count + 2,
                   "something came between the header and the window");
            for (uint64_t i = 1; i <= head.part_count; ++i) {
                EXPECT(s.inbox.model[updates + i].kind == ModelUpdateKind::SnapshotPart &&
                           s.inbox.model[updates + i].bytes.size() <= wire::kMaxPartBytes,
                       "not a part");
            }
            EXPECT(s.inbox.model.back().kind == ModelUpdateKind::Snapshot, "no window last");
            const auto& regions =
                s.replica.current().edit->tracks.back()->midi_playlists[0]->regions;
            EXPECT(regions.size() == 3 && !regions[1].data->missing &&
                       regions[1].data->events().size() == 400 &&
                       regions[1].data->events()[5].data2 == 6,
                   "the opened session's MIDI did not arrive");

            // A part that does not decode: refused, and the Load after it too.
            const std::size_t after = s.inbox.model.size();
            m.action = ModelAction::LoadPart;
            m.op = parts[0].substr(0, parts[0].size() - 1);
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Failed,
                   "a part cut short was accepted");
            other.settings.name = "Never opened";
            EXPECT(model::encode_load_parts(other, info, session, parts, error, nullptr, 1500),
                   error);
            m.action = ModelAction::Load;
            m.op = wire::encode_load("", session);
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Failed,
                   "a Load after a refused part was opened");
            EXPECT(mentions(r.message, "a part of the session was refused"),
                   "the refusal does not name the part: " + r.message);
            EXPECT(s.inbox.model.size() == after, "a refusal sent an update");
            EXPECT(s.replica.current().settings.name == "Opened in parts",
                   "a refusal changed the document");
            return ok();
        }
    };
    return C(rig, "load in parts, snapshot in parts").go();
}

// A take that stops writing is said at once, with the take and the reason,
// and on the state block; stopping it reports the take as failed; the next
// take that records cleanly clears both.
// Inserts (ADR-0012 §8). Every engine sends a catalog after the Snapshot.
// One that offers a usable plugin with the first track's channel count is
// held to the whole path: the insert in the document and the state block,
// its parameters checked against the catalog, its bypass shown, undo
// taking it out. One that offers none (an engine built before
// engine/plugin_client) must refuse an insert by saying why, as
// NotImplemented or Failed, and change nothing.
int check_inserts(Rig& rig) {
    struct C : Check {
        using Check::Check;

        bool apply(Session& s, const model::Operation& op, CommandReply& r) {
            ModelRequest m;
            m.action = ModelAction::Apply;
            std::string error;
            if (!model::encode_operation(op, m.op, error)) return false;
            return s.await(rig_, s.engine->submit(m), r);
        }

        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            EXPECT(s.until(rig_, [&]() { return s.replica.synced() && !s.inbox.catalogs.empty(); }),
                   "no plugin catalog after the snapshot");
            const PluginCatalog catalog = s.inbox.catalogs.back();
            // Protocol 14 (ADR-0017 Y2): only a VST3 is bridged, by yabridge;
            // VST2 and CLAP are not hosted, natively or under Wine (Y6).
            for (const PluginInfo& p : catalog.plugins) {
                EXPECT(p.bridge == PluginBridge::None || p.format == PluginFormat::Vst3,
                       p.name + " is marked as running under Wine but is not a VST3");
            }
            const model::TrackEdit& track = *s.replica.current().edit->tracks.at(0);
            const PluginInfo* usable = nullptr;
            for (const PluginInfo& p : catalog.plugins) {
                if (p.usable && !p.params.empty() && p.inputs == track.channels &&
                    p.outputs == track.channels) {
                    usable = &p;
                    break;
                }
            }
            StateView& v = s.engine->state();
            model::AddInsert add;
            add.track = track.id;
            add.insert = s.replica.current().next_id;
            add.slot = 2;
            add.plugin = usable != nullptr
                             ? model::PluginRef{static_cast<model::PluginFormat>(usable->format),
                                                usable->id, usable->name, usable->version}
                             : model::PluginRef{model::PluginFormat::Lv2,
                                                "urn:sonora:conformance:absent", "Absent", "0"};
            const uint64_t rev0 = s.replica.current().revision;
            const std::size_t updates = s.inbox.model.size();
            CommandReply r;
            EXPECT(apply(s, add, r), "no reply to AddInsert");

            PluginParameterChange c;
            c.insert = add.insert;
            c.key = usable != nullptr ? usable->params[0].key : std::string("gain");
            if (usable == nullptr) {
                EXPECT(r.result == DispatchResult::NotImplemented ||
                           r.result == DispatchResult::Failed,
                       "an insert of a plugin the catalog does not offer was accepted");
                EXPECT(!r.message.empty(), "a refused insert does not say why");
                EXPECT(s.next_publish(rig_), "nothing published after the refusal");
                EXPECT(s.inbox.model.size() == updates, "a refused insert sent an update");
                EXPECT(!v.track(0).inserts[2].present(), "a refused insert shows in its slot");
                EXPECT(s.await(rig_, s.engine->send(c), r) && r.result != DispatchResult::Ok,
                       "a parameter change for no insert was accepted");
                EXPECT(!r.message.empty(), "a refused parameter change does not say why");
                return ok();
            }

            EXPECT(r.result == DispatchResult::Ok, "AddInsert was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() { return s.replica.current().revision == rev0 + 1; }),
                   "no update for AddInsert: " + s.replica_error);
            EXPECT(s.replica.current().edit->tracks[0]->inserts.size() == 1,
                   "the replica has no insert");
            EXPECT(s.until(rig_, [&]() { return v.track(0).inserts[2].present(); }),
                   "the state block never showed the insert in slot C");
            const InsertSnapshot shown = v.track(0).inserts[2];
            EXPECT((shown.flags & kInsertActive) != 0, "a new insert is not active");
            EXPECT((shown.flags & kInsertBypassed) == 0, "a new insert shows bypassed");

            // Parameters by stable key, inside the catalog's range.
            const PluginParam& q = usable->params[0];
            c.value = q.max;
            EXPECT(s.await(rig_, s.engine->send(c), r) && r.result == DispatchResult::Ok,
                   "a parameter change in range was refused: " + r.message);
            c.value = std::numeric_limits<float>::quiet_NaN();
            EXPECT(s.await(rig_, s.engine->send(c), r) && r.result == DispatchResult::Failed,
                   "a NaN parameter value was accepted");
            c.value = q.min;
            c.key = q.key + "-not-a-key";
            EXPECT(s.await(rig_, s.engine->send(c), r) && r.result == DispatchResult::Failed,
                   "a key the plugin does not have was accepted");

            // A value moved in the plugin's own editor reaches every replica
            // as SetInsertParams when the gesture ends, and when the editor
            // closes on a move with no gesture end.
            const auto value_in_replica = [&](const std::string& key, float& out) {
                const model::InsertMix* m = s.replica.current().mix.find_insert(add.insert);
                if (m == nullptr) return false;
                for (const model::ParamValue& p : m->params) {
                    if (p.key != key) continue;
                    out = p.value;
                    return true;
                }
                return false;
            };
            const float moved = q.min + (q.max - q.min) / 4;
            if (rig_.edit_in_plugin_editor(*s.engine, add.insert, q.key, moved, false)) {
                float got = 0.0f;
                EXPECT(s.until(rig_, [&]() { return value_in_replica(q.key, got) && got == moved; }),
                       "a value moved in the plugin's editor never reached the replica");
                const PluginParam& q2 = usable->params.back();
                if (rig_.edit_in_plugin_editor(*s.engine, add.insert, q2.key, q2.max, true)) {
                    EXPECT(s.until(rig_,
                                   [&]() { return value_in_replica(q2.key, got) && got == q2.max; }),
                           "a value moved before the editor closed never reached the replica");
                    EXPECT(value_in_replica(q.key, got) && got == moved,
                           "recording one editor value lost another");
                }
            }

            // FlushPluginState answers after the state it recorded: when the
            // reply arrives, the replica already holds it.
            const std::string digest(64, 'e');
            if (rig_.set_plugin_state(*s.engine, add.insert, digest, 2048)) {
                ModelRequest flush;
                flush.action = ModelAction::FlushPluginState;
                flush.op = std::to_string(add.insert);
                EXPECT(s.await(rig_, s.engine->submit(flush), r) &&
                           r.result == DispatchResult::Ok,
                       "FlushPluginState was refused: " + r.message);
                const model::InsertMix* m = s.replica.current().mix.find_insert(add.insert);
                EXPECT(m != nullptr && m->state && m->state->sha256 == digest &&
                           m->state->bytes == 2048,
                       "the reply to FlushPluginState came before its SetInsertState");
            }

            // Values are mix state: bypass shows, and is not an undo step.
            EXPECT(apply(s, model::SetInsertBypass{add.insert, true}, r) &&
                       r.result == DispatchResult::Ok,
                   "SetInsertBypass was refused: " + r.message);
            EXPECT(s.until(rig_,
                           [&]() { return (v.track(0).inserts[2].flags & kInsertBypassed) != 0; }),
                   "the state block never showed the bypass");

            // An occupied slot is refused and changes nothing.
            model::AddInsert again = add;
            again.insert = s.replica.current().next_id;
            const std::size_t before = s.inbox.model.size();
            EXPECT(apply(s, again, r) && r.result == DispatchResult::Failed,
                   "an insert onto an occupied slot was accepted");
            EXPECT(s.next_publish(rig_) && s.inbox.model.size() == before,
                   "a refused insert sent an update");

            // Structure is undoable (owner decision O1's default): undo takes
            // the insert out, and its slot empties.
            ModelRequest m;
            m.action = ModelAction::Undo;
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Ok,
                   "undo failed");
            EXPECT(s.until(rig_, [&]() {
                       return s.replica.current().edit->tracks[0]->inserts.empty();
                   }),
                   "undo left the insert");
            EXPECT(s.until(rig_, [&]() { return !v.track(0).inserts[2].present(); }),
                   "the state block still shows an undone insert");
            return ok();
        }
    };
    return C(rig, "plugin inserts").go();
}

// PluginFault names an insert by id, and one fault stands per insert: an
// active insert whose plugin is not here (or not hostable at all) has one,
// an inactive one has none, and leaving the chain (removed, made inactive,
// another session opened) clears it with an empty text. A replayed or
// stale fault would put a red slot on a plugin that is fine.
int check_plugin_faults(Rig& rig) {
    struct C : Check {
        using Check::Check;

        // What a client keeps on screen: the latest text per insert, empty
        // text taking it off.
        static std::map<uint64_t, std::string> standing(const Inbox& in) {
            std::map<uint64_t, std::string> out;
            for (const EngineEvent& e : in.events) {
                if (e.kind != EventKind::PluginFault) continue;
                if (e.text.empty()) {
                    out.erase(e.insert);
                } else {
                    out[e.insert] = e.text;
                }
            }
            return out;
        }

        bool apply(Session& s, const model::Operation& op, CommandReply& r) {
            ModelRequest m;
            std::string error;
            if (!model::encode_operation(op, m.op, error)) return false;
            return s.await(rig_, s.engine->submit(m), r);
        }

        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            EXPECT(s.until(rig_, [&]() { return s.replica.synced(); }), "no snapshot");
            EXPECT(standing(s.inbox).empty(), "a fault stands in a session with no insert");

            // A session holding an active and an inactive insert of a plugin
            // no engine has.
            model::Snapshot doc = s.replica.current();
            const model::PluginRef absent{model::PluginFormat::Lv2,
                                          "urn:sonora:conformance:absent", "Absent", "0"};
            const model::Id active = doc.next_id;
            const model::Id inactive = doc.next_id + 1;
            auto edit = std::make_shared<model::EditState>(*doc.edit);
            auto track = std::make_shared<model::TrackEdit>(*edit->tracks.at(0));
            track->inserts = {model::Insert{active, 0, absent, true},
                              model::Insert{inactive, 5, absent, false}};
            edit->tracks[0] = track;
            doc.edit = edit;
            doc.mix.inserts = {model::InsertMix{active, false, {}, {}},
                               model::InsertMix{inactive, false, {}, {}}};
            doc.next_id += 2;
            ModelRequest load;
            load.action = ModelAction::Load;
            std::string error;
            EXPECT(model::serialize(doc, model::WriteInfo{"conformance", 1}, load.op, error),
                   error);
            load.op = wire::encode_load("", load.op);
            CommandReply r;
            EXPECT(s.await(rig_, s.engine->submit(load), r) && r.result == DispatchResult::Ok,
                   "the session was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() { return standing(s.inbox).count(active) != 0; }),
                   "no PluginFault for the active insert, by its id");
            EXPECT(standing(s.inbox).size() == 1, "an inactive insert has a fault standing");
            const EngineEvent* e = s.last_event(EventKind::PluginFault);
            EXPECT(e != nullptr && e->track == -1 && e->frame == 0,
                   "a PluginFault names its insert by track or slot number");
            EXPECT(mentions(e->text, "Absent"), "the fault does not name the plugin");

            // Removed: cleared. Undone: standing again.
            EXPECT(apply(s, model::RemoveInsert{active}, r) && r.result == DispatchResult::Ok,
                   "RemoveInsert was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() { return standing(s.inbox).empty(); }),
                   "a removed insert's fault still stands");
            ModelRequest undo;
            undo.action = ModelAction::Undo;
            EXPECT(s.await(rig_, s.engine->submit(undo), r) && r.result == DispatchResult::Ok,
                   "undo failed");
            EXPECT(s.until(rig_, [&]() { return standing(s.inbox).count(active) != 0; }),
                   "an insert undo brought back has no fault standing");

            // Out of the chain: cleared.
            EXPECT(apply(s, model::SetInsertActive{active, false}, r) &&
                       r.result == DispatchResult::Ok,
                   "SetInsertActive was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() { return standing(s.inbox).empty(); }),
                   "an inactive insert's fault still stands");

            // Another session: the old one's faults go, the new one's stand.
            EXPECT(apply(s, model::SetInsertActive{active, true}, r) &&
                       r.result == DispatchResult::Ok,
                   "SetInsertActive was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() { return standing(s.inbox).count(active) != 0; }),
                   "an insert made active again has no fault standing");
            model::Snapshot plain = s.replica.current();
            auto plain_edit = std::make_shared<model::EditState>(*plain.edit);
            auto plain_track = std::make_shared<model::TrackEdit>(*plain_edit->tracks.at(0));
            plain_track->inserts.clear();
            plain_edit->tracks[0] = plain_track;
            plain.edit = plain_edit;
            plain.mix.inserts.clear();
            EXPECT(model::serialize(plain, model::WriteInfo{"conformance", 1}, load.op, error),
                   error);
            load.op = wire::encode_load("", load.op);
            EXPECT(s.await(rig_, s.engine->submit(load), r) && r.result == DispatchResult::Ok,
                   "the session without inserts was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() { return standing(s.inbox).empty(); }),
                   "a fault from the session before still stands");
            return ok();
        }
    };
    return C(rig, "plugin faults by insert").go();
}

// FlushPluginState: the request File > Save sends first. With nothing
// running it changes nothing and says Ok; it refuses what is not an
// insert.
int check_flush_plugin_state(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            EXPECT(s.until(rig_, [&]() { return s.replica.synced(); }), "no snapshot");
            ModelRequest flush;
            flush.action = ModelAction::FlushPluginState;
            CommandReply r;
            const std::size_t updates = s.inbox.model.size();
            EXPECT(s.await(rig_, s.engine->submit(flush), r) && r.result == DispatchResult::Ok,
                   "a flush with no insert was refused: " + r.message);
            EXPECT(s.next_publish(rig_) && s.inbox.model.size() == updates,
                   "a flush with nothing to record sent an update");
            flush.op = "12x";
            EXPECT(s.await(rig_, s.engine->submit(flush), r) && r.result == DispatchResult::Failed,
                   "a flush of \"12x\" was accepted");
            EXPECT(mentions(r.message, "not an insert id"), "the refusal does not say why");
            flush.op = std::to_string(s.replica.current().next_id + 100);
            EXPECT(s.await(rig_, s.engine->submit(flush), r) && r.result == DispatchResult::Failed,
                   "a flush of an insert no track holds was accepted");
            EXPECT(mentions(r.message, "is not on any track"), "the refusal does not say why");
            return ok();
        }
    };
    return C(rig, "flush plugin state").go();
}

// ADR-0018 §2, §4 (protocol 16): a Load that names its session file moves
// the engine into that file's folder. The SessionFolder event says so, and
// a take recorded after the Load is in the loaded session's "Audio Files/",
// numbered from that folder. A Load during a take is refused and the folder
// stays, as for a path that is no session file or is not absolute.
int check_load_enters_its_folder(Rig& rig) {
    struct C : Check {
        using Check::Check;

        static std::string folder(const Inbox& in) {
            for (auto it = in.events.rbegin(); it != in.events.rend(); ++it) {
                if (it->kind == EventKind::SessionFolder) return it->text;
            }
            return {};
        }

        int go() {
            namespace fs = std::filesystem;
            static int n = 0;
            std::error_code ec;
            const fs::path root = fs::temp_directory_path(ec) /
                                  ("sonora-conformance-folder-" + std::to_string(
                                       reinterpret_cast<std::uintptr_t>(this)) + "-" +
                                   std::to_string(n++));
            fs::remove_all(root, ec);
            struct Gone {
                fs::path p;
                ~Gone() {
                    std::error_code e;
                    fs::remove_all(p, e);
                }
            } gone{root};
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            EXPECT(s.until(rig_, [&]() { return s.replica.synced(); }), "no snapshot");
            StateView& v = s.engine->state();

            // The session as the interface's File > Open finds it: its file
            // on disk, the same bytes in the Load.
            model::Snapshot song = s.replica.current();
            song.settings.name = "Song";
            std::string bytes, error;
            EXPECT(model::serialize(song, model::WriteInfo{"conformance", 1}, bytes, error), error);
            const fs::path dir = root / "Song";
            const fs::path file = dir / "Song.sonora";
            fs::create_directories(dir, ec);
            {
                std::ofstream out(file, std::ios::binary);
                out << bytes;
            }
            EXPECT(fs::is_regular_file(file, ec), "the session file could not be written");
            ModelRequest load;
            load.action = ModelAction::Load;
            load.op = wire::encode_load(file.string(), bytes);
            CommandReply r;
            EXPECT(s.await(rig_, s.engine->submit(load), r) && r.result == DispatchResult::Ok,
                   "the session was refused: " + r.message);
            const std::string expected = dir.lexically_normal().string();
            EXPECT(s.until(rig_, [&]() { return folder(s.inbox) == expected; }),
                   "no SessionFolder naming " + expected + " after the Load, but \"" +
                       folder(s.inbox) + "\"");

            EXPECT(s.await(rig_, s.command("track.toggle_arm", 0), r) &&
                       r.result == DispatchResult::Ok,
                   "arming failed: " + r.message);
            EXPECT(s.next_publish(rig_), "nothing published after arming");
            const int32_t take = v.transport().take;
            EXPECT(take == 1, "the next take is " + std::to_string(take) +
                                  ", not the empty folder's first");
            EXPECT(s.await(rig_, s.command("transport.record"), r) &&
                       r.result == DispatchResult::Ok,
                   "record failed: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       return v.transport().state == TransportState::Recording &&
                              v.transport().playhead > 0;
                   }),
                   "the take never ran");

            // A take belongs to the session it was started in.
            const std::size_t announced = s.count_events(EventKind::SessionFolder);
            fs::create_directories(root / "Other", ec);
            {
                std::ofstream out(root / "Other" / "Other.sonora", std::ios::binary);
                out << bytes;
            }
            load.op = wire::encode_load((root / "Other" / "Other.sonora").string(), bytes);
            EXPECT(s.await(rig_, s.engine->submit(load), r) && r.result == DispatchResult::Failed,
                   "a session was opened during a take");
            EXPECT(mentions(r.message, "stop recording before opening another session"),
                   "the refusal does not say why");

            EXPECT(s.await(rig_, s.command("transport.stop"), r) &&
                       r.result == DispatchResult::Ok,
                   "stop failed: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       return v.transport().state == TransportState::Stopped &&
                              v.transport().take == take + 1;
                   }),
                   "the take did not end");
            const int files = rig_.take_files_in(*s.engine, expected, take);
            if (files >= 0) {
                EXPECT(files > 0, "take " + std::to_string(take) + " is not in " + expected +
                                      "/Audio Files");
                EXPECT(rig_.take_files_in(*s.engine, (root / "Other").string(), take) == 0,
                       "the take is in the folder of the session refused");
            }

            // Refusals keep the folder.
            load.op = wire::encode_load((root / "Nowhere" / "Nowhere.sonora").string(), bytes);
            EXPECT(s.await(rig_, s.engine->submit(load), r) && r.result == DispatchResult::Failed,
                   "a Load naming no file was accepted");
            EXPECT(mentions(r.message, "no session file"), "the refusal does not say why");
            load.op = wire::encode_load("Song.sonora", bytes);
            EXPECT(s.await(rig_, s.engine->submit(load), r) && r.result == DispatchResult::Failed,
                   "a Load naming a relative path was accepted");
            EXPECT(mentions(r.message, "absolute path"), "the refusal does not say why");
            EXPECT(s.next_publish(rig_), "nothing published after the refusals");
            EXPECT(s.count_events(EventKind::SessionFolder) == announced &&
                       folder(s.inbox) == expected,
                   "a refused Load moved the session folder");
            if (files < 0) return skip("the rig cannot tell where a take was written");
            return ok();
        }
    };
    return C(rig, "load enters its session folder").go();
}

// Save (protocol 11): the session file is in place when the reply says Ok;
// what is not a session file, and a path that is not absolute, are refused.
int check_save(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            namespace fs = std::filesystem;
            static int n = 0;
            std::error_code ec;
            const fs::path root = fs::temp_directory_path(ec) /
                                  ("sonora-conformance-save-" + std::to_string(
                                       reinterpret_cast<std::uintptr_t>(this)) + "-" +
                                   std::to_string(n++));
            fs::remove_all(root, ec);
            struct Gone {
                fs::path p;
                ~Gone() {
                    std::error_code e;
                    fs::remove_all(p, e);
                }
            } gone{root};
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            EXPECT(s.until(rig_, [&]() { return s.replica.synced(); }), "no snapshot");
            model::WriteInfo info{"conformance", 1};
            std::string bytes, error;
            EXPECT(model::serialize(s.replica.current(), info, bytes, error), error);
            const fs::path file = root / "Conf" / "Conf.sonora";
            ModelRequest save;
            save.action = ModelAction::Save;
            save.op = wire::encode_save(file.string(), bytes);
            CommandReply r;
            EXPECT(s.await(rig_, s.engine->submit(save), r) && r.result == DispatchResult::Ok,
                   "a save was refused: " + r.message);
            std::ifstream in(file, std::ios::binary);
            std::stringstream got;
            got << in.rdbuf();
            EXPECT(got.str() == bytes, "the session file does not hold what was saved");
            save.op = wire::encode_save(file.string(), "{not json");
            EXPECT(s.await(rig_, s.engine->submit(save), r) && r.result == DispatchResult::Failed,
                   "a save of what is not a session file was accepted");
            save.op = wire::encode_save("Conf.sonora", bytes);
            EXPECT(s.await(rig_, s.engine->submit(save), r) && r.result == DispatchResult::Failed,
                   "a save to a relative path was accepted");
            EXPECT(mentions(r.message, "absolute path"), "the refusal does not say why");
            return ok();
        }
    };
    return C(rig, "save").go();
}

int check_take_failed(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            if (!rig_.set_takes_fail(*s.engine, true)) {
                return skip("this rig cannot make a take fail");
            }
            StateView& v = s.engine->state();
            CommandReply r;
            EXPECT(s.await(rig_, s.command("track.toggle_arm", 0), r) &&
                       r.result == DispatchResult::Ok,
                   "arming failed");
            EXPECT(s.next_publish(rig_), "nothing published");
            const int32_t take = v.transport().take;
            EXPECT(s.await(rig_, s.command("transport.record"), r) &&
                       r.result == DispatchResult::Ok,
                   "record failed: " + r.message);
            EXPECT(s.until(rig_, [&]() { return s.last_event(EventKind::TakeFailed) != nullptr; }),
                   "no TakeFailed event");
            const EngineEvent e = *s.last_event(EventKind::TakeFailed);
            EXPECT(e.frame == static_cast<uint64_t>(take), "TakeFailed names another take");
            EXPECT(mentions(e.text, ("take " + std::to_string(take)).c_str()) &&
                       mentions(e.text, "stop the take"),
                   "TakeFailed does not say which take and what to do");
            EXPECT(s.until(rig_, [&]() {
                       return (v.transport().health_flags & kHealthTakeFailed) != 0;
                   }),
                   "the state block never showed the take failed");
            // Said once per take, not on every check.
            EXPECT(s.next_publish(rig_) && s.next_publish(rig_), "nothing published");
            EXPECT(s.count_events(EventKind::TakeFailed) == 1, "TakeFailed said more than once");

            EXPECT(s.await(rig_, s.command("transport.stop"), r), "no reply to stop");
            EXPECT(r.result == DispatchResult::Failed && !r.message.empty(),
                   "stopping a failed take did not report it");
            EXPECT(s.until(rig_, [&]() {
                       return v.transport().state == TransportState::Stopped;
                   }),
                   "a failed take did not stop");
            // The flag stays until a take records cleanly.
            EXPECT(s.next_publish(rig_), "nothing published");
            EXPECT((v.transport().health_flags & kHealthTakeFailed) != 0,
                   "the failure was forgotten before a clean take");

            EXPECT(rig_.set_takes_fail(*s.engine, false), "the rig cannot undo the failure");
            EXPECT(s.await(rig_, s.command("transport.record"), r) &&
                       r.result == DispatchResult::Ok,
                   "the next take did not start: " + r.message);
            EXPECT(s.until(rig_, [&]() {
                       return s.count_events(EventKind::TakeFailed) == 2 &&
                              (v.transport().health_flags & kHealthTakeFailed) == 0;
                   }),
                   "a clean take did not clear the failure");
            const EngineEvent cleared = *s.last_event(EventKind::TakeFailed);
            EXPECT(cleared.text.empty(), "the clearing event carries text");
            EXPECT(cleared.frame == static_cast<uint64_t>(take + 1), "it names another take");
            EXPECT(s.await(rig_, s.command("transport.stop"), r) &&
                       r.result == DispatchResult::Ok,
                   "the clean take did not stop cleanly: " + r.message);
            return ok();
        }
    };
    return C(rig, "a failed take").go();
}

// Inputs that go away are said with a reason and shown on the state block,
// per track and as a health flag; when they come back, both clear.
int check_inputs_missing(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            StateView& v = s.engine->state();
            const uint32_t n = s.engine->hello().track_count;
            EXPECT(s.until(rig_, [&]() {
                       const TransportSnapshot t = v.transport();
                       return t.inputs_connected == n && (t.health_flags & kHealthNoInputs) == 0;
                   }),
                   "the inputs are not connected at the start");
            if (!rig_.set_inputs_present(*s.engine, false)) {
                return skip("this rig cannot take the inputs away");
            }
            EXPECT(s.until(rig_, [&]() {
                       const EngineEvent* e = s.last_event(EventKind::InputsMissing);
                       return e != nullptr && !e->text.empty();
                   }),
                   "no InputsMissing event with a reason");
            EXPECT(s.until(rig_, [&]() {
                       const TransportSnapshot t = v.transport();
                       return t.inputs_connected == 0 && (t.health_flags & kHealthNoInputs) != 0;
                   }),
                   "the state block never showed the inputs gone");
            for (uint32_t i = 0; i < n; ++i) {
                EXPECT(!v.track(i).input_connected(), "a track still shows its input");
            }
            EXPECT(rig_.set_inputs_present(*s.engine, true), "the rig cannot bring them back");
            EXPECT(s.until(rig_, [&]() {
                       const EngineEvent* e = s.last_event(EventKind::InputsMissing);
                       const TransportSnapshot t = v.transport();
                       return e != nullptr && e->text.empty() && t.inputs_connected == n &&
                              (t.health_flags & kHealthNoInputs) == 0;
                   }),
                   "the inputs came back and the interface was not told");
            for (uint32_t i = 0; i < n; ++i) {
                EXPECT(v.track(i).input_connected(), "a track does not show its input back");
            }
            return ok();
        }
    };
    return C(rig, "inputs missing").go();
}

int check_peaks(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            if (!rig_.has_signal()) return skip("no signal on this rig's inputs");
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            StateView& a = s.engine->state();
            StateView b = a;   // a second reader, cursor of its own
            const uint64_t hb = a.transport().heartbeat;
            EXPECT(s.until(rig_, [&]() { return a.transport().heartbeat >= hb + 3; }),
                   "nothing published");
            const float pa = a.take_peak(0);
            const float pb = b.take_peak(0);
            EXPECT(pa > 0.0f, "the first reader saw no peak");
            EXPECT(pb > 0.0f, "the first reader's read took the second reader's peak");
            EXPECT(pb >= pa, "the second reader saw less over a longer span");
            return ok();
        }
    };
    return C(rig, "peak cursors per reader").go();
}

int check_alive(Rig& rig) {
    struct C : Check {
        using Check::Check;
        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            StateView& v = s.engine->state();
            const uint64_t hb = v.transport().heartbeat;
            EXPECT(s.until(rig_, [&]() { return v.transport().heartbeat > hb; }),
                   "the heartbeat does not move");
            EXPECT(v.engine_alive(rig_.now_ns()), "a live engine reads as dead");
            EXPECT(!v.transport().stale, "an idle engine's state reads as stale");

            rig_.kill(*s.engine);
            const uint64_t timeout =
                std::max(StateView::kMinAliveTimeoutNs, 10 * v.publish_period_ns());
            // Let the reader see the last beat, then the silence.
            s.engine->poll(s.inbox);
            (void)v.engine_alive(rig_.now_ns());
            const uint64_t until = rig_.now_ns() + timeout + timeout / 2;
            while (rig_.now_ns() < until) {
                s.engine->poll(s.inbox);
                (void)v.engine_alive(rig_.now_ns());
                rig_.wait_ns(20'000'000);
            }
            EXPECT(!v.engine_alive(rig_.now_ns()), "a dead engine reads as alive");
            const std::size_t replies = s.inbox.replies.size();
            s.command("track.arm_all");
            for (int i = 0; i < 10; ++i) {
                s.engine->poll(s.inbox);
                rig_.wait_ns(kStepNs);
            }
            EXPECT(s.inbox.replies.size() == replies, "a dead engine replied");
            return ok();
        }
    };
    return C(rig, "heartbeat and engine death").go();
}

// A take on disk the open session does not name is offered at the open,
// never placed silently (session-format.md, "Crash recovery"): one
// RecoveryOffer standing, naming the file; another session's take, even
// one of the same name, is not in it, since its session id differs. Left,
// the offer is withdrawn and the document remembers the take as left, so
// an open of the session as it now is offers nothing; the file is never
// deleted, and an open of the session from before still offers it. Placed,
// the take is on the track its header names, at its TimeReference, as one
// undo step.
int check_recovery_offer(Rig& rig) {
    struct C : Check {
        using Check::Check;

        // The offer a client shows: the latest RecoveryOffer's text.
        static std::string offer(const Inbox& in) {
            for (auto it = in.events.rbegin(); it != in.events.rend(); ++it) {
                if (it->kind == EventKind::RecoveryOffer) return it->text;
            }
            return {};
        }

        bool open_again(Session& s, const model::Snapshot& doc, CommandReply& r) {
            ModelRequest load;
            load.action = ModelAction::Load;
            std::string error;
            if (!model::serialize(doc, model::WriteInfo{"conformance", 1}, load.op, error)) {
                return false;
            }
            load.op = wire::encode_load("", load.op);
            return s.await(rig_, s.engine->submit(load), r) && r.result == DispatchResult::Ok;
        }

        static const model::Region* take_region(const model::Snapshot& doc, model::Id track) {
            for (const auto& t : doc.edit->tracks) {
                if (t->id != track) continue;
                for (const auto& p : t->playlists) {
                    if (p->id != t->active_playlist) continue;
                    for (const model::Region& r : p->regions) {
                        if (r.name == "Audio 1_07") return &r;
                    }
                }
            }
            return nullptr;
        }

        int go() {
            Session s;
            EXPECT(s.open(rig_), "connect failed: " + s.error);
            EXPECT(s.until(rig_, [&]() { return s.replica.synced(); }), "no snapshot");
            const model::Snapshot doc = s.replica.current();
            const model::Id track = doc.edit->tracks.at(0)->id;
            const std::string name = doc.settings.name;
            const std::string id = doc.settings.session_id;
            EXPECT(!id.empty(), "the engine's document has no session id");
            if (!rig_.leave_orphan_take(*s.engine, "Audio 1_07.wav", name, id, 7, track, 48000,
                                        4800)) {
                return skip("this rig cannot leave a take on disk");
            }
            // Another session of the same name: two unsaved ones are both
            // "Untitled".
            const std::string other = id == "00000000-0000-4000-8000-000000000001"
                                          ? "00000000-0000-4000-8000-000000000002"
                                          : "00000000-0000-4000-8000-000000000001";
            EXPECT(rig_.leave_orphan_take(*s.engine, "Audio 2_08.wav", name, other, 8, track, 0,
                                          4800),
                   "the rig left one take and not the other");
            EXPECT(offer(s.inbox).empty(), "an offer stands before the session was opened again");

            CommandReply r;
            EXPECT(open_again(s, doc, r), "the session was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() { return !offer(s.inbox).empty(); }),
                   "no RecoveryOffer at the open");
            const EngineEvent* e = s.last_event(EventKind::RecoveryOffer);
            EXPECT(mentions(e->text, "Audio 1_07.wav"), "the offer does not name the take");
            EXPECT(e->text.find("Audio 2_08") == std::string::npos,
                   "another session's take is offered");
            EXPECT(e->frame == 1, "the offer does not count its takes");
            EXPECT(take_region(s.replica.current(), track) == nullptr,
                   "the take was placed before the operator said so");

            // Left: withdrawn, nothing placed, and remembered.
            EXPECT(s.await(rig_, s.command("session.leave_recovered"), r) &&
                       r.result == DispatchResult::Ok,
                   "session.leave_recovered failed: " + r.message);
            EXPECT(s.until(rig_, [&]() { return offer(s.inbox).empty(); }),
                   "the offer still stands after it was left");
            const auto left = [&]() {
                const auto& l = s.replica.current().settings.left_takes;
                return l == std::vector<std::string>{"Audio Files/Audio 1_07.wav"};
            };
            EXPECT(s.until(rig_, left), "the document does not remember the take as left");
            EXPECT(take_region(s.replica.current(), track) == nullptr,
                   "leaving the take placed it");
            EXPECT(s.await(rig_, s.command("session.place_recovered"), r) &&
                       r.result == DispatchResult::Failed,
                   "placing with no offer standing was not refused");

            // The session as it now is, opened again: nothing to offer. The
            // offer would be sent before the open's reply.
            const model::Snapshot after = s.replica.current();
            const std::size_t offers_before = s.inbox.events.size();
            EXPECT(open_again(s, after, r), "the session was refused: " + r.message);
            for (std::size_t i = offers_before; i < s.inbox.events.size(); ++i) {
                const EngineEvent& ev = s.inbox.events[i];
                EXPECT(ev.kind != EventKind::RecoveryOffer || ev.text.empty(),
                       "a take left at one open is offered again at the next");
            }

            // The session from before it was left: offered again, then placed.
            EXPECT(open_again(s, doc, r), "the session was refused: " + r.message);
            EXPECT(s.until(rig_, [&]() { return !offer(s.inbox).empty(); }),
                   "a session that did not leave the take is not offered it");
            EXPECT(s.await(rig_, s.command("session.place_recovered"), r) &&
                       r.result == DispatchResult::Ok,
                   "session.place_recovered failed: " + r.message);
            EXPECT(s.until(rig_, [&]() { return take_region(s.replica.current(), track) != nullptr; }),
                   "the placed take is not on its track");
            const model::Region* placed = take_region(s.replica.current(), track);
            EXPECT(placed->position == 48000 && placed->length == 4800,
                   "the take is not at its TimeReference, its length on disk");
            const model::Source* src =
                s.replica.current().sources->find(placed->channels.at(0).source);
            EXPECT(src != nullptr && src->path == "Audio Files/Audio 1_07.wav",
                   "the region does not play the take's file");
            EXPECT(s.until(rig_, [&]() { return offer(s.inbox).empty(); }),
                   "the offer still stands after the takes were placed");

            ModelRequest undo;
            undo.action = ModelAction::Undo;
            EXPECT(s.await(rig_, s.engine->submit(undo), r) && r.result == DispatchResult::Ok,
                   "the placement could not be undone: " + r.message);
            EXPECT(s.until(rig_, [&]() { return take_region(s.replica.current(), track) == nullptr; }),
                   "undo left the take on the timeline");
            EXPECT(!s.inbox.model.empty() && s.inbox.model.back().label == "Place Recovered Takes",
                   "the undo step is not named for the placement");
            return ok();
        }
    };
    return C(rig, "recovery offer").go();
}

}  // namespace

int run(Rig& rig) {
    int failures = 0;
    failures += check_hello(rig);
    failures += check_version_mismatch(rig);
    failures += check_dispatch_results(rig);
    failures += check_arming(rig);
    failures += check_take(rig);
    failures += check_play(rig);
    failures += check_locate(rig);
    failures += check_click(rig);
    failures += check_loop_toggle(rig);
    failures += check_punch_on_the_fly(rig);
    failures += check_parameters(rig);
    failures += check_mix(rig);
    failures += check_input_monitor(rig);
    failures += check_model(rig);
    failures += check_compound(rig);
    failures += check_load(rig);
    failures += check_load_in_parts(rig);
    failures += check_inserts(rig);
    failures += check_plugin_faults(rig);
    failures += check_flush_plugin_state(rig);
    failures += check_save(rig);
    failures += check_load_enters_its_folder(rig);
    failures += check_recovery_offer(rig);
    failures += check_take_failed(rig);
    failures += check_inputs_missing(rig);
    failures += check_peaks(rig);
    failures += check_alive(rig);
    return failures;
}

}  // namespace sonora::api::conformance
