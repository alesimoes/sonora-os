// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/plan_exchange.h"

namespace sonora {

PlanExchange::~PlanExchange() {
    for (std::size_t i = 0; i < retired_count_; ++i) destroy_plan(retired_[i].plan);
    if (PlaybackPlan* p = plan_.load(std::memory_order_relaxed)) destroy_plan(p);
}

bool PlanExchange::publish(PlaybackPlan* next) {
    if (!can_publish()) return false;
    PlaybackPlan* const old = plan_.exchange(next, std::memory_order_seq_cst);
    const uint64_t s = blocks_.load(std::memory_order_seq_cst);
    if (old) retired_[retired_count_++] = Retired{old, s};
    return true;
}

std::size_t PlanExchange::reclaim(MayFree may_free, void* context) {
    return reclaim_where(false, may_free, context);
}

std::size_t PlanExchange::reclaim_after_deactivate(MayFree may_free, void* context) {
    return reclaim_where(true, may_free, context);
}

std::size_t PlanExchange::reclaim_where(bool stalled, MayFree may_free,
                                        void* context) {
    const uint64_t now = blocks_.load(std::memory_order_seq_cst);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < retired_count_; ++i) {
        const Retired r = retired_[i];
        const bool ended = stalled || now != r.blocks_at_retire;
        if (ended && (!may_free || may_free(context, r.plan))) {
            destroy_plan(r.plan);
        } else {
            retired_[kept++] = r;
        }
    }
    retired_count_ = kept;
    return kept;
}

bool PlanExchange::is_retired(const PlaybackPlan* plan) const {
    for (std::size_t i = 0; i < retired_count_; ++i) {
        if (retired_[i].plan == plan) return true;
    }
    return false;
}

}  // namespace sonora
