#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The "Split into stems" dialog state (logic.stem_split, ADR-0223). Qt-free.
// The split runs on a worker thread through StemRenderer; the audio thread is
// never involved. Tracks are added on the caller's thread by finish(), and only
// when the whole render succeeded, so a cancel or an error adds nothing.
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sonora/stems/stem_render.h"

namespace sonora::ui_stems {

enum class Method { HarmonicPercussive, VocalCentre };
const char* method_label(Method m);
// Shown beside the choice: these are not ML separators.
const char* method_caveat(Method m);

// A copy of the region audio, taken before the worker starts, so the worker
// never touches the live clip.
struct SourceAudio {
    std::vector<std::vector<float>> channels;  // channels[ch][frame]
    int sample_rate = 0;
    std::string name;  // the source track name, base of the new track names
};

// Where the new tracks go: beside the source. Never edits the source clip.
class TrackImporter {
public:
    virtual ~TrackImporter() = default;
    virtual bool import_stems(const std::vector<stems::RenderedStem>& stems, std::string* err) = 0;
};

using SeparatorFactory = std::function<std::unique_ptr<stems::StemSeparator>(Method, int channels)>;
SeparatorFactory default_separator_factory();

enum class Phase { Idle, Running, Done, Cancelled, Failed };

class StemSplitModel {
public:
    StemSplitModel(stems::StemFileWriter& writer, SeparatorFactory factory)
        : writer_(writer), factory_(std::move(factory)) {}
    ~StemSplitModel();

    // New dialog session: forgets the last result, notice and progress.
    void reset();
    void set_method(Method m) { method_ = m; }
    Method method() const { return method_; }

    // Starts the worker. False, with a held notice, when it cannot (busy, no
    // audio, a mono source for the centre method, no directory).
    bool start(SourceAudio source, const std::string& dest_dir, TrackImporter& importer);
    void cancel() { cancel_.store(true); }
    // 0..1, safe from the UI thread while the worker runs.
    double progress() const { return progress_.load(); }
    bool running() const { return phase_.load() == Phase::Running; }
    // Joins the worker, then imports on success. Returns the final phase.
    Phase finish();

    Phase phase() const { return phase_.load(); }
    int tracks_added() const { return tracks_added_; }
    // Held until dismissed: a refusal or failure the operator must read.
    const std::string& notice() const { return notice_; }
    void dismiss_notice() { notice_.clear(); }
    std::string status() const;

private:
    void work(SourceAudio source, std::string dest_dir);
    bool refuse(const std::string& why);

    stems::StemFileWriter& writer_;
    SeparatorFactory factory_;
    Method method_ = Method::HarmonicPercussive;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<double> progress_{0.0};
    std::atomic<Phase> phase_{Phase::Idle};
    TrackImporter* importer_ = nullptr;
    std::vector<stems::RenderedStem> rendered_;
    std::string error_;
    std::string notice_;
    int tracks_added_ = 0;
};

}  // namespace sonora::ui_stems
