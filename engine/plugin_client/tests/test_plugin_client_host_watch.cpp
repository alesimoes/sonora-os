// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0213: a host socket is in the control thread's WaitSet only while the
// holder that drains it is serviced on the turn after a wake.
//
//   gesture        an editor gesture from a Ready host ends the sleep, and
//                  the turn that follows takes it; a quiet or drained socket
//                  does not end the sleep
//   parked_unread  a retiring chain (its owner calls poll_parked() only)
//                  whose host left datagrams unread and has a large send
//                  pending on a full socket: the turns wake once for the
//                  datagrams and then sleep; a host signalled to die is not
//                  listed for reading
//
// The test binary is its own scripted host (SONORA_FAKE_HOST), so no plugin
// and no sonora-plugin-host are needed. Wakes are counted per turn, never
// timed: a spin is a wake on every turn.
#include "sonora/platform/ipc.h"
#include "sonora/platform/wait_set.h"
#include "sonora/plugin/host_control.h"
#include "sonora/plugin_client/host_supervisor.h"
#include "sonora/plugin_client/plugin_client.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace sonora;
using namespace sonora::plugin_client;
namespace pc = sonora::plugin::control;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

uint64_t now_ms() {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// The test names it in the environment its hosts inherit.
std::string marker_path() {
    const char* m = std::getenv("SONORA_WATCH_MARKER");
    return m ? m : "";
}

// ---- the scripted host

// echo-gesture: a GestureBegin and a ParamTouched for every datagram the
// engine sends, as an editor knob grabbed would.
// deaf-chatty: reads nothing after Load; once the marker file exists, sends
// one gesture burst, then beats until killed.
int fake_host(const std::string& mode) {
    std::string err;
    auto ctl = platform::ControlChannel::adopt(3, err);
    struct stat st {};
    if (!ctl || ::fstat(4, &st) != 0) return 2;
    void* base = ::mmap(nullptr, std::size_t(st.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, 4, 0);
    if (base == MAP_FAILED) return 2;
    plugin::ChannelHeader& h = *static_cast<plugin::ChannelHeader*>(base);
    pc::Reassembler in(pc::kMaxMessageBytes);
    const auto receive = [&](std::string& message) {
        std::string datagram;
        for (;;) {
            if (ctl->receive(datagram, pc::kMaxFrameBytes, nullptr, 5000, err) != platform::IoStatus::Ok) return false;
            switch (in.add(datagram, message, err)) {
                case pc::Reassembler::Result::Complete: return true;
                case pc::Reassembler::Result::Error: return false;
                case pc::Reassembler::Result::Partial: break;
            }
        }
    };
    const auto msg = [&](const auto& m) {
        std::string out;
        pc::encode(m, out);
        std::vector<std::string> frames;
        if (!pc::split_frames(out, frames, err)) return;
        for (const std::string& f : frames) ctl->send(f, 1000, err);
    };
    std::string m;
    pc::EngineHello ehello;
    pc::Load load;
    if (!receive(m) || !pc::decode(m, ehello, err) || !receive(m) || !pc::decode(m, load, err)) return 2;
    pc::Hello hello;
    hello.pid = uint64_t(::getpid());
    msg(hello);
    h.latency_frames.store(h.block_frames, std::memory_order_relaxed);
    h.latency_generation.store(1, std::memory_order_release);
    h.state.store(uint32_t(plugin::HostState::Ready), std::memory_order_release);
    pc::Loaded loaded;
    loaded.latency_frames = h.block_frames;
    loaded.latency_generation = 1;
    for (const pc::LoadInstance& li : load.instances) {
        pc::LoadedInstance i;
        for (uint32_t p = 0; p < li.params; ++p) i.params.push_back({"p" + std::to_string(p), 0.f, 1.f, 0.f});
        loaded.instances.push_back(std::move(i));
    }
    msg(loaded);
    if (mode == "deaf-chatty") {
        const std::string marker = marker_path();
        bool sent = false;
        for (;;) {
            h.heartbeat.fetch_add(1, std::memory_order_relaxed);
            struct stat ms {};
            if (!sent && !marker.empty() && ::stat(marker.c_str(), &ms) == 0) {
                sent = true;
                msg(pc::GestureBegin{0, 0});
                for (int k = 0; k < 20; ++k) msg(pc::ParamTouched{0, 0, float(k % 2)});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    while (h.shutdown.load(std::memory_order_acquire) == 0) {
        h.heartbeat.fetch_add(1, std::memory_order_relaxed);
        std::string datagram;
        const platform::IoStatus r = ctl->receive(datagram, pc::kMaxFrameBytes, nullptr, 2, err);
        if (r == platform::IoStatus::Closed) break;
        if (r == platform::IoStatus::Ok) {
            msg(pc::GestureBegin{0, 0});
            msg(pc::ParamTouched{0, 0, 0.25f});
        }
    }
    return 0;
}

// ---- the engine side

std::string self_path() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, std::size_t(n)) : std::string();
}

struct Rig {
    PluginClient client;
    SupervisorConfig scfg;
    std::unique_ptr<HostSupervisor> sup;
    Rig() : client(client_config()) {
        scfg.host_path = self_path();
        scfg.sample_rate = 48000;
        scfg.block_frames = 128;
        scfg.ready_timeout_ms = 10000;
        sup = std::make_unique<HostSupervisor>(scfg, client);
    }
    static ClientConfig client_config() {
        ClientConfig c;
        c.max_channels = 16;
        c.max_stores = 4;
        c.max_nodes = 4;
        c.store_slots = 1024;
        return c;
    }
    // What the control thread sleeps on, as engine/control lists it.
    std::vector<platform::WaitSet::Watch> watches(std::vector<HostWatch>* raw = nullptr) const {
        std::vector<HostWatch> hw;
        sup->watch(hw);
        std::vector<platform::WaitSet::Watch> w;
        for (const HostWatch& h : hw)
            w.push_back({h.handle, h.writable ? platform::WaitSet::When::Writable : platform::WaitSet::When::Readable});
        if (raw) *raw = hw;
        return w;
    }
};

ChainSpec fake_spec() {
    ChainSpec s;
    s.plugins = {"urn:sonora:fake#one"};
    s.param_counts = {2};
    s.channels = 1;
    return s;
}

// poll() every 5 ms until the first host is bound, or 5 s.
bool until_swapped(SupervisedChain& c) {
    const uint64_t end = now_ms() + 5000;
    while (now_ms() < end) {
        if (c.poll(now_ms()) == SupervisedChain::Event::Swapped) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool listed(const std::vector<HostWatch>& hw, int handle, bool writable) {
    for (const HostWatch& h : hw)
        if (h.handle == handle && h.writable == writable) return true;
    return false;
}

bool has_type(const std::vector<std::string>& messages, pc::Type type) {
    for (const std::string& m : messages) {
        pc::Type t{};
        if (pc::peek_type(m, t) && t == type) return true;
    }
    return false;
}

void stop(SupervisedChain& chain) {
    chain.request_stop(now_ms());
    const uint64_t end = now_ms() + 5000;
    bool stopped = false;
    while (!stopped && now_ms() < end) {
        stopped = chain.stopped(now_ms());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(stopped, "stopped once asked");
}

int scenario_gesture() {
    std::printf("gesture: an editor gesture ends the control thread's sleep\n");
    ::setenv("SONORA_FAKE_HOST", "echo-gesture", 1);
    Rig rig;
    if (!rig.sup->ok()) return 1;
    SupervisedChain chain(*rig.sup, fake_spec(), 0, 0);
    std::string error;
    check(chain.begin(error), "spawned");
    check(until_swapped(chain), "Ready and bound");
    HostProcess* h = chain.host();
    if (!h) return 1;
    (void)chain.take_host_messages();
    platform::WaitSet sleep;
    if (!sleep.watches_handles()) {
        std::printf("  no poll set here: the rest is skipped\n");
        return 0;
    }
    std::vector<HostWatch> hw;
    std::vector<platform::WaitSet::Watch> ws = rig.watches(&hw);
    check(listed(hw, h->socket_handle(), false), "its socket is listed for reading");
    check(sleep.wait(ws.data(), ws.size(), 200) == platform::WaitSet::Woke::Timeout,
          "a quiet socket does not end the sleep");

    // The knob is grabbed: anything the engine sends makes the host answer
    // with a gesture.
    std::string bytes;
    pc::encode(pc::Shutdown{}, bytes);
    check(h->send(bytes, error) && h->pump(now_ms()) && !h->wants_write(), "a frame sent to the host");
    ws = rig.watches();
    check(sleep.wait(ws.data(), ws.size(), 5000) == platform::WaitSet::Woke::Handle,
          "the gesture ends the sleep, not the timeout");
    // The turn after the wake: the owner's poll reads it.
    check(chain.poll(now_ms()) == SupervisedChain::Event::None, "the turn: the chain polled");
    std::vector<std::string> got = chain.take_host_messages();
    check(has_type(got, pc::Type::GestureBegin), "that turn takes the GestureBegin");
    // The ParamTouched may land just after; each turn waits on the socket.
    for (int turn = 0; turn < 20 && !has_type(got, pc::Type::ParamTouched); ++turn) {
        ws = rig.watches();
        sleep.wait(ws.data(), ws.size(), 1000);
        chain.poll(now_ms());
        std::vector<std::string> more = chain.take_host_messages();
        got.insert(got.end(), more.begin(), more.end());
    }
    check(has_type(got, pc::Type::ParamTouched), "and its touch");
    ws = rig.watches();
    check(sleep.wait(ws.data(), ws.size(), 200) == platform::WaitSet::Woke::Timeout,
          "drained: the socket no longer ends the sleep");
    stop(chain);
    return 0;
}

int scenario_parked_unread() {
    std::printf("parked_unread: a retiring chain's unread datagrams and full socket do not spin\n");
    const std::string marker = "/tmp/sonora-watch-marker-" + std::to_string(::getpid());
    std::remove(marker.c_str());
    ::setenv("SONORA_WATCH_MARKER", marker.c_str(), 1);
    ::setenv("SONORA_FAKE_HOST", "deaf-chatty", 1);
    int r = 0;
    {
        Rig rig;
        if (!rig.sup->ok()) return 1;
        SupervisedChain chain(*rig.sup, fake_spec(), 0, 0);
        std::string error;
        check(chain.begin(error), "spawned");
        check(until_swapped(chain), "Ready and bound");
        HostProcess* h = chain.host();
        if (!h) return 1;
        platform::WaitSet sleep;
        if (!sleep.watches_handles() || h->exit_handle() < 0) {
            std::printf("  no poll set or no pidfd here: the rest is skipped\n");
            stop(chain);
            return 0;
        }
        // From here the chain is retiring: its owner only calls
        // poll_parked() (engine/control's retiring_, awaiting its epoch).
        // A large send pending on the deaf host's full socket.
        std::string bytes;
        pc::encode(pc::Shutdown{}, bytes);
        for (int i = 0; i < 100000 && !h->wants_write(); ++i) {
            if (!h->send(bytes, error)) break;
            h->pump(now_ms());
        }
        check(h->wants_write(), "frames wait on its full socket");
        if (std::FILE* f = std::fopen(marker.c_str(), "w")) std::fclose(f);

        std::vector<HostWatch> hw;
        std::vector<platform::WaitSet::Watch> ws = rig.watches(&hw);
        check(listed(hw, h->socket_handle(), false) && listed(hw, h->socket_handle(), true),
              "listed for reading and for writing");
        // The burst arrives: one wake, then the parked turn drains it.
        check(sleep.wait(ws.data(), ws.size(), 5000) == platform::WaitSet::Woke::Handle, "the burst wakes a turn");
        uint32_t wakes = 0;
        const uint32_t turns = 20;
        for (uint32_t t = 0; t < turns; ++t) {
            chain.poll_parked(now_ms());
            ws = rig.watches();
            if (sleep.wait(ws.data(), ws.size(), 50) != platform::WaitSet::Woke::Timeout) ++wakes;
        }
        char line[128];
        std::snprintf(line, sizeof line, "%u wakes in %u parked turns after the burst: the sleep holds", wakes,
                      turns);
        check(wakes <= 2, line);
        if (wakes > 2) r = 1;
        check(h->wants_write(), "the send is still pending: the hold is not a drained socket");

        // Signalled to die: its exit handle is the wake, not its socket.
        h->signal_kill();
        rig.watches(&hw);
        check(!listed(hw, h->socket_handle(), false), "killed: its socket is not listed for reading");
        stop(chain);
    }
    std::remove(marker.c_str());
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    if (const char* mode = std::getenv("SONORA_FAKE_HOST")) {
        for (int i = 1; i < argc; ++i) {
            const std::string k = argv[i];
            if (k == "--version") {
                char line[128];
                pc::format_host_version_line(line, sizeof line);
                std::printf("%s\n", line);
                return 0;
            }
            if (k == "--control-fd") return fake_host(mode);
        }
    }
    const std::string scenario = argc > 1 ? argv[1] : "";
    int r = 2;
    if (scenario == "gesture") r = scenario_gesture();
    else if (scenario == "parked_unread") r = scenario_parked_unread();
    else std::fprintf(stderr, "unknown scenario %s\n", scenario.c_str());
    if (failures) std::printf("%d check(s) failed\n", failures);
    return failures ? 1 : r;
}
