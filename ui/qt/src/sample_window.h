// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sample view's samples from the session's WAVs (ADR-0096 §2): a window
// of frames around what the view last asked for, read through
// common/audiofile on this class's own worker thread, never on the GUI
// thread and never on the engine's audio thread. read() answers from memory
// only; while the window is still loading it answers silence and ready() is
// false, so the view says "loading" and the Pencil refuses rather than draw
// over zeros that are not the audio.
//
// Header-only on purpose: ui/qt's CMake files are held by other features
// (ADR-0096 §7). A follow-up moves the body to sample_window.cpp.
#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "model_access.h"
#include "sample_view.h"
#include "sonora/wav_reader.h"

namespace sonora::ui {

// Frames one window holds: about 5.5 s at 48 kHz, 2 MB for a stereo float
// window. A view at kSampleViewMaxFpp across a 4000 px display asks for
// 16000 frames, so scrolling a few screens either way needs no reload. The
// load speed from a USB stick that would tune this is unmeasured (ADR-0096
// "Unverified").
inline constexpr model::Frames kSampleWindowFrames = 1 << 18;
// Sources held at once. A view at sample zoom shows a few clips; one evicted
// is read again when it is next drawn.
inline constexpr std::size_t kSampleWindowSources = 16;

class FileSampleSource final : public SampleSource {
public:
    struct Located {
        std::string wav;  // absolute
        model::Frames length = 0;
    };
    // Runs on the GUI thread, inside read() and ready(); false for a source
    // it cannot place (no session folder, no such source).
    using Locate = std::function<bool(model::Id, Located&)>;

    // Where a source of the session lives: its path is relative to the
    // session folder, for a recorded take and for the copy a sample.edit
    // made in "Audio Files/" alike, the way FilePeakSource's locate places
    // one (main.cpp).
    static bool locate_in(const model::SourcePool& pool, const std::string& folder, model::Id id,
                          Located& out) {
        if (folder.empty()) return false;
        const model::Source* s = pool.find(id);
        if (s == nullptr) return false;
        out.wav = folder + "/" + s->path;
        out.length = s->length;
        return true;
    }

    // `on_ready` is called from the worker when a window lands: the caller
    // queues a redraw onto its own thread.
    FileSampleSource(Locate locate, std::function<void()> on_ready)
        : locate_(std::move(locate)), on_ready_(std::move(on_ready)), worker_([this] { work(); }) {}

    ~FileSampleSource() override {
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }

    FileSampleSource(const FileSampleSource&) = delete;
    FileSampleSource& operator=(const FileSampleSource&) = delete;

    void read(model::Id source, uint16_t channel, model::Frames first, int count,
              float* out) const override {
        for (int i = 0; i < count; ++i) out[i] = 0.0f;
        if (count <= 0) return;
        const std::shared_ptr<Entry> e = entry(source);
        if (e == nullptr) return;
        std::lock_guard<std::mutex> lock(m_);
        e->used = ++clock_;
        const model::Frames a = std::max<model::Frames>(first, 0);
        const model::Frames b = std::min<model::Frames>(first + count, e->where.length);
        if (b <= a) return;
        e->want_first = a;
        e->want_end = b;
        e->asked = true;
        if (e->failed) return;
        if (!covers(*e)) {
            request(e);
            return;
        }
        if (channel >= e->channels) return;
        for (model::Frames f = a; f < b; ++f) {
            const std::size_t at =
                static_cast<std::size_t>(f - e->have_first) * e->channels + channel;
            out[f - first] = e->data[at];
        }
    }

    bool ready(model::Id source) const override {
        const std::shared_ptr<Entry> e = entry(source);
        if (e == nullptr) return false;
        std::lock_guard<std::mutex> lock(m_);
        return e->asked && !e->failed && covers(*e);
    }

    // Blocks until the worker has nothing queued. For the tests: an event,
    // not a clock.
    void wait_idle() const {
        std::unique_lock<std::mutex> lock(m_);
        idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
    }

private:
    struct Entry {
        Located where;
        // What read() last asked for, clamped to the source.
        model::Frames want_first = 0, want_end = 0;
        bool asked = false;
        // The window loaded: interleaved frames [have_first, have_end).
        model::Frames have_first = 0, have_end = 0;
        uint16_t channels = 0;
        std::vector<float> data;
        // The window the next load reads.
        model::Frames load_first = 0, load_end = 0;
        bool queued = false;
        // The file did not open, or is not the length the session says: it
        // stays silent and not ready, never drawn as if it were the audio.
        bool failed = false;
        uint64_t used = 0;
    };

