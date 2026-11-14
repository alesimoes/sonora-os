// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/ui_elastic/warp_markers.h"

#include <algorithm>

namespace sonora::ui {

namespace {

const api::WarpMarker kStart{0, 0};

// Fixed decimals by integer arithmetic: std::to_string(double) follows the
// locale's decimal point, which Qt sets from the environment.
std::string fixed(std::int64_t value, std::int64_t scale, int places) {
    std::string whole = std::to_string(value / scale);
    std::string frac = std::to_string(value % scale);
    frac.insert(0, static_cast<std::size_t>(places) - frac.size(), '0');
    return whole + "." + frac;
}

std::string seconds(std::int64_t frames, std::uint32_t rate) {
    if (rate == 0) return std::to_string(frames);
    const std::int64_t ms = (frames * 1000 + rate / 2) / rate;
    return fixed(ms, 1000, 3) + " s";
}

std::string marker_text(const api::WarpMarker& m) {
    return std::to_string(m.source) + ":" + std::to_string(m.target);
}

}  // namespace

void WarpMarkers::reset(std::uint64_t region, std::int64_t length) {
    region_ = region;
    length_ = length;
    markers_.clear();
}

bool WarpMarkers::source_fits(const api::WarpMarker* prev, const api::WarpMarker* next,
                              std::int64_t source, std::string& why) const {
    const api::WarpMarker& before = prev != nullptr ? *prev : kStart;
    if (source > length_) {
        why = "a warp marker sits inside the region; " + std::to_string(source) +
              " is past its end (" + std::to_string(length_) + " frames)";
        return false;
    }
    if (source <= before.source || (next != nullptr && source >= next->source)) {
        why = "there is a warp marker at " + std::to_string(source) +
              " or the region starts there; move that one instead";
        return false;
    }
    return true;
}

bool WarpMarkers::target_fits(const api::WarpMarker* prev, const api::WarpMarker* next,
                              api::WarpMarker m, std::string& why) const {
    const api::WarpMarker& before = prev != nullptr ? *prev : kStart;
    if (m.target <= before.target || (next != nullptr && m.target >= next->target)) {
        why = "a marker landing at " + std::to_string(m.target) +
              " would cross its neighbour and play time backwards; keep it between " +
              std::to_string(before.target) + " and " +
              (next != nullptr ? std::to_string(next->target) : std::string("the end"));
        return false;
    }
    // Only the last marker sets the length: what follows it keeps its speed.
    if (next == nullptr && m.target + (length_ - m.source) > api::kMaxElasticFrames) {
        why = "the stretched region would be longer than an elastic render holds in memory";
        return false;
    }
    return true;
}

bool WarpMarkers::add(std::int64_t source, std::int64_t target, std::string& why) {
    if (region_ == 0) {
        why = "select a region to place warp markers on";
        return false;
    }
    if (markers_.size() >= api::kMaxElasticMarkers) {
        why = "a region holds at most " + std::to_string(api::kMaxElasticMarkers) +
              " warp markers";
        return false;
    }
    const api::WarpMarker m{source, target};
    const auto at = std::lower_bound(
        markers_.begin(), markers_.end(), source,
        [](const api::WarpMarker& x, std::int64_t s) { return x.source < s; });
    const api::WarpMarker* prev = at == markers_.begin() ? nullptr : &*(at - 1);
    const api::WarpMarker* next = at == markers_.end() ? nullptr : &*at;
    if (!source_fits(prev, next, source, why) || !target_fits(prev, next, m, why)) return false;
    markers_.insert(at, m);
    return true;
}

bool WarpMarkers::move(std::size_t index, std::int64_t target, std::string& why) {
    if (index >= markers_.size()) {
        why = "there is no warp marker " + std::to_string(index + 1);
        return false;
    }
    const api::WarpMarker* prev = index == 0 ? nullptr : &markers_[index - 1];
    const api::WarpMarker* next = index + 1 < markers_.size() ? &markers_[index + 1] : nullptr;
    const api::WarpMarker m{markers_[index].source, target};
    if (!target_fits(prev, next, m, why)) return false;
    markers_[index] = m;
    return true;
}

bool WarpMarkers::remove(std::size_t index, std::string& why) {
    if (index >= markers_.size()) {
        why = "there is no warp marker " + std::to_string(index + 1);
        return false;
    }
    // Removing one never breaks the order of the rest.
    markers_.erase(markers_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

std::int64_t WarpMarkers::output_length() const {
    if (markers_.empty()) return length_;
    const api::WarpMarker& last = markers_.back();
    return last.target + (length_ - last.source);
}

double WarpMarkers::speed(std::size_t index) const {
    if (index >= markers_.size()) return 1.0;
    const api::WarpMarker& before = index == 0 ? kStart : markers_[index - 1];
    const api::WarpMarker& m = markers_[index];
    return static_cast<double>(m.target - before.target) /
           static_cast<double>(m.source - before.source);
}

bool WarpMarkers::words(api::ElasticAlgorithm algorithm, std::string& out,
                        std::string& why) const {
    if (markers_.empty()) {
        why = "place a warp marker first: a warp with none moves nothing";
        return false;
    }
    std::string list;
    for (const api::WarpMarker& m : markers_) {
        if (!list.empty()) list += ",";
        list += marker_text(m);
    }
    out = "elastic warp " + list + " " + api::elastic_algorithm_name(algorithm);
    return true;
}

std::vector<WarpMarkerRow> WarpMarkers::rows(std::uint32_t sample_rate) const {
    std::vector<WarpMarkerRow> out;
    for (std::size_t i = 0; i < markers_.size(); ++i) {
        const auto hundredths = static_cast<std::int64_t>(speed(i) * 100.0 + 0.5);
        out.push_back({seconds(markers_[i].source, sample_rate),
                       seconds(markers_[i].target, sample_rate), "x" + fixed(hundredths, 100, 2)});
    }
    return out;
}

}  // namespace sonora::ui
