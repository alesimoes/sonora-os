// SPDX-License-Identifier: GPL-3.0-or-later
//
// PluginHosting (ADR-0012 checklist 5 and its amendment of 2026-10-14): the
// control thread's chains, driven from the document as the control server
// drives them, against real sonora-plugin-host processes. The rig plays the
// server's loop: the session's callback (process() with a stamped block),
// hosting.poll(), a plan rebuilt when asked, the controller pumped.
//
//   open_starts_silent   a session opened: the chain is silent (latency 0)
//                        until its host is Ready, then bound at 128
//   new_insert_dry       an insert added on a dry track: unbound (dry,
//                        aligned) until Ready, then bound
//   edit_keeps_old       an insert added to a running chain: the old host
//                        plays until the new one is Ready (latency 128 to
//                        228), and the old one leaves by epoch
//   undo_abandons        an undo before the new host is Ready: that start is
//                        killed, the old binding never stops
//   missing              a plugin not installed is left out, faulted
//                        "not installed"; alone, the track is dry (B)
//   refused              a plugin the probe refuses is left out, faulted, the
//                        rest runs (C)
//   params               a PluginParameterChange resolved by key against
//                        the running instance's Loaded table, refused for a
//                        key it lacks or a value out of range
//   down_keeps_latency   a plugin that crashes every block: restarting
//                        (dead binding, latency kept), then down: silent at
//                        its last checked latency (A)
//   gestures             a GestureEnd becomes SetInsertParams with the
//                        insert's whole list, a bypass touch SetInsertBypass
//
// Built where the host is (tools/build-plugins.sh). Exit 77: a Debian
// plugin this needs is missing.
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sonora/api/state_block.h"
#include "sonora/engine_control_in_process.h"
#include "sonora/fake_backend.h"
#include "sonora/model/history.h"
#include "sonora/model/operations.h"
#include "sonora/playback_controller.h"
#include "sonora/plugin/host_control.h"
#include "sonora/plugin_hosting.h"
#include "sonora/session.h"

using namespace sonora;
using namespace sonora::model;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

constexpr int kSkip = 77;
constexpr uint32_t kBlock = 128;
const std::string kDir = "test_plugin_hosting_session";
const char* const kEgAmp = "http://lv2plug.in/plugins/eg-amp";
const char* const kLookahead = "urn:sonora:test#lookahead";
const char* const kSleep = "urn:sonora:test#sleep";
const char* const kCrash = "urn:sonora:test#crash";
const char* const kLiar = "urn:sonora:test#liar";

uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}
uint64_t now_ms() { return now_ns() / 1'000'000; }

struct Args {
    std::string host, scenario;
    std::vector<std::string> bundles;
};

AddTrack track_op(Id track, Id playlist, const std::string& name) {
    AddTrack op;
    op.track = track;
    op.playlist = playlist;
    op.name = name;
    op.playlist_name = name + ".01";
    op.mix.input.push_back(0);
    op.mix.pan = -1.0f;
    return op;
}

AddInsert insert_op(Id track, Id insert, uint8_t slot, const char* uri, std::vector<ParamValue> params = {}) {
    AddInsert op;
    op.track = track;
    op.insert = insert;
    op.slot = slot;
    op.plugin = PluginRef{PluginFormat::Lv2, uri, uri, "test"};
    op.params = std::move(params);
    return op;
}

// The control server's loop, in miniature.
struct Rig {
    std::unique_ptr<Session> session;
    std::unique_ptr<PlaybackController> ctl;
    std::unique_ptr<PluginHosting> hosting;
    History history;
    std::vector<float> out[2];
    float* outs[2];
    std::vector<model::Operation> ops_seen;

    explicit Rig(const Args& a, int tracks = 2) : history(new_session(SessionSettings{})) {
        std::filesystem::remove_all(kDir);
        SessionConfig cfg;
        cfg.dir = kDir;
        cfg.track_count = tracks;
        cfg.ring_seconds = 0.1;
        cfg.playback.chunk_frames = 256;
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = 128;
        cfg.playback.max_block_frames = 256;
        cfg.playback.max_segments = 64;
        cfg.playback.max_param_stores = 16;
        session = std::make_unique<Session>(cfg);
        session->create_dirs();
        for (int o = 0; o < 2; ++o) {
            out[o].assign(kBlock, 0.f);
            outs[o] = out[o].data();
        }
        ctl = std::make_unique<PlaybackController>(*session, kDir);
        PluginHostingConfig hc;
        hc.host_path = a.host;
        hc.bundles = a.bundles;
        hc.session_dir = kDir;
        hc.block_frames = kBlock;
        hc.max_block_frames = 256;
        hc.max_stores = 16;
        hosting = std::make_unique<PluginHosting>(*session, hc);
        ctl->set_chain_resolver([this](const TrackEdit& t, uint32_t e, ChainBinding& b) { hosting->resolve(t, e, b); });
    }
    ~Rig() {
        hosting.reset();
        ctl.reset();
        session.reset();
        std::filesystem::remove_all(kDir);
    }

