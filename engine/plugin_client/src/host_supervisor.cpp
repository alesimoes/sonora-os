// SPDX-License-Identifier: GPL-3.0-or-later
// The control thread's supervisor of plugin hosts (host_supervisor.h). Not
// for the audio thread: it starts processes, allocates and waits.
#include "sonora/plugin_client/host_supervisor.h"

#include "sonora/platform/ipc.h"
#include "sonora/platform/process.h"
#include "sonora/platform/wake.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>

namespace sonora::plugin_client {

namespace {

uint64_t steady_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

const char* refusal_text(plugin::Refusal r) {
    switch (r) {
    case plugin::Refusal::None: return "faulted";
    case plugin::Refusal::NotFound: return "not installed";
    case plugin::Refusal::ChannelMismatch: return "cannot run at this channel count";
    case plugin::Refusal::UnsupportedFeature: return "needs a feature the host lacks";
    case plugin::Refusal::UnsupportedPort: return "has a port the host cannot connect";
    case plugin::Refusal::InstantiateFailed: return "failed to instantiate";
    case plugin::Refusal::WroteOutsideBuffers: return "wrote outside its buffers";
    case plugin::Refusal::NonFiniteOutput: return "produced NaN or Inf from silence";
    case plugin::Refusal::TooManyInstances: return "too many plugins in the chain";
    case plugin::Refusal::ParamMismatch: return "has another parameter list than the scan saw";
    }
    return "refused";
}

std::string join(const std::vector<std::string>& v, char sep) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : std::string(1, sep)) + x;
    return s;
}

}  // namespace

// ---- HostProcess

HostProcess::~HostProcess() { stop(); }

bool HostProcess::running() { return child_ && child_->running(); }

bool HostProcess::hung(uint64_t now_ms) {
    if (plugin::checked_state(header_->state.load(std::memory_order_acquire)) != plugin::HostState::Ready)
        return false;
    const uint64_t beat = header_->heartbeat.load(std::memory_order_relaxed);
    if (beat != last_beat_) {
        last_beat_ = beat;
        last_beat_ms_ = now_ms;
        return false;
    }
    return now_ms - last_beat_ms_ > heartbeat_timeout_ms_;
}

void HostProcess::stop() {
    if (!child_ || !child_->running()) return;
    header_->shutdown.store(1, std::memory_order_release);
    // A timer-paced host sees it within its stale sleep (10 ms); one in a
    // plugin that never returns does not, hence the bound.
    if (!child_->wait(2000)) {
        child_->kill();
        child_->wait(-1);
    }
}

void HostProcess::kill() {
    if (!child_ || !child_->running()) return;
    child_->kill();
    child_->wait(-1);
}

HostProcess::Startup HostProcess::poll_startup(uint64_t now_ms, std::string& error) {
    if (startup_ != Startup::Starting) {
        error = startup_error_;
        return startup_;
    }
    const auto fail = [&](std::string why, bool refused) {
        kill();
        startup_ = Startup::Failed;
        startup_error_ = std::move(why);
        refused_ = refused;
        error = startup_error_;
        return startup_;
    };
    const uint32_t n = uint32_t(plugins_.size());
    std::string name;
    for (const std::string& p : plugins_) name += (name.empty() ? "" : ",") + p;
    plugin::ChannelHeader& h = *header_;
    const plugin::HostState state = plugin::checked_state(h.state.load(std::memory_order_acquire));
    if (state == plugin::HostState::Refused || state == plugin::HostState::Faulted) {
        const auto why = static_cast<plugin::Refusal>(h.refusal.load(std::memory_order_relaxed));
        const int32_t at = plugin::checked_instance(h.refusal_instance.load(std::memory_order_relaxed), n);
        return fail((at >= 0 ? plugins_[uint32_t(at)] : name) + ": " +
                        (why <= plugin::Refusal::ParamMismatch ? refusal_text(why) : "refused"),
                    true);
    }
    if (state == plugin::HostState::Ready) {
        // Contracts/plugin, "Latency": generation (acquire) before the figure.
        h.latency_generation.load(std::memory_order_acquire);
        latency_ = plugin::checked_latency(h.latency_frames.load(std::memory_order_relaxed));
        if (latency_ == plugin::kLatencyInvalid) return fail(name + ": reported an implausible latency", true);
        last_beat_ = h.heartbeat.load(std::memory_order_relaxed);
        last_beat_ms_ = now_ms;
        startup_ = Startup::Ready;
        return startup_;
    }
    if (!running()) return fail(name + ": the host " + exit_text() + " before it was ready", false);
    if (now_ms > ready_deadline_ms_) {
        return fail(name + ": not ready within " + std::to_string(ready_timeout_ms_) + " ms", false);
    }
    return Startup::Starting;
}

