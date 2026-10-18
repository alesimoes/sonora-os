// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control thread's side of the plugin boundary (ADR-0012 §4, §5.3):
// starts sonora-plugin-host processes, brings each to Ready, binds it to a
// channel slot of PluginClient, watches it, and restarts it make-before-
// break. Never on the audio thread: everything here allocates, starts
// processes or waits.
//
// The host speaks contracts/plugin/host_control.h (wire 1) on descriptor 3,
// with its regions at 4 and 5 (--control-fd 3 --shm-fd 4 --clock-fd 5; the
// 2026-10-13 amendment of ADR-0012 §4): the engine writes EngineHello and
// Load first, checks the host's Hello (versions, pid) and its Loaded (one
// entry per instance, each parameter table the size the scan said, the
// latency plausible), and from then on takes only what a host may send,
// each message checked (instance, dense parameter, rates). A host that
// breaks the contract is killed and follows the crash path (§5.3). The
// region stays the truth for Ready and latency; a message is a wake-up and
// the text. Every socket operation is non-blocking (HostProcess::pump).
//
// Still open:
//   - the host is watched by polling (ChildProcess, waitpid WNOHANG) from
//     poll(); TODO(pidfd): a pidfd in the control server's poll set.
//   - GetState for the instances that carry over an edit is the control
//     server's (request_state); a restart starts from the store and the
//     last state the server holds in spec.states.
// Hosts must be started from the control server's long-lived thread: the
// host's death signal follows the thread that started it (plugins/host
// ipc.h, die_with_parent).
#pragma once

#include "sonora/plugin/host_control.h"
#include "sonora/plugin/protocol.h"
#include "sonora/plugin_client/plugin_client.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sonora::platform {
class ChildProcess;
class ControlChannel;
class SharedRegion;
}  // namespace sonora::platform

namespace sonora::plugin_client {

struct SupervisorConfig {
    std::string host_path;             // sonora-plugin-host
    std::vector<std::string> bundles;  // --bundle, for every host
    uint32_t sample_rate = 48000;
    uint32_t block_frames = 128;
    uint32_t max_frames = 1024;
    int32_t host_rt_priority = 70;  // below the audio thread's 80 (ADR-0012 §2)
    uint64_t ready_timeout_ms = 10000;    // §5.3
    uint64_t heartbeat_timeout_ms = 500;  // §4: a hang outside a plugin
    uint32_t max_restarts = 3;            // §5.3: at most 3 in 60 s
    uint64_t restart_window_ms = 60000;
    // Where `--version` output is captured at start-up.
    std::string scratch_dir = "/tmp";
};

// One chain as the scan and the model describe it.
struct ChainSpec {
    std::vector<std::string> plugins;    // LV2 URIs or vst3:<class id>, in slot order
    std::vector<uint32_t> param_counts;  // dense parameters of each, from the scan
    uint32_t channels = 1;               // signal channels, plus key_channels after them
    uint32_t key_channels = 0;           // host --sidechain
    // The model's values, seeded into the table before the host starts so it
    // instantiates with them (§5.1): param_slots values, then one bypass per
    // instance. Empty: zeros.
    std::vector<float> values;
    std::vector<float> bypass;
    // Per instance, a StateBlob from an earlier GetState to restore in Load
    // (host_control.h); empty or short: none.
    std::vector<std::string> states;
    // Load's flags (kLoadReactivate).
    uint32_t load_flags = 0;
};

// One running host and its channel region. Owned by a SupervisedChain.
class HostProcess {
public:
    ~HostProcess();
    HostProcess(const HostProcess&) = delete;
    HostProcess& operator=(const HostProcess&) = delete;

    plugin::ChannelHeader& header() { return *header_; }
    void* region() { return base_; }
    const plugin::Layout& layout() const { return layout_; }
    const plugin::ParamMap& map() const { return map_; }
    // chain + IPC block, through checked_latency, read after Ready. What the
    // plan builder puts in the InsertChain node for PDC (§7.3).
    uint32_t latency_frames() const { return latency_; }

