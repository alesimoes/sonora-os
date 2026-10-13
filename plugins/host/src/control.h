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

#include <atomic>
#include <cmath>
#include <cstdint>
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
    // Touches dropped so far: lost ones (not queued) and Unmapped ones.
    uint64_t dropped() const { return lost() + unmapped_.load(std::memory_order_relaxed); }
    // Touches that could not be queued: one of them may have been an end.
    uint64_t lost() const { return lost_.load(std::memory_order_acquire); }

private:
    static constexpr uint32_t kCapacity = 1024;
    std::mutex mutex_;
    Event ring_[kCapacity] = {};
    uint32_t head_ = 0, count_ = 0;
    std::atomic<uint64_t> lost_{0}, unmapped_{0};
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
    bool any_open() const { return !open_.empty(); }

private:
    struct Open {
        float last = 0.f;
        bool pending = false;  // a touch not sent yet
        uint64_t sent_ns = 0;  // when the last ParamTouched went
    };
    std::map<std::pair<uint32_t, uint32_t>, Open> open_;
};

template <class Send>
void GestureTracker::apply(const TouchQueue::Event& e, uint64_t now_ns, Send&& send) {
    using namespace plugin::control;
    const auto key = std::make_pair(e.instance, e.dense);
    std::string bytes;
    auto it = open_.find(key);
    switch (e.kind) {
    case TouchKind::Begin:
        if (it != open_.end()) return;
        open_[key] = Open{e.value, false, 0};
        encode(GestureBegin{e.instance, e.dense}, bytes);
        send(bytes);
        return;
    case TouchKind::Perform:
        if (it == open_.end()) {
            // A touch outside a gesture is a gesture of its own.
            encode(GestureBegin{e.instance, e.dense}, bytes);
            send(bytes);
            encode(ParamTouched{e.instance, e.dense, e.value}, bytes);
            send(bytes);
            encode(GestureEnd{e.instance, e.dense, e.value}, bytes);
            send(bytes);
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
        // Not open: already ended (after a drop), or never begun. Ignored.
        if (it == open_.end()) return;
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
    }
    open_.clear();
}

}  // namespace sonora::plugin_host
