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
//   values_while_starting  a param and the bypass changed while the host
//                        starts reach its store once it is Ready (B2, #80)
//   reload_starts_afresh the same session opened again: a new host from the
//                        document's values, silent until Ready (B3, #80)
//   repost_bypass_off    an un-bypass the audio thread dropped is re-posted
//                        (the #80 review's should-fix 1)
//   gesture_left_open    a host that dies mid-touch: the touched value is
//                        written to the document (M3, #80)
//   failed_edit          an edit whose host crashes at start: faulted, the
//                        old chain plays and follows the document, no
//                        respawn on later changes (M4, #80)
//   state_not_restored   a saved state past the cap, gone, not matching its
//                        hash, or refused by the host: the insert runs at
//                        defaults plus its params, with a PluginFault that
//                        stands while the document names that state
//   work_refused         a plugin whose worker refused messages (its ring
//                        full): one notice per run naming plugin, track and
//                        slot, a count that only rises, no standing fault
//                        (ADR-0012 change G); another worker plugin counts
//                        nothing
//   touch_faults         a host's touches lost on the way (per chain) and
//                        touches of a parameter with no dense index (per
//                        insert): each counted, one notice per run naming
//                        what it can, no standing fault; a fault naming no
//                        plugin of the chain is ignored
//   work_refused_new_session  a session opened with the same insert id:
//                        none of the last one's count or notices
//   server_work_refused  the same through the server: the count in the
//                        state block's InsertState, one Notice, no fault
//   rescan_drops_stale   a catalog in which a plugin's record changed (a
//                        rescan found it upgraded) drops the answer cached
//                        for it; an unchanged record keeps it; the running
//                        chain keeps its own table until its next start
//   state_flush          FlushPluginState's reads, files and SetInsertState;
//                        a crashed host restarts from the flushed state, and
//                        the session file written and reopened restores it
//   server_state_flush   the same through the server: the reply after the
//                        file and the update; an unknown insert refused;
//                        a save into another folder carries the state
//
// Built where the host is (tools/build-plugins.sh). Exit 77: a Debian
// plugin this needs is missing.
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "sonora/api/state_block.h"
#include "sonora/api/wire.h"
#include "sonora/engine_control_in_process.h"
#include "sonora/fake_backend.h"
#include "sonora/model/history.h"
#include "sonora/model/operations.h"
#include "sonora/model/replication.h"
#include "sonora/model/session_file.h"
#include "sonora/playback_controller.h"
#include "sonora/plugin/host_control.h"
#include "sonora/plugin_hosting.h"
#include "sonora/plugin_scanner.h"
#include "sonora/session.h"
#include "sonora/session_files/session_folder.h"

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
const char* const kWorker = "urn:sonora:test#worker";

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
    // The chains' probes go through the scan worker (amendment of
    // 2026-10-19, item 3); no catalog scan here, only chain starts.
    std::unique_ptr<PluginScanner> scanner;
    std::unique_ptr<PluginHosting> hosting;
    History history;
    std::vector<float> out[2];
    float* outs[2];
    std::vector<model::Operation> ops_seen;

    // Residency held: the reader runs no pass, so a plan built waits,
    // pending, and the playing one stays (ADR-0009 §4.4).
    bool hold_reader = false;

    explicit Rig(const Args& a, int tracks = 2, uint64_t ready_timeout_ms = 10000)
        : history(new_session(SessionSettings{})) {
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
        hc.ready_timeout_ms = ready_timeout_ms;
        PluginScannerConfig sc;
        sc.host_path = a.host;
        sc.bundles = a.bundles;
        sc.scan_at_start = false;
        scanner = std::make_unique<PluginScanner>(sc);
        hosting = std::make_unique<PluginHosting>(*session, hc, *scanner);
        ctl->set_chain_resolver([this](const TrackEdit& t, uint32_t e, ChainBinding& b) { hosting->resolve(t, e, b); });
    }
    ~Rig() {
        hosting.reset();
        scanner.reset();
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
        if (!session->reader().running() && !hold_reader) session->reader().run_pass();
        scanner->exchange(false);
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
    // The #80 review: while a plan still names the old host it stays alive.
    // Residency is held, so the plan built once the new host is Ready waits
    // and the playing one keeps the old binding.
    rig.hold_reader = true;
    check(rig.until([&] { return (rig.hosting->insert_flags(501) & api::kInsertRunning) != 0; }, 15000),
          "the new host is Ready while residency is held");
    bool alive = true;
    for (int i = 0; i < 60; ++i) {
        rig.turn();
        alive = alive && rig.hosting->hosts() == 2 && rig.bound_at(0, kBlock) &&
                rig.chain_node(0)->channel_slot == old_slot;
    }
    check(alive, "the old host stays alive and bound while the playing plan names it");
    rig.hold_reader = false;
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

// Code-reviewer finding 3 on #95: a chain dropped because nothing of it is
// left to run (its running insert moved away, a missing one stays) must
// forget what it ran, or the moved insert is "not running" on its new
// track, which A precedes in the chain map.
int scenario_move_off_missing(const Args& a) {
    std::printf("move_off_missing: eg-amp moved off a track that keeps a missing insert\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(track_op(102, 103, "B"));
    rig.apply(insert_op(100, 500, 0, kEgAmp));
    rig.apply(insert_op(100, 501, 1, "urn:sonora:test#not-installed"));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "A runs eg-amp without the missing one");
    rig.apply(MoveInsert{500, 102, 0});
    rig.show();
    check(rig.until([&] { return rig.bound_at(1, kBlock); }, 15000), "eg-amp runs on B");
    std::string why;
    check(rig.hosting->set_param(500, "gain", -6.f, why), "a parameter change reaches it on B");
    if (!why.empty()) std::printf("  why: %s\n", why.c_str());
    float v = 0.f;
    check(rig.until([&] { return rig.hosting->host_value(500, "gain", v) && v == -6.f; }, 5000),
          "and its host reads the value");
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

// B2 on #80: values the document changes while a session's host starts are
// what its store holds once it is Ready. The sleep plugin, seeded at 300 ms
// a block, holds the host in validation long enough to change them.
int scenario_values_while_starting(const Args& a) {
    std::printf("values_while_starting: values changed while the host validates\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 0, kSleep, {{"sleep_ms", 300.f}}));
    rig.apply(insert_op(100, 501, 1, kEgAmp));
    rig.show(true);
    check(rig.until([&] { return rig.hosting->hosts() == 1; }, 10000), "the host is starting");
    check((rig.hosting->insert_flags(501) & api::kInsertRunning) == 0, "not Ready yet");
    rig.apply(SetInsertParams{501, {{"gain", -20.f}}});
    rig.apply(SetInsertBypass{501, true});
    rig.apply(SetInsertParams{500, {{"sleep_ms", 0.f}}});
    rig.show(false);
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "bound at 128 once Ready");
    float gain = 0.f, bypass = 0.f, sleep = -1.f;
    check(rig.until([&] { return rig.hosting->host_value(501, "gain", gain) && gain == -20.f; }, 10000),
          "the store holds the gain set while it started (-20)");
    check(rig.until([&] { return rig.hosting->host_value(501, "", bypass) && bypass == 1.f; }, 10000),
          "and the bypass");
    check(rig.until([&] { return rig.hosting->host_value(500, "sleep_ms", sleep) && sleep == 0.f; }, 10000),
          "and the sleep plugin's 0 ms");
    std::printf("  gain %.1f, bypass %.0f, sleep_ms %.0f\n", double(gain), double(bypass), double(sleep));
    return failures ? 1 : 0;
}

// B3 on #80: a session opened again (a revert, or another session whose ids
// collide) keeps no host of the one it replaces.
int scenario_reload_starts_afresh(const Args& a) {
    std::printf("reload_starts_afresh: the same session opened again\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 0, kEgAmp));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "eg-amp running");
    const uint32_t old_slot = rig.chain_node(0)->channel_slot;
    const uint32_t old_generation = rig.chain_node(0)->channel_generation;
    std::string why;
    float gain = 0.f;
    check(rig.hosting->set_param(500, "gain", -20.f, why), "gain -20 in the running host, not in the document");
    check(rig.until([&] { return rig.hosting->host_value(500, "gain", gain) && gain == -20.f; }, 2000),
          "its store holds -20");
    rig.show(true);
    check(rig.hosting->loading_total() == 1 && rig.hosting->loading() == 1, "one chain loading again");
    check(rig.until([&] { return rig.silent_at(0, 0); }, 2000), "silent, latency 0, until the new host is Ready");
    // The old slot may be reused once the old host left; its generation
    // moves either way.
    check(rig.until(
              [&] {
                  return rig.bound_at(0, kBlock) && (rig.chain_node(0)->channel_slot != old_slot ||
                                                     rig.chain_node(0)->channel_generation != old_generation);
              },
              15000),
          "bound at 128 on a new host's slot");
    check(rig.hosting->host_value(500, "gain", gain) && gain == 0.f, "its store is the document's (gain 0)");
    std::printf("  gain %.1f\n", double(gain));
    check(rig.until([&] { return rig.hosting->hosts() == 1; }, 5000), "the old host left");
    return failures ? 1 : 0;
}

// The #80 review's should-fix 1: an un-bypass the audio thread dropped (its
// plan replaced before the callback drained it) is posted again.
int scenario_repost_bypass_off(const Args& a) {
    std::printf("repost_bypass_off: an un-bypass dropped by a plan swap\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 0, kEgAmp));
    rig.apply(SetInsertBypass{500, true});
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "eg-amp running");
    float bypass = 0.f;
    check(rig.until([&] { return rig.hosting->host_value(500, "", bypass) && bypass == 1.f; }, 2000), "bypassed");
    const uint64_t dropped = rig.session->plugin_params_dropped();
    rig.apply(SetInsertBypass{500, false});
    rig.hosting->show(rig.history.current(), false);
    // Another plan published before the next callback: the change, tagged
    // with the plan it replaces, is dropped.
    rig.replan();
    rig.session->reader().run_pass();
    rig.ctl->pump();
    rig.turn();
    check(rig.session->plugin_params_dropped() > dropped, "the un-bypass was dropped by the audio thread");
    check(rig.until([&] { return rig.hosting->host_value(500, "", bypass) && bypass == 0.f; }, 3000),
          "re-posted: the store is not bypassed, as the document says");
    return failures ? 1 : 0;
}

// M3 on #80: a touch posts its value to the store; the host dies before its
// GestureEnd; the document gets the value, as the store kept it.
int scenario_gesture_left_open(const Args& a) {
    std::printf("gesture_left_open: the crash plugin touched to crash, mid-gesture\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 0, kCrash));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "running");
    namespace pc = plugin::control;
    std::string bytes;
    std::vector<model::Operation> ops;
    pc::encode(pc::ParamTouched{0, 0, 1.f}, bytes);
    rig.hosting->handle_host_message(100, bytes, rig.history.current(), ops);
    check(ops.empty(), "a touch alone records nothing");
    const auto recorded = [&] {
        const InsertMix* m = rig.history.current().mix.find_insert(500);
        if (!m) return false;
        for (const ParamValue& v : m->params)
            if (v.key == "crash" && v.value == 1.f) return true;
        return false;
    };
    check(rig.until(recorded, 5000), "the host died mid-gesture: crash = 1 is in the document");
    return failures ? 1 : 0;
}

// M4 on #80: a crash plugin added in front of a running eg-amp, crashing at
// its first block: the edit's start fails, the fault stands, eg-amp plays
// on and follows the document, and nothing respawns on later changes.
int scenario_failed_edit(const Args& a) {
    std::printf("failed_edit: a crashing plugin added to a running chain\n");
    Rig rig(a, 2, 3000);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 1, kEgAmp));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "eg-amp running");
    const uint32_t slot = rig.chain_node(0)->channel_slot;
    rig.apply(insert_op(100, 501, 0, kCrash, {{"crash", 1.f}}));
    rig.show(false);
    check(rig.until([&] { return rig.hosting->faults().count(501) == 1; }, 10000), "the new insert is faulted");
    if (rig.hosting->faults().count(501)) std::printf("  fault: %s\n", rig.hosting->faults().at(501).c_str());
    check(rig.hosting->faults().count(500) == 1, "and eg-amp, whose edited chain did not start");
    check(rig.until([&] { return rig.hosting->hosts() == 1; }, 3000), "only the old host is alive");
    bool steady = true;
    for (int k = 0; k < 5; ++k) {
        rig.apply(SetInsertParams{500, {{"gain", -3.f - float(k)}}});
        rig.show(false);
        for (int i = 0; i < 10; ++i) {
            rig.turn();
            steady = steady && rig.hosting->hosts() == 1 && rig.hosting->faults().count(501) == 1;
        }
    }
    check(steady, "five document changes later: no new host was started, the fault stands");
    check(rig.bound_at(0, kBlock) && rig.chain_node(0)->channel_slot == slot, "the old chain still plays");
    float gain = 0.f;
    check(rig.until([&] { return rig.hosting->host_value(500, "gain", gain) && gain == -7.f; }, 2000),
          "and follows the document (gain -7)");
    rig.apply(SetInsertActive{501, false});
    rig.show(false);
    check(rig.until([&] { return rig.hosting->faults().count(500) == 0; }, 3000),
          "the crashing insert made inactive: eg-amp's fault is gone");
    return failures ? 1 : 0;
}

// BACKLOG Track 4 item 8 (#80 review): a saved state that cannot be restored
// loaded the plugin at defaults and said nothing. ADR-0012 §6.2 and
// host_control.h ("shown loaded with defaults plus its params, as for a hash
// mismatch") want it said: a PluginFault on that insert, standing while the
// document names that state, gone once it names another or none.
int scenario_state_not_restored(const Args& a) {
    std::printf("state_not_restored: eg-amp on four tracks, each with a state that cannot be restored\n");
    namespace pc = plugin::control;
    Rig rig(a, 4);
    std::filesystem::create_directories(kDir + "/Plugin State");
    const auto write = [](const std::string& sha, const std::string& bytes) {
        std::FILE* f = std::fopen((kDir + "/" + pc::state_file_name(sha)).c_str(), "wb");
        if (!f) return;
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    };
    // A: one byte past one state's cap (sparse: costs no disk).
    const std::string big(64, 'a');
    write(big, "");
    std::filesystem::resize_file(kDir + "/" + pc::state_file_name(big), pc::kMaxStateBytes + 1);
    // B: named, never written.
    const std::string gone(64, 'b');
    // C: bytes that do not hash to the name.
    const std::string bad(64, 'c');
    write(bad, "not the state the session saved");
    // D: a well-formed state, but another plugin's: the host refuses it
    // (Loaded: StateResult::Refused).
    std::string other, e;
    check(pc::encode_state_blob(pc::StateBlob{pc::Format::Lv2, kLookahead, {"x"}}, other, e), "a state blob");
    const std::string theirs = sonora::sha256_hex(other);
    write(theirs, other);

    const Id tracks[4] = {100, 102, 104, 106};
    const Id inserts[4] = {500, 501, 502, 503};
    const std::string shas[4] = {big, gone, bad, theirs};
    // Ids are handed out in order: every track before any insert.
    for (int i = 0; i < 4; ++i) rig.apply(track_op(tracks[i], tracks[i] + 1, std::string(1, char('A' + i))));
    for (int i = 0; i < 4; ++i) {
        rig.apply(insert_op(tracks[i], inserts[i], 0, kEgAmp, {{"gain", -3.f}}));
        rig.apply(SetInsertState{inserts[i], StateRef{pc::state_file_name(shas[i]), shas[i], 1}});
    }
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock) && rig.bound_at(1, kBlock) && rig.bound_at(2, kBlock) &&
                                 rig.bound_at(3, kBlock); },
                    20000),
          "all four run, at defaults plus their params");
    const char* const what[4] = {"over the cap", "gone", "not matching its hash", "refused by the host"};
    for (int i = 0; i < 4; ++i) {
        const auto& f = rig.hosting->faults();
        const bool said = f.count(inserts[i]) && f.at(inserts[i]).find("saved state") != std::string::npos;
        std::string line = std::string("a state ") + what[i] + ": a PluginFault on its insert";
        check(said, line.c_str());
        if (f.count(inserts[i])) std::printf("  fault: %s\n", f.at(inserts[i]).c_str());
        const uint32_t flags = rig.hosting->insert_flags(inserts[i]);
        check((flags & api::kInsertRunning) && (flags & api::kInsertFaulted), "  running, and shown faulted");
    }
    float gain = 0.f;
    check(rig.hosting->host_value(500, "gain", gain) && gain == -3.f, "its params were applied (gain -3)");
    rig.apply(SetInsertState{500, std::nullopt});
    rig.show(false);
    check(rig.until([&] { return rig.hosting->faults().count(500) == 0; }, 3000),
          "the document names no state for A: its fault is gone");
    check(rig.hosting->faults().count(501) == 1 && rig.hosting->faults().count(503) == 1, "the others still stand");
    check(rig.bound_at(0, kBlock), "and A was not restarted for it");
    return failures ? 1 : 0;
}