    bool running();
    // Ready and its heartbeat has not moved for heartbeat_timeout_ms.
    bool hung(uint64_t now_ms);

    enum class Startup { Starting, Ready, Failed };
    // Never waits (code-reviewer L6 on PR #49): one look at the region and
    // the process. Ready once the host published Ready, with the latency
    // read through checked_latency. Failed, with `error` naming the plugin
    // and the reason, when it refused, faulted, exited, reported an
    // implausible latency or passed ready_timeout_ms; the host is then
    // killed and reaped. Ready and Failed are final: later calls repeat them.
    Startup poll_startup(uint64_t now_ms, std::string& error);
    // Failed in a way a restart cannot change: the region says Refused or
    // Faulted at load or validation, or the latency the host reported at
    // Ready is implausible. SupervisedChain reads it when a start fails, to
    // go down as a refusal instead of retrying (#72 review item 2).
    bool refused() const { return refused_; }
    // The instance a refusal named, or -1 (none, or not a refusal).
    int32_t refused_instance() const { return refused_instance_; }
    // Every block submitted has been returned (or the host is gone). For
    // the control thread and harnesses; never the audio thread's question.
    bool idle();
    // Shutdown on the socket and the shutdown word, a bounded wait, then
    // SIGKILL; reaps it.
    void stop();
    void kill();
    // stop() without the wait, for the control thread: Shutdown and the
    // word now; stopped() true once it has exited, killing it if it is
    // still running `grace_ms` after the request.
    void request_stop(uint64_t now_ms);
    bool stopped(uint64_t now_ms, uint64_t grace_ms = 2000);
    // The latency re-read from the region by contracts/plugin's rule
    // (generation, then the figure through checked_latency), after a
    // LatencyChanged. True when it moved; an implausible figure keeps the
    // old one.
    bool refresh_latency();
    // "exited with code N" / "killed by signal N", once it is not running.
    std::string exit_text();

    // ---- HostControl (host_control.h), control thread, never waits.

    // Sends what is queued and takes what has arrived, each message checked
    // against the contract. False when the host broke it (a message it may
    // not send, out of order, a Hello or Loaded that disagrees, an instance
    // or parameter not in its chain, a touch rate past M2, a message half
    // received for kStalledMessageMs, frames the engine cannot queue): the
    // host is then killed, and protocol_fault() says why. poll_startup()
    // calls it; a running host's owner calls it every poll.
    bool pump(uint64_t now_ms);
    const std::string& protocol_fault() const { return protocol_fault_; }
    // The host's Loaded, once Ready: per instance its latency, bypass and
    // its dense parameters' stable keys and ranges, against which a
    // client's PluginParameterChange{insert, key} resolves (§8).
    const plugin::control::Loaded& loaded() const { return loaded_; }
    // Checked host messages since the last call, in arrival order, encoded:
    // gestures, touches, editor and state replies, LatencyChanged, Fault;
    // then one GestureEnd per parameter whose gestures a burst coalesced
    // (M2). For the control server, which decodes them again (they decode).
    // The owner takes them every poll: what a host sends unasked is held up
    // to kMaxHeldMessages / kMaxHeldBytes, past which the host is a fault
    // (H1 on #78); replies are bounded by the requests outstanding.
    // What the control server still checks itself (L3 on #78): a
    // LatencyChanged or Reconfigured figure is re-read from the region,
    // which wins (refresh_latency); an EditorFailed's text and a Fault's
    // count are shown, not trusted.
    std::vector<std::string> take_messages();
    // Queues a message for the host. A request (GetState, DescribeValue,
    // OpenEditor, Reconfigure) is remembered with the reply type that
    // answers it, and only that reply is then accepted. False, with
    // `error`, for a request id already outstanding, or when the queue
    // would pass its cap: the caller then treats the host as not reading
    // its socket.
    bool send(const std::string& encoded, std::string& error);
    // For a poll set (M3 on #78): the engine's end of the socket, and
    // whether frames wait to be sent (poll it for POLLOUT then).
    int socket_handle() const;
    bool wants_write() const { return link_.wants_write(); }
    // GestureEnds a burst coalesced (M2): counted, never a kill.
    uint64_t gesture_ends_coalesced() const { return gesture_ends_coalesced_; }

