// SPDX-License-Identifier: GPL-3.0-or-later
#include "elastic_rig.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "sonora/model/operations.h"
#include "sonora/wav_writer.h"

namespace elastic_test {

namespace fs = std::filesystem;
using namespace sonora;

float take_at(std::int64_t i) { return static_cast<float>((i * 7) % 1000) / 1000.0f - 0.5f; }

namespace {

bool write_take(const std::string& path, std::uint32_t rate, std::uint16_t channels,
                std::int64_t frames) {
    WavWriter w;
    if (!w.open(path, rate, channels, WavWriter::Format::Float32, 0)) return false;
    std::vector<float> block(static_cast<std::size_t>(frames) * channels);
    for (std::int64_t i = 0; i < frames; ++i) {
        for (std::uint16_t c = 0; c < channels; ++c) {
            block[static_cast<std::size_t>(i) * channels + c] = take_at(i);
        }
    }
    return w.write(block.data(), static_cast<std::size_t>(frames)) && w.close();
}

model::PlaceRegion region_op(const Rig& r, Frames position, Frames length, Frames offset) {
    model::PlaceRegion p;
    p.playlist = r.playlist;
    p.region.id = r.doc->current().next_id;
    p.region.name = "loop";
    p.region.position = position;
    p.region.length = length;
    p.region.source_offset = offset;
    for (std::uint16_t c = 0; c < r.channels; ++c) p.region.channels.push_back({r.source, c});
    return p;
}

}  // namespace

bool make_rig(Rig& r, const char* dir, std::int64_t frames, Frames position, Frames length,
              Frames offset, Frames gain_at) {
    fs::remove_all(dir);
    r.dir = dir;
    if (!fs::create_directories(r.audio_dir())) return false;
    model::SessionSettings settings;
    settings.name = "elastic";
    settings.sample_rate = r.rate;
    settings.audio_format = model::AudioFormat::Float32;
    r.doc = std::make_unique<model::Document>(model::new_session(settings));
    std::string err;
    model::ReplicationUpdate u;
    model::AddTrack t;
    t.track = r.doc->current().next_id;
    t.name = "Drums";
    t.channels = r.channels;
    t.playlist = t.track + 1;
    t.playlist_name = "Drums";
    t.mix.input = {0, 1};
    if (!r.doc->apply(t, u, err)) {
        std::fprintf(stderr, "AddTrack: %s\n", err.c_str());
        return false;
    }
    r.track = t.track;
    r.playlist = t.playlist;
    if (!write_take(r.take(), r.rate, r.channels, frames)) return false;
    model::AddSource add;
    add.source.id = r.doc->current().next_id;
    add.source.path = "Audio Files/take.wav";
    add.source.channels = r.channels;
    add.source.length = frames;
    add.source.format = model::AudioFormat::Float32;
    r.source = add.source.id;
    if (!r.doc->apply(add, u, err)) return false;
    model::PlaceRegion p = region_op(r, position, length, offset);
    if (gain_at >= 0) p.region.gain_line = {{gain_at, -6.0f}};
    r.region = p.region.id;
    if (!r.doc->apply(p, u, err)) {
        std::fprintf(stderr, "PlaceRegion: %s\n", err.c_str());
        return false;
    }
    return true;
}

bool place_region(Rig& r, Frames position, Frames length, Frames offset, Id& id) {
    model::PlaceRegion p = region_op(r, position, length, offset);
    id = p.region.id;
    model::ReplicationUpdate u;
    std::string err;
    return r.doc->apply(p, u, err);
}

const model::Region* region_of(const Rig& r, Id id) {
    for (const auto& t : r.doc->current().edit->tracks) {
        for (const auto& p : t->playlists) {
            for (const model::Region& g : p->regions) {
                if (g.id == id) return &g;
            }
        }
    }
    return nullptr;
}

const model::Source* source_of(const Rig& r, Id region) {
    const model::Region* g = region_of(r, region);
    return g == nullptr ? nullptr : r.doc->current().sources->find(g->channels[0].source);
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

std::size_t files_in(const std::string& dir) {
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        (void)e;
        ++n;
    }
    return n;
}

}  // namespace elastic_test