// The state plugin's block counter, from a StateBlob: the integer after
// "blocks" in its Turtle (plugins/host/tests/plugin_driver.cpp reads it the
// same way).
bool state_counter(const std::string& bytes, int64_t& n) {
    namespace pc = plugin::control;
    pc::StateBlob b;
    std::string e;
    if (!pc::decode_state_blob(bytes, b, e) || b.sections.empty()) return false;
    const std::string& s = b.sections[0];
    const std::size_t at = s.find("blocks");
    if (at == std::string::npos) return false;
    const std::size_t d = s.find_first_of("0123456789", at + 6);
    if (d == std::string::npos) return false;
    n = std::strtoll(s.c_str() + d, nullptr, 10);
    return true;
}

// One FlushPluginState as the server runs it: the reads, the files (here on
// this thread, through the saver's own SessionFolder::write_states), then
// the document. The flush's counter for `insert`, or -1.
int64_t flush(Rig& rig, Id insert, PluginHosting::Flush* out = nullptr) {
    const uint64_t id = rig.hosting->request_states(0, rig.history.current());
    std::vector<PluginHosting::Flush> done;
    rig.until(
        [&] {
            for (PluginHosting::Flush& f : rig.hosting->take_flushes()) done.push_back(std::move(f));
            return !done.empty();
        },
        8000);
    if (done.size() != 1 || done[0].id != id) {
        std::printf("  %zu flushes done, not flush %llu alone\n", done.size(), (unsigned long long)id);
        return -1;
    }
    for (const std::string& f : done[0].failed) std::printf("  failed: %s\n", f.c_str());
    std::vector<session_files::StateFile> files;
    for (const auto& s : done[0].states)
        if (!s.sha256.empty()) files.push_back({s.sha256, s.bytes});
    session_files::SessionFolder folder;
    std::string why;
    if (!folder.write_states(kDir, files, why)) {
        std::printf("  write_states: %s\n", why.c_str());
        return -1;
    }
    for (const model::Operation& op : rig.hosting->states_saved(done[0], rig.history.current())) {
        rig.ops_seen.push_back(op);
        rig.apply(op);
    }
    rig.show(false);
    int64_t n = -1;
    for (const auto& s : done[0].states)
        if (s.insert == insert && !state_counter(s.bytes, n)) n = -1;
    if (n < 0) std::printf("  no counter for insert %llu in %zu states\n", (unsigned long long)insert, done[0].states.size());
    if (out) *out = std::move(done[0]);
    return n;
}