    static constexpr uint32_t kMaxHeldMessages = 4096;
    static constexpr std::size_t kMaxHeldBytes = std::size_t{4} << 20;

private:
    friend class HostSupervisor;
    HostProcess() = default;
    bool fault(std::string why);
    bool on_message(std::string_view m, uint64_t now_ms);
    bool hold(std::string_view m, bool unsolicited);
    // M2: a token bucket per parameter for touches, refilled by wall time
    // and widened by a stalled pump; past it, a fault.
    uint32_t touch_slot(uint32_t instance, uint32_t param) const;
    bool touch_allowed(uint32_t instance, uint32_t param, uint64_t now_ms);
    // M2: GestureEnd past kMaxGestureEndsPerSecond is coalesced; past
    // gesture_kill_threshold() in one second, a fault.
    enum class GestureVerdict { Pass, Coalesced, Kill };
    GestureVerdict gesture_end(uint32_t instance, uint32_t param, float value, uint64_t now_ms);
    uint32_t gesture_kill_threshold() const;

    std::unique_ptr<platform::ControlChannel> ctl_;
    plugin::control::Link link_;
    bool ctl_closed_ = false;
    bool hello_ = false;
    bool loaded_seen_ = false;
    bool refused_seen_ = false;
    bool load_sent_ = false;
    plugin::control::Loaded loaded_;
    std::string protocol_fault_;
    struct Held {
        std::string bytes;
        bool unsolicited = false;
    };
    std::deque<Held> messages_;
    uint32_t held_unsolicited_ = 0;
    std::size_t held_unsolicited_bytes_ = 0;
    struct Outstanding {
        plugin::control::Type reply = plugin::control::Type::State;
        uint32_t instance = 0;
    };
    std::map<uint32_t, Outstanding> outstanding_;
    std::set<uint32_t> open_editors_;
    struct InstanceId {
        plugin::control::Format format = plugin::control::Format::Lv2;
        std::string id;
    };
    std::vector<InstanceId> instances_;
    // Touch buckets, one per (instance, dense parameter or bypass).
    std::vector<uint32_t> touch_base_;
    std::vector<double> touch_tokens_;
    std::vector<uint64_t> touch_ms_;
    double ge_tokens_ = 0;
    uint64_t ge_ms_ = 0;
    uint64_t ge_window_start_ms_ = 0;
    uint32_t ge_window_count_ = 0;
    std::map<std::pair<uint32_t, uint32_t>, float> coalesced_;
    uint64_t gesture_ends_coalesced_ = 0;
    uint64_t last_pump_ms_ = 0;
    uint64_t pump_stall_ms_ = 0;
    uint64_t refused_seen_ms_ = 0;
    std::vector<uint32_t> param_counts_;
    int32_t refused_instance_ = -1;
    std::string refused_text_;
    uint64_t stop_requested_ms_ = 0;

    std::unique_ptr<platform::SharedRegion> region_;
    std::unique_ptr<platform::ChildProcess> child_;
    void* base_ = nullptr;
    plugin::ChannelHeader* header_ = nullptr;
    plugin::Layout layout_;
    plugin::ParamMap map_;
    uint32_t latency_ = 0;
    uint64_t last_beat_ = 0;
    uint64_t last_beat_ms_ = 0;
    uint64_t heartbeat_timeout_ms_ = 0;
    // Start-up, for poll_startup().
    std::vector<std::string> plugins_;
    uint64_t ready_deadline_ms_ = 0;
    uint64_t ready_timeout_ms_ = 0;
    Startup startup_ = Startup::Starting;
    std::string startup_error_;
    bool refused_ = false;
};

class HostSupervisor {
public:
    // Creates the session's clock region (sealed, read-only for hosts) and
    // runs `host --version`. ok() is false, with why() saying so, when the
    // seal is not available (auditor L2) or the host does not speak this
    // engine's layout and clock versions (§5.5): plugin hosting is then
    // disabled for the run, loudly, and recording is untouched. Also when
    // the client's block_frames is not config.block_frames.
    HostSupervisor(const SupervisorConfig& config, PluginClient& client);
    // Teardown order: deactivate the backend, destroy every SupervisedChain
    // (after the plans naming their slots are retired), then this. It takes
    // the clock from the client with detach_clock() before unmapping it.
    ~HostSupervisor();
    HostSupervisor(const HostSupervisor&) = delete;
    HostSupervisor& operator=(const HostSupervisor&) = delete;