    bool apply(const model::Operation& op) {
        std::string e;
        const bool ok = history.apply(op, e);
        if (!ok) std::printf("  apply refused: %s\n", e.c_str());
        return ok;
    }
    // The server after a document change.
    void show(bool opened = false) {
        hosting->show(history.current(), opened);
        replan();
    }
    void replan() {
        std::string e;
        if (ctl->show(history.current(), e)) hosting->built(ctl->last_built_epoch());
        else std::printf("  plan refused: %s\n", e.c_str());
    }
    // One turn: a callback, the hosts, the plan.
    void turn() {
        backend::AudioBlock b{};
        b.outputs = outs;
        b.output_count = 2;
        b.nframes = kBlock;
        b.callback_ns = now_ns();
        session->process(b);
        if (!session->reader().running()) session->reader().run_pass();
        PluginHosting::PollResult r = hosting->poll(now_ms(), history.current(), ctl->oldest_live_epoch());
        for (model::Operation& op : r.ops) {
            ops_seen.push_back(op);
            apply(op);
        }
        if (r.replan || !r.ops.empty()) {
            hosting->show(history.current(), false);
            replan();
        }
        ctl->pump();
        std::this_thread::sleep_for(std::chrono::microseconds(2667));
    }
    bool until(const std::function<bool()>& pred, uint64_t max_ms) {
        const uint64_t end = now_ms() + max_ms;
        while (now_ms() < end) {
            if (pred()) return true;
            turn();
        }
        return pred();
    }
    // The playing plan's InsertChain node of engine track `t`, or null.
    const PlanNode* chain_node(uint32_t t) const {
        const PlaybackPlan* p = session->plans().current();
        if (!p) return nullptr;
        for (uint32_t i = 0; i < p->node_count; ++i) {
            const PlanNode& n = p->nodes[i];
            if (n.kind == NodeKind::InsertChain && p->tracks[n.track].engine_track == t) return &n;
        }
        return nullptr;
    }
    bool bound_at(uint32_t t, uint32_t latency) const {
        const PlanNode* n = chain_node(t);
        return n && n->channel_slot != kNoChannelSlot && n->channel_slot != kSilentChannelSlot &&
               n->latency_frames == latency;
    }
    bool silent_at(uint32_t t, uint32_t latency) const {
        const PlanNode* n = chain_node(t);
        return n && n->channel_slot == kSilentChannelSlot && n->latency_frames == latency;
    }
    bool unbound(uint32_t t) const {
        const PlanNode* n = chain_node(t);
        return n == nullptr || (n->channel_slot == kNoChannelSlot && n->latency_frames == 0);
    }
};

bool installed(const Args& a) {
    // eg-amp is Debian's (lv2-examples); the rest are in-tree.
    for (const std::string& b : a.bundles) (void)b;
    struct stat st {};
    return ::stat("/usr/lib/lv2/eg-amp.lv2", &st) == 0;
}

int scenario_open_starts_silent(const Args& a) {
    std::printf("open_starts_silent: a session opened with eg-amp on track A\n");
    Rig rig(a);
    check(rig.hosting->ok(), "hosting is up");
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 0, kEgAmp));
    rig.show(true);
    check(rig.until([&] { return rig.session->plans().current() != nullptr && rig.chain_node(0); }, 2000),
          "a plan with the chain node is playing");
    check(rig.silent_at(0, 0), "silent, latency 0, while the host starts (item 1)");
    check(rig.hosting->loading_total() == 1, "one chain loading");
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "bound at 128 once Ready");
    check(rig.hosting->loading() == 0, "nothing loading");
    check(rig.hosting->faults().empty(), "no fault stands");
    check((rig.hosting->insert_flags(500) & api::kInsertRunning) != 0, "the insert shows running");
    return failures ? 1 : 0;
}