// BACKLOG Track 4 item 5: FlushPluginState with real hosts. A plugin's
// state (the state plugin's run() count) read from its running host,
// written to "Plugin State/<sha256>.state", named in the document by
// SetInsertState; a host killed as a crash would kill it restarts from that
// state, not from the one it started with; and the session file, written
// and read back, opens the plugin with it. A plugin with no state of its
// own (eg-amp) gets no state and no operation.
int scenario_state_flush(const Args& a) {
    namespace pc = plugin::control;
    std::printf("state_flush: the state plugin on A, eg-amp on B\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(track_op(102, 103, "B"));
    rig.apply(insert_op(100, 500, 0, "urn:sonora:test#state"));
    rig.apply(insert_op(102, 501, 0, kEgAmp));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock) && rig.bound_at(1, kBlock); }, 15000), "both running");
    for (int i = 0; i < 600; ++i) rig.turn();

    PluginHosting::Flush f;
    const int64_t first = flush(rig, 500, &f);
    std::printf("  first flush: %lld blocks counted\n", (long long)first);
    check(f.failed.empty() && f.states.size() == 2, "both instances answered");
    check(first >= 300, "the state carries the blocks the plugin ran (300 or more)");
    const InsertMix* m = rig.history.current().mix.find_insert(500);
    std::string sha;
    for (const auto& s : f.states)
        if (s.insert == 500) sha = s.sha256;
    check(m && m->state && m->state->sha256 == sha && m->state->file == plugin_state_file(sha),
          "SetInsertState names the file by its digest");
    std::ifstream in(kDir + "/" + plugin_state_file(sha), std::ios::binary);
    std::ostringstream bytes;
    bytes << in.rdbuf();
    check(!sha.empty() && pc::state_matches(bytes.str(), sha), "the file holds the bytes its name hashes");
    const InsertMix* amp = rig.history.current().mix.find_insert(501);
    bool amp_empty = false;
    for (const auto& s : f.states) amp_empty = amp_empty || (s.insert == 501 && s.sha256.empty());
    check(amp_empty && (!amp || !amp->state), "eg-amp keeps no state: none named");
    int set_states = 0;
    for (const auto& op : rig.ops_seen) set_states += std::holds_alternative<SetInsertState>(op) ? 1 : 0;
    check(set_states == 1, "one SetInsertState, none for eg-amp");

    // A crash: the restart restores the flushed state, so it counts on from
    // there; a fresh instance would count from 0.
    check(rig.hosting->kill_host_for_test(100), "A's host killed");
    check(rig.until([&] { return (rig.hosting->insert_flags(500) & api::kInsertRestarting) != 0; }, 5000),
          "restarting");
    check(rig.until([&] { return (rig.hosting->insert_flags(500) & api::kInsertRunning) != 0; }, 15000),
          "running again");
    const int64_t after_crash = flush(rig, 500);
    std::printf("  after the crash: %lld\n", (long long)after_crash);
    check(after_crash >= first, "the restarted plugin counts on from the flushed state");

    // The session file: written, read back, opened. The new host restores
    // the state the file names.
    std::string text, e;
    check(serialize(rig.history.current(), WriteInfo{"test", 1}, text, e), "the session serialises");
    Loaded loaded;
    LoadError le;
    check(deserialize(text, loaded, le), "and reads back");
    const InsertMix* lm = loaded.snapshot.mix.find_insert(500);
    check(lm && lm->state && *lm->state == *rig.history.current().mix.find_insert(500)->state,
          "with the state reference");
    rig.history = History(loaded.snapshot);
    rig.show(true);
    // The playing plan can still name the old host for a turn or two: the
    // insert's own flag says the new one is Ready.
    check(rig.until([&] { return (rig.hosting->insert_flags(500) & api::kInsertRunning) != 0; }, 15000),
          "the reopened session runs A");
    check(rig.hosting->faults().count(500) == 0, "its state was restored (no fault)");
    const int64_t reopened = flush(rig, 500);
    std::printf("  after reopening: %lld\n", (long long)reopened);
    check(reopened >= after_crash && reopened >= first, "the reopened plugin counts on from the saved state");
    return failures ? 1 : 0;
}

