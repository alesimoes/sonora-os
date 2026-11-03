// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/consolidate.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>

#include "sonora/fade_curve.h"
#include "sonora/model/comping.h"
#include "sonora/model/operations.h"
#include "sonora/playback_plan.h"
#include "sonora/session_files/session_folder.h"
#include "sonora/wav_reader.h"
#include "sonora/wav_writer.h"

namespace sonora {

namespace {

constexpr model::Frames kBlock = 4096;

// One region as playback plays it (plan_builder.cpp builds the same).
struct Segment {
    int64_t start = 0, end = 0, source_offset = 0;
    float gain = 1.0f;
    int64_t fade_in_end = 0, fade_out_start = 0;
    FadeCurve fade_in_shape = FadeCurve::Linear, fade_out_shape = FadeCurve::Linear;
    const model::Region* region = nullptr;
};

FadeCurve curve_of(model::FadeShape s) {
    switch (s) {
        case model::FadeShape::Linear: return FadeCurve::Linear;
        case model::FadeShape::SCurve: return FadeCurve::SCurve;
        case model::FadeShape::EqualPower: break;
    }
    return FadeCurve::EqualPower;
}

// The same arithmetic as the audio thread's segment_gain(), so a consolidated
// file sounds like the range it replaces.
float gain_at(const Segment& s, int64_t t) {
    float g = s.gain;
    if (t < s.fade_in_end) {
        const float x = static_cast<float>(t - s.start) / static_cast<float>(s.fade_in_end - s.start);
        g *= fade_in_gain(s.fade_in_shape, x);
    }
    if (t >= s.fade_out_start) {
        const float x = static_cast<float>(t - s.fade_out_start) /
                        static_cast<float>(s.end - s.fade_out_start);
        g *= fade_in_gain(s.fade_out_shape, 1.0f - x);
    }
    return g;
}

const model::TrackEdit* track_of(const model::Snapshot& s, model::Id id) {
    for (const auto& t : s.edit->tracks) {
        if (t->id == id) return t.get();
    }
    return nullptr;
}

// A track's name is the operator's, and may hold a separator.
std::string file_safe(std::string name) {
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':') c = '_';
    }
    return name;
}

bool make_dir(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return std::filesystem::is_directory(path, ec);
}

// The members the model asks for once `source` (of `length` frames) is in
// the pool. `with` is the snapshot after AddSource.
bool plan_members(const model::Snapshot& now, const ConsolidateRequest& rq, model::Id playlist,
                  const model::TrackEdit& track, const model::AddSource& add,
                  model::Comp& comp, std::string& why) {
    model::Snapshot with;
    if (!model::apply(now, add, with, why)) return false;
    model::Id id = now.next_id + 1;
    const model::ConsolidateRange cr{
        rq.track, playlist, rq.start, rq.end, add.source.id,
        rq.name.empty() ? track.name + " Consolidated" : rq.name};
    return model::consolidate_range(with, cr, [&id] { return id++; }, comp, why);
}

}  // namespace