int scenario_new_insert_dry(const Args& a) {
    std::printf("new_insert_dry: eg-amp added to a dry track\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.show(true);
    rig.until([&] { return rig.session->plans().current() != nullptr; }, 2000);
    rig.apply(insert_op(100, 500, 0, kEgAmp));
    rig.show(false);
    rig.until([&] { return rig.chain_node(0) != nullptr; }, 2000);
    check(rig.unbound(0), "unbound, dry and aligned, while the host starts (item 1)");
    check(rig.hosting->loading_total() == 0, "not a session open: nothing counted as loading");
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "bound at 128 once Ready");
    return failures ? 1 : 0;
}

int scenario_edit_keeps_old(const Args& a) {
    std::printf("edit_keeps_old: the look-ahead added in front of a running eg-amp\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 1, kEgAmp));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "eg-amp running");
    const uint32_t old_slot = rig.chain_node(0)->channel_slot;
    rig.apply(insert_op(100, 501, 0, kLookahead));
    rig.show(false);
    bool old_held = true;
    const bool swapped = rig.until(
        [&] {
            old_held = old_held && (rig.bound_at(0, kBlock + 100) || (rig.bound_at(0, kBlock) &&
                                                                       rig.chain_node(0)->channel_slot == old_slot));
            return rig.bound_at(0, kBlock + 100);
        },
        15000);
    check(swapped, "the new chain is bound at 228");
    check(old_held, "until then the old host's binding played, unchanged (make-before-break)");
    check(rig.chain_node(0)->channel_slot != old_slot, "on a new slot");
    check(rig.until([&] { return rig.hosting->hosts() == 1; }, 5000), "the old host left once its plans retired");
    return failures ? 1 : 0;
}

int scenario_undo_abandons(const Args& a) {
    std::printf("undo_abandons: a slow plugin added, then undone before it is Ready\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 0, kEgAmp));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "eg-amp running");
    const uint32_t slot = rig.chain_node(0)->channel_slot;
    rig.apply(insert_op(100, 501, 1, kSleep, {{"sleep_ms", 1500.f}}));
    rig.show(false);
    check(rig.until([&] { return rig.hosting->hosts() == 2; }, 10000), "the new chain's host is starting");
    check(rig.history.undo(), "undo");
    rig.show(false);
    check(rig.until([&] { return rig.hosting->hosts() == 1; }, 3000), "the abandoned start is killed");
    for (int i = 0; i < 40; ++i) rig.turn();
    check(rig.bound_at(0, kBlock) && rig.chain_node(0)->channel_slot == slot, "the old binding never stopped");
    return failures ? 1 : 0;
}

int scenario_missing(const Args& a) {
    std::printf("missing: a plugin not installed, alone on A and beside eg-amp on B\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(track_op(102, 103, "B"));
    rig.apply(insert_op(100, 500, 0, "urn:sonora:test#not-installed"));
    rig.apply(insert_op(102, 501, 0, "urn:sonora:test#not-installed"));
    rig.apply(insert_op(102, 502, 1, kEgAmp));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(1, kBlock); }, 15000), "B runs eg-amp without the missing one");
    check(rig.unbound(0), "A, with nothing left, is dry (change B)");
    const auto& f = rig.hosting->faults();
    check(f.count(500) && f.at(500).find("not installed") != std::string::npos, "A's insert: not installed");
    check(f.count(501) && f.count(502) == 0, "B's missing insert faulted, eg-amp not");
    check((rig.hosting->insert_flags(500) & api::kInsertMissing) != 0, "the state block says missing");
    return failures ? 1 : 0;
}

int scenario_refused(const Args& a) {
    std::printf("refused: the liar beside eg-amp\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 0, kEgAmp));
    rig.apply(insert_op(100, 501, 1, kLiar));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "eg-amp runs without the liar (change C)");
    const auto& f = rig.hosting->faults();
    check(f.count(501) == 1 && f.count(500) == 0, "the liar is faulted, eg-amp is not");
    if (f.count(501)) std::printf("  fault: %s\n", f.at(501).c_str());
    check((rig.hosting->insert_flags(501) & api::kInsertFaulted) != 0, "the state block says faulted");
    return failures ? 1 : 0;
}

