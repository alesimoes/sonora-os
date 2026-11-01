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
#include <thread>

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

HostProcess::~HostProcess() {
    stop();
    if (registry_) registry_->live.erase(this);
}

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
    // Shutdown on the socket (host_control.h, "Shutdown") and the region's
    // word, for a host whose main thread is not reading.
    if (ctl_ && !ctl_closed_) {
        std::string bytes, error;
        plugin::control::encode(plugin::control::Shutdown{}, bytes);
        if (link_.queue(bytes, error)) pump(steady_ms());
    }
    header_->shutdown.store(1, std::memory_order_release);
    // A timer-paced host sees it within its stale sleep (10 ms); one in a
    // plugin that never returns does not, hence the bound.
    if (!child_->wait(2000)) {
        child_->kill();
        child_->wait(-1);
    }
}

void HostProcess::request_stop(uint64_t now_ms) {
    if (stop_requested_ms_ != 0 || !child_ || !child_->running()) return;
    stop_requested_ms_ = now_ms ? now_ms : 1;
    if (ctl_ && !ctl_closed_) {
        std::string bytes, error;
        plugin::control::encode(plugin::control::Shutdown{}, bytes);
        if (link_.queue(bytes, error)) pump(now_ms);
    }
    header_->shutdown.store(1, std::memory_order_release);
}

bool HostProcess::stopped(uint64_t now_ms, uint64_t grace_ms) {
    if (!child_ || !child_->running()) return true;
    pump(now_ms);
    // The signal only: running() reaps it on a later call, so a child slow
    // to die (D state on a slow stick) never holds the control thread (M5
    // on #80).
    if (stop_requested_ms_ != 0 && now_ms - stop_requested_ms_ >= grace_ms) child_->kill();
    return !child_->running();
}

void HostProcess::signal_kill() {
    if (child_ && child_->running()) child_->kill();
}