// Through the server on drivers/fake: FlushPluginState is answered after the
// state file is on disk and the SetInsertState update has gone out; an
// insert that is not on any track is refused.
int scenario_server_state_flush(const Args& a) {
    namespace pc = plugin::control;
    std::printf("server_state_flush: FlushPluginState through EngineControlServer\n");
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
    h.apply(insert_op(100, 500, 0, "urn:sonora:test#state"), e);
    EngineControlServer::Options opts;
    opts.plugin_host = a.host;
    opts.plugin_bundles = a.bundles;
    opts.plugin_scan_at_start = false;
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
              "the insert runs");
        until([] { return false; }, 300);
        const std::size_t updates_before = in.model.size();
        api::ModelRequest flush;
        flush.action = api::ModelAction::FlushPluginState;
        const uint64_t seq = engine.submit(flush);
        api::ModelRequest bad;
        bad.action = api::ModelAction::FlushPluginState;
        bad.op = "999";
        const uint64_t bad_seq = engine.submit(bad);
        api::CommandReply ok_reply, bad_reply;
        std::size_t updates_at_reply = 0;
        check(until(
                  [&] {
                      for (const auto& r : in.replies) {
                          if (r.seq == seq && ok_reply.seq != seq) {
                              ok_reply = r;
                              updates_at_reply = in.model.size();
                          }
                          if (r.seq == bad_seq) bad_reply = r;
                      }
                      return ok_reply.seq == seq && bad_reply.seq == bad_seq;
                  },
                  10000),
              "both flushes answered");
        check(ok_reply.result == DispatchResult::Ok, "the flush: Ok");
        if (!ok_reply.message.empty()) std::printf("  message: %s\n", ok_reply.message.c_str());
        check(updates_at_reply > updates_before, "  its SetInsertState update came before the reply");
        check(bad_reply.result == DispatchResult::Failed && bad_reply.message.find("not on any track") != std::string::npos,
              "an insert on no track: Failed, why");
        int files = 0;
        bool matches = true;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(kDir + "/Plugin State", ec)) {
            const std::string name = entry.path().filename().string();
            if (name.size() < 6 || name.substr(name.size() - 6) != ".state") continue;
            ++files;
            std::ifstream f(entry.path(), std::ios::binary);
            std::ostringstream s;
            s << f.rdbuf();
            matches = matches && pc::state_matches(s.str(), name.substr(0, name.size() - 6));
        }
        check(files == 1 && matches, "one state file, holding the bytes its name hashes");

        // #131 B1: the session file saved into a folder other than --dir,
        // as File > Save puts "Mix" in ~/Sessions/Mix, must hold the states
        // it names; a copy of that folder alone reopens with them.
        Replica replica;
        for (const api::ModelUpdate& m : in.model) {
            ReplicationUpdate u;
            u.kind = static_cast<ReplicationUpdate::Kind>(m.kind);
            u.part_count = m.part_count;
            u.part_bytes = m.part_bytes;
            u.revision = m.revision;
            u.undo_floor = m.undo_floor;
            u.next_id = m.next_id;
            u.bytes = m.bytes;
            u.label = m.label;
            std::string why;
            replica.receive(u, why);
        }
        check(replica.synced(), "the replica follows the document");
        const std::filesystem::path elsewhere = std::filesystem::absolute(kDir + "_elsewhere");
        std::filesystem::remove_all(elsewhere);
        std::string bytes;
        check(replica.synced() && serialize(replica.current(), WriteInfo{"test", 1}, bytes, error), "serialised");
        api::ModelRequest save;
        save.action = api::ModelAction::Save;
        save.op = api::wire::encode_save((elsewhere / "Mix" / "Mix.sonora").string(), bytes);
        const uint64_t save_seq = engine.submit(save);
        api::CommandReply save_reply;
        check(until(
                  [&] {
                      for (const auto& r : in.replies)
                          if (r.seq == save_seq) save_reply = r;
                      return save_reply.seq == save_seq;
                  },
                  10000),
              "the save answered");
        if (!save_reply.message.empty()) std::printf("  message: %s\n", save_reply.message.c_str());
        check(save_reply.result == DispatchResult::Ok, "the save into another folder: Ok");
        const std::filesystem::path copy = std::filesystem::absolute(kDir + "_copy");
        std::filesystem::remove_all(copy);
        std::filesystem::copy(elsewhere / "Mix", copy, std::filesystem::copy_options::recursive, ec);
        Loaded loaded;
        LoadError load_error;
        std::ifstream sf(copy / "Mix.sonora", std::ios::binary);
        std::ostringstream ss;
        ss << sf.rdbuf();
        const bool opened = deserialize(ss.str(), loaded, load_error);
        check(opened, "the copy opens");
        const InsertMix* im = opened ? loaded.snapshot.mix.find_insert(500) : nullptr;
        check(im != nullptr && im->state, "  and names the insert's state");
        if (im != nullptr && im->state) {
            std::ifstream st(copy / im->state->file, std::ios::binary);
            std::ostringstream sb;
            sb << st.rdbuf();
            check(pc::state_matches(sb.str(), im->state->sha256), "  with the bytes its name hashes");
        }
        std::filesystem::remove_all(elsewhere);
        std::filesystem::remove_all(copy);
        engine.shutdown();
        audio.deactivate();
        engine.server().after_deactivate();
        session.stop();
    }
    std::filesystem::remove_all(kDir);
    return failures ? 1 : 0;
}