int scenario_params(const Args& a) {
    std::printf("params: a PluginParameterChange by key\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 0, kEgAmp));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "eg-amp running");
    std::string why;
    const uint64_t dropped = rig.session->plugin_params_dropped();
    check(rig.hosting->set_param(500, "gain", -20.f, why), "gain -20 is taken");
    check(!rig.hosting->set_param(500, "no-such-key", 0.f, why) && !why.empty(), "an unknown key is refused");
    std::printf("  why: %s\n", why.c_str());
    check(!rig.hosting->set_param(500, "gain", 1000.f, why), "a value past the range is refused");
    check(!rig.hosting->set_param(999, "gain", 0.f, why), "an insert that is not running is refused");
    for (int i = 0; i < 10; ++i) rig.turn();
    check(rig.session->plugin_params_dropped() == dropped, "the change reached the node");
    return failures ? 1 : 0;
}

int scenario_down_keeps_latency(const Args& a) {
    std::printf("down_keeps_latency: the crash plugin set to crash on every block\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 0, kCrash));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "running");
    std::string why;
    check(rig.hosting->set_param(500, "crash", 1.f, why), "crash set");
    check(rig.until([&] { return (rig.hosting->insert_flags(500) & api::kInsertRestarting) != 0; }, 5000),
          "restarting");
    if (rig.hosting->faults().count(500)) std::printf("  fault: %s\n", rig.hosting->faults().at(500).c_str());
    check(rig.chain_node(0) && rig.chain_node(0)->latency_frames == kBlock,
          "while restarting the plan keeps the dead binding's latency (change A)");
    check(rig.until([&] { return rig.silent_at(0, kBlock); }, 30000), "down: silent at its last latency, 128");
    const auto& f = rig.hosting->faults();
    check(f.count(500) && f.at(500).find("3 restarts") != std::string::npos, "the fault says it stays down");
    if (f.count(500)) std::printf("  fault: %s\n", f.at(500).c_str());
    return failures ? 1 : 0;
}

int scenario_gestures(const Args& a) {
    std::printf("gestures: GestureEnd and a bypass touch become document operations\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 0, kEgAmp));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "eg-amp running");
    namespace pc = plugin::control;
    std::string bytes;
    std::vector<model::Operation> ops;
    pc::encode(pc::GestureEnd{0, 0, -6.f}, bytes);
    rig.hosting->handle_host_message(100, bytes, rig.history.current(), ops);
    bool params = false;
    for (const auto& op : ops) {
        if (const auto* s = std::get_if<SetInsertParams>(&op)) {
            for (const ParamValue& v : s->params) params = params || (s->insert == 500 && v.key == "gain" && v.value == -6.f);
        }
    }
    check(params, "SetInsertParams for insert 500 with gain -6");
    ops.clear();
    bytes.clear();
    pc::encode(pc::GestureEnd{0, plugin::kBypassParam, 1.f}, bytes);
    rig.hosting->handle_host_message(100, bytes, rig.history.current(), ops);
    bool bypass = false;
    for (const auto& op : ops)
        if (const auto* s = std::get_if<SetInsertBypass>(&op)) bypass = s->insert == 500 && s->bypass;
    check(bypass, "SetInsertBypass for insert 500");
    return failures ? 1 : 0;
}

// The whole path through EngineControlServer on drivers/fake (its own
// real-time thread, quantum 128): a session opened with eg-amp on track A
// starts its host from the server's thread once the period is known; the
// state block shows the insert running and the plan carries 128; no fault
// stands; a PluginParameterChange by key is Ok, by an unknown key Failed.
int scenario_server(const Args& a) {
    std::printf("server: EngineControlServer hosts a session's insert on drivers/fake\n");
    std::filesystem::remove_all(kDir);
    SessionConfig cfg;
    cfg.dir = kDir;
    cfg.track_count = 2;
    cfg.ring_seconds = 0.5;
    cfg.playback.max_param_stores = 16;
    Session session(cfg);
    session.create_dirs();
    FakeBackend::Config bc;
    FakeBackend::Device d = FakeBackend::onboard();
    bc.devices = {d};
    bc.mode = FakeBackend::Mode::Thread;
    FakeBackend audio(bc);
    std::string error;
    check(audio.open({"sonora", 2, 2, 48000, ""}, session, error) && audio.activate(error), "fake backend up");
    session.disk().start();
    History h(new_session(SessionSettings{}));
    std::string e;
    h.apply(track_op(100, 101, "A"), e);
    h.apply(insert_op(100, 500, 0, kEgAmp), e);
    EngineControlServer::Options opts;
    opts.plugin_host = a.host;
    opts.plugin_bundles = a.bundles;
    opts.initial_document = std::make_shared<Snapshot>(h.current());
    {
        InProcessEngine engine(session, &audio, opts);
        check(engine.connect(error), "connected");
        api::Inbox in;
        const auto until = [&](const std::function<bool()>& pred, uint64_t ms) {
            const uint64_t end = now_ms() + ms;
            while (now_ms() < end) {
                engine.poll(in);
                if (pred()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            engine.poll(in);
            return pred();
        };
        check(until([&] { return (engine.state().track(0).inserts[0].flags & api::kInsertRunning) != 0; }, 20000),
              "the state block shows the insert running");
        check((engine.state().transport().health_flags & api::kHealthPluginsDisabled) == 0,
              "the plugins-disabled health flag is down");
        const PlaybackPlan* p = session.plans().current();
        bool bound = false;
        for (uint32_t i = 0; p && i < p->node_count; ++i)
            bound = bound || (p->nodes[i].kind == NodeKind::InsertChain && p->nodes[i].latency_frames == kBlock &&
                              p->nodes[i].channel_slot != kSilentChannelSlot &&
                              p->nodes[i].channel_slot != kNoChannelSlot);
        check(bound, "the playing plan binds the chain at 128");
        std::string last_fault = "none";
        for (const auto& ev : in.events)
            if (ev.kind == api::EventKind::PluginFault && ev.insert == 500) last_fault = ev.text;
        check(last_fault == "none" || last_fault.empty(), "no fault stands for the insert");
        api::PluginParameterChange c;
        c.insert = 500;
        c.key = "gain";
        c.value = -20.f;
        const uint64_t ok_seq = engine.send(c);
        c.key = "no-such-key";
        const uint64_t bad_seq = engine.send(c);
        api::CommandReply ok_reply, bad_reply;
        check(until(
                  [&] {
                      for (const auto& r : in.replies) {
                          if (r.seq == ok_seq) ok_reply = r;
                          if (r.seq == bad_seq) bad_reply = r;
                      }
                      return ok_reply.seq == ok_seq && bad_reply.seq == bad_seq;
                  },
                  2000),
              "both changes answered");
        check(ok_reply.result == DispatchResult::Ok, "gain by key: Ok");
        check(bad_reply.result == DispatchResult::Failed && !bad_reply.message.empty(), "an unknown key: Failed, why");
        engine.shutdown();
        audio.deactivate();
        engine.server().after_deactivate();
        session.stop();
    }
    std::filesystem::remove_all(kDir);
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--host") a.host = argv[i + 1];
        else if (k == "--scenario") a.scenario = argv[i + 1];
        else if (k == "--bundle") a.bundles.push_back(argv[i + 1]);
    }
    if (a.host.empty() || a.scenario.empty()) {
        std::fprintf(stderr, "usage: %s --host PATH --scenario NAME [--bundle DIR]...\n", argv[0]);
        return 2;
    }
    if (!installed(a)) {
        std::printf("SKIP: eg-amp is not installed (tools/Dockerfile.plugins)\n");
        return kSkip;
    }
    int rc = 2;
    if (a.scenario == "open_starts_silent") rc = scenario_open_starts_silent(a);
    else if (a.scenario == "new_insert_dry") rc = scenario_new_insert_dry(a);
    else if (a.scenario == "edit_keeps_old") rc = scenario_edit_keeps_old(a);
    else if (a.scenario == "undo_abandons") rc = scenario_undo_abandons(a);
    else if (a.scenario == "missing") rc = scenario_missing(a);
    else if (a.scenario == "refused") rc = scenario_refused(a);
    else if (a.scenario == "params") rc = scenario_params(a);
    else if (a.scenario == "down_keeps_latency") rc = scenario_down_keeps_latency(a);
    else if (a.scenario == "gestures") rc = scenario_gestures(a);
    else if (a.scenario == "server") rc = scenario_server(a);
    else std::fprintf(stderr, "unknown scenario %s\n", a.scenario.c_str());
    if (rc == 0 && failures) rc = 1;
    std::printf("%s: %s\n", a.scenario.c_str(), rc == 0 ? "ok" : rc == kSkip ? "skipped" : "FAILED");
    return rc;
}
