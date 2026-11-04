// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import Audio (ui.editing.import), without Qt: the files chosen are
// converted to the session's rate and copied into its "Audio Files/" as BWF
// on a worker thread, and then, on the interface thread, placed as one
// Compound -- a new track per file, its source, its region -- so one undo
// step takes the placement back. The sources are a pool outside undo
// (timeline-model.md), so undo leaves the copies on disk, as it does a take.
//
// The same shape as the engine's Consolidate: the file is written first and
// named by the AddSource that follows. A file written but not placed (the
// model refused, the session changed while it converted, the operator
// cancelled) is deleted, never left for the recovery offer to find.
//
// The worker never touches the controller or the document: everything it
// needs is copied in start(), and its results are read only after it has
// said it is done.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "edit_controller.h"
#include "sonora/audio_import.h"

namespace sonora::ui {

// The most channels one imported file may have: a track is mono or stereo.
constexpr int kImportMaxChannels = 2;

class AudioImporter {
public:
    enum class State { Idle, Running, Done, Failed, Cancelled };

    explicit AudioImporter(EditController& ctl) : ctl_(ctl) {}
    ~AudioImporter();
    AudioImporter(const AudioImporter&) = delete;
    AudioImporter& operator=(const AudioImporter&) = delete;

    // Checks every file (readable WAV, 1 or 2 channels) and starts copying
    // them into `<folder>/Audio Files/`. Refused, nothing started, with
    // `why` naming the file that is the problem.
    bool start(const std::vector<std::string>& files, const std::string& folder,
               bool at_insertion, std::string& why);

    State state() const { return state_.load(std::memory_order_acquire); }
    // 0 to 1000 across all the files.
    int progress() const;
    // The file being converted, for the status text.
    std::string current() const;
    // Why the last import failed; empty after a cancel.
    const std::string& failure() const { return failure_; }

    // Asks the worker to stop; it deletes what it wrote. Done when state()
    // leaves Running.
    void cancel() { cancel_.store(true, std::memory_order_relaxed); }

    // Once state() is Done, on the interface thread: names the tracks, and
    // submits the Compound. `folder` is where the session is now; if it or
    // the session's rate is not what start() saw, the copies are deleted and
    // nothing is placed. Always leaves the importer Idle.
    bool commit(const std::string& folder, std::string& why);
    // Deletes anything a Failed or Cancelled run left and goes Idle.
    void reset();

    // How many tracks one import may make (a Compound's member limit).
    static std::size_t most_files();

private:
    struct Item {
        std::string src;
        std::string stem;   // the region's and the track's name
        std::string rel;    // "Audio Files/<stem>.wav"
        std::string part;   // absolute, while being written
        std::string final_path;
        uint16_t channels = 0;
        int64_t frames = 0;
        bool floating = false;
        int64_t time_reference = 0;
        bool written = false;
    };

    void run();
    void discard_files();

    EditController& ctl_;
    std::vector<Item> items_;
    std::string folder_;
    uint32_t rate_ = 0;
    bool at_insertion_ = false;
    model::Frames insertion_ = 0;

    std::thread worker_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> cancel_{false};
    std::atomic<int> item_progress_{0};
    std::atomic<std::size_t> item_index_{0};
    std::string failure_;
};

}  // namespace sonora::ui