bool HostProcess::refresh_latency() {
    if (!header_) return false;
    header_->latency_generation.load(std::memory_order_acquire);
    const uint32_t l = plugin::checked_latency(header_->latency_frames.load(std::memory_order_relaxed));
    if (l == plugin::kLatencyInvalid || l == latency_) return false;
    latency_ = l;
    return true;
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
        signal_kill();
        startup_ = Startup::Failed;
        startup_error_ = std::move(why);
        refused_ = refused;
        error = startup_error_;
        return startup_;
    };
    if (ready_deadline_ms_ == 0) ready_deadline_ms_ = now_ms + ready_timeout_ms_;
    const uint32_t n = uint32_t(plugins_.size());
    std::string name;
    for (const std::string& p : plugins_) name += (name.empty() ? "" : ",") + p;
    // The socket first: a Hello or Loaded that disagrees is a broken
    // contract, never a refusal.
    if (!pump(now_ms)) return fail(name + ": " + protocol_fault_, false);
    plugin::ChannelHeader& h = *header_;
    const plugin::HostState state = plugin::checked_state(h.state.load(std::memory_order_acquire));
    if (state == plugin::HostState::Refused || state == plugin::HostState::Faulted) {
        // The region says it first; the Refused message with its text
        // follows (host_control.h, "Ordering"). Give the text a moment
        // rather than lose it (L1 on #78): a host that exited has written
        // it already, one still running gets 200 ms.
        if (refused_text_.empty() && !refused_seen_) {
            if (refused_seen_ms_ == 0) refused_seen_ms_ = now_ms ? now_ms : 1;
            if (!running()) pump(now_ms);
            else if (now_ms - refused_seen_ms_ < 200) return Startup::Starting;
        }
        const auto why = static_cast<plugin::Refusal>(h.refusal.load(std::memory_order_relaxed));
        const int32_t at = plugin::checked_instance(h.refusal_instance.load(std::memory_order_relaxed), n);
        refused_instance_ = at;
        std::string text = (at >= 0 ? plugins_[uint32_t(at)] : name) + ": " +
                           (why <= plugin::Refusal::ParamMismatch ? refusal_text(why) : "refused");
        if (!refused_text_.empty()) text += " (" + refused_text_ + ")";
        return fail(text, true);
    }
    // Ready is the region's word, and Loaded (sent after it) carries the key
    // tables: both, before the chain is bound (host_control.h, "Ordering").
    if (state == plugin::HostState::Ready && loaded_seen_) {
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

// ---- HostProcess: HostControl (host_control.h)

namespace {

plugin::control::Io io_of(platform::IoStatus s) {
    switch (s) {
        case platform::IoStatus::Ok: return plugin::control::Io::Ok;
        case platform::IoStatus::WouldBlock: return plugin::control::Io::WouldBlock;
        case platform::IoStatus::Closed: return plugin::control::Io::Closed;
        case platform::IoStatus::Error: return plugin::control::Io::Error;
    }
    return plugin::control::Io::Error;
}

}  // namespace

bool HostProcess::fault(std::string why) {
    if (protocol_fault_.empty()) protocol_fault_ = "protocol fault: " + why;
    ctl_closed_ = true;
    // The signal only: pump() runs on the control thread, and a child in D
    // state would hold it in waitpid. Its owner reaps it.
    signal_kill();
    return false;
}

uint32_t HostProcess::touch_slot(uint32_t instance, uint32_t param) const {
    // One slot per dense parameter and one for the bypass, per instance,
    // laid out at spawn: bounded by the chain, never grown by a host.
    if (instance >= param_counts_.size()) return UINT32_MAX;
    const uint32_t base = touch_base_[instance];
    if (param == plugin::kBypassParam) return base + param_counts_[instance];
    return param < param_counts_[instance] ? base + param : UINT32_MAX;
}

bool HostProcess::touch_allowed(uint32_t instance, uint32_t param, uint64_t now_ms) {
    // M2 (rt-engineer on #78): a token bucket per parameter, refilled by
    // wall time. Its capacity is the contract's per-second limit plus what
    // the time since the last pump would have refilled, so the backlog of a
    // control thread that stalled arrives whole and is not taken for a flood.
    const uint32_t s = touch_slot(instance, param);
    if (s == UINT32_MAX) return false;
    const double rate = plugin::control::kMaxTouchesPerSecondPerParam;
    const double cap = rate + rate * double(pump_stall_ms_) / 1000.0;
    double tokens = touch_tokens_[s] + rate * double(now_ms - touch_ms_[s]) / 1000.0;
    if (touch_ms_[s] == 0) tokens = cap;
    tokens = std::min(tokens, cap);
    touch_ms_[s] = now_ms;
    if (tokens < 1.0) {
        touch_tokens_[s] = tokens;
        return false;
    }
    touch_tokens_[s] = tokens - 1.0;
    return true;
}

HostProcess::GestureVerdict HostProcess::gesture_end(uint32_t instance, uint32_t param, float value,
                                                     uint64_t now_ms) {
    namespace pc = plugin::control;
    // Past the human-rate kill threshold in one second: a host gone wrong.
    if (now_ms - ge_window_start_ms_ >= 1000) {
        ge_window_start_ms_ = now_ms;
        ge_window_count_ = 0;
    }
    if (++ge_window_count_ > gesture_kill_threshold()) return GestureVerdict::Kill;
    // Within kMaxGestureEndsPerSecond: passed on. Past it (a preset change
    // in the plugin's own editor ends a gesture per parameter at once):
    // coalesced, the newest value per parameter, handed over by
    // take_messages() (M2, rt-engineer on #78). Never a kill for that.
    const double rate = pc::kMaxGestureEndsPerSecond;
    double tokens = ge_tokens_ + rate * double(now_ms - ge_ms_) / 1000.0;
    if (ge_ms_ == 0) tokens = rate;
    tokens = std::min(tokens, rate + rate * double(pump_stall_ms_) / 1000.0);
    ge_ms_ = now_ms;
    if (tokens >= 1.0) {
        ge_tokens_ = tokens - 1.0;
        return GestureVerdict::Pass;
    }
    ge_tokens_ = tokens;
    coalesced_[{instance, param}] = value;
    ++gesture_ends_coalesced_;
    return GestureVerdict::Coalesced;
}

uint32_t HostProcess::gesture_kill_threshold() const {
    // Ten times the human limit, or four whole preset changes (every slot
    // of the chain) in one second, whichever is more.
    uint32_t slots = 0;
    for (const uint32_t c : param_counts_) slots += c + 1;
    return std::max(10 * plugin::control::kMaxGestureEndsPerSecond, 4 * slots);
}

bool HostProcess::on_message(std::string_view m, uint64_t now_ms) {
    namespace pc = plugin::control;
    pc::Type t{};
    std::string e;
    // Untrusted (host_control.h): only what a host may send, each decoded
    // whole and checked against the chain the engine built.
    if (!pc::peek_type(m, t) || !pc::host_to_engine(t)) return fault("a message type no host sends");
    if (!hello_ && t != pc::Type::Hello) return fault("a message before Hello");
    const auto in_chain = [this](uint32_t instance, uint32_t param) {
        return pc::param_in_chain(map_, instance, param) ||
               (instance < param_counts_.size() && param < param_counts_[instance]);
    };
    switch (t) {
        case pc::Type::Hello: {
            pc::Hello h;
            if (!pc::decode(m, h, e)) return fault("Hello: " + e);
            if (hello_) return fault("a second Hello");
            if (h.wire_version != pc::kHostWireVersion || h.layout_version != plugin::kLayoutVersion ||
                h.clock_version != plugin::kClockLayoutVersion) {
                return fault("Hello names wire " + std::to_string(h.wire_version) + " layout " +
                             std::to_string(h.layout_version) + " clock " + std::to_string(h.clock_version) +
                             ", not this engine's: the host binary changed since start-up");
            }
            if (!child_ || int64_t(h.pid) != child_->pid()) {
                return fault("Hello names pid " + std::to_string(h.pid) + ", not the host the engine started");
            }
            hello_ = true;
            return true;
        }
        case pc::Type::Loaded: {
            pc::Loaded l;
            if (!pc::decode(m, l, e)) return fault("Loaded: " + e);
            if (loaded_seen_) return fault("a second Loaded");
            if (l.instances.size() != param_counts_.size()) {
                return fault("Loaded lists " + std::to_string(l.instances.size()) + " instances for a chain of " +
                             std::to_string(param_counts_.size()));
            }
            for (std::size_t i = 0; i < l.instances.size(); ++i) {
                if (l.instances[i].params.size() != param_counts_[i]) {
                    return fault("Loaded instance " + std::to_string(i) + " has " +
                                 std::to_string(l.instances[i].params.size()) + " parameters; the scan saw " +
                                 std::to_string(param_counts_[i]));
                }
                if (plugin::checked_latency(l.instances[i].latency_frames) == plugin::kLatencyInvalid)
                    return fault("Loaded instance " + std::to_string(i) + " reports an implausible latency");
            }
            if (plugin::checked_latency(l.latency_frames) == plugin::kLatencyInvalid)
                return fault("Loaded reports an implausible latency");
            loaded_ = std::move(l);
            loaded_seen_ = true;
            return true;
        }
        case pc::Type::Refused: {
            pc::Refused r;
            if (!pc::decode(m, r, e)) return fault("Refused: " + e);
            // Exactly one answer to Load (L1 on #78).
            if (loaded_seen_ || refused_seen_) return fault("a Refused after the answer to Load");
            refused_seen_ = true;
            refused_text_ = r.text;
            return true;
        }
        // Replies: each matches a request this engine has outstanding, of
        // the type that request is answered with, and is held only until
        // taken (H1 on #78: a reply to nothing is a fault, so a host cannot
        // make the engine hold what it never asked for).
        case pc::Type::State: {
            pc::State s;
            if (!pc::decode(m, s, e)) return fault("State: " + e);
            const auto it = outstanding_.find(s.request);
            if (it == outstanding_.end() || it->second.reply != t || it->second.instance != s.instance)
                return fault("a State answering no GetState");
            outstanding_.erase(it);
            if (s.status == pc::StateStatus::Ok) {
                // Its identity is that instance's (host_control.h).
                pc::StateBlob blob;
                if (!pc::decode_state_blob(s.bytes, blob, e)) return fault("State: " + e);
                if (s.instance >= instances_.size() || blob.format != instances_[s.instance].format ||
                    blob.id != instances_[s.instance].id)
                    return fault("a State of another plugin than instance " + std::to_string(s.instance));
            }
            return hold(m, false);
        }
        case pc::Type::ValueText: {
            pc::ValueText v;
            if (!pc::decode(m, v, e)) return fault("ValueText: " + e);
            const auto it = outstanding_.find(v.request);
            if (it == outstanding_.end() || it->second.reply != t) return fault("a ValueText answering no DescribeValue");
            outstanding_.erase(it);
            return hold(m, false);
        }
        case pc::Type::EditorOpened:
        case pc::Type::EditorFailed: {
            uint32_t request = 0, instance = 0;
            if (t == pc::Type::EditorOpened) {
                pc::EditorOpened o;
                if (!pc::decode(m, o, e)) return fault("EditorOpened: " + e);
                request = o.request;
                instance = o.instance;
            } else {
                pc::EditorFailed f;
                if (!pc::decode(m, f, e)) return fault("EditorFailed: " + e);
                request = f.request;
                instance = f.instance;
            }
            const auto it = outstanding_.find(request);
            if (it == outstanding_.end() || it->second.reply != pc::Type::EditorOpened ||
                it->second.instance != instance)
                return fault("an editor reply answering no OpenEditor");
            outstanding_.erase(it);
            if (t == pc::Type::EditorOpened) open_editors_.insert(instance);
            return hold(m, false);
        }
        case pc::Type::Reconfigured: {
            pc::Reconfigured r;
            if (!pc::decode(m, r, e)) return fault("Reconfigured: " + e);
            const auto it = outstanding_.find(r.request);
            if (it == outstanding_.end() || it->second.reply != t) return fault("a Reconfigured answering no Reconfigure");
            outstanding_.erase(it);
            return hold(m, false);
        }
        // Unsolicited: checked against the chain, rate-limited where the
        // contract says (M2), and held under a cap.
        case pc::Type::GestureBegin: {
            pc::GestureBegin g;
            if (!pc::decode(m, g, e)) return fault("GestureBegin: " + e);
            if (!loaded_seen_ || !in_chain(g.instance, g.param)) return fault("GestureBegin for no parameter of the chain");
            flush_coalesced(g.instance, g.param);
            break;
        }
        case pc::Type::ParamTouched: {
            pc::ParamTouched p;
            if (!pc::decode(m, p, e)) return fault("ParamTouched: " + e);
            if (!loaded_seen_ || !in_chain(p.instance, p.param)) return fault("ParamTouched for no parameter of the chain");
            if (!touch_allowed(p.instance, p.param, now_ms)) {
                return fault("more than " + std::to_string(pc::kMaxTouchesPerSecondPerParam) +
                             " ParamTouched per second for instance " + std::to_string(p.instance) + " parameter " +
                             std::to_string(p.param) + " (M2)");
            }
            flush_coalesced(p.instance, p.param);
            break;
        }
        case pc::Type::GestureEnd: {
            pc::GestureEnd g;
            if (!pc::decode(m, g, e)) return fault("GestureEnd: " + e);
            if (!loaded_seen_ || !in_chain(g.instance, g.param)) return fault("GestureEnd for no parameter of the chain");
            switch (gesture_end(g.instance, g.param, g.value, now_ms)) {
                case GestureVerdict::Pass: flush_coalesced(g.instance, g.param); break;
                case GestureVerdict::Coalesced: return true;
                case GestureVerdict::Kill:
                    return fault("more than " + std::to_string(gesture_kill_threshold()) +
                                 " GestureEnd in one second (M2)");
            }
            break;
        }
        case pc::Type::EditorClosed: {
            pc::EditorClosed c;
            if (!pc::decode(m, c, e)) return fault("EditorClosed: " + e);
            // Exactly once per EditorOpened (host_control.h, "Editors").
            if (open_editors_.erase(c.instance) == 0) return fault("EditorClosed for no open editor");
            for (const pc::TouchedValue& v : c.moved)
                if (!in_chain(c.instance, v.param)) return fault("EditorClosed moves no parameter of the chain");
            break;
        }
        case pc::Type::LatencyChanged: {
            pc::LatencyChanged l;
            if (!pc::decode(m, l, e)) return fault("LatencyChanged: " + e);
            break;
        }
        case pc::Type::Fault: {
            pc::Fault f;
            if (!pc::decode(m, f, e)) return fault("Fault: " + e);
            if (f.instance != -1 && (f.instance < 0 || uint32_t(f.instance) >= param_counts_.size()))
                return fault("a Fault naming no instance of the chain");
            break;
        }
        default: return fault("a message type no host sends");
    }
    return hold(m, true);
}

void HostProcess::flush_coalesced(uint32_t instance, uint32_t param) {
    // #78 note 1: a coalesced end handed over after a newer message of the
    // same parameter would close that newer gesture with the older value.
    // It goes now, in its place, ahead of the message that passed.
    const auto it = coalesced_.find({instance, param});
    if (it == coalesced_.end()) return;
    std::string bytes;
    plugin::control::encode(plugin::control::GestureEnd{instance, param, it->second}, bytes);
    coalesced_.erase(it);
    messages_.push_back({std::move(bytes), false});
}

bool HostProcess::hold(std::string_view m, bool unsolicited) {
    // H1 on #78: what a host sends unasked is held under a cap; the owner
    // takes it every poll (take_messages). Replies are bounded by the
    // requests outstanding.
    if (unsolicited) {
        if (held_unsolicited_ + 1 > kMaxHeldMessages || held_unsolicited_bytes_ + m.size() > kMaxHeldBytes)
            return fault("more than " + std::to_string(kMaxHeldMessages) + " messages or " +
                         std::to_string(kMaxHeldBytes >> 20) + " MiB held unasked (H1)");
        ++held_unsolicited_;
        held_unsolicited_bytes_ += m.size();
    }
    messages_.push_back({std::string(m), unsolicited});
    return true;
}

bool HostProcess::pump(uint64_t now_ms) {
    namespace pc = plugin::control;
    if (!protocol_fault_.empty()) return false;
    if (!ctl_ || ctl_closed_) return true;
    // How long since the last pump: the touch and gesture buckets let the
    // backlog of that long through (M2).
    pump_stall_ms_ = last_pump_ms_ != 0 && now_ms > last_pump_ms_ ? now_ms - last_pump_ms_ : 0;
    last_pump_ms_ = now_ms;
    std::string error;
    const pc::Io f = link_.flush([&](std::string_view d) { return io_of(ctl_->send(d, 0, error)); });
    // Closed: gone, and the process exit says why (running(), exit_text());
    // what it sent before going is still read below.
    if (f == pc::Io::Error) return fault("cannot send: " + error);
    // M3 on #78: the Ready bound runs from the moment the whole Load has
    // left, not from the spawn, so a large state moving at poll cadence is
    // not taken for a hang. (The control server can also wait for the
    // socket to be writable: wants_write(), socket_handle().)
    if (!load_sent_ && !link_.wants_write()) {
        load_sent_ = true;
        ready_deadline_ms_ = now_ms + ready_timeout_ms_;
    }
    const uint64_t now_ns = now_ms * 1'000'000ull;
    pc::Io r = pc::Io::Ok;
    // A bounded number of datagrams per drain (fairness between hosts);
    // a few drains per pump so a startup's Hello and Loaded land together.
    for (int pass = 0; pass < 4 && r == pc::Io::Ok; ++pass) {
        r = link_.drain([&](std::string& d) { return io_of(ctl_->receive(d, pc::kMaxFrameBytes, nullptr, 0, error)); },
                        [&](std::string_view m) { return on_message(m, now_ms); }, error, now_ns);
    }
    if (!protocol_fault_.empty()) return false;
    if (r == pc::Io::Error) return fault(error.empty() ? "a frame out of place" : error);
    if (r == pc::Io::Closed || f == pc::Io::Closed) ctl_closed_ = true;
    // M1: a message half received that stopped growing pins engine memory.
    if (link_.stalled(now_ns, uint64_t(pc::kStalledMessageMs) * 1'000'000ull)) {
        return fault("a message half received for " + std::to_string(pc::kStalledMessageMs) + " ms (M1)");
    }
    return true;
}

std::vector<std::string> HostProcess::take_messages() {
    std::vector<std::string> out;
    out.reserve(messages_.size() + coalesced_.size());
    for (Held& h : messages_) out.push_back(std::move(h.bytes));
    messages_.clear();
    held_unsolicited_ = 0;
    held_unsolicited_bytes_ = 0;
    // A burst coalesced (M2): the newest value of each parameter, as one
    // GestureEnd each, after what passed.
    for (const auto& [key, value] : coalesced_) {
        std::string bytes;
        plugin::control::encode(plugin::control::GestureEnd{key.first, key.second, value}, bytes);
        out.push_back(std::move(bytes));
    }
    coalesced_.clear();
    return out;
}

bool HostProcess::send(const std::string& encoded, std::string& error) {
    namespace pc = plugin::control;
    if (!ctl_ || ctl_closed_) {
        error = "the host's socket is closed";
        return false;
    }
    // A request is remembered with the reply it is answered by, so the
    // reply is checked against it and nothing else is accepted (H1 on #78).
    pc::Type t{};
    if (!pc::peek_type(encoded, t) || !pc::engine_to_host(t)) {
        error = "not a message the engine sends";
        return false;
    }
    Outstanding o;
    uint32_t request = 0;
    bool asks = true;
    std::string e;
    switch (t) {
        case pc::Type::GetState: {
            pc::GetState m;
            if (!pc::decode(encoded, m, e)) break;
            request = m.request;
            o = {pc::Type::State, m.instance};
            break;
        }
        case pc::Type::DescribeValue: {
            pc::DescribeValue m;
            if (!pc::decode(encoded, m, e)) break;
            request = m.request;
            o = {pc::Type::ValueText, m.instance};
            break;
        }
        case pc::Type::OpenEditor: {
            pc::OpenEditor m;
            if (!pc::decode(encoded, m, e)) break;
            request = m.request;
            o = {pc::Type::EditorOpened, m.instance};
            break;
        }
        case pc::Type::Reconfigure: {
            pc::Reconfigure m;
            if (!pc::decode(encoded, m, e)) break;
            request = m.request;
            o = {pc::Type::Reconfigured, 0};
            break;
        }
        default: asks = false; break;
    }
    if (!e.empty()) {
        error = e;
        return false;
    }
    if (asks && outstanding_.count(request)) {
        error = "request " + std::to_string(request) + " is already outstanding";
        return false;
    }
    // Past kMaxQueuedBytes the host is not reading its socket (M1): the
    // caller kills it.
    if (!link_.queue(encoded, error)) return false;
    if (asks) outstanding_[request] = o;
    return true;
}

int HostProcess::socket_handle() const { return ctl_ ? ctl_->native_handle() : -1; }

int HostProcess::exit_handle() const { return child_ ? child_->exit_handle() : -1; }

void HostProcess::watch(std::vector<HostWatch>& out) {
    if (!child_ || !child_->running()) return;
    if (child_->exit_handle() >= 0) out.push_back({child_->exit_handle(), false});
    // A host that broke the contract is pumped no more (pump() returns at
    // once), so its socket would stay writable with nobody writing.
    if (ctl_ && !ctl_closed_ && protocol_fault_.empty() && link_.wants_write())
        out.push_back({ctl_->native_handle(), true});
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
    uint32_t layout = 0, clock_version = 0, wire = 0;
    if (!exited) {
        why_ = "plugin hosting is disabled: " + config.host_path + " --version did not exit within 5 s";
        return;
    }
    // Layout, clock and wire, each once (host_control.h, ADR-0012 §5.5).
    if (!child->exited_normally() || child->exit_code() != 0 ||
        !plugin::control::parse_host_version_line(line.c_str(), layout, clock_version, wire)) {
        why_ = "plugin hosting is disabled: " + config.host_path + " --version did not answer (" + line + ")";
        return;
    }
    if (layout != plugin::kLayoutVersion || clock_version != plugin::kClockLayoutVersion ||
        wire != plugin::control::kHostWireVersion) {
        why_ = "plugin hosting is disabled: " + config.host_path + " speaks layout " + std::to_string(layout) +
               " clock " + std::to_string(clock_version) + " wire " + std::to_string(wire) +
               "; this engine speaks layout " + std::to_string(plugin::kLayoutVersion) + " clock " +
               std::to_string(plugin::kClockLayoutVersion) + " wire " +
               std::to_string(plugin::control::kHostWireVersion);
        return;
    }
    client_.attach_clock(clock);
    ok_ = true;
}

HostSupervisor::~HostSupervisor() {
    // Teardown, so a bounded wait is allowed here: one grace for every host
    // adopted (stopped() signals the kill past it), then their regions go
    // before the clock does.
    const uint64_t t0 = steady_ms();
    while (!reap(steady_ms()) && steady_ms() - t0 < 2500) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    adopted_.clear();
    // Teardown order (code-reviewer M1, auditor L3 on PR #49): the backend
    // is deactivated first, so no callback runs and this returns at once.
    // If one is still inside, detach_clock waits for it to leave; if it
    // never does, the clock stays mapped (leaked) rather than unmapped
    // under the audio thread.
    if (!client_.detach_clock()) (void)clock_region_.release();
}

void HostSupervisor::watch(std::vector<HostWatch>& out) const {
    for (HostProcess* h : registry_->live) h->watch(out);
}

void HostSupervisor::adopt(std::unique_ptr<HostProcess> host) {
    if (!host) return;
    host->request_stop(steady_ms());
    adopted_.push_back(std::move(host));
}

bool HostSupervisor::reap(uint64_t now_ms) {
    for (auto it = adopted_.begin(); it != adopted_.end();)
        it = (*it)->stopped(now_ms) ? adopted_.erase(it) : std::next(it);
    return adopted_.empty();
}

std::unique_ptr<HostProcess> HostSupervisor::spawn(const ChainSpec& spec, std::string& error,
                                                   int32_t* refused_instance) {
    namespace pc = plugin::control;
    const std::string name = join(spec.plugins, ',');
    if (refused_instance) *refused_instance = -1;
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
    // M1 on #78: the states against their caps before anything is copied,
    // encoded or started. One past kMaxStateBytes is that instance's
    // refusal (the host's decode would refuse it, and a restart would only
    // repeat it); together past the Load cap, the chain's.
    std::size_t total = 0;
    for (uint32_t i = 0; i < n && i < spec.states.size(); ++i) {
        if (spec.states[i].size() > pc::kMaxStateBytes) {
            if (refused_instance) *refused_instance = int32_t(i);
            error = spec.plugins[i] + ": its state is " + std::to_string(spec.states[i].size()) +
                    " bytes, over the " + std::to_string(pc::kMaxStateBytes >> 20) + " MiB a plugin state may be";
            return nullptr;
        }
        total += spec.states[i].size();
    }
    // The fixed part of a Load: the ids, a few words per instance.
    std::size_t fixed = 64;
    for (const std::string& p : spec.plugins) fixed += p.size() + 32;
    if (total + fixed > pc::kMaxMessageBytes) {
        // Low item of the PR #51 review: said with the chain's names.
        error = name + ": the chain's plugin states exceed the 64 MiB Load cap";
        return nullptr;
    }

    std::unique_ptr<HostProcess> host(new HostProcess());
    host->registry_ = registry_;
    registry_->live.insert(host.get());
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

    // HostControl (host_control.h, "Handshake"): EngineHello first, Load
    // right behind it, queued before the fork; the chain crosses the socket,
    // not the command line.
    pc::Load load;
    load.key_channels = spec.key_channels;
    load.flags = spec.load_flags;
    for (uint32_t i = 0; i < n; ++i) {
        pc::LoadInstance li;
        const bool vst3 = spec.plugins[i].rfind("vst3:", 0) == 0;
        li.format = vst3 ? pc::Format::Vst3 : pc::Format::Lv2;
        li.id = vst3 ? spec.plugins[i].substr(5) : spec.plugins[i];
        if (!pc::valid_plugin_id(li.format, li.id)) {
            error = spec.plugins[i] + ": not a plugin id";
            return nullptr;
        }
        li.params = spec.param_counts[i];
        if (i < spec.states.size()) li.state = spec.states[i];
        host->instances_.push_back({li.format, li.id});
        load.instances.push_back(std::move(li));
    }
    // The touch buckets, one per dense parameter and bypass, laid out now.
    uint32_t slots = 0;
    for (const uint32_t c : spec.param_counts) {
        host->touch_base_.push_back(slots);
        slots += c + 1;
    }
    host->touch_tokens_.assign(slots, 0.0);
    host->touch_ms_.assign(slots, 0);
    std::string hello_bytes, load_bytes, queue_error;
    pc::encode(pc::EngineHello{}, hello_bytes);
    pc::encode(load, load_bytes);
    if (!host->link_.queue(hello_bytes, queue_error)) {
        error = name + ": " + queue_error;
        return nullptr;
    }
    if (load_bytes.size() > pc::kMaxMessageBytes || !host->link_.queue(load_bytes, queue_error)) {
        // Low item of the PR #51 review: said with the insert's name.
        error = name + ": the chain's plugin states exceed the 64 MiB Load cap";
        return nullptr;
    }
    int child_end = -1;
    if (!platform::ControlChannel::pair(host->ctl_, child_end, error)) return nullptr;
    host->param_counts_ = spec.param_counts;

    std::vector<std::string> argv = {config_.host_path, "--control-fd", "3", "--shm-fd", "4", "--clock-fd", "5",
                                     "--engine-pid", std::to_string(platform::current_process_id())};
    for (const std::string& b : config_.bundles) {
        argv.push_back("--bundle");
        argv.push_back(b);
    }
    platform::ChildProcess::Options o;
    o.handles = {{3, child_end}};
    o.regions = {{4, host->region_.get(), true}, {5, clock_region_.get(), false}};
    host->child_ = platform::ChildProcess::spawn(argv, o, error);
    // The child's copy is in place (or there is no child): ours goes now, so
    // the host's exit closes the pair.
    platform::ControlChannel::close_handle(child_end);
    if (!host->child_) return nullptr;
    // Ready within the bound (§5.3), noticed by poll_startup(), whose
    // caller's clock sets the deadline at its first look: one time base
    // (#72 review item 1), never this function's own.
    host->plugins_ = spec.plugins;
    host->ready_timeout_ms_ = config_.ready_timeout_ms;
    host->ready_deadline_ms_ = 0;
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
    const uint64_t now = steady_ms();
    retired();
    // A host still starting was never bound: no plan names it.
    if (starting_) let_go(std::move(starting_), now, true);
    if (slot_ != kNoSlot) sup_.client().retire(slot_);
    if (current_) let_go(std::move(current_), now, false);
    // An owner that asked first (request_stop, then stopped()) leaves nothing
    // running here. One that did not must not wait 2 s for a host that
    // ignores its Shutdown: the supervisor takes it and reaps it later.
    for (std::unique_ptr<HostProcess>& h : let_go_)
        if (!h->stopped(now)) sup_.adopt(std::move(h));
    let_go_.clear();
}

void SupervisedChain::let_go(std::unique_ptr<HostProcess> host, uint64_t now_ms, bool kill) {
    // What it sent is the control server's still (L2 on #78).
    std::vector<std::string> m = host->take_messages();
    orphans_.insert(orphans_.end(), std::make_move_iterator(m.begin()), std::make_move_iterator(m.end()));
    host->request_stop(now_ms);
    if (kill) host->signal_kill();
    let_go_.push_back(std::move(host));
}

bool SupervisedChain::reap_let_go(uint64_t now_ms) {
    for (auto it = let_go_.begin(); it != let_go_.end();) {
        if (!(*it)->stopped(now_ms)) {
            ++it;
            continue;
        }
        std::vector<std::string> m = (*it)->take_messages();
        orphans_.insert(orphans_.end(), std::make_move_iterator(m.begin()), std::make_move_iterator(m.end()));
        it = let_go_.erase(it);  // its region is released only now
    }
    return let_go_.empty();
}

void SupervisedChain::set_state(uint32_t instance, std::string blob) {
    if (instance >= spec_.plugins.size()) return;
    if (spec_.states.size() < spec_.plugins.size()) spec_.states.resize(spec_.plugins.size());
    spec_.states[instance] = std::move(blob);
}

bool SupervisedChain::begin(std::string& error) {
    const plugin::ParamMap map = plugin::make_param_map(spec_.param_counts.data(), uint32_t(spec_.param_counts.size()));
    std::vector<float> values(map.param_slots, 0.f), bypass(plugin::kMaxInstances, 0.f);
    for (std::size_t i = 0; i < values.size() && i < spec_.values.size(); ++i) values[i] = spec_.values[i];
    for (std::size_t i = 0; i < bypass.size() && i < spec_.bypass.size(); ++i) bypass[i] = spec_.bypass[i];
    sup_.client().reset_store(store_, map, values.data(), bypass.data());
    sup_.client().reset_node(node_);
    int32_t refused = -1;
    starting_ = sup_.spawn(spec_, error, &refused);
    if (!starting_) {
        down_ = true;
        why_ = error;
        // A state over its cap is that instance's refusal (M1 on #78).
        refused_ = refused >= 0;
        refused_instance_ = refused;
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

std::vector<std::string> SupervisedChain::take_host_messages() {
    // L2 on #78: what a replaced or retired host sent before it went comes
    // first (the control server closes the gestures it left open with the
    // last ParamTouched it saw), then the retiring host's, then the bound
    // one's.
    std::vector<std::string> out;
    out.swap(orphans_);
    for (const std::unique_ptr<HostProcess>& h : let_go_) {
        std::vector<std::string> m = h->take_messages();
        out.insert(out.end(), std::make_move_iterator(m.begin()), std::make_move_iterator(m.end()));
    }
    for (HostProcess* h : {retiring_.get(), current_.get()}) {
        if (!h) continue;
        std::vector<std::string> m = h->take_messages();
        out.insert(out.end(), std::make_move_iterator(m.begin()), std::make_move_iterator(m.end()));
    }
    return out;
}

bool SupervisedChain::refresh_latency() { return current_ && current_->refresh_latency(); }

void SupervisedChain::request_stop(uint64_t now_ms) {
    // Never bound, so nothing fades: killed now, reaped by stopped().
    if (starting_) {
        starting_->request_stop(now_ms);
        starting_->signal_kill();
    }
    if (current_) current_->request_stop(now_ms);
    if (retiring_) retiring_->request_stop(now_ms);
    for (const std::unique_ptr<HostProcess>& h : let_go_) h->request_stop(now_ms);
}

bool SupervisedChain::stopped(uint64_t now_ms) {
    bool all = reap_let_go(now_ms);
    if (starting_) all = starting_->stopped(now_ms) && all;
    if (current_) all = current_->stopped(now_ms) && all;
    if (retiring_) all = retiring_->stopped(now_ms) && all;
    return all;
}

SupervisedChain::Event SupervisedChain::poll_starting(uint64_t now_ms) {
    std::string error;
    switch (starting_->poll_startup(now_ms, error)) {
        case HostProcess::Startup::Starting: return Event::None;
        case HostProcess::Startup::Failed: {
            const bool restart = current_ != nullptr;
            // A refusal is the plugin's answer, not an accident: it is
            // reported, never retried. The control server leaves the named
            // instance out and starts the rest (amendment of 2026-10-14,
            // change C).
            refused_ = starting_->refused();
            refused_instance_ = starting_->refused_instance();
            let_go(std::move(starting_), now_ms, true);
            down_ = true;
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
    reap_let_go(now_ms);
    if (down_) {
        // A host replace() killed before the chain went down: reaped here
        // (a WNOHANG waitpid), not left a zombie until the chain goes.
        if (current_) (void)current_->running();
        return Event::None;
    }
    // The bound host's socket, every poll: its messages are taken by the
    // control server, and a broken contract kills it here (crash path).
    const bool contract_kept = !current_ || current_->pump(now_ms);
    // A retiring host's last messages are read too (L2 on #78).
    if (retiring_) retiring_->pump(now_ms);
    if (starting_) return poll_starting(now_ms);
    if (!current_ || retiring_) return Event::None;
    if (!contract_kept) {
        culprit_ = -1;
        for (uint64_t& n : streak_samples_) n = 0;
        return replace(now_ms, current_->protocol_fault());
    }
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
    // until the plan naming its slot is retired. Signalled, not waited for:
    // a host stuck in D state would hold the control thread in waitpid.
    // running() reaps it later, or let_go() once its slot is retired.
    current_->signal_kill();
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
    int32_t refused = -1;
    starting_ = sup_.spawn(spec_, error, &refused);
    if (!starting_) {
        down_ = true;
        refused_ = refused >= 0;
        refused_instance_ = refused;
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
    // Asked to stop, not waited for; its region is released once it has
    // exited (§5.3), by reap_let_go.
    let_go(std::move(retiring_), steady_ms(), false);
}

}  // namespace sonora::plugin_client