// BACKLOG Track 4 item 5: the worker's refused messages were counted in the
// host and told to nobody, so a sampler that never got its sample played on
// as if it had. ADR-0012 change G, as for non-finite output: one Notice the
// first time in a run, naming plugin, track and slot, and the count from
// then on; nothing stands, since the chain plays on.
int scenario_work_refused(const Args& a) {
    std::printf("work_refused: the worker test plugin floods its worker on A, not on B\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(track_op(102, 103, "B"));
    rig.apply(insert_op(100, 500, 0, kWorker, {{"gain", 0.0625f}}));  // the test plugin's flood
    rig.apply(insert_op(102, 501, 0, kWorker, {{"gain", 0.5f}}));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock) && rig.bound_at(1, kBlock); }, 20000), "both run");
    std::vector<std::string> notices;
    const auto take = [&] {
        for (std::string& n : rig.hosting->take_notices()) notices.push_back(std::move(n));
    };
    check(rig.until(
              [&] {
                  take();
                  return rig.hosting->work_refused(500) > 0;
              },
              3000),
          "A's refused worker messages are counted");
    // Two more intervals of flood: still one notice, a rising count.
    const uint64_t first = rig.hosting->work_refused(500);
    bool standing = false;
    for (int i = 0; i < 900; ++i) {
        rig.turn();
        take();
        standing = standing || rig.hosting->faults().count(500) != 0;
    }
    check(rig.hosting->work_refused(500) > first, "  and the count rises while the flood goes on");
    check(!standing, "  no fault stands for it");
    const uint32_t flags = rig.hosting->insert_flags(500);
    check((flags & api::kInsertRunning) && !(flags & api::kInsertFaulted), "  shown running, not faulted");
    check(notices.size() == 1, "one notice for the run, however many refusals");
    if (!notices.empty()) std::printf("  notice: %s\n", notices[0].c_str());
    check(!notices.empty() && notices[0].find(kWorker) != std::string::npos &&
              notices[0].find("(A, slot A)") != std::string::npos &&
              notices[0].find("worker refused") != std::string::npos,
          "  naming the plugin, its track and its slot");
    check(rig.hosting->work_refused(501) == 0, "B's, whose worker keeps up, counts nothing");
    rig.apply(SetInsertParams{500, {{"gain", 1.f}}});
    rig.show(false);
    for (int i = 0; i < 200; ++i) rig.turn();
    const uint64_t after = rig.hosting->work_refused(500);
    check(after > 0, "the flood over, the count is kept");
    // A new run: the chain built again, flooding again, says it again, and
    // the count only rises.
    rig.apply(SetInsertActive{500, false});
    rig.show(false);
    check(rig.until([&] { return rig.unbound(0); }, 5000), "made inactive");
    rig.apply(SetInsertActive{500, true});
    rig.apply(SetInsertParams{500, {{"gain", 0.0625f}}});
    rig.show(false);
    check(rig.until(
              [&] {
                  take();
                  return rig.bound_at(0, kBlock) && notices.size() == 2;
              },
              20000),
          "active again and flooding: a second notice, for the new run");
    check(rig.hosting->work_refused(500) >= after, "  the count never went down");
    return failures ? 1 : 0;
}

// Fault{EventsDropped} and Fault{ParamUnmapped} (HostControl wire 3) as the
// host sends them, handed to track A's chain as the server's poll does; the
// host's own sending is plugin_host_vst3_gesture's.
int scenario_touch_faults(const Args& a) {
    std::printf("touch_faults: lost and unmapped touches from A's host are counted and said once a run\n");
    namespace pc = plugin::control;
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(track_op(102, 103, "B"));
    rig.apply(insert_op(100, 500, 0, kEgAmp));
    rig.apply(insert_op(100, 501, 1, kEgAmp));
    rig.apply(insert_op(102, 502, 0, kEgAmp));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock) && rig.bound_at(1, kBlock); }, 20000), "both run");
    rig.hosting->take_notices();
    const auto send = [&](const pc::Fault& f) {
        std::string bytes;
        std::vector<model::Operation> ops;
        pc::encode(f, bytes);
        rig.hosting->handle_host_message(100, bytes, rig.history.current(), ops);
        return ops.empty();
    };
    bool no_ops = send(pc::Fault{pc::FaultKind::EventsDropped, -1, 3, "touches not queued"});
    no_ops = send(pc::Fault{pc::FaultKind::EventsDropped, -1, 2, "touches not queued"}) && no_ops;
    no_ops = send(pc::Fault{pc::FaultKind::ParamUnmapped, 1, 4, "no dense index"}) && no_ops;
    no_ops = send(pc::Fault{pc::FaultKind::ParamUnmapped, 1, 1, "no dense index"}) && no_ops;
    // Instance 2 is no plugin of A's chain of two.
    no_ops = send(pc::Fault{pc::FaultKind::ParamUnmapped, 2, 7, "no dense index"}) && no_ops;
    check(no_ops, "no document operation from any of them");
    check(rig.hosting->touches_lost(100) == 5, "A's lost touches are counted, summed: 5");
    check(rig.hosting->touches_lost(102) == 0, "  B's count nothing");
    check(rig.hosting->param_unmapped(501) == 5, "slot B's unmapped touches are counted, summed: 5");
    check(rig.hosting->param_unmapped(500) == 0 && rig.hosting->param_unmapped(502) == 0,
          "  the other inserts', and the one past the chain, count nothing");
    std::vector<std::string> notices = rig.hosting->take_notices();
    for (const std::string& n : notices) std::printf("  notice: %s\n", n.c_str());
    check(notices.size() == 2, "two notices: one for the chain, one for the insert");
    const auto has = [&](const char* a1, const char* a2) {
        for (const std::string& n : notices)
            if (n.find(a1) != std::string::npos && n.find(a2) != std::string::npos) return true;
        return false;
    };
    check(has("A: ", "were lost on the way") && has("A: ", "read back and recorded"),
          "  the lost touches name the track, and say the values were read back");
    check(has("(A, slot B)", "did not record that change"), "  the unmapped ones name the plugin, track and slot");
    bool standing = false;
    for (int i = 0; i < 100; ++i) {
        rig.turn();
        standing = standing || rig.hosting->faults().count(500) || rig.hosting->faults().count(501);
    }
    check(!standing, "no fault stands for them");
    const uint32_t flags = rig.hosting->insert_flags(501);
    check((flags & api::kInsertRunning) && !(flags & api::kInsertFaulted), "  slot B shown running, not faulted");
    // A new run: the chain built again says them again; the counts only rise.
    const uint32_t old_slot = rig.chain_node(0)->channel_slot;
    rig.apply(SetInsertActive{500, false});
    rig.show(false);
    check(rig.until([&] { return rig.bound_at(0, kBlock) && rig.chain_node(0)->channel_slot != old_slot; }, 20000),
          "the chain built again without slot A");
    rig.hosting->take_notices();
    send(pc::Fault{pc::FaultKind::EventsDropped, -1, 1, ""});
    send(pc::Fault{pc::FaultKind::ParamUnmapped, 0, 1, ""});  // slot B is the chain's first now
    notices = rig.hosting->take_notices();
    check(notices.size() == 2, "a new run: both said again");
    check(rig.hosting->touches_lost(100) == 6 && rig.hosting->param_unmapped(501) == 6,
          "  the counts kept and rising");
    // Ids are per session (#121 review): a session opened with the same
    // track and insert ids starts from 0, with none of the last one's notices.
    send(pc::Fault{pc::FaultKind::EventsDropped, -1, 1, ""});  // its notices left untaken
    rig.history = History(new_session(SessionSettings{}));
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 501, 0, kEgAmp));
    rig.show(true);
    // The last session's host plays until a plan without it: wait for this
    // session's own.
    check(rig.until([&] { return rig.bound_at(0, kBlock) && (rig.hosting->insert_flags(501) & api::kInsertRunning); },
                    20000),
          "a new session: its insert 501 runs");
    check(rig.hosting->touches_lost(100) == 0 && rig.hosting->param_unmapped(501) == 0,
          "  with counts of 0, not the last session's");
    check(rig.hosting->take_notices().empty(), "  and none of its notices");
    send(pc::Fault{pc::FaultKind::ParamUnmapped, 0, 2, ""});
    send(pc::Fault{pc::FaultKind::EventsDropped, -1, 3, ""});
    check(rig.hosting->take_notices().size() == 2 && rig.hosting->param_unmapped(501) == 2 &&
              rig.hosting->touches_lost(100) == 3,
          "  its own faults: said afresh, counted from 0");
    return failures ? 1 : 0;
}

