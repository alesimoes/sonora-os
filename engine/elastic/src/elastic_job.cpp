// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <exception>
#include <new>
#include <utility>

#include "sonora/elastic/elastic_job.h"

namespace sonora::elastic {

void remove_copy(const ElasticJob::Result& result) {
    if (!result.ok || result.output.source.path.empty()) return;
    const std::string full = result.session_dir + "/" + result.output.source.path;
    std::remove(full.c_str());
}

ElasticJob::ElasticJob(StretcherFactory stretcher, std::unique_ptr<OnsetSource> onsets)
    : stretcher_(std::move(stretcher)), onsets_(std::move(onsets)) {}

ElasticJob::~ElasticJob() { abandon(); }

void ElasticJob::start(ElasticPlan plan, std::string session_dir) {
    cancel_.store(false, std::memory_order_relaxed);
    finished_.store(false, std::memory_order_relaxed);
    permille_.store(0, std::memory_order_relaxed);
    said_ = 0;
    active_ = true;
    result_ = Result{};
    result_.plan = std::move(plan);
    result_.session_dir = std::move(session_dir);
    // The worker owns result_ until finished_ says otherwise.
    worker_ = std::thread([this]() {
        Result& r = result_;
        try {
            r.ok = render_elastic(r.plan, r.session_dir, stretcher_, onsets_.get(), cancel_,
                                  permille_, r.output, r.error);
        } catch (const std::bad_alloc&) {
            r.ok = false;
            r.error = "not enough memory to hold the region while it is stretched";
        } catch (const std::exception& e) {
            r.ok = false;
            r.error = std::string("the stretch failed: ") + e.what();
        }
        r.cancelled = cancel_.load(std::memory_order_relaxed);
        finished_.store(true, std::memory_order_release);
    });
}

ElasticJob::Result ElasticJob::collect() {
    if (worker_.joinable()) worker_.join();
    active_ = false;
    Result r = std::move(result_);
    result_ = Result{};
    return r;
}

bool ElasticJob::abandon() {
    if (!active_) return false;
    cancel();
    remove_copy(collect());
    return true;
}

}  // namespace sonora::elastic
