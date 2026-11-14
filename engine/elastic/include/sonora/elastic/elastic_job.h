// SPDX-License-Identifier: GPL-3.0-or-later
//
// One elastic render on a worker thread (ADR-0136 §2), as ConsolidateJob
// runs a consolidate: the region's span and its handles are read, stretched
// and written to "Audio Files/<track>_elastic_NN.wav", created exclusively
// and synced; the take is only read. The control thread starts it, reads its
// progress each turn and puts the copy in the document (ElasticService).
// Never the audio thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "sonora/elastic/elastic_plan.h"
#include "sonora/elastic/onset_source.h"
#include "sonora/stretch/stretcher.h"

namespace sonora::elastic {

// A stretcher for one render at `sample_rate`. One that works in blocks
// stops early, failing, once `cancel` is set.
using StretcherFactory = std::function<std::unique_ptr<stretch::Stretcher>(
    std::uint32_t sample_rate, const std::atomic<bool>& cancel)>;

// Reads, plans quantize's markers, stretches and writes the copy. False,
// with the reason, on any failure or once `cancel` is set; the copy is then
// removed, so nothing is left in "Audio Files/". `permille` follows it.
bool render_elastic(const ElasticPlan& plan, const std::string& session_dir,
                    const StretcherFactory& stretcher, OnsetSource* onsets,
                    const std::atomic<bool>& cancel, std::atomic<std::uint32_t>& permille,
                    ElasticOutput& out, std::string& why);

class ElasticJob {
public:
    ElasticJob(StretcherFactory stretcher, std::unique_ptr<OnsetSource> onsets);
    ElasticJob(const ElasticJob&) = delete;
    ElasticJob& operator=(const ElasticJob&) = delete;
    // Cancels and joins, so a render never outlives the server, and removes
    // a copy that finished and was never placed.
    ~ElasticJob();

    struct Result {
        bool ok = false;
        bool cancelled = false;
        ElasticPlan plan;
        std::string session_dir;
        ElasticOutput output;
        std::string error;
    };

    // True from start() until collect(). The control thread's.
    bool active() const { return active_; }
    // Only when not active().
    void start(ElasticPlan plan, std::string session_dir);
    void cancel() { cancel_.store(true, std::memory_order_relaxed); }
    std::uint32_t permille() const { return permille_.load(std::memory_order_relaxed); }
    // The permille last announced, so a long render is not a flood of events.
    std::uint32_t& said() { return said_; }
    bool finished() const { return finished_.load(std::memory_order_acquire); }
    // Joins the worker and hands over what it made. Only when finished().
    Result collect();
    // Cancels, joins and removes whatever was written. True when one ran.
    bool abandon();

private:
    StretcherFactory stretcher_;
    std::unique_ptr<OnsetSource> onsets_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> finished_{false};
    std::atomic<std::uint32_t> permille_{0};
    std::uint32_t said_ = 0;
    bool active_ = false;
    Result result_;
};

// Removes the copy a result names, if it wrote one.
void remove_copy(const ElasticJob::Result& result);

}  // namespace sonora::elastic
