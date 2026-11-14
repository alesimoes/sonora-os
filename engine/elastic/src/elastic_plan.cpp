// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/elastic/elastic_plan.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sonora::elastic {

namespace {

constexpr const char* kNothing = "; nothing was stretched";

struct Found {
    const model::TrackEdit* track = nullptr;
    const model::Playlist* playlist = nullptr;
    const model::Region* region = nullptr;
};

Found find_region(const model::Snapshot& now, model::Id id) {
    Found f;
    for (const auto& t : now.edit->tracks) {
        for (const auto& p : t->playlists) {
            for (const model::Region& r : p->regions) {
                if (r.id != id) continue;
                f.track = t.get();
                f.playlist = p.get();
                f.region = &r;
            }
        }
    }
    return f;
}

// The room the region has on its playlist: up to the next region's start.
model::Frames room_of(const model::Playlist& playlist, const model::Region& region) {
    model::Frames room = api::kMaxElasticFrames;
    for (const model::Region& r : playlist.regions) {
        if (r.id != region.id && r.position >= region.end()) {
            room = std::min(room, r.position - region.position);
        }
    }
    return room;
}

bool same_audio(const model::Region& a, const model::Region& b) {
    return a.position == b.position && a.length == b.length &&
           a.source_offset == b.source_offset && a.channels == b.channels &&
           a.gain_line == b.gain_line;
}

bool region_markers(const api::ElasticRequest& rq, model::Frames length,
                    std::vector<stretch::Marker>& out, std::string& why) {
    out.clear();
    if (rq.mode == api::ElasticMode::Ratio) {
        const model::Frames target = api::elastic_output_frames(rq, length);
        if (target < 1) {
            why = "a ratio of " + api::elastic_detail::format_micro(rq.ratio) +
                  " leaves nothing of a " + std::to_string(length) + "-frame region";
            return false;
        }
        out.push_back({length, target});
        return true;
    }
    for (const api::WarpMarker& m : rq.markers) out.push_back({m.source, m.target});
    return true;
}

// The level of the gain line at source frame `src`, linear in amplitude
// between points and held outside them, as the engine reads it.
float line_db_at(const std::vector<model::GainPoint>& line, model::Frames src) {
    if (src <= line.front().at) return line.front().db;
    if (src >= line.back().at) return line.back().db;
    std::size_t hi = 1;
    while (line[hi].at <= src) ++hi;
    const model::GainPoint& a = line[hi - 1];
    const model::GainPoint& b = line[hi];
    const double ga = std::pow(10.0, a.db / 20.0), gb = std::pow(10.0, b.db / 20.0);
    const double x = static_cast<double>(src - a.at) / static_cast<double>(b.at - a.at);
    return static_cast<float>(20.0 * std::log10(ga + x * (gb - ga)));
}

// The gain line in the copy's frames: each point moves with the audio under
// it; points before the copy fold into one at frame 0 holding their level,
// and points the stretch packs onto one frame keep the first.
std::vector<model::GainPoint> stretched_line(const ElasticPlan& plan,
                                             const stretch::WarpMap& map) {
    const std::vector<model::GainPoint>& line = plan.region.gain_line;
    std::vector<model::GainPoint> out;
    if (line.empty()) return out;
    if (plan.s0 > 0 && line.front().at < plan.s0) out.push_back({0, line_db_at(line, plan.s0)});
    for (const model::GainPoint& p : line) {
        if (p.at < plan.s0) continue;
        const double src = static_cast<double>(std::min(p.at - plan.s0, map.source_length()));
        const auto at = static_cast<model::Frames>(std::llround(target_at(map, src)));
        if (!out.empty() && at <= out.back().at) continue;
        out.push_back({at, p.db});
    }
    return out;
}

}  // namespace

bool elastic_check(const model::Snapshot& now, const api::ElasticRequest& request,
                   ElasticPlan& out, std::string& why) {
    const Found f = find_region(now, request.region);
    if (f.region == nullptr) {
        why = "region " + std::to_string(request.region) + " is not in the session" + kNothing;
        return false;
    }
    if (f.track->kind != model::TrackKind::Audio) {
        why = f.track->name + " is not an audio track" + kNothing;
        return false;
    }
    const model::Region& region = *f.region;
    if (!api::validate_elastic_request(request, region.length, why)) {
        why += kNothing;
        return false;
    }
    ElasticPlan plan;
    plan.request = request;
    plan.region = region;
    plan.playlist = f.playlist->id;
    plan.track = f.track->id;
    plan.track_name = f.track->name;
    plan.sample_rate = now.settings.sample_rate;
    plan.format = now.settings.audio_format;
    plan.max_length = room_of(*f.playlist, region);
    model::Frames limit = region.source_offset + region.length +
                          static_cast<model::Frames>(kElasticHandleSeconds * plan.sample_rate);
    for (const model::SourceRef& ref : region.channels) {
        const model::Source* src = now.sources->find(ref.source);
        if (src == nullptr) {
            why = "region \"" + region.name + "\": its source is not in the pool" + kNothing;
            return false;
        }
        plan.files.push_back({src->path, ref.channel});
        limit = std::min(limit, src->length);
    }
    if (plan.files.empty()) {
        why = "region \"" + region.name + "\" plays no file" + kNothing;
        return false;
    }
    const model::Frames handle =
        static_cast<model::Frames>(kElasticHandleSeconds * plan.sample_rate);
    plan.s0 = std::max<model::Frames>(0, region.source_offset - handle);
    plan.lead = region.source_offset - plan.s0;
    plan.tail = std::max<model::Frames>(0, limit - region.source_offset - region.length);
    const model::Source* first = now.sources->find(region.channels[0].source);
    plan.time_reference = std::max<std::int64_t>(0, first->time_reference + plan.s0);

    if (request.mode == api::ElasticMode::Quantize) {
        if (!plan.tempo.build(now.settings, why)) {
            why = "the tempo map cannot be read: " + why + kNothing;
            return false;
        }
    } else {
        if (!region_markers(request, region.length, plan.markers, why)) {
            why += kNothing;
            return false;
        }
        stretch::WarpMap map;
        if (!input_map(plan, plan.markers, map, why)) {
            why += kNothing;
            return false;
        }
        if (!fits_before_next(plan, api::elastic_output_frames(request, region.length), why)) {
            return false;
        }
    }
    out = std::move(plan);
    return true;
}

