// SPDX-License-Identifier: GPL-3.0-or-later
// GetState requests in flight (plugin_hosting_state.h). Control thread only.
#include "sonora/plugin_hosting_state.h"

namespace sonora {

void PluginStateRequests::add(uint32_t id, Request r) {
    r.sent = 0;
    r.armed = false;
    r.deadline_ms = 0;
    requests_[id] = std::move(r);
}

bool PluginStateRequests::copy_out() const {
    for (const auto& [id, r] : requests_)
        if (r.copies && r.sent != 0) return true;
    return false;
}

std::vector<std::pair<uint32_t, std::string>> PluginStateRequests::due(uint64_t now_ms) {
    std::vector<std::pair<uint32_t, std::string>> out;
    bool copying = copy_out();
    for (auto& [id, r] : requests_) {
        if (r.sent != 0) continue;
        if (r.copies) {
            if (copying) continue;
            copying = true;
        }
        r.sent = next_sent_++;
        out.emplace_back(id, std::move(r.frame));
        r.frame.clear();
        arm_next(r.track, now_ms);
    }
    return out;
}

void PluginStateRequests::arm_next(model::Id track, uint64_t now_ms) {
    Request* first = nullptr;
    for (auto& [id, r] : requests_) {
        if (r.track != track || r.sent == 0) continue;
        if (r.armed) return;  // one is already being served
        if (!first || r.sent < first->sent) first = &r;
    }
    if (!first) return;
    first->armed = true;
    first->deadline_ms = now_ms + uint64_t(first->timeout_ms);
}

bool PluginStateRequests::answered(uint32_t id, model::Id track, uint64_t now_ms, Request& out) {
    const auto it = requests_.find(id);
    if (it == requests_.end() || it->second.track != track || it->second.sent == 0) return false;
    out = std::move(it->second);
    requests_.erase(it);
    arm_next(track, now_ms);
    return true;
}

PluginStateRequests::Request PluginStateRequests::remove(uint32_t id, uint64_t now_ms) {
    Request out;
    const auto it = requests_.find(id);
    if (it == requests_.end()) return out;
    out = std::move(it->second);
    requests_.erase(it);
    arm_next(out.track, now_ms);
    return out;
}

std::vector<PluginStateRequests::Request> PluginStateRequests::remove_track(model::Id track) {
    std::vector<Request> out;
    for (auto it = requests_.begin(); it != requests_.end();) {
        if (it->second.track != track) {
            ++it;
            continue;
        }
        out.push_back(std::move(it->second));
        it = requests_.erase(it);
    }
    return out;
}

std::vector<PluginStateRequests::Request> PluginStateRequests::late(uint64_t now_ms) const {
    // At most one is armed per track, so each track is listed once.
    std::vector<Request> out;
    for (const auto& [id, r] : requests_)
        if (r.armed && now_ms >= r.deadline_ms) out.push_back(r);
    return out;
}

bool PluginStateRequests::flush_pending(uint64_t flush) const {
    for (const auto& [id, r] : requests_)
        if (r.flush == flush) return true;
    return false;
}

const PluginStateRequests::Request* PluginStateRequests::find(uint32_t id) const {
    const auto it = requests_.find(id);
    return it == requests_.end() ? nullptr : &it->second;
}

}  // namespace sonora