// Ids are per session: a session opened with an insert of the same id as
// one that was refused in the last carries none of its count, and none of
// its notices not yet taken; a refusal in the new one is said afresh.
int scenario_work_refused_new_session(const Args& a) {
    std::printf("work_refused_new_session: refusals in session A, then B with the same insert id\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 0, kWorker, {{"gain", 0.0625f}}));  // the test plugin's flood
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock) && rig.hosting->work_refused(500) > 0; }, 20000),
          "session A: insert 500's refused worker messages are counted");
    // Its notice is left untaken, as one would be if the open came first.
    rig.history = History(new_session(SessionSettings{}));
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 0, kWorker, {{"gain", 1.f}}));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 20000), "session B: its insert 500 runs");
    bool zero = true;
    for (int i = 0; i < 200; ++i) {
        rig.turn();
        zero = zero && rig.hosting->work_refused(500) == 0;
    }
    check(zero, "  with a count of 0, not session A's");
    check(rig.hosting->take_notices().empty(), "  and none of session A's notices");
    rig.apply(SetInsertParams{500, {{"gain", 0.0625f}}});
    rig.show(false);
    std::vector<std::string> notices;
    check(rig.until(
              [&] {
                  for (std::string& n : rig.hosting->take_notices()) notices.push_back(std::move(n));
                  return !notices.empty();
              },
              5000),
          "flooding in B: a fresh notice");
    check(notices.size() == 1 && rig.hosting->work_refused(500) > 0, "  one, and B's own count");
    return failures ? 1 : 0;
}

// The same through EngineControlServer: the count reaches the state block's
// InsertState, the Notice reaches the client once, and no PluginFault is
// posted for the insert.
int scenario_server_work_refused(const Args& a) {
    std::printf("server_work_refused: the worker flood on drivers/fake, through the server\n");
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
    h.apply(insert_op(100, 500, 0, kWorker, {{"gain", 0.0625f}}), e);
    EngineControlServer::Options opts;
    opts.plugin_host = a.host;
    opts.plugin_bundles = a.bundles;
    opts.plugin_scan_at_start = false;
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
        const auto refused = [&] { return engine.state().track(0).inserts[0].work_refused; };
        check(until([&] { return refused() > 0; }, 20000), "the state block counts the refused worker messages");
        const uint32_t first = refused();
        until([] { return false; }, 2500);
        check(refused() > first, "  and the count rises");
        const uint32_t flags = engine.state().track(0).inserts[0].flags;
        check((flags & api::kInsertRunning) && !(flags & api::kInsertFaulted), "  the insert running, not faulted");
        int notices = 0;
        bool fault = false;
        for (const auto& ev : in.events) {
            if (ev.kind == api::EventKind::Notice && ev.text.find("worker refused") != std::string::npos) ++notices;
            if (ev.kind == api::EventKind::PluginFault && ev.insert == 500 && !ev.text.empty()) fault = true;
        }
        check(notices == 1, "one Notice reached the client");
        check(!fault, "no PluginFault for the insert");
        engine.shutdown();
        audio.deactivate();
        engine.server().after_deactivate();
        session.stop();
    }
    std::filesystem::remove_all(kDir);
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
    opts.plugin_scan_at_start = false;
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
        // The state block says Running at Ready; the plan that binds the
        // chain is adopted by the audio thread a turn or two later. Read
        // through the state block, which the server's thread writes from
        // the playing plan's report: the plan itself is that thread's, and
        // reading it from here was a data race (TSan on #104). At open the
        // chain is silent at latency 0, so 128 is the bound host's.
        const auto bound = [&] { return engine.state().track(0).latency_frames == kBlock; };
        check(until(bound, 5000), "the playing plan binds the chain at 128");
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

// The owner's decision of 2026-10-18 (ADR-0012 amendment, item 1): play
// from a freshly opened session waits until its plug-ins have loaded or
// failed, at most the bound, with "Loading plug-ins (n of m)" said once and
// never kept for a client that attaches later; record never waits; only a
// stop, a second space or a take withdraws it. A sleep plugin seeded at
// `sleep_ms` per block delays its host's Ready by about that (validation
// runs a block).
struct PlayWait {
    float sleep_ms = 1500.f;
    uint64_t wait_ms = 10000;
    std::string host;  // empty: the real host
    enum class Expect { ReadyFirst, BoundFirst, NoWait, Cancelled } expect = Expect::ReadyFirst;
};