    bool ok() const { return ok_; }
    const std::string& why() const { return why_; }
    const SupervisorConfig& config() const { return config_; }
    PluginClient& client() { return client_; }
    // Regions the OS would not lock (auditor L3 on PR #45, M2 on PR #49),
    // the clock's included; reported, not fatal.
    uint64_t region_lock_failures() const { return lock_failures_; }
    bool clock_locked() const { return ok_ && !clock_unlocked_; }

    // A host for `spec`, spawned and NOT waited for: its region created,
    // seeded and locked, EngineHello and Load queued on its socket, the
    // process started. Ready (or the refusal, or the timeout,
    // ready_timeout_ms from now) is noticed with
    // HostProcess::poll_startup(). Null with `error` when it cannot even be
    // spawned. The states are checked first, before anything is copied or
    // started (M1 on #78): one over kMaxStateBytes names its instance in
    // `*refused_instance` (a refusal, not a crash: retrying cannot help),
    // and together over the Load cap they give "the chain's plugin states
    // exceed the 64 MiB Load cap" (host_control.h). What the control
    // server's thread calls (code-reviewer L6 on PR #49).
    // The Ready bound (L4 on #78): ready_timeout_ms from the moment the
    // whole Load has been sent, which covers the contract's Hello and Load
    // timeouts (10 s each) with one, stricter bound; before the Load has
    // left, the same bound from the spawn (a host not reading its socket).
    std::unique_ptr<HostProcess> spawn(const ChainSpec& spec, std::string& error,
                                       int32_t* refused_instance = nullptr);
    // spawn(), then wait for poll_startup() to settle: BLOCKS the caller up
    // to ready_timeout_ms. For harnesses and tools, never the control
    // server's thread. Null with `error` naming the plugin and the reason
    // (refusal, exit code, timeout) when it does not get to Ready.
    std::unique_ptr<HostProcess> start(const ChainSpec& spec, std::string& error);

private:
    SupervisorConfig config_;
    PluginClient& client_;
    std::unique_ptr<platform::SharedRegion> clock_region_;
    bool ok_ = false;
    std::string why_;
    uint64_t lock_failures_ = 0;
    bool clock_unlocked_ = false;
};

// One insert chain under supervision: its host, its channel slot, and the
// restart policy of §5.3. Control thread only.
class SupervisedChain {
public:
    enum class Event {
        None,
        // A host is Ready and bound, the first or a replacement: put node()
        // in a new plan, then call retired() once the plans naming the old
        // slot (if any) are retired.
        Swapped,
        // The host was killed (exited, hung, removed) and a replacement is
        // starting; the old slot stays in the plan, silent. why() says why.
        Restarting,
        // The first start failed, the plugin was refused, or restarts are
        // exhausted (3 in 60 s): the path stays silent; why() says so.
        Down,
    };

    SupervisedChain(HostSupervisor& supervisor, ChainSpec spec, uint32_t param_store, uint32_t node_state);
    ~SupervisedChain();
    SupervisedChain(const SupervisedChain&) = delete;
    SupervisedChain& operator=(const SupervisedChain&) = delete;

