// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The host's half of HostControl (contracts/plugin host_control.h): the
// socket on --control-fd, read by a thread of its own that wakes the main
// thread (control_word), and what turns a plugin's touches into gestures.
// Nothing here runs on the processing thread, except TouchQueue::touch when
// a plugin calls its handler from process(), which never blocks.

#include "processor.h"
#include "sonora/platform/ipc.h"
#include "sonora/plugin/host_control.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sonora::plugin_host {

class ControlLink {
public:
    ~ControlLink();

    // Adopts the inherited descriptor and starts the reader.
    bool open(int fd, std::string& error);

    // The main thread. The next whole message the engine sent, waiting at
    // most `timeout_ns` (0: never). False when there is none: then closed()
    // says whether there will never be one.
    bool next(std::string& message, uint64_t timeout_ns);
    // The engine closed its end, or framed wrongly (error() says how).
    bool closed() const { return closed_.load(std::memory_order_acquire); }
    std::string error() const;

    // The main thread. Framed and sent in order, waiting at most a few
    // seconds for a reader that is slow; false when the engine is gone or
    // not reading, after which the host stops.
    bool send(const std::string& message);
    template <class M>
    bool send_message(const M& m) {
        std::string bytes;
        plugin::control::encode(m, bytes);
        return send(bytes);
    }

    void close();

private:
    void read_loop();

    std::unique_ptr<platform::ControlChannel> channel_;
    std::thread reader_;
    mutable std::mutex mutex_;
    std::vector<std::string> inbox_;
    std::string error_;
    std::atomic<bool> closed_{false};
    std::size_t taken_ = 0;
};

// A plugin's touches, from any thread, into a fixed ring the main thread
// drains. A touch that cannot be queued at once (the ring full, or the main
// thread holding it) is dropped and counted, never waited for.
class TouchQueue final : public TouchSink {
public:
    struct Event {
        uint32_t instance;
        TouchKind kind;
        uint32_t dense;
        float value;
    };

    void touch(uint32_t instance, TouchKind kind, uint32_t dense, float value) noexcept override;
    // The main thread: what came since the last call, in order.
    void drain(std::vector<Event>& out);
    // Touches that could not be queued: one of them may have been an end.
    uint64_t lost() const { return lost_.load(std::memory_order_acquire); }
    // Unmapped touches by one plugin of the chain so far.
    uint64_t unmapped(uint32_t instance) const {
        return instance < plugin::kMaxInstances ? unmapped_[instance].load(std::memory_order_relaxed) : 0;
    }

private:
    static constexpr uint32_t kCapacity = 1024;
    std::mutex mutex_;
    Event ring_[kCapacity] = {};
    uint32_t head_ = 0, count_ = 0;
    std::atomic<uint64_t> lost_{0};
    std::atomic<uint64_t> unmapped_[plugin::kMaxInstances] = {};
};

// The main thread's gesture rules (host_control.h, "Parameter touches"):
// every touch inside a gesture, touches coalesced to kTouchIntervalMs per
// parameter, GestureEnd carrying the plugin's value at the end, open
// gestures ended before the host stops and whenever a touch was dropped.
// Calls `send(const std::string&)` with encoded messages.
class GestureTracker {
public:
    template <class Send>
    void apply(const TouchQueue::Event& e, uint64_t now_ns, Send&& send);
    // Pending coalesced touches whose interval has passed. Returns the
    // earliest time a pending one becomes due, or 0 when none is pending.
    template <class Send>
    uint64_t flush(uint64_t now_ns, Send&& send);
    // GestureEnd for every gesture still open, each with the plugin's
    // current value where `current(instance, param, float&)` can read it,
    // else the last touch. Before the host stops, and at once after any
    // touch was dropped: the dropped one may have been an end (M3 on
    // PR #51), and no gesture may stay open for want of it.
    template <class Current, class Send>
    void end_all(Current&& current, Send&& send);
    // After touches were lost, a value the plugin moved may be in no
    // gesture the host saw (a lone edit lost; host_control.h). For every
    // instance below `instances`, its `count(i)` dense parameters and its
    // bypass: where no gesture is open and `current(i, p, float&)` differs
    // from the value last applied to the plugin, the plugin's value is
    // recorded as a one-shot gesture. `engine(i, p, float& table, float&
    // applied)` gives the engine's table value and the one applied; a slot
    // where they differ is a change in flight and is skipped.
    template <class Count, class Engine, class Current, class Send>
    void recover(uint32_t instances, Count&& count, Engine&& engine, Current&& current, Send&& send);
    bool any_open() const { return !open_.empty(); }

private:
    struct Open {
        float last = 0.f;
        bool pending = false;  // a touch not sent yet
        uint64_t sent_ns = 0;  // when the last ParamTouched went
    };
    template <class Send>
    static void one_shot(uint32_t instance, uint32_t param, float value, Send&& send);

    std::map<std::pair<uint32_t, uint32_t>, Open> open_;
    // Gestures end_all ended, at the value it sent: their own late end at
    // that value says nothing new.
    std::map<std::pair<uint32_t, uint32_t>, float> ended_;
};