bool HostProcess::idle() {
    if (!running() || !header_) return true;
    return header_->completed_block.load(std::memory_order_acquire) ==
           header_->submitted_block.load(std::memory_order_acquire);
}

std::string HostProcess::exit_text() {
    if (!child_) return "never started";
    if (child_->running()) return "still running";
    if (child_->exited_normally()) return "exited with code " + std::to_string(child_->exit_code());
    return "killed by signal " + std::to_string(child_->signal_number());
}

// ---- HostSupervisor

HostSupervisor::HostSupervisor(const SupervisorConfig& config, PluginClient& client)
    : config_(config), client_(client) {
    std::string error;
    // The host paces on the clock's quantum and the client submits its own
    // block_frames; if they differ, every block faults the host (code-reviewer
    // L4 on PR #49).
    if (client.config().block_frames != config.block_frames) {
        why_ = "plugin hosting is disabled: the client's block is " + std::to_string(client.config().block_frames) +
               " frames and the supervisor's " + std::to_string(config.block_frames);
        return;
    }
    clock_region_ = platform::SharedRegion::create("sonora-plugin-clock", sizeof(plugin::ClockHeader), error);
    if (!clock_region_) {
        why_ = "plugin hosting is disabled: cannot create the session clock: " + error;
        return;
    }
    if (!clock_region_->sealed_against_writes()) {
        // Auditor L2: without the seal a host can reopen the clock writable
        // and forge another host's tick; there is no degraded mode.
        why_ = "plugin hosting is disabled: this kernel cannot seal the session clock against writable "
               "mappings (F_SEAL_FUTURE_WRITE, Linux 5.1)";
        return;
    }
    plugin::ClockHeader* clock = plugin::init_clock(clock_region_->writable_data(), config.sample_rate,
                                                    config.block_frames, plugin::default_wake_mode());
    if (!plugin::clock_valid(*clock, clock_region_->bytes())) {
        why_ = "plugin hosting is disabled: the session's rate and quantum make no valid clock";
        return;
    }
    // The audio thread writes it every callback: faulted in by init_clock,
    // kept in here, a refusal counted (auditor M2 on PR #49).
    std::string lock_error;
    if (!clock_region_->lock(lock_error)) {
        clock_unlocked_ = true;
        ++lock_failures_;
    }

    // §5.5: checked at start-up, not first use. The capture file's name is
    // not guessable, so nothing can be planted for the host's O_TRUNC.
    const std::string out = config.scratch_dir + "/sonora-plugin-host-version-" +
                            std::to_string(platform::current_process_id()) + "-" +
                            std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()) +
                            ".txt";
    platform::ChildProcess::Options o;
    o.stdout_path = out;
    auto child = platform::ChildProcess::spawn({config.host_path, "--version"}, o, error);
    if (!child) {
        why_ = "plugin hosting is disabled: " + error;
        return;
    }
    // A host that prints its line and then hangs is not one we accept: it
    // is killed, reaped, and the timeout is the failure (code-reviewer L3).
    const bool exited = child->wait(5000);
    if (!exited) {
        child->kill();
        child->wait(-1);
    }
    std::string line;
    {
        std::ifstream f(out);
        std::getline(f, line);
    }
    std::remove(out.c_str());
    uint32_t layout = 0, clock_version = 0;
    if (!exited) {
        why_ = "plugin hosting is disabled: " + config.host_path + " --version did not exit within 5 s";
        return;
    }
    if (!child->exited_normally() || child->exit_code() != 0 ||
        !plugin::parse_version_line(line.c_str(), layout, clock_version)) {
        why_ = "plugin hosting is disabled: " + config.host_path + " --version did not answer (" + line + ")";
        return;
    }
    if (layout != plugin::kLayoutVersion || clock_version != plugin::kClockLayoutVersion) {
        why_ = "plugin hosting is disabled: " + config.host_path + " speaks layout " + std::to_string(layout) +
               " clock " + std::to_string(clock_version) + "; this engine speaks layout " +
               std::to_string(plugin::kLayoutVersion) + " clock " + std::to_string(plugin::kClockLayoutVersion);
        return;
    }
    client_.attach_clock(clock);
    ok_ = true;
}

