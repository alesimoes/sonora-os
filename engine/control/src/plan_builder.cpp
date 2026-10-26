// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/plan_builder.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "sonora/click.h"
#include "sonora/model/musical_map.h"

namespace sonora {
namespace {

std::string str(uint64_t v) { return std::to_string(v); }

FadeCurve curve(model::FadeShape s) { return static_cast<FadeCurve>(static_cast<uint8_t>(s)); }

static_assert(static_cast<uint8_t>(model::FadeShape::Linear) ==
                  static_cast<uint8_t>(FadeCurve::Linear) &&
              static_cast<uint8_t>(model::FadeShape::EqualPower) ==
                  static_cast<uint8_t>(FadeCurve::EqualPower) &&
              static_cast<uint8_t>(model::FadeShape::SCurve) ==
                  static_cast<uint8_t>(FadeCurve::SCurve),
              "the builder casts one to the other");

const model::Playlist* active_playlist(const model::TrackEdit& t) {
    for (const auto& p : t.playlists) {
        if (p->id == t.active_playlist) return p.get();
    }
    return nullptr;
}

// Chunks a window starting at `p` needs, summed over every track: per
// segment overlapping it, channels x (ceil(overlap / C) + 1).
uint64_t demand_at(const std::vector<std::vector<PlanSegment>>& tracks, int64_t p, int64_t w,
                   uint32_t chunk) {
    uint64_t total = 0;
    for (const auto& segs : tracks) {
        // Ends never decrease along a track: the first that ends after p.
        auto it = std::partition_point(segs.begin(), segs.end(),
                                       [p](const PlanSegment& s) { return s.end <= p; });
        for (; it != segs.end() && it->start < p + w; ++it) {
            const int64_t overlap = std::min(p + w, it->end) - std::max(p, it->start);
            if (overlap <= 0) continue;
            total += static_cast<uint64_t>(it->channels) *
                     (static_cast<uint64_t>((overlap + chunk - 1) / chunk) + 1);
        }
    }
    return total;
}

}  // namespace

std::vector<ClickSegment> click_segments(const model::SessionSettings& settings) {
    std::vector<ClickSegment> out;
    model::MusicalMap map;
    std::string why;
    if (!map.build(settings, why)) return out;
    const auto& entries = map.entries();
    const std::size_t reachable = map.reachable();
    out.reserve(reachable);
    for (std::size_t k = 0; k < reachable; ++k) {
        const model::MusicalMap::Entry& e = entries[k];
        const model::Ticks bt = e.beat_ticks;
        if (bt <= 0 || e.beats == 0) continue;
        // A tempo change alone carries the bar in progress on (session-
        // format.md, "How the map is counted"), so its first beat is the
        // next on that bar's grid; a meter change starts a bar at its tick.
        const model::Ticks into = e.tick - e.bar_tick;
        const model::Ticks first = e.bar_tick + (into + bt - 1) / bt * bt;
        ClickSegment c;
        c.at = e.at;
        c.first_tick = static_cast<uint64_t>(first - e.tick);
        c.beat_ticks = static_cast<uint64_t>(bt);
        c.num = e.num;
        c.den = e.den;
        c.beats_per_bar = e.beats;
        c.first_beat = static_cast<uint32_t>(((first - e.bar_tick) / bt) % e.beats);
        if (k + 1 < reachable) {
            const model::Ticks end = entries[k + 1].tick;
            c.beats = first < end ? static_cast<uint64_t>((end - first + bt - 1) / bt) : 0;
        } else {
            c.beats = first <= model::kMaxMusical
                          ? static_cast<uint64_t>((model::kMaxMusical - first) / bt + 1)
                          : 0;
        }
        c.first_frame = c.beats != 0 ? click_beat_frame(c, 0) : e.at;
        out.push_back(c);
    }
    return out;
}

float pan_exponent_for(float pan_law_db) {
    if (!(pan_law_db >= model::kMinPanLawDb)) pan_law_db = model::kMinPanLawDb;
    if (pan_law_db > 0.0f) pan_law_db = 0.0f;
    // 0.5 ^ e = 10 ^ (law / 20)
    return static_cast<float>(pan_law_db / (20.0 * std::log10(0.5)));
}

float gain_of_db(float db) {
    if (!(db >= model::kMinGainDb)) db = model::kMinGainDb;
    if (db > model::kMaxGainDb) db = model::kMaxGainDb;
    return std::pow(10.0f, db / 20.0f);
}

bool build_plan(const model::Snapshot& snapshot, const BuildLimits& limits,
                const SourceSlot& source_slot, uint64_t epoch, PlaybackPlan*& out,
                PlanReport& report, std::string& error) {
    PlanContinuity continuity(limits.delay_lines);
    return build_plan(snapshot, limits, source_slot, ChainResolver(), continuity, epoch, out,
                      report, error);
}

bool build_plan(const model::Snapshot& snapshot, const BuildLimits& limits,
                const SourceSlot& source_slot, const ChainResolver& chains,
                PlanContinuity& continuity, uint64_t epoch, PlaybackPlan*& out,
                PlanReport& report, std::string& error) {
    out = nullptr;
    report = PlanReport{};
    const auto& tracks = snapshot.edit->tracks;
    // The audio thread divides by it.
    if (limits.chunk_frames == 0) {
        error = "the playback chunk size is 0";
        return false;
    }
    if (tracks.size() > limits.engine_tracks) {
        error = "the session has " + str(tracks.size()) + " tracks; the engine was created with " +
                str(limits.engine_tracks) + " (limit: tracks)";
        return false;
    }

    // Per engine track, flattened, in timeline order.
    std::vector<std::vector<PlanSegment>> per_track(limits.engine_tracks);
    std::vector<uint16_t> track_channels(limits.engine_tracks,
                                         static_cast<uint16_t>(limits.channels_per_track));
    std::unordered_map<model::Id, uint32_t> slot_of;
    for (std::size_t ti = 0; ti < tracks.size(); ++ti) {
        const model::TrackEdit& t = *tracks[ti];
        const std::string where = "track \"" + t.name + "\"";
        if (t.kind != model::TrackKind::Audio) {
            // Its engine track plays silence, through the instrument
            // track's inserts if it has any, until MIDI reaches the audio
            // thread (ADR-0014 M-3) and instruments are hosted (M-5).
            if (t.kind == model::TrackKind::Instrument) track_channels[ti] = t.channels;
            ++report.silent_midi_tracks;
            continue;
        }
        if (t.channels < 1 || t.channels > 2) {
            error = where + " has " + str(t.channels) +
                    " channels; Phase 2 plays mono and stereo tracks";
            return false;
        }
        track_channels[ti] = t.channels;
        const model::Playlist* pl = active_playlist(t);
        if (!pl) {
            error = where + ": its active playlist does not exist";
            return false;
        }
        std::unordered_map<model::Id, std::size_t> index;
        std::vector<PlanSegment>& segs = per_track[ti];
        std::vector<bool> kept;
        for (const model::Region& r : pl->regions) {
            PlanSegment s;
            s.start = r.position;
            s.end = r.end();
            s.source_offset = r.source_offset;
            s.channels = t.channels;
            s.gain = gain_of_db(r.gain_db);
            s.fade_in_end = r.position + r.fade_in.length;
            s.fade_in_shape = curve(r.fade_in.shape);
            s.fade_out_start = r.end() - r.fade_out.length;
            s.fade_out_shape = curve(r.fade_out.shape);
            if (r.channels.size() != t.channels) {
                error = where + ", region " + str(r.id) + ": " + str(r.channels.size()) +
                        " source channels for a " + str(t.channels) + "-channel track";
                return false;
            }
            for (uint16_t c = 0; c < t.channels; ++c) {
                const model::Source* src = snapshot.sources->find(r.channels[c].source);
                if (!src) {
                    error = where + ", region " + str(r.id) + ": source " +
                            str(r.channels[c].source) + " is not in the pool";
                    return false;
                }
                auto it = slot_of.find(src->id);
                if (it == slot_of.end()) {
                    uint32_t slot = 0;
                    if (!source_slot(*src, slot, error)) return false;
                    it = slot_of.emplace(src->id, slot).first;
                }
                s.source_slot[c] = it->second;
                s.source_channel[c] = r.channels[c].channel;
            }
            index[r.id] = segs.size();
            segs.push_back(s);
            kept.push_back(!r.mute);
        }
        for (const model::Crossfade& x : pl->crossfades) {
            const auto l = index.find(x.left);
            const auto r = index.find(x.right);
            if (l == index.end() || r == index.end()) {
                error = where + ": a crossfade names region " + str(x.left) + " or " +
                        str(x.right) + ", which the playlist does not hold";
                return false;
            }
            model::Frames before = 0;
            model::Frames after = 0;
            model::crossfade_extent(x, before, after);
            PlanSegment& left = segs[l->second];
            PlanSegment& right = segs[r->second];
            const int64_t cut = right.start;
            left.end = cut + after;
            left.fade_out_start = cut - before;
            left.fade_out_shape = curve(x.shape);
            right.start = cut - before;
            right.source_offset -= before;
            right.fade_in_end = cut + after;
            right.fade_in_shape = curve(x.shape);
        }
        std::vector<PlanSegment> playing;
        for (std::size_t i = 0; i < segs.size(); ++i) {
            if (kept[i]) playing.push_back(segs[i]);
        }
        segs.swap(playing);
        for (std::size_t i = 0; i < segs.size(); ++i) {
            // The audio thread indexes the chunk buffer from source_offset
            // unchecked; a crossfade has just moved it back by its length.
            if (segs[i].source_offset < 0 || segs[i].end <= segs[i].start) {
                error = where + ": a segment at " + str(segs[i].start) +
                        " starts before its source or is empty";
                return false;
            }
            if (i == 0) continue;
            // What the audio thread's cursor and binary search rely on.
            if (segs[i].start < segs[i - 1].start || segs[i].end < segs[i - 1].end) {
                error = where + ": segments out of order at " + str(segs[i].start);
                return false;
            }
        }
    }

    // Chunk slots: per segment and channel, one per source chunk it spans.
    const uint32_t chunk = limits.chunk_frames;
    uint64_t segment_total = 0;
    uint64_t slot_total = 0;
    for (auto& segs : per_track) {
        for (PlanSegment& s : segs) {
            const int64_t last_frame = s.source_offset + (s.end - s.start) - 1;
            const auto first = static_cast<uint32_t>(s.source_offset / chunk);
            const auto count = static_cast<uint32_t>(last_frame / chunk - first + 1);
            for (uint16_t c = 0; c < s.channels; ++c) {
                s.first_chunk_slot[c] = static_cast<uint32_t>(slot_total);
                s.first_chunk_index[c] = first;
                s.chunk_count[c] = count;
                slot_total += count;
            }
        }
        segment_total += segs.size();
    }
    if (segment_total > limits.max_segments) {
        error = "the plan needs " + str(segment_total) + " segments; the session allows " +
                str(limits.max_segments) + " (limit: max_segments)";
        return false;
    }
    if (slot_total > limits.max_chunk_slots) {
        error = "the plan needs " + str(slot_total) + " chunk slots; the session allows " +
                str(limits.max_chunk_slots) + " (limit: max_chunk_slots)";
        return false;
    }

    // A track with no active insert keeps the fused TrackOut (ADR-0012 §7.1).
    std::vector<ChainBinding> bindings(limits.engine_tracks);
    for (uint32_t ti = 0; ti < limits.engine_tracks; ++ti) {
        bindings[ti].stable_key = kSpareTrackKeyTag | (ti + 1);
    }
    uint32_t active_chains = 0;
    for (std::size_t ti = 0; ti < tracks.size(); ++ti) {
        const model::TrackEdit& t = *tracks[ti];
        ChainBinding& b = bindings[ti];
        auto key = continuity.track_keys.find(t.id);
        if (key == continuity.track_keys.end()) {
            if (continuity.next_key > kMaxTrackKey) {
                error = "the session has seen more than " + str(kMaxTrackKey) +
                        " tracks; save and reopen it (limit: track keys)";
                return false;
            }
            key = continuity.track_keys.emplace(t.id, continuity.next_key++).first;
        }
        b.stable_key = key->second;
        b.active = std::any_of(t.inserts.begin(), t.inserts.end(),
                               [](const model::Insert& in) { return in.active; });
        if (!b.active) continue;
        b.chain_id = t.id;
        if (chains) chains(t, static_cast<uint32_t>(ti), b);
        // The resolver binds a chain; it decides neither whether there is
        // one nor the track's key.
        b.active = true;
        b.stable_key = key->second;
        ++active_chains;
    }

    PlanSizes sizes;
    sizes.tracks = limits.engine_tracks;
    sizes.segments = static_cast<uint32_t>(segment_total);
    sizes.nodes = graph_nodes(limits.engine_tracks, active_chains);
    sizes.edges = graph_edges(limits.engine_tracks, active_chains);
    sizes.chunk_slots = static_cast<uint32_t>(slot_total);
    const std::vector<ClickSegment> click = click_segments(snapshot.settings);
    sizes.click_segments = static_cast<uint32_t>(click.size());
    PlaybackPlan* plan = create_plan(sizes);
    if (!plan) {
        error = "out of memory for a plan of " + str(segment_total) + " segments";
        return false;
    }
    plan->epoch = epoch;
    plan->model_revision = snapshot.revision;
    plan->chunk_frames = chunk;
    const model::MonitorBus& bus = snapshot.mix.bus;
    plan->monitor_out[0] = bus.outputs[0];
    plan->monitor_out[1] = bus.outputs[1];
    plan->pan_exponent = pan_exponent_for(bus.pan_law_db);
    // Phase 2's click goes where the monitor bus goes (ADR-0020 §2).
    plan->click_out[0] = bus.outputs[0];
    plan->click_out[1] = bus.outputs[1];
    std::copy(click.begin(), click.end(), plan->click);
    uint32_t at = 0;
    for (uint32_t t = 0; t < limits.engine_tracks; ++t) {
        PlanTrack& pt = plan->tracks[t];
        pt.engine_track = t;
        pt.channels = track_channels[t];
        pt.first_segment = at;
        pt.segment_count = static_cast<uint32_t>(per_track[t].size());
        for (const PlanSegment& s : per_track[t]) plan->segments[at++] = s;
    }
    layout_track_graph(*plan, bindings.data());
    report.track_shortfall.assign(limits.engine_tracks, 0);
    const Compensation comp =
        compensate(*plan, limits.max_delay_frames, continuity.pool, report.track_shortfall.data());
    report.compensation_frames = comp.compensation_frames;
    report.compensation_shortfall_frames = comp.shortfall_frames;
    report.clamped_edges = comp.clamped_edges;
    report.undelayed_edges = comp.undelayed_edges;
    // What the plan carries, not what the resolver asked for: a binding the
    // layout left unbound plays with latency 0, and the state block must
    // show the latency that is compensated (PR #59 re-review).
    report.track_latency.assign(limits.engine_tracks, 0);
    for (uint32_t ni = 0; ni < plan->node_count; ++ni) {
        const PlanNode& n = plan->nodes[ni];
        if (n.kind != NodeKind::InsertChain) continue;
        const uint32_t t = plan->tracks[n.track].engine_track;
        if (t < limits.engine_tracks) report.track_latency[t] = n.latency_frames;
    }

    // Peak demand: the sum changes only where a window edge crosses a
    // segment edge, so those positions are the candidates.
    const int64_t w = limits.window_frames;
    uint64_t peak = demand_at(per_track, 0, w, chunk);
    for (const auto& segs : per_track) {
        for (const PlanSegment& s : segs) {
            for (int64_t p : {s.start, s.start - w + 1, s.end - w, s.end - 1}) {
                if (p >= 0) peak = std::max(peak, demand_at(per_track, p, w, chunk));
            }
        }
    }
    plan->peak_chunk_demand = peak;
    report.peak_chunk_demand = peak;
    report.over_half_pool = peak * 2 > limits.pool_chunks;
    report.segments = sizes.segments;
    report.chunk_slots = sizes.chunk_slots;
    out = plan;
    return true;
}

}  // namespace sonora