int play_waits(const Args& a, const PlayWait& w) {
    std::filesystem::remove_all(kDir);
    SessionConfig cfg;
    cfg.dir = kDir;
    cfg.track_count = 2;
    cfg.ring_seconds = 0.5;
    cfg.playback.max_param_stores = 16;
    Session session(cfg);
    session.create_dirs();
    FakeBackend::Config bc;
    bc.devices = {FakeBackend::onboard()};
    bc.mode = FakeBackend::Mode::Thread;
    FakeBackend audio(bc);
    std::string error;
    check(audio.open({"sonora", 2, 2, 48000, ""}, session, error) && audio.activate(error), "fake backend up");
    session.disk().start();
    History h(new_session(SessionSettings{}));
    std::string e;
    h.apply(track_op(100, 101, "A"), e);
    h.apply(insert_op(100, 500, 0, kSleep, {{"sleep_ms", w.sleep_ms}}), e);
    EngineControlServer::Options opts;
    opts.plugin_host = w.host.empty() ? a.host : w.host;
    opts.plugin_bundles = a.bundles;
    opts.plugin_scan_at_start = false;
    opts.plugin_open_wait_ms = w.wait_ms;
    opts.initial_document = std::make_shared<Snapshot>(h.current());
    {
        // The bound runs from session open, not from the play (ADR-0012
        // item 1): the server starts it while connecting, before the play is
        // sent. Lower bounds count from before the engine exists and upper
        // bounds from the play, so neither depends on how long connect took
        // (a slow sanitizer once answered 799 ms after an 800 ms bound).
        const uint64_t t_open = now_ms();
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
        const auto running = [&] { return (engine.state().track(0).inserts[0].flags & api::kInsertRunning) != 0; };
        const auto replied = [&](uint64_t seq, api::CommandReply& out) {
            for (const auto& r : in.replies)
                if (r.seq == seq) out = r;
            return out.seq == seq;
        };
        const auto notices = [&] {
            uint32_t n = 0;
            for (const auto& ev : in.events)
                n += ev.kind == api::EventKind::Notice && ev.text.find("Loading plug-ins") != std::string::npos;
            return n;
        };
        api::CommandRequest play;
        play.command = "transport.play";
        const uint64_t t0 = now_ms();
        const uint64_t seq = engine.send(play);
        api::CommandReply reply;
        if (w.expect == PlayWait::Expect::Cancelled) {
            check(!until([&] { return replied(seq, reply); }, 300), "play waits");
            // Not a cancel: a return to zero runs, the play still waits.
            api::CommandRequest rtz;
            rtz.command = "transport.return_to_zero";
            const uint64_t rtz_seq = engine.send(rtz);
            api::CommandReply rtz_reply;
            check(until([&] { return replied(rtz_seq, rtz_reply); }, 2000), "return to zero answered");
            check(!until([&] { return replied(seq, reply); }, 300), "the play still waits after a return to zero");
            api::CommandRequest stop;
            stop.command = "transport.stop";
            engine.send(stop);
            check(until([&] { return replied(seq, reply); }, 2000), "a stop answers the waiting play");
            check(reply.result == DispatchResult::Failed && reply.message == "cancelled", "Failed, cancelled");
            for (int i = 0; i < 40; ++i) until([] { return false; }, 5);
            check(session.transport().state() == TransportState::Stopped, "the transport stays stopped");
        } else {
            // Space pressed twice: one wait, one notice, both answered.
            const uint64_t seq2 = engine.send(play);
            api::CommandReply reply2;
            check(until([&] { return replied(seq, reply) && replied(seq2, reply2); }, 20000), "both plays answered");
            const uint64_t answered = now_ms();
            const uint64_t waited = answered - t0;
            const uint64_t since_open = answered - t_open;
            char line[160];
            std::snprintf(line, sizeof line, "answered Ok after %llu ms (%llu ms since open)",
                          (unsigned long long)waited, (unsigned long long)since_open);
            check(reply.result == DispatchResult::Ok && reply2.result == DispatchResult::Ok, line);
            if (w.expect == PlayWait::Expect::NoWait) {
                check(waited < 1500, "hosting disabled: play did not wait for plug-ins that will never load");
                check(notices() == 0, "and said nothing about loading");
            } else {
                check(notices() == 1, "\"Loading plug-ins\" said once");
                bool zero_of_one = false;
                for (const auto& ev : in.events)
                    zero_of_one = zero_of_one || ev.text.find("Loading plug-ins (0 of 1)") != std::string::npos;
                check(zero_of_one, "as \"Loading plug-ins (0 of 1)\"");
            }
            if (w.expect == PlayWait::Expect::ReadyFirst) {
                // Ready needs a validation block at sleep_ms, so play waited
                // at least that, and ended on the plug-in, well before the
                // bound. (Once Ready, a plug-in this slow misses every block
                // and is restarted: Running is too brief to sample.)
                check(since_open >= uint64_t(w.sleep_ms) - 100 && waited < w.wait_ms - 1000,
                      "play waited for the plug-in, not for the bound");
            } else if (w.expect == PlayWait::Expect::BoundFirst) {
                check(!running() && since_open >= w.wait_ms && waited < w.wait_ms + 2000,
                      "the bound passed first: play went ahead with the plug-in still loading");
            }
            check(until([&] { return session.transport().state() == TransportState::Rolling; }, 3000), "rolling");
            // A client attaching after the wait is not told plug-ins are
            // loading: the notice was transient.
            std::mutex m;
            std::vector<std::string> frames;
            const EngineControlServer::ClientId late = engine.server().attach(
                [&](EngineControlServer::Frame bytes) {
                    std::lock_guard<std::mutex> lock(m);
                    frames.push_back(*bytes);
                },
                {});
            for (int i = 0; i < 60; ++i) until([] { return false; }, 5);
            bool told = false;
            {
                std::lock_guard<std::mutex> lock(m);
                check(!frames.empty(), "the late client got its Hello");
                for (const std::string& f : frames) told = told || f.find("Loading plug-ins") != std::string::npos;
            }
            check(!told, "a client attaching later is not told plug-ins are loading");
            engine.server().detach(late);
        }
        engine.shutdown();
        audio.deactivate();
        engine.server().after_deactivate();
        session.stop();
    }
    std::filesystem::remove_all(kDir);
    return failures ? 1 : 0;
}

int scenario_play_waits(const Args& a) {
    std::printf("play_waits: a session opened with a plug-in that takes 1.5 s to load, play pressed twice at once\n");
    return play_waits(a, PlayWait{1500.f, 10000, "", PlayWait::Expect::ReadyFirst});
}

int scenario_play_wait_bounded(const Args& a) {
    std::printf("play_wait_bounded: a plug-in that takes 4 s, the wait bounded at 800 ms\n");
    return play_waits(a, PlayWait{4000.f, 800, "", PlayWait::Expect::BoundFirst});
}

// #83 review item 1: a host the supervisor refuses (here not a host at all)
// leaves nothing loading, so play does not wait.
int scenario_play_no_hosting(const Args& a) {
    std::printf("play_no_hosting: --plugin-host is not a Sonora host, play pressed at once\n");
    return play_waits(a, PlayWait{1500.f, 10000, "/bin/true", PlayWait::Expect::NoWait});
}

// #83 review items 3 and 5: a return to zero leaves the play waiting; a
// stop withdraws it.
int scenario_play_wait_cancel(const Args& a) {
    std::printf("play_wait_cancel: play waits, a return to zero, then a stop\n");
    return play_waits(a, PlayWait{4000.f, 10000, "", PlayWait::Expect::Cancelled});
}

}  // namespace