HostSupervisor::~HostSupervisor() {
    // Teardown order (code-reviewer M1, auditor L3 on PR #49): the backend
    // is deactivated first, so no callback runs and this returns at once.
    // If one is still inside, detach_clock waits for it to leave; if it
    // never does, the clock stays mapped (leaked) rather than unmapped
    // under the audio thread.
    if (!client_.detach_clock()) (void)clock_region_.release();
}

std::unique_ptr<HostProcess> HostSupervisor::spawn(const ChainSpec& spec, std::string& error) {
    const std::string name = join(spec.plugins, ',');
    if (!ok_) {
        error = why_;
        return nullptr;
    }
    const uint32_t n = uint32_t(spec.plugins.size());
    if (n == 0 || n > plugin::kMaxInstances || spec.param_counts.size() != n) {
        error = name + ": a chain is 1 to 16 plugins, each with its parameter count";
        return nullptr;
    }
    const uint32_t channels = spec.channels + spec.key_channels;
    if (spec.channels == 0 || channels > client_.config().max_chain_channels) {
        error = name + ": " + std::to_string(channels) + " channels is more than a chain carries";
        return nullptr;
    }

    std::unique_ptr<HostProcess> host(new HostProcess());
    host->heartbeat_timeout_ms_ = config_.heartbeat_timeout_ms;
    host->map_ = plugin::make_param_map(spec.param_counts.data(), n);
    if (host->map_.param_slots > client_.config().store_slots) {
        error = name + ": more parameters than a store holds";
        return nullptr;
    }
    host->layout_ = plugin::make_layout(channels, config_.max_frames, host->map_.param_slots);
    // A NEW region for every host, never reused (§5.3).
    host->region_ = platform::SharedRegion::create_shared_writable("sonora-plugin-channel",
                                                                   host->layout_.region_size, error);
    if (!host->region_) return nullptr;
    host->base_ = host->region_->writable_data();
    // A2 condition 4 and auditor L3: faulted in, and kept in when the OS
    // lets us; a refusal is counted and reported, never silent.
    plugin::prefault(host->base_, host->region_->bytes());
    std::string lock_error;
    if (!host->region_->lock(lock_error)) ++lock_failures_;
    host->header_ = plugin::init_header(host->base_, host->layout_, config_.sample_rate, config_.block_frames, n,
                                        spec.param_counts.data(), config_.host_rt_priority);
    std::atomic<float>* table = plugin::value_table(host->base_, host->layout_);
    for (uint32_t s = 0; s < host->map_.param_slots; ++s)
        plugin::seed_value(table, s, s < spec.values.size() ? spec.values[s] : 0.f);
    for (uint32_t i = 0; i < n && i < spec.bypass.size(); ++i)
        host->header_->bypass[i].store(spec.bypass[i], std::memory_order_relaxed);

    std::vector<std::string> argv = {config_.host_path, "--shm-fd", "3", "--clock-fd", "4", "--chain", name,
                                     "--engine-pid", std::to_string(platform::current_process_id())};
    for (const std::string& b : config_.bundles) {
        argv.push_back("--bundle");
        argv.push_back(b);
    }
    if (spec.key_channels) {
        argv.push_back("--sidechain");
        argv.push_back(std::to_string(spec.key_channels));
    }
    argv.insert(argv.end(), spec.extra_args.begin(), spec.extra_args.end());
    platform::ChildProcess::Options o;
    o.regions = {{3, host->region_.get(), true}, {4, clock_region_.get(), false}};
    host->child_ = platform::ChildProcess::spawn(argv, o, error);
    if (!host->child_) return nullptr;
    // Ready within the bound (§5.3), noticed by poll_startup().
    host->plugins_ = spec.plugins;
    host->ready_timeout_ms_ = config_.ready_timeout_ms;
    host->ready_deadline_ms_ = steady_ms() + config_.ready_timeout_ms;
    return host;
}

