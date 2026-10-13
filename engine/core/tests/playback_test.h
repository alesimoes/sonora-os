// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared by engine/core's playback tests: the CHECK macro, sources written
// with known contents, and plans built by hand. engine/control builds real
// plans from a model; these tests are about what engine/core does with one.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "sonora/playback_plan.h"
#include "sonora/wav_writer.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace playback_test {

// Source frame f of channel c. Exact in float, distinct per frame and
// channel, so a wrong chunk or a crossed channel is a wrong value.
inline float source_value(uint32_t source, uint32_t c, int64_t f) {
    return static_cast<float>(source * 1000 + c * 100) + static_cast<float>(f % 65536) / 65536.0f;
}

// A 32-bit float file, so what is read back is exactly what was written.
inline bool write_source(const std::string& path, uint32_t source, uint16_t channels,
                         int64_t frames, uint64_t time_reference = 0) {
    std::remove(path.c_str());
    sonora::WavWriter w;
    if (!w.open(path, 48000, channels, sonora::WavWriter::Format::Float32, time_reference)) {
        return false;
    }
    std::vector<float> buf(static_cast<std::size_t>(frames) * channels);
    for (int64_t f = 0; f < frames; ++f) {
        for (uint16_t c = 0; c < channels; ++c) {
            buf[static_cast<std::size_t>(f) * channels + c] = source_value(source, c, f);
        }
    }
    return w.write(buf.data(), static_cast<std::size_t>(frames)) && w.close();
}

struct SegmentSpec {
    uint32_t track = 0;            // index into the plan's tracks
    int64_t start = 0;
    int64_t end = 0;
    int64_t source_offset = 0;
    uint32_t source_slot = 0;
    uint16_t channels = 1;
    uint16_t source_channel0 = 0;  // channel c plays source_channel0 + c
    float gain = 1.0f;
    int64_t fade_in = 0;
    int64_t fade_out = 0;
    sonora::FadeCurve fade_in_shape = sonora::FadeCurve::Linear;
    sonora::FadeCurve fade_out_shape = sonora::FadeCurve::Linear;
};

// The Phase 2 graph over `tracks` engine tracks (0..tracks-1), with the
// given segments, which must be sorted by track then start. Chunk slots are
// laid out the way engine/control's builder lays them out.
inline sonora::PlaybackPlan* make_plan(uint64_t epoch, uint32_t tracks,
                                       const std::vector<SegmentSpec>& specs,
                                       uint32_t chunk_frames, uint32_t edge_delay = 0) {
    uint32_t slots = 0;
    for (const SegmentSpec& s : specs) {
        const int64_t first = s.source_offset / chunk_frames;
        const int64_t last = (s.source_offset + (s.end - s.start) - 1) / chunk_frames;
        slots += static_cast<uint32_t>(last - first + 1) * s.channels;
    }
    sonora::PlanSizes sizes;
    sizes.tracks = tracks;
    sizes.segments = static_cast<uint32_t>(specs.size());
    sizes.nodes = tracks + 2;
    sizes.edges = tracks + 1;
    sizes.chunk_slots = slots;
    sonora::PlaybackPlan* p = sonora::create_plan(sizes);
    p->epoch = epoch;
    p->chunk_frames = chunk_frames;
    p->pan_exponent = 0.5f;
    uint32_t slot = 0;
    for (uint32_t i = 0; i < specs.size(); ++i) {
        const SegmentSpec& s = specs[i];
        sonora::PlanSegment& g = p->segments[i];
        g.start = s.start;
        g.end = s.end;
        g.source_offset = s.source_offset;
        g.channels = s.channels;
        g.gain = s.gain;
        g.fade_in_end = s.start + s.fade_in;
        g.fade_out_start = s.end - s.fade_out;
        g.fade_in_shape = s.fade_in_shape;
        g.fade_out_shape = s.fade_out_shape;
        const int64_t first = s.source_offset / chunk_frames;
        const int64_t last = (s.source_offset + (s.end - s.start) - 1) / chunk_frames;
        for (uint16_t c = 0; c < s.channels; ++c) {
            g.source_slot[c] = s.source_slot;
            g.source_channel[c] = static_cast<uint16_t>(s.source_channel0 + c);
            g.first_chunk_slot[c] = slot;
            g.first_chunk_index[c] = static_cast<uint32_t>(first);
            g.chunk_count[c] = static_cast<uint32_t>(last - first + 1);
            slot += g.chunk_count[c];
        }
    }
    for (uint32_t t = 0; t < tracks; ++t) {
        p->tracks[t].engine_track = t;
        p->tracks[t].channels = 1;
        p->tracks[t].first_segment = 0;
        p->tracks[t].segment_count = 0;
    }
    for (uint32_t i = 0; i < specs.size(); ++i) {
        sonora::PlanTrack& t = p->tracks[specs[i].track];
        if (t.segment_count == 0) t.first_segment = i;
        ++t.segment_count;
        t.channels = specs[i].channels;
    }
    sonora::layout_monitor_graph(*p);
    for (uint32_t t = 0; t < tracks; ++t) p->edges[t].delay_frames = edge_delay;
    return p;
}

}  // namespace playback_test