    // The first host, spawned and not waited for: resets the store from
    // spec.values and the node state, starts the host, and returns. poll()
    // reports Swapped when it is Ready and bound, or Down. False, with
    // `error`, only when the host could not be spawned at all.
    bool begin(std::string& error);
    // begin(), then poll() until it settles: BLOCKS up to the Ready bound.
    // For harnesses, never the control server's thread. False with `error`
    // (= why()) when the chain went down instead.
    bool start(std::string& error);
    // A host is spawned and not yet Ready (the first, or a replacement).
    bool starting() const { return starting_ != nullptr; }
    // The bound host has returned every block it was given, or there is
    // none. For harnesses and the control thread (rt-engineer item 5 on
    // #59), in place of reading the region's header.
    bool idle();

    // What the plan's InsertChain node carries now (§7.1).
    InsertChainRef node() const;
    uint32_t latency_frames() const;
    HostProcess* host() { return current_.get(); }

    // Every 50 ms or so; never waits. A host starting is looked at once
    // (Swapped when it is Ready and bound, Down when it failed). A bound
    // host that exited, hung, or whose slot the audio thread removed after
    // 8 misses is killed, and if the policy allows a replacement is spawned
    // (Restarting) while the old slot stays in the plan (make-before-break);
    // its Ready is a later Swapped. `now_ms` is a steady clock.
    Event poll(uint64_t now_ms);
    // The plan that named the previous slot has been retired (ADR-0009
    // §4.1): its host is stopped, its region released, its slot freed.
    void retired();

    // The checked messages (HostProcess::take_messages) of the chain's
    // hosts, for the control server, which must take them every poll: those
    // of hosts already let go first (L2 on #78), then the retiring one's,
    // then the bound one's.
    std::vector<std::string> take_host_messages();
    // The bound host's latency re-read after a LatencyChanged; true when it
    // moved (the caller builds a plan with it, §5.4).
    bool refresh_latency();
    // For a chain being let go, after every plan naming its slots has been
    // retired: every host asked to stop without waiting, and stopped() true
    // once all have exited (killed past the grace). The destructor then
    // waits for nothing.
    void request_stop(uint64_t now_ms);
    bool stopped(uint64_t now_ms);
    // A replacement is starting while the old host's slot stays bound.
    bool restarting() const { return starting_ != nullptr && current_ != nullptr; }
    // Down because the plugin refused (deterministic; ADR-0012 amendment of
    // 2026-10-14, change C: the control server leaves that instance out and
    // starts the chain without it), and which instance, or -1.
    bool refused() const { return refused_; }
    int32_t refused_instance() const { return refused_instance_; }

    uint32_t restarts() const { return restarts_total_; }
    // The chain index named for the last removal or hang: the instance the
    // host reported running (checked_instance) most often in this thread's
    // samples while the slot had a miss streak, the removal's own sample
    // included (PR #26 finding 6). -1 when none was seen.
    int32_t culprit() const { return culprit_; }
    bool down() const { return down_; }
    const std::string& why() const { return why_; }

private:
    // Kills the bound host and spawns its replacement into starting_:
    // Restarting, or Down when the policy or the spawn says no.
    Event replace(uint64_t now_ms, const std::string& reason);
    // starting_ looked at once.
    Event poll_starting(uint64_t now_ms);
    bool bind(std::unique_ptr<HostProcess> host);

    HostSupervisor& sup_;
    ChainSpec spec_;
    uint32_t store_;
    uint32_t node_;
    std::unique_ptr<HostProcess> current_;
    std::unique_ptr<HostProcess> starting_;
    uint32_t slot_ = kNoSlot;
    uint32_t generation_ = 0;
    std::unique_ptr<HostProcess> retiring_;
    uint32_t retiring_slot_ = kNoSlot;
    std::deque<uint64_t> restart_times_;
    uint64_t streak_samples_[plugin::kMaxInstances] = {};
    int32_t culprit_ = -1;
    uint32_t restarts_total_ = 0;
    bool down_ = false;
    bool refused_ = false;
    int32_t refused_instance_ = -1;
    std::string why_;
    // Messages of hosts let go, kept for take_host_messages (L2 on #78).
    std::vector<std::string> orphans_;
};

}  // namespace sonora::plugin_client