std::unique_ptr<HostProcess> HostSupervisor::start(const ChainSpec& spec, std::string& error) {
    std::unique_ptr<HostProcess> host = spawn(spec, error);
    if (!host) return nullptr;
    plugin::ChannelHeader& h = host->header();
    for (;;) {
        // done_word before the look, so a change in between ends the wait.
        const uint32_t seen = h.done_word.load(std::memory_order_acquire);
        switch (host->poll_startup(steady_ms(), error)) {
            case HostProcess::Startup::Ready: return host;
            case HostProcess::Startup::Failed: return nullptr;
            case HostProcess::Startup::Starting: break;
        }
        platform::wait_for_change(h.done_word, seen, 20'000'000);
    }
}

// ---- SupervisedChain

SupervisedChain::SupervisedChain(HostSupervisor& supervisor, ChainSpec spec, uint32_t param_store,
                                 uint32_t node_state)
    : sup_(supervisor), spec_(std::move(spec)), store_(param_store), node_(node_state) {}

SupervisedChain::~SupervisedChain() {
    // The caller has retired every plan naming either slot. Each slot is
    // retired (a new generation) before its region is released.
    retired();
    // A host still starting was never bound: no plan names it.
    if (starting_) starting_->kill();
    starting_.reset();
    if (slot_ != kNoSlot) sup_.client().retire(slot_);
    if (current_) current_->stop();
    current_.reset();
}

bool SupervisedChain::begin(std::string& error) {
    const plugin::ParamMap map = plugin::make_param_map(spec_.param_counts.data(), uint32_t(spec_.param_counts.size()));
    std::vector<float> values(map.param_slots, 0.f), bypass(plugin::kMaxInstances, 0.f);
    for (std::size_t i = 0; i < values.size() && i < spec_.values.size(); ++i) values[i] = spec_.values[i];
    for (std::size_t i = 0; i < bypass.size() && i < spec_.bypass.size(); ++i) bypass[i] = spec_.bypass[i];
    sup_.client().reset_store(store_, map, values.data(), bypass.data());
    sup_.client().reset_node(node_);
    starting_ = sup_.spawn(spec_, error);
    if (!starting_) {
        down_ = true;
        why_ = error;
        return false;
    }
    return true;
}

bool SupervisedChain::start(std::string& error) {
    if (!begin(error)) return false;
    while (starting_) {
        HostProcess& h = *starting_;
        const uint32_t seen = h.header().done_word.load(std::memory_order_acquire);
        const Event e = poll_starting(steady_ms());
        if (e == Event::Down) {
            error = why_;
            return false;
        }
        if (e == Event::Swapped) return true;
        platform::wait_for_change(h.header().done_word, seen, 20'000'000);
    }
    return true;
}

bool SupervisedChain::idle() { return !current_ || current_->idle(); }

SupervisedChain::Event SupervisedChain::poll_starting(uint64_t now_ms) {
    std::string error;
    switch (starting_->poll_startup(now_ms, error)) {
        case HostProcess::Startup::Starting: return Event::None;
        case HostProcess::Startup::Failed: {
            const bool restart = current_ != nullptr;
            starting_.reset();
            down_ = true;
            // A refusal is the plugin's answer, not an accident: it is
            // reported, never retried.
            why_ = restart ? join(spec_.plugins, ',') + ": " + why_ + "; the restart failed: " + error : error;
            return Event::Down;
        }
        case HostProcess::Startup::Ready: break;
    }
    // Make-before-break: the replaced host's slot stays named by the
    // playing plan until the caller has swapped and called retired().
    if (current_) {
        retiring_ = std::move(current_);
        retiring_slot_ = slot_;
    }
    if (!bind(std::move(starting_))) {
        starting_.reset();
        if (retiring_) {
            current_ = std::move(retiring_);
            retiring_slot_ = kNoSlot;
        }
        down_ = true;
        why_ = join(spec_.plugins, ',') + ": no free channel slot";
        return Event::Down;
    }
    return Event::Swapped;
}