template <class Send>
void GestureTracker::one_shot(uint32_t instance, uint32_t param, float value, Send&& send) {
    using namespace plugin::control;
    std::string bytes;
    encode(GestureBegin{instance, param}, bytes);
    send(bytes);
    encode(ParamTouched{instance, param, value}, bytes);
    send(bytes);
    encode(GestureEnd{instance, param, value}, bytes);
    send(bytes);
}

template <class Send>
void GestureTracker::apply(const TouchQueue::Event& e, uint64_t now_ns, Send&& send) {
    using namespace plugin::control;
    const auto key = std::make_pair(e.instance, e.dense);
    std::string bytes;
    auto it = open_.find(key);
    switch (e.kind) {
    case TouchKind::Begin:
        if (it != open_.end()) return;
        ended_.erase(key);
        open_[key] = Open{e.value, false, 0};
        encode(GestureBegin{e.instance, e.dense}, bytes);
        send(bytes);
        return;
    case TouchKind::Perform:
        if (it == open_.end()) {
            // A touch outside a gesture is a gesture of its own.
            ended_.erase(key);
            one_shot(e.instance, e.dense, e.value, send);
            return;
        }
        it->second.last = e.value;
        if (now_ns - it->second.sent_ns >= uint64_t(kTouchIntervalMs) * 1000000ull) {
            encode(ParamTouched{e.instance, e.dense, e.value}, bytes);
            send(bytes);
            it->second.sent_ns = now_ns;
            it->second.pending = false;
        } else {
            it->second.pending = true;
        }
        return;
    case TouchKind::End:
        if (it == open_.end()) {
            // Not open: ended by end_all after a drop, or its begin and
            // touches lost. Unless end_all already sent this value, the end
            // is the only word of the move, so it is recorded (#121).
            const auto was = ended_.find(key);
            const bool said = was != ended_.end() && was->second == e.value;
            if (was != ended_.end()) ended_.erase(was);
            if (!said && std::isfinite(e.value)) one_shot(e.instance, e.dense, e.value, send);
            return;
        }
        // The plugin's value at the end (VST3: the controller's, read at
        // endEdit), not the last touch queued: a touch lost on the way
        // must not make it stale (M3).
        encode(GestureEnd{e.instance, e.dense, e.value}, bytes);
        send(bytes);
        open_.erase(it);
        return;
    case TouchKind::Unmapped:
        return;  // counted by the queue
    }
}

template <class Send>
uint64_t GestureTracker::flush(uint64_t now_ns, Send&& send) {
    using namespace plugin::control;
    const uint64_t interval = uint64_t(kTouchIntervalMs) * 1000000ull;
    uint64_t due = 0;
    std::string bytes;
    for (auto& [key, g] : open_) {
        if (!g.pending) continue;
        if (now_ns - g.sent_ns >= interval) {
            encode(ParamTouched{key.first, key.second, g.last}, bytes);
            send(bytes);
            g.sent_ns = now_ns;
            g.pending = false;
        } else if (due == 0 || g.sent_ns + interval < due) {
            due = g.sent_ns + interval;
        }
    }
    return due;
}

template <class Current, class Send>
void GestureTracker::end_all(Current&& current, Send&& send) {
    using namespace plugin::control;
    std::string bytes;
    for (auto& [key, g] : open_) {
        float v = g.last;
        float now = 0.f;
        if (current(key.first, key.second, now) && std::isfinite(now)) v = now;
        encode(GestureEnd{key.first, key.second, v}, bytes);
        send(bytes);
        ended_[key] = v;
    }
    open_.clear();
}

template <class Count, class Engine, class Current, class Send>
void GestureTracker::recover(uint32_t instances, Count&& count, Engine&& engine, Current&& current, Send&& send) {
    const auto check = [&](uint32_t i, uint32_t p) {
        if (open_.count({i, p})) return;
        float table = 0.f, was = 0.f, now = 0.f;
        if (!engine(i, p, table, was)) return;
        // The engine writes its table before the next block applies it: a
        // slot still in flight holds the operator's change, and the plugin
        // its old value, which is no move of the plugin's (#121).
        if (std::memcmp(&table, &was, sizeof table) != 0) return;
        if (!current(i, p, now) || !std::isfinite(now) || now == was) return;
        // A format's own rounding (a stepped parameter) is no move.
        if (std::fabs(now - was) <= 1e-6f * std::max(1.f, std::fabs(was))) return;
        // end_all sent this value, and the engine has not posted it back yet.
        if (const auto e = ended_.find({i, p}); e != ended_.end() && e->second == now) return;
        ended_.erase({i, p});
        one_shot(i, p, now, send);
    };
    for (uint32_t i = 0; i < instances; ++i) {
        const uint32_t n = count(i);
        for (uint32_t p = 0; p < n; ++p) check(i, p);
        check(i, plugin::kBypassParam);
    }
}

}  // namespace sonora::plugin_host