bool render_range(const model::Snapshot& snapshot, const std::string& session_dir,
                  const ConsolidateRequest& rq, const ConsolidateHooks& hooks, RenderedRange& out,
                  std::string& error) {
    out = RenderedRange{};
    const model::TrackEdit* track = track_of(snapshot, rq.track);
    if (track == nullptr) {
        error = "no such track; nothing was consolidated";
        return false;
    }
    if (track->kind != model::TrackKind::Audio || track->channels < 1) {
        error = track->name + " is not an audio track; nothing was consolidated";
        return false;
    }
    const model::Id want = rq.playlist != 0 ? rq.playlist : track->active_playlist;
    const model::Playlist* pl = nullptr;
    for (const auto& p : track->playlists) {
        if (p->id == want) pl = p.get();
    }
    if (pl == nullptr) {
        error = "the range is not a playlist of " + track->name + "; nothing was consolidated";
        return false;
    }
    if (rq.start < 0 || rq.end <= rq.start) {
        error = "the range to consolidate is empty; nothing was consolidated";
        return false;
    }

    std::vector<Segment> segs;
    std::vector<bool> muted;
    std::map<model::Id, std::size_t> index;
    for (const model::Region& r : pl->regions) {
        Segment s;
        s.start = r.position;
        s.end = r.end();
        s.source_offset = r.source_offset;
        s.gain = auto_gain_of_db(r.gain_db);
        s.fade_in_end = r.position + r.fade_in.length;
        s.fade_in_shape = curve_of(r.fade_in.shape);
        s.fade_out_start = r.end() - r.fade_out.length;
        s.fade_out_shape = curve_of(r.fade_out.shape);
        s.region = &r;
        index[r.id] = segs.size();
        segs.push_back(s);
        muted.push_back(r.mute);
    }
    // A crossfade pulls the two regions' edges across the cut, as the plan
    // does, whether or not either is muted.
    for (const model::Crossfade& x : pl->crossfades) {
        const auto l = index.find(x.left);
        const auto r = index.find(x.right);
        if (l == index.end() || r == index.end()) {
            error = "a crossfade names a region the playlist does not hold; nothing was consolidated";
            return false;
        }
        model::Frames before = 0, after = 0;
        model::crossfade_extent(x, before, after);
        Segment& left = segs[l->second];
        Segment& right = segs[r->second];
        const int64_t cut = right.start;
        left.end = cut + after;
        left.fade_out_start = cut - before;
        left.fade_out_shape = curve_of(x.shape);
        right.start = cut - before;
        right.source_offset -= before;
        right.fade_in_end = cut + after;
        right.fade_in_shape = curve_of(x.shape);
    }
    std::vector<Segment> playing;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        const Segment& s = segs[i];
        if (muted[i] || s.end <= rq.start || s.start >= rq.end) continue;
        if (s.source_offset < 0 || s.end <= s.start || s.region->channels.size() != track->channels) {
            error = "region \"" + s.region->name +
                    "\" cannot be rendered: it starts before its source; nothing was consolidated";
            return false;
        }
        playing.push_back(s);
    }
    segs.swap(playing);

    std::map<model::Id, std::unique_ptr<WavReader>> readers;
    std::map<model::Id, std::string> paths;
    for (const Segment& s : segs) {
        for (const model::SourceRef& ref : s.region->channels) {
            const model::Source* src = snapshot.sources->find(ref.source);
            if (src == nullptr) {
                error = "region \"" + s.region->name + "\": its source is not in the pool";
                return false;
            }
            if (readers.count(ref.source) == 0) {
                auto rd = std::make_unique<WavReader>();
                std::string why;
                if (!rd->open(session_dir + "/" + src->path, why)) {
                    error = src->path + ": " + why;
                    return false;
                }
                if (rd->sample_rate() != snapshot.settings.sample_rate) {
                    error = src->path + " is at " + std::to_string(rd->sample_rate()) +
                            " Hz, and the session runs at " +
                            std::to_string(snapshot.settings.sample_rate) + " Hz";
                    return false;
                }
                paths[ref.source] = src->path;
                readers[ref.source] = std::move(rd);
            }
            if (ref.channel >= readers[ref.source]->channels()) {
                error = src->path + " has no channel " + std::to_string(ref.channel);
                return false;
            }
        }
    }

    const std::string dir = session_dir + "/" + session_files::kAudioDir;
    if (!make_dir(dir)) {
        error = "cannot create " + dir + ": " + std::strerror(errno);
        return false;
    }
    WavWriter writer;
    writer.set_description("Consolidated");
    const WavWriter::Format format = snapshot.settings.audio_format == model::AudioFormat::Float32
                                         ? WavWriter::Format::Float32
                                         : WavWriter::Format::Int24;
    std::string file;
    bool opened = false;
    // open() refuses an existing name with EEXIST and leaves it untouched.
    for (int n = 1; n <= 9999 && !opened; ++n) {
        char digits[16];
        std::snprintf(digits, sizeof digits, "%02d", n);
        file = file_safe(track->name) + "_cons_" + digits + ".wav";
        opened = writer.open(dir + "/" + file, snapshot.settings.sample_rate, track->channels,
                             format, static_cast<uint64_t>(rq.start));
        if (!opened && errno != EEXIST) {
            error = "cannot create " + file + ": " + std::strerror(errno);
            return false;
        }
    }
    if (!opened) {
        error = "no free consolidated file name for " + track->name;
        return false;
    }
    const std::string full = dir + "/" + file;
    const auto abandon = [&](const std::string& why) {
        writer.close();
        std::remove(full.c_str());
        error = why;
        return false;
    };

    const uint16_t ch = track->channels;
    std::vector<float> mix(static_cast<std::size_t>(kBlock) * ch);
    std::vector<float> raw;
    for (model::Frames b0 = rq.start; b0 < rq.end; b0 += kBlock) {
        if (hooks.cancel != nullptr && hooks.cancel->load(std::memory_order_relaxed)) {
            return abandon("consolidate was cancelled; nothing was changed");
        }
        const model::Frames b1 = std::min(b0 + kBlock, rq.end);
        std::fill(mix.begin(), mix.end(), 0.0f);
        for (const Segment& s : segs) {
            const int64_t from = std::max<int64_t>(b0, s.start);
            const int64_t to = std::min<int64_t>(b1, s.end);
            if (to <= from) continue;
            const int64_t count = to - from;
            for (uint16_t c = 0; c < ch; ++c) {
                const model::SourceRef& ref = s.region->channels[c];
                WavReader& rd = *readers[ref.source];
                raw.resize(static_cast<std::size_t>(count) * rd.channels());
                const int64_t got = rd.read(s.source_offset + (from - s.start), count, raw.data());
                if (got != count) {
                    return abandon(paths[ref.source] + " is shorter than region \"" +
                                   s.region->name + "\" needs, or cannot be read");
                }
                for (int64_t i = 0; i < count; ++i) {
                    mix[static_cast<std::size_t>(from - b0 + i) * ch + c] +=
                        raw[static_cast<std::size_t>(i) * rd.channels() + ref.channel] *
                        gain_at(s, from + i);
                }
            }
        }
        if (!writer.write(mix.data(), static_cast<std::size_t>(b1 - b0))) {
            return abandon("cannot write " + file + ": " + writer.failure());
        }
        if (hooks.progress) {
            hooks.progress(static_cast<double>(b1 - rq.start) /
                           static_cast<double>(rq.end - rq.start));
        }
    }
    // Synced, with its folder, before the document may name it.
    if (!writer.checkpoint() || !writer.close()) {
        return abandon("cannot write " + file + ": " +
                       (writer.failure().empty() ? "the disk refused it" : writer.failure()));
    }
    out.relative_path = std::string(session_files::kAudioDir) + "/" + file;
    out.frames = rq.end - rq.start;
    return true;
}

