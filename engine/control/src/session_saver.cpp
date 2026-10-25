// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/session_saver.h"

#include <utility>

namespace sonora {

SessionSaver::SessionSaver(Checkpoints checkpoints, Ready ready, std::chrono::milliseconds poll,
                           session_files::SessionFolder::SyncDirectory sync_directory)
    : checkpoints_(std::move(checkpoints)),
      ready_(std::move(ready)),
      poll_(poll),
      folder_(sync_directory) {}

SessionSaver::~SessionSaver() { stop(); }

void SessionSaver::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this]() { run(); });
}

void SessionSaver::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void SessionSaver::set_take_open(bool open) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (take_open_ == open) return;
        take_open_ = open;
    }
    wake_.notify_all();
}

void SessionSaver::set_transport_stopped(bool stopped) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ == stopped) return;
        stopped_ = stopped;
    }
    wake_.notify_all();
}

void SessionSaver::submit(Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
        ++in_hand_;
    }
    wake_.notify_all();
}

std::vector<SessionSaver::Done> SessionSaver::take_done() {
    std::vector<Done> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.swap(done_);
    for (const Done& d : out) {
        if (d.seq != 0) --in_hand_;
    }
    return out;
}

std::size_t SessionSaver::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_hand_;
}

bool SessionSaver::pace(std::unique_lock<std::mutex>& lock) {
    if (!take_open_ || stopping_) return false;
    // A bump after the save arrived, not one already behind it: the next
    // checkpoint may be due any moment, and a commit must not sit in front
    // of it.
    const uint32_t at = checkpoints_();
    bool waited = false;
    while (take_open_ && !stopping_ && checkpoints_() == at) {
        waited = true;
        wake_.wait_for(lock, poll_);
    }
    if (waited) paced_.fetch_add(1, std::memory_order_acq_rel);
    return waited;
}

void SessionSaver::clean(Done& out) {
    std::string caution;
    folder_.clean(caution);
    cleanups_.fetch_add(1, std::memory_order_acq_rel);
    if (!caution.empty()) out.notice = caution;
}

void SessionSaver::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this]() {
            return stopping_ || !jobs_.empty() || (clean_pending_ && stopped_ && !take_open_);
        });
        if (!jobs_.empty()) {
            Job job = std::move(jobs_.front());
            jobs_.pop_front();
            Done d;
            d.client = job.client;
            d.seq = job.seq;
            d.paced = pace(lock);
            // Read with the pacing, under the lock: a take that opened after
            // the save began defers only its cleanup.
            const bool defer_clean = take_open_;
            lock.unlock();
            std::string why, caution;
            d.ok = folder_.save(job.file, job.bytes, job.document, why, caution);
            d.message = d.ok ? caution : why;
            // O4, after a durable save only: with the folder unsynced the old
            // session file can come back after a power cut, and its sequence
            // files must still be there.
            const bool durable = d.ok && caution.empty();
            if (durable && !defer_clean) {
                clean(d);
                // Said with the save's own reply, as a caution.
                d.message = std::move(d.notice);
                d.notice.clear();
            }
            // The document's states may be freed here, off the control thread.
            job = Job{};
            lock.lock();
            if (durable && defer_clean) clean_pending_ = true;
            // The file on disk may still be the one a deferred cleanup's save
            // replaced; the next durable save's cleanup covers it.
            if (!durable) clean_pending_ = false;
            done_.push_back(std::move(d));
            lock.unlock();
            if (ready_) ready_();
            lock.lock();
            continue;
        }
        if (clean_pending_ && stopped_ && !take_open_) {
            clean_pending_ = false;
            lock.unlock();
            Done d;
            clean(d);
            lock.lock();
            if (!d.notice.empty()) {
                done_.push_back(std::move(d));
                lock.unlock();
                if (ready_) ready_();
                lock.lock();
            }
            continue;
        }
        if (stopping_) return;
    }
}

}  // namespace sonora