int scenario_rescan_drops_stale(const Args& a) {
    std::printf("rescan_drops_stale: eg-amp's record changes under a running chain\n");
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 0, kEgAmp));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "eg-amp running");
    const uint32_t slot = rig.chain_node(0)->channel_slot;
    std::vector<std::string> keys;
    check(rig.hosting->probe_answer_for_test(kEgAmp, keys) &&
              std::find(keys.begin(), keys.end(), "gain") != keys.end(),
          "the cached answer has eg-amp's table");
    const uint64_t dropped = rig.hosting->probe_answers_dropped();

    // As the worker would publish after a rescan found the plugin changed:
    // the same catalog with one more parameter in eg-amp's record.
    const auto republish = [&](bool change) {
        auto c = std::make_shared<PluginScanner::Catalog>(*rig.scanner->catalog());
        c->serial += 1000;
        for (auto& [k, r] : c->probes) {
            if (k.first != kEgAmp || !change) continue;
            plugin::probe::Param p = r.params.at(0);
            p.key = "gain2";
            p.symbol = "gain2";
            r.params.push_back(p);
        }
        rig.scanner->inject_catalog_for_test(std::move(c));
    };
    republish(false);
    for (int i = 0; i < 20; ++i) rig.turn();
    check(rig.hosting->probe_answers_dropped() == dropped, "an unchanged record keeps the cached answer");

    republish(true);
    check(rig.until(
              [&] {
                  return rig.hosting->probe_answer_for_test(kEgAmp, keys) &&
                         std::find(keys.begin(), keys.end(), "gain2") != keys.end();
              },
              2000),
          "a changed record replaces the cached answer");
    check(rig.hosting->probe_answers_dropped() == dropped + 1, "one answer dropped");
    for (int i = 0; i < 20; ++i) rig.turn();
    check(rig.bound_at(0, kBlock) && rig.chain_node(0)->channel_slot == slot && rig.hosting->hosts() == 1,
          "the running chain is not restarted");
    std::string why;
    check(rig.hosting->set_param(500, "gain", -6.f, why), "it still takes its own keys");
    check(!rig.hosting->set_param(500, "gain2", 0.f, why), "and only them, until its next start");
    return failures ? 1 : 0;
}

// Code-reviewer on #104: a rescan that loses a running chain's plugin, or
// now blocks it, must not touch the chain (ADR-0012): it keeps playing as
// it is and follows the document's values; the new answer applies at its
// next start.
int rescan_keeps_running(const Args& a, plugin::probe::Outcome now) {
    Rig rig(a);
    rig.apply(track_op(100, 101, "A"));
    rig.apply(insert_op(100, 500, 0, kEgAmp));
    rig.show(true);
    check(rig.until([&] { return rig.bound_at(0, kBlock); }, 15000), "eg-amp running");
    const uint32_t slot = rig.chain_node(0)->channel_slot;
    const uint64_t dropped = rig.hosting->probe_answers_dropped();
    auto c = std::make_shared<PluginScanner::Catalog>(*rig.scanner->catalog());
    c->serial += 1000;
    for (auto& [k, r] : c->probes) {
        if (k.first != kEgAmp) continue;
        r.outcome = now;
        r.params.clear();
        r.reason = now == plugin::probe::Outcome::Blocked ? "its probe crashed (signal 11)" : "";
    }
    rig.scanner->inject_catalog_for_test(std::move(c));
    check(rig.until([&] { return rig.hosting->probe_answers_dropped() == dropped + 1; }, 2000),
          "the rescan's answer replaces the cached one");
    bool kept = true;
    for (int i = 0; i < 40; ++i) {
        rig.turn();
        kept = kept && rig.bound_at(0, kBlock) && rig.chain_node(0)->channel_slot == slot && rig.hosting->hosts() == 1;
    }
    check(kept, "the running chain keeps playing, unchanged, on its own host");
    check((rig.hosting->insert_flags(500) & api::kInsertRunning) != 0 && rig.hosting->faults().empty(),
          "the insert shows running, with no fault");
    rig.apply(SetInsertParams{500, {{"gain", -12.f}}});
    rig.show(false);
    float v = 0.f;
    check(rig.until([&] { return rig.hosting->host_value(500, "gain", v) && v == -12.f; }, 2000),
          "a value the document changes still reaches it");
    check(rig.bound_at(0, kBlock) && rig.chain_node(0)->channel_slot == slot, "and it is still the same chain");
    return failures ? 1 : 0;
}

int scenario_rescan_loses_running(const Args& a) {
    std::printf("rescan_loses_running: a rescan finds a running chain's plugin gone\n");
    return rescan_keeps_running(a, plugin::probe::Outcome::NotInstalled);
}

int scenario_rescan_blocks_running(const Args& a) {
    std::printf("rescan_blocks_running: a rescan blocks a running chain's plugin\n");
    return rescan_keeps_running(a, plugin::probe::Outcome::Blocked);
}

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
    else if (a.scenario == "values_while_starting") rc = scenario_values_while_starting(a);
    else if (a.scenario == "reload_starts_afresh") rc = scenario_reload_starts_afresh(a);
    else if (a.scenario == "repost_bypass_off") rc = scenario_repost_bypass_off(a);
    else if (a.scenario == "gesture_left_open") rc = scenario_gesture_left_open(a);
    else if (a.scenario == "failed_edit") rc = scenario_failed_edit(a);
    else if (a.scenario == "move_off_missing") rc = scenario_move_off_missing(a);
    else if (a.scenario == "state_not_restored") rc = scenario_state_not_restored(a);
    else if (a.scenario == "work_refused") rc = scenario_work_refused(a);
    else if (a.scenario == "server_work_refused") rc = scenario_server_work_refused(a);
    else if (a.scenario == "work_refused_new_session") rc = scenario_work_refused_new_session(a);
    else if (a.scenario == "touch_faults") rc = scenario_touch_faults(a);
    else if (a.scenario == "play_waits") rc = scenario_play_waits(a);
    else if (a.scenario == "play_wait_bounded") rc = scenario_play_wait_bounded(a);
    else if (a.scenario == "play_no_hosting") rc = scenario_play_no_hosting(a);
    else if (a.scenario == "play_wait_cancel") rc = scenario_play_wait_cancel(a);
    else if (a.scenario == "rescan_drops_stale") rc = scenario_rescan_drops_stale(a);
    else if (a.scenario == "rescan_loses_running") rc = scenario_rescan_loses_running(a);
    else if (a.scenario == "rescan_blocks_running") rc = scenario_rescan_blocks_running(a);
    else if (a.scenario == "state_flush") rc = scenario_state_flush(a);
    else if (a.scenario == "server_state_flush") rc = scenario_server_state_flush(a);
    else std::fprintf(stderr, "unknown scenario %s\n", a.scenario.c_str());
    if (rc == 0 && failures) rc = 1;
    std::printf("%s: %s\n", a.scenario.c_str(), rc == 0 ? "ok" : rc == kSkip ? "skipped" : "FAILED");
    return rc;
}