bool consolidate(model::Document& document, const Session& session,
                 const std::string& session_dir, const ConsolidateRequest& rq,
                 const ConsolidateHooks& hooks, std::vector<model::ReplicationUpdate>& updates,
                 std::string& error) {
    if (session.recording_take() != 0 || session.takes_closing()) {
        error = "stop recording before consolidating; nothing was consolidated";
        return false;
    }
    const model::Snapshot now = document.current();
    const model::TrackEdit* track = track_of(now, rq.track);
    if (track == nullptr) {
        error = "no such track; nothing was consolidated";
        return false;
    }
    const model::Id playlist = rq.playlist != 0 ? rq.playlist : track->active_playlist;

    // Asked of the model first with the file's size, so a range it would
    // refuse costs no render.
    model::AddSource add;
    add.source.id = now.next_id;
    add.source.path = std::string(session_files::kAudioDir) + "/pending.wav";
    add.source.channels = track->channels;
    add.source.length = rq.end - rq.start;
    model::Comp comp;
    if (!plan_members(now, rq, playlist, *track, add, comp, error)) return false;

    RenderedRange rendered;
    ConsolidateRequest r = rq;
    r.playlist = playlist;
    if (!render_range(now, session_dir, r, hooks, rendered, error)) return false;
    const std::string full = session_dir + "/" + rendered.relative_path;

    // The numbers come from the file, as for every source.
    WavReader file;
    std::string why;
    if (!file.open(full, why)) {
        std::remove(full.c_str());
        error = rendered.relative_path + ": " + why;
        return false;
    }
    add.source.path = rendered.relative_path;
    add.source.channels = file.channels();
    add.source.length = file.frames();
    add.source.format = file.encoding() == WavReader::Encoding::Float32 ? model::AudioFormat::Float32
                                                                        : model::AudioFormat::Int24;
    add.source.time_reference = static_cast<int64_t>(file.time_reference());
    add.source.kind = model::SourceKind::Imported;
    file.close();

    const auto refused = [&](const std::string& because) {
        error = "the session refused the consolidated file, which stays in " +
                std::string(session_files::kAudioDir) + " but not in the session: " + because;
        return false;
    };
    if (!plan_members(now, r, playlist, *track, add, comp, why)) return refused(why);
    model::Compound c;
    c.label = "Consolidate";
    c.ops.push_back(add);
    for (model::Member& m : comp.members) c.ops.push_back(std::move(m));
    model::ReplicationUpdate u;
    if (!document.apply(c, u, why)) return refused(why);
    updates.push_back(std::move(u));
    return true;
}

}  // namespace sonora
