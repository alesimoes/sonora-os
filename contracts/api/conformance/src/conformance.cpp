// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/api/conformance.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "sonora/api/state_block.h"
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

    uint64_t command(const std::string& id, int32_t target = -1) {
        CommandRequest r;
        r.command = id;
        r.target = target;
        return engine->send(r);
    }

    // Every model update goes to the replica as it arrives, the way an
    // interface's tick would hand it on.
    void feed_replica() {
        for (; fed_ < inbox.model.size(); ++fed_) {
            const ModelUpdate& m = inbox.model[fed_];
            model::ReplicationUpdate u;
            u.kind = m.kind == ModelUpdateKind::Op       ? model::ReplicationUpdate::Kind::Op
                     : m.kind == ModelUpdateKind::Undo   ? model::ReplicationUpdate::Kind::Undo
                     : m.kind == ModelUpdateKind::Redo   ? model::ReplicationUpdate::Kind::Redo
                                                         : model::ReplicationUpdate::Kind::Snapshot;
            u.revision = m.revision;
            u.undo_floor = m.undo_floor;
            u.next_id = m.next_id;
            u.bytes = m.bytes;
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
            m.op = bytes;
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
            m.op = "{\"not\": \"a session\"}";
            EXPECT(s.await(rig_, s.engine->submit(m), r) && r.result == DispatchResult::Failed,
                   "bytes that are not a session were loaded");
            EXPECT(mentions(r.message, "session"), "the refusal does not say why");
            other.settings.sample_rate = s.engine->hello().sample_rate == 44100 ? 48000 : 44100;
            EXPECT(model::serialize(other, info, m.op, error), error);
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

// A take that stops writing is said at once, with the take and the reason,
// and on the state block; stopping it reports the take as failed; the next
// take that records cleanly clears both.
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

}  // namespace

int run(Rig& rig) {
    int failures = 0;
    failures += check_hello(rig);
    failures += check_version_mismatch(rig);
    failures += check_dispatch_results(rig);
    failures += check_arming(rig);
    failures += check_take(rig);
    failures += check_parameters(rig);
    failures += check_model(rig);
    failures += check_load(rig);
    failures += check_take_failed(rig);
    failures += check_inputs_missing(rig);
    failures += check_peaks(rig);
    failures += check_alive(rig);
    return failures;
}

}  // namespace sonora::api::conformance