bool fits_before_next(const ElasticPlan& plan, model::Frames length, std::string& why) {
    if (length <= plan.max_length) return true;
    why = "the stretched region would be " + std::to_string(length) + " frames long and reach " +
          std::to_string(length - plan.max_length) +
          " frames into the next region; move that region or shorten the stretch" + kNothing;
    return false;
}

bool input_map(const ElasticPlan& plan, const std::vector<stretch::Marker>& markers,
               stretch::WarpMap& out, std::string& why) {
    const model::Frames total = plan.lead + plan.region.length + plan.tail;
    std::vector<stretch::Marker> all;
    all.reserve(markers.size() + 1);
    if (plan.lead > 0) all.push_back({plan.lead, plan.lead});
    for (const stretch::Marker& m : markers) {
        all.push_back({plan.lead + m.source, plan.lead + m.target});
    }
    // WarpMap holds no marker on the input's last frame (the region ends at
    // its source's end): the same span is a marker one frame earlier and a
    // last frame at its own speed.
    if (!all.empty() && all.back().source == total) {
        stretch::Marker& last = all.back();
        last.source -= 1;
        last.target -= 1;
        const bool fits = all.size() < 2 || (last.source > all[all.size() - 2].source &&
                                             last.target > all[all.size() - 2].target);
        if (!fits) {
            why = "the last two warp markers are one frame apart where the region's file ends";
            return false;
        }
    }
    return out.set(total, std::move(all), why);
}

double target_at(const stretch::WarpMap& map, double src) {
    src = std::clamp(src, 0.0, static_cast<double>(map.source_length()));
    stretch::Marker a{0, 0};
    for (const stretch::Marker& b : map.markers()) {
        if (src <= static_cast<double>(b.source)) {
            const double t = (src - static_cast<double>(a.source)) /
                             static_cast<double>(b.source - a.source);
            return static_cast<double>(a.target) + t * static_cast<double>(b.target - a.target);
        }
        a = b;
    }
    return static_cast<double>(a.target) + (src - static_cast<double>(a.source));
}

bool elastic_compound(const model::Snapshot& now, const ElasticPlan& plan,
                      const ElasticOutput& output, model::Compound& out, std::string& why) {
    const std::string kept = "; nothing was changed";
    const Found f = find_region(now, plan.region.id);
    if (f.region == nullptr || f.playlist->id != plan.playlist ||
        !same_audio(*f.region, plan.region)) {
        why = "the region was edited while it was stretched" + kept;
        return false;
    }
    if (output.region_length > room_of(*f.playlist, *f.region)) {
        why = "a region moved into the stretched region's way while it rendered" + kept;
        return false;
    }
    stretch::WarpMap map;
    if (!input_map(plan, output.markers, map, why)) return false;

    model::AddSource add;
    add.source = output.source;
    add.source.id = now.next_id;
    model::SetRegionSources sources;
    sources.playlist = plan.playlist;
    sources.region = plan.region.id;
    for (std::size_t c = 0; c < plan.files.size(); ++c) {
        sources.channels.push_back({add.source.id, static_cast<std::uint16_t>(c)});
    }
    sources.source_offset = plan.lead;
    sources.gain_line = stretched_line(plan, map);
    model::TrimRegion trim;
    trim.playlist = plan.playlist;
    trim.region = plan.region.id;
    trim.edge = model::Edge::End;
    trim.to = plan.region.position + output.region_length;

    model::Compound c;
    c.label = std::string("Elastic Audio: ") + api::elastic_algorithm_name(plan.request.algorithm);
    c.ops.push_back(std::move(add));
    // The new file holds the stretched span, the old one the original: each
    // must hold the region's span when it is pointed at.
    const bool shrinks = output.region_length < plan.region.length;
    if (shrinks) c.ops.push_back(trim);
    c.ops.push_back(std::move(sources));
    if (output.region_length > plan.region.length) c.ops.push_back(trim);
    out = std::move(c);
    return true;
}

}  // namespace sonora::elastic