bool SupervisedChain::bind(std::unique_ptr<HostProcess> host) {
    const uint32_t slot = sup_.client().acquire_slot();
    if (slot == kNoSlot) return false;
    ChannelBinding b;
    b.region = host->region();
    b.layout = host->layout();
    b.map = host->map();
    b.latency_frames = host->latency_frames();
    generation_ = sup_.client().assign(slot, b);
    slot_ = slot;
    current_ = std::move(host);
    return true;
}

InsertChainRef SupervisedChain::node() const {
    InsertChainRef r;
    r.channel_slot = slot_;
    r.channel_generation = generation_;
    r.param_store = store_;
    r.node_state = node_;
    r.channels = spec_.channels + spec_.key_channels;
    return r;
}

uint32_t SupervisedChain::latency_frames() const { return current_ ? current_->latency_frames() : 0; }

SupervisedChain::Event SupervisedChain::poll(uint64_t now_ms) {
    if (down_) return Event::None;
    if (starting_) return poll_starting(now_ms);
    if (!current_ || retiring_) return Event::None;
    const SlotStatus st = sup_.client().slot_status(slot_);
    // The culprit is sampled here, on the control thread, while a streak
    // runs: current_instance is a control-thread word (auditor L1 on #49).
    const bool removed = st.removed;
    if (st.streak > 0 || removed) {
        const int32_t running = plugin::checked_instance(
            current_->header().current_instance.load(std::memory_order_relaxed), uint32_t(spec_.plugins.size()));
        if (running >= 0) ++streak_samples_[running];
    } else {
        for (uint64_t& n : streak_samples_) n = 0;
    }
    std::string reason;
    if (!current_->running()) {
        reason = "the host " + current_->exit_text();
    } else if (removed) {
        reason = "removed after " + std::to_string(kMaxConsecutiveMisses) + " missed blocks";
    } else if (current_->hung(now_ms)) {
        reason = "the host stopped answering (heartbeat)";
    } else {
        return Event::None;
    }
    culprit_ = -1;
    uint64_t most = 0;
    for (uint32_t i = 0; i < plugin::kMaxInstances; ++i)
        if (streak_samples_[i] > most) most = streak_samples_[i], culprit_ = int32_t(i);
    for (uint64_t& n : streak_samples_) n = 0;
    if (culprit_ >= 0 && uint32_t(culprit_) < spec_.plugins.size())
        reason += ", " + spec_.plugins[uint32_t(culprit_)] + " running";
    return replace(now_ms, reason);
}

SupervisedChain::Event SupervisedChain::replace(uint64_t now_ms, const std::string& reason) {
    // §3 and §4: a removed or hung host is killed; its region stays mapped
    // until the plan naming its slot is retired.
    current_->kill();
    while (!restart_times_.empty() && now_ms - restart_times_.front() > sup_.config().restart_window_ms)
        restart_times_.pop_front();
    if (restart_times_.size() >= sup_.config().max_restarts) {
        down_ = true;
        why_ = join(spec_.plugins, ',') + ": " + reason + "; " + std::to_string(sup_.config().max_restarts) +
               " restarts in " + std::to_string(sup_.config().restart_window_ms / 1000) +
               " s, so the chain stays down (retry, make inactive or remove)";
        return Event::Down;
    }
    restart_times_.push_back(now_ms);
    ++restarts_total_;
    // Start-new-before-stop-old: the old slot stays in the plan, silent,
    // until the new host is Ready and the caller swaps (§5.3). Parameters
    // come from the store, copied in by the new channel's first submit.
    // Spawned, not waited for: its Ready is a later poll's Swapped.
    std::string error;
    starting_ = sup_.spawn(spec_, error);
    if (!starting_) {
        down_ = true;
        why_ = join(spec_.plugins, ',') + ": " + reason + "; the restart failed: " + error;
        return Event::Down;
    }
    why_ = reason;
    return Event::Restarting;
}

void SupervisedChain::retired() {
    if (!retiring_) return;
    // The slot first: its generation moves, so even a plan retired out of
    // order cannot reach the region released below (code-reviewer L5).
    sup_.client().retire(retiring_slot_);
    retiring_slot_ = kNoSlot;
    retiring_->stop();
    retiring_.reset();  // the region is released only now (§5.3)
}

}  // namespace sonora::plugin_client