    static bool covers(const Entry& e) {
        return e.have_end > e.have_first && e.have_first <= e.want_first &&
               e.want_end <= e.have_end;
    }

    std::shared_ptr<Entry> entry(model::Id source) const {
        Located where;
        if (!locate_ || !locate_(source, where)) return nullptr;
        std::lock_guard<std::mutex> lock(m_);
        auto it = entries_.find(source);
        // Another session's source with the same id, or a take that grew.
        if (it != entries_.end() &&
            (it->second->where.wav != where.wav || it->second->where.length != where.length)) {
            entries_.erase(it);
            it = entries_.end();
        }
        if (it == entries_.end()) {
            evict();
            auto e = std::make_shared<Entry>();
            e->where = std::move(where);
            e->used = ++clock_;
            it = entries_.emplace(source, std::move(e)).first;
        }
        return it->second;
    }

    // Under m_. The least recently drawn source goes, unless it is loading.
    void evict() const {
        if (entries_.size() < kSampleWindowSources) return;
        auto oldest = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second->queued) continue;
            if (oldest == entries_.end() || it->second->used < oldest->second->used) oldest = it;
        }
        if (oldest != entries_.end()) entries_.erase(oldest);
    }

    // Under m_. A window centred on what was asked, at least twice its width.
    void request(const std::shared_ptr<Entry>& e) const {
        const model::Frames asked = e->want_end - e->want_first;
        const model::Frames size = std::max<model::Frames>(kSampleWindowFrames, 2 * asked);
        const model::Frames mid = e->want_first + asked / 2;
        const model::Frames first =
            std::clamp<model::Frames>(mid - size / 2, 0, std::max<model::Frames>(0, e->where.length - size));
        e->load_first = first;
        e->load_end = std::min<model::Frames>(e->where.length, first + size);
        if (e->queued) return;
        e->queued = true;
        queue_.push_back(e);
        cv_.notify_one();
    }

    void work() {
        for (;;) {
            std::shared_ptr<Entry> e;
            Located where;
            model::Frames first = 0, end = 0;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_) return;
                e = queue_.front();
                queue_.erase(queue_.begin());
                e->queued = false;
                where = e->where;
                first = e->load_first;
                end = e->load_end;
                busy_ = true;
            }
            std::vector<float> data;
            uint16_t channels = 0;
            const bool ok = load(where, first, end, data, channels);
            {
                std::lock_guard<std::mutex> lock(m_);
                if (ok) {
                    e->data = std::move(data);
                    e->channels = channels;
                    e->have_first = first;
                    e->have_end = end;
                } else {
                    e->failed = true;
                }
            }
            // Redraw is requested before idle is announced, so a waiter that
            // sees idle also sees the redraw.
            if (ok && on_ready_) on_ready_();
            {
                std::lock_guard<std::mutex> lock(m_);
                busy_ = false;
            }
            idle_.notify_all();
        }
    }

    // On the worker. A frame past what the file gave is silence, as read()
    // documents for a frame outside the source.
    static bool load(const Located& where, model::Frames first, model::Frames end,
                     std::vector<float>& data, uint16_t& channels) {
        WavReader reader;
        std::string why;
        if (!reader.open(where.wav, why) || reader.frames() != where.length ||
            reader.channels() == 0) {
            return false;
        }
        channels = reader.channels();
        const model::Frames frames = std::max<model::Frames>(0, end - first);
        data.assign(static_cast<std::size_t>(frames) * channels, 0.0f);
        if (frames == 0) return true;
        return reader.read(first, frames, data.data()) >= 0;
    }

    Locate locate_;
    std::function<void()> on_ready_;
    mutable std::mutex m_;
    mutable std::condition_variable cv_, idle_;
    mutable std::map<model::Id, std::shared_ptr<Entry>> entries_;
    mutable std::vector<std::shared_ptr<Entry>> queue_;
    mutable uint64_t clock_ = 0;
    mutable bool busy_ = false;
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace sonora::ui
