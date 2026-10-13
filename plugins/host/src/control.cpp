// SPDX-License-Identifier: GPL-3.0-or-later
#include "control.h"

#include "ipc.h"

namespace sonora::plugin_host {

namespace pc = plugin::control;

namespace {
// How long the main thread lets a send wait for an engine that is not
// reading before it gives up on it. The engine's own bound on a host that
// does not read is kMaxQueuedBytes; this is the host's.
constexpr int kSendTimeoutMs = 5000;
}  // namespace

ControlLink::~ControlLink() { close(); }

bool ControlLink::open(int fd, std::string& error) {
    channel_ = platform::ControlChannel::adopt(fd, error);
    if (!channel_) return false;
    reader_ = std::thread([this] { read_loop(); });
    return true;
}

void ControlLink::close() {
    if (channel_) channel_->shutdown();
    if (reader_.joinable()) reader_.join();
}

std::string ControlLink::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

// Blocks in receive; every whole message goes to the inbox and wakes the
// main thread. Ends on the engine's close, a framing error, or close().
void ControlLink::read_loop() {
    pc::Reassembler reassembler(pc::kMaxMessageBytes);  // engine -> host
    std::string datagram, message, why;
    for (;;) {
        const platform::IoStatus s = channel_->receive(datagram, pc::kMaxFrameBytes, nullptr, -1, why);
        if (s == platform::IoStatus::WouldBlock) continue;
        if (s != platform::IoStatus::Ok) {
            if (s == platform::IoStatus::Error) {
                std::lock_guard<std::mutex> lock(mutex_);
                error_ = why;
            }
            break;
        }
        const auto r = reassembler.add(datagram, message, why);
        if (r == pc::Reassembler::Result::Partial) continue;
        if (r == pc::Reassembler::Result::Error) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = "the engine framed a message wrongly: " + why;
            break;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inbox_.push_back(std::move(message));
        }
        message.clear();
        notify_control();
    }
    closed_.store(true, std::memory_order_release);
    notify_control();
}

bool ControlLink::next(std::string& message, uint64_t timeout_ns) {
    const uint64_t end = monotonic_ns() + timeout_ns;
    for (;;) {
        const uint32_t seen = control_word().load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (taken_ < inbox_.size()) {
                message = std::move(inbox_[taken_++]);
                if (taken_ == inbox_.size()) {
                    inbox_.clear();
                    taken_ = 0;
                }
                return true;
            }
        }
        if (closed()) return false;
        const uint64_t now = monotonic_ns();
        if (now >= end) return false;
        wait_for_change(control_word(), seen, end - now);
    }
}

bool ControlLink::send(const std::string& message) {
    if (!channel_) return false;
    std::vector<std::string> frames;
    std::string why;
    if (!pc::split_frames(message, frames, why)) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = why;
        return false;
    }
    for (const std::string& f : frames) {
        if (channel_->send(f, kSendTimeoutMs, why) != platform::IoStatus::Ok) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (error_.empty()) error_ = why.empty() ? "the engine is not reading its end" : why;
            return false;
        }
    }
    return true;
}

void TouchQueue::touch(uint32_t instance, TouchKind kind, uint32_t dense, float value) noexcept {
    if (kind == TouchKind::Unmapped) {
        unmapped_.fetch_add(1, std::memory_order_relaxed);
        notify_control();
        return;
    }
    // One try_lock, never a wait: a plugin may call its handler from
    // process() on the SCHED_FIFO thread. Losing the race to the main
    // thread's copy-out loses the touch, which is counted, and the main
    // thread then ends every open gesture with the plugin's current value
    // (host_control.h, M3), so a lost end never leaves one open.
    if (!mutex_.try_lock()) {
        lost_.fetch_add(1, std::memory_order_release);
        notify_control();
        return;
    }
    if (count_ == kCapacity) {
        mutex_.unlock();
        lost_.fetch_add(1, std::memory_order_release);
        notify_control();
        return;
    }
    ring_[(head_ + count_) % kCapacity] = Event{instance, kind, dense, value};
    ++count_;
    mutex_.unlock();
    notify_control();
}

void TouchQueue::drain(std::vector<Event>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (; count_ > 0; --count_, head_ = (head_ + 1) % kCapacity) out.push_back(ring_[head_]);
}

}  // namespace sonora::plugin_host
