// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/sample_edit.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <sstream>

#include "sonora/model/operations.h"
#include "sonora/session_files/session_folder.h"
#include "sonora/wav_reader.h"
#include "sonora/wav_writer.h"

namespace sonora {

namespace {

constexpr model::Frames kBlock = 4096;
// Frames of context kept either side of a stretch; Repair reads two.
constexpr std::size_t kContext = 4;

struct VerbName {
    const char* name;
    SampleVerb verb;
    const char* label;
};
constexpr VerbName kVerbs[] = {
    {"repair", SampleVerb::Repair, "Repair"},       {"draw", SampleVerb::Draw, "Draw"},
    {"silence", SampleVerb::Silence, "Silence"},    {"reverse", SampleVerb::Reverse, "Reverse"},
    {"invert", SampleVerb::Invert, "Invert"},       {"gain", SampleVerb::Gain, "Gain"},
    {"normalize", SampleVerb::Normalize, "Normalize"},
    {"fade_in", SampleVerb::FadeIn, "Fade In"},     {"fade_out", SampleVerb::FadeOut, "Fade Out"},
};

const char* label_of(SampleVerb v) {
    for (const VerbName& n : kVerbs) {
        if (n.verb == v) return n.label;
    }
    return "Edit";
}

bool whole_number(const std::string& s, long long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || errno != 0) return false;
    out = v;
    return true;
}

bool real_number(const std::string& s, float& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    out = static_cast<float>(v);
    return true;
}

std::string file_safe(std::string name) {
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':') c = '_';
    }
    return name;
}

// The level of the gain line at source frame `src`, linear in amplitude
// between points and held outside them (the engine's own reading of it).
float line_db_at(const std::vector<model::GainPoint>& line, model::Frames src) {
    if (line.empty()) return 0.0f;
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

// The gain line in the copy's source frames, which start `s0` later: the
// points before the copy fold into one at frame 0 holding their level there.
std::vector<model::GainPoint> shifted_line(const std::vector<model::GainPoint>& line,
                                           model::Frames s0) {
    std::vector<model::GainPoint> out;
    if (line.empty()) return out;
    if (s0 > 0 && line.front().at < s0) {
        out.push_back({0, line_db_at(line, s0)});
    }
    for (const model::GainPoint& p : line) {
        if (p.at < s0) continue;
        if (p.at == s0 && !out.empty()) continue;
        out.push_back({p.at - s0, p.db});
    }
    return out;
}

float peak_of(const std::vector<float>& buf, std::size_t lead, std::size_t count) {
    float peak = 0.0f;
    for (std::size_t i = 0; i < count; ++i) peak = std::max(peak, std::fabs(buf[lead + i]));
    return peak;
}

}  // namespace

bool parse_sample_edit(const std::string& text, SampleEditRequest& out, std::string& why) {
    out = SampleEditRequest{};
    std::istringstream in(text);
    std::string verb, region, from, to, channel, param;
    in >> verb >> region >> from >> to >> channel >> param;
    std::string extra;
    if (in >> extra) {
        why = "sample.edit takes VERB REGION FROM TO CHANNEL [PARAM]; \"" + extra + "\" is left over";
        return false;
    }
    if (channel.empty()) {
        why = "sample.edit takes VERB REGION FROM TO CHANNEL [PARAM]";
        return false;
    }
    bool known = false;
    for (const VerbName& n : kVerbs) {
        if (verb == n.name) {
            out.verb = n.verb;
            known = true;
        }
    }
    if (!known) {
        why = "\"" + verb + "\" is not a sample edit; use repair, draw, silence, reverse, invert, "
              "gain, normalize, fade_in or fade_out";
        return false;
    }
    long long r = 0, f = 0, t = 0, c = 0;
    if (!whole_number(region, r) || r <= 0 || !whole_number(from, f) || f < 0 ||
        !whole_number(to, t) || t <= f) {
        why = "the region and the range must be whole numbers, the range from 0 and not empty";
        return false;
    }
    out.region = static_cast<model::Id>(r);
    out.from = f;
    out.to = t;
    if (channel == "all") {
        out.channel = -1;
    } else if (whole_number(channel, c) && c >= 1 && c <= 1024) {
        out.channel = static_cast<int>(c - 1);
    } else {
        why = "\"" + channel + "\" is not a channel; give a number from 1, or all";
        return false;
    }
    switch (out.verb) {
        case SampleVerb::Gain:
        case SampleVerb::Normalize: {
            if (param.empty()) {
                if (out.verb == SampleVerb::Gain) {
                    why = "gain needs the change in dB";
                    return false;
                }
                out.db = 0.0f;
            } else if (!real_number(param, out.db)) {
                why = "\"" + param + "\" is not a level in dB";
                return false;
            }
            if (out.db < model::kMinGainDb || out.db > model::kMaxGainDb) {
                why = "the level must be from -144 to 24 dB";
                return false;
            }
            break;
        }
        case SampleVerb::Draw: {
            if (param.empty()) {
                why = "draw needs points as FRAME:LEVEL,FRAME:LEVEL";
                return false;
            }
            std::istringstream pts(param);
            std::string item;
            while (std::getline(pts, item, ',')) {
                const std::size_t colon = item.find(':');
                long long frame = 0;
                float level = 0.0f;
                if (colon == std::string::npos || !whole_number(item.substr(0, colon), frame) ||
                    frame < 0 || !real_number(item.substr(colon + 1), level) || level < -1.0f ||
                    level > 1.0f) {
                    why = "\"" + item + "\" is not a point; give FRAME:LEVEL with the level from -1 to 1";
                    return false;
                }
                if (!out.points.empty() && frame <= out.points.back().first) {
                    why = "the points of a drawing must be in increasing order of frame";
                    return false;
                }
                out.points.emplace_back(frame, level);
                if (out.points.size() > 4096) {
                    why = "a drawing holds at most 4096 points";
                    return false;
                }
            }
            break;
        }
        default:
            if (!param.empty()) {
                why = std::string(label_of(out.verb)) + " takes no parameter; \"" + param +
                      "\" is left over";
                return false;
            }
    }
    return true;
}

bool process_samples(const SampleEditRequest& rq, std::vector<float>& buf, std::size_t lead,
                     std::size_t count, std::size_t tail, model::Frames first, std::string& why) {
    if (count == 0 || buf.size() < lead + count + tail) {
        why = "nothing to edit";
        return false;
    }
    float* x = buf.data() + lead;
    switch (rq.verb) {
        case SampleVerb::Repair: {
            if (static_cast<model::Frames>(count) > kMaxRepairFrames) {
                why = "Repair redraws at most " + std::to_string(kMaxRepairFrames) +
                      " samples, for a click; silence or fade a longer stretch";
                return false;
            }
            const bool has0 = lead > 0, has1 = tail > 0;
            if (!has0 && !has1) {
                std::fill(x, x + count, 0.0f);
                return true;
            }
            const float p0 = has0 ? x[-1] : x[count];
            const float p1 = has1 ? x[count] : x[-1];
            if (!has0 || !has1) {
                std::fill(x, x + count, has0 ? p0 : p1);
                return true;
            }
            // A cubic Hermite arc from the sample before to the sample
            // after, with the slopes the audio had going in and coming out.
            const float m0 = lead >= 2 ? x[-1] - x[-2] : 0.0f;
            const float m1 = tail >= 2 ? x[count + 1] - x[count] : 0.0f;
            const float len = static_cast<float>(count + 1);
            for (std::size_t i = 0; i < count; ++i) {
                const float t = static_cast<float>(i + 1) / len;
                const float t2 = t * t, t3 = t2 * t;
                x[i] = (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * len * m0 +
                       (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * len * m1;
            }
            return true;
        }
        case SampleVerb::Draw: {
            if (rq.points.empty()) {
                why = "a drawing needs at least one point";
                return false;
            }
            std::size_t hi = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const model::Frames f = first + static_cast<model::Frames>(i);
                while (hi < rq.points.size() && rq.points[hi].first <= f) ++hi;
                if (hi == 0) {
                    x[i] = rq.points.front().second;
                } else if (hi == rq.points.size()) {
                    x[i] = rq.points.back().second;
                } else {
                    const auto& a = rq.points[hi - 1];
                    const auto& b = rq.points[hi];
                    const float u = static_cast<float>(f - a.first) /
                                    static_cast<float>(b.first - a.first);
                    x[i] = a.second + u * (b.second - a.second);
                }
            }
            return true;
        }
        case SampleVerb::Silence: std::fill(x, x + count, 0.0f); return true;
        case SampleVerb::Reverse: std::reverse(x, x + count); return true;
        case SampleVerb::Invert:
            for (std::size_t i = 0; i < count; ++i) x[i] = -x[i];
            return true;
        case SampleVerb::Gain: {
            const float g = static_cast<float>(std::pow(10.0, static_cast<double>(rq.db) / 20.0));
            for (std::size_t i = 0; i < count; ++i) x[i] *= g;
            return true;
        }
        case SampleVerb::Normalize: {
            const float peak = peak_of(buf, lead, count);
            if (peak < 1e-9f) {
                why = "the stretch is silent; there is nothing to normalise";
                return false;
            }
            const float g = static_cast<float>(std::pow(10.0, static_cast<double>(rq.db) / 20.0)) / peak;
            for (std::size_t i = 0; i < count; ++i) x[i] *= g;
            return true;
        }
        case SampleVerb::FadeIn:
            for (std::size_t i = 0; i < count; ++i) {
                x[i] *= static_cast<float>(i) / static_cast<float>(count);
            }
            return true;
        case SampleVerb::FadeOut:
            for (std::size_t i = 0; i < count; ++i) {
                x[i] *= static_cast<float>(count - 1 - i) / static_cast<float>(count);
            }
            return true;
    }
    return false;
}

bool sample_edit(model::Document& document, const Session& session, const std::string& session_dir,
                 const SampleEditRequest& rq, std::vector<model::ReplicationUpdate>& updates,
                 std::string& error) {
    const std::string nothing = "; nothing was edited";
    if (session.recording_take() != 0 || session.takes_closing()) {
        error = "stop recording before editing samples" + nothing;
        return false;
    }
    const model::Snapshot now = document.current();
    const model::TrackEdit* track = nullptr;
    const model::Playlist* playlist = nullptr;
    const model::Region* region = nullptr;
    for (const auto& t : now.edit->tracks) {
        for (const auto& p : t->playlists) {
            for (const model::Region& r : p->regions) {
                if (r.id != rq.region) continue;
                track = t.get();
                playlist = p.get();
                region = &r;
            }
        }
    }
    if (region == nullptr) {
        error = "no such region" + nothing;
        return false;
    }
    if (track->kind != model::TrackKind::Audio) {
        error = track->name + " is not an audio track" + nothing;
        return false;
    }
    if (rq.from < region->position || rq.to > region->end() || rq.to <= rq.from) {
        error = "select a stretch inside the region first" + nothing;
        return false;
    }
    const std::size_t nch = region->channels.size();
    if (rq.channel >= static_cast<int>(nch)) {
        error = "the region has " + std::to_string(nch) + (nch == 1 ? " channel" : " channels") +
                ", not " + std::to_string(rq.channel + 1) + nothing;
        return false;
    }
    const model::Frames count = rq.to - rq.from;
    if (count > kMaxSampleEditFrames) {
        error = "that stretch is " + std::to_string(count) + " frames; a sample edit takes at most " +
                std::to_string(kMaxSampleEditFrames) + nothing;
        return false;
    }

    // What the copy holds: the region's span and a handle either side, as far
    // as every channel's source goes.
    std::map<model::Id, std::unique_ptr<WavReader>> readers;
    for (const model::SourceRef& ref : region->channels) {
        const model::Source* src = now.sources->find(ref.source);
        if (src == nullptr) {
            error = "region \"" + region->name + "\": its source is not in the pool" + nothing;
            return false;
        }
        if (readers.count(ref.source) != 0) continue;
        auto rd = std::make_unique<WavReader>();
        std::string why;
        if (!rd->open(session_dir + "/" + src->path, why)) {
            error = src->path + ": " + why + nothing;
            return false;
        }
        if (rd->sample_rate() != now.settings.sample_rate) {
            error = src->path + " is at " + std::to_string(rd->sample_rate()) +
                    " Hz, and the session runs at " + std::to_string(now.settings.sample_rate) +
                    " Hz" + nothing;
            return false;
        }
        if (ref.channel >= rd->channels()) {
            error = src->path + " has no channel " + std::to_string(ref.channel + 1) + nothing;
            return false;
        }
        readers[ref.source] = std::move(rd);
    }
    const model::Frames handle =
        static_cast<model::Frames>(kSampleEditHandleSeconds * now.settings.sample_rate);
    model::Frames limit = region->source_offset + region->length + handle;
    for (const model::SourceRef& ref : region->channels) {
        limit = std::min(limit, now.sources->find(ref.source)->length);
    }
    const model::Frames s0 = std::max<model::Frames>(0, region->source_offset - handle);
    const model::Frames s1 = std::max(limit, region->source_offset + region->length);
    const model::Frames total = s1 - s0;
    const model::Frames a = region->source_offset + (rq.from - region->position) - s0;
    const model::Frames b = a + count;

    // The stretch of each channel being edited, with context.
    const std::size_t lead = static_cast<std::size_t>(std::min<model::Frames>(a, kContext));
    const std::size_t tail = static_cast<std::size_t>(std::min<model::Frames>(total - b, kContext));
    const std::size_t size = lead + static_cast<std::size_t>(count) + tail;
    std::vector<std::vector<float>> edited(nch);
    std::vector<float> raw;
    for (std::size_t c = 0; c < nch; ++c) {
        if (rq.channel >= 0 && static_cast<std::size_t>(rq.channel) != c) continue;
        const model::SourceRef& ref = region->channels[c];
        WavReader& rd = *readers[ref.source];
        raw.resize(size * rd.channels());
        const int64_t got = rd.read(s0 + a - static_cast<model::Frames>(lead),
                                    static_cast<int64_t>(size), raw.data());
        if (got != static_cast<int64_t>(size)) {
            error = now.sources->find(ref.source)->path + " cannot be read where the stretch is" + nothing;
            return false;
        }
        edited[c].resize(size);
        for (std::size_t i = 0; i < size; ++i) edited[c][i] = raw[i * rd.channels() + ref.channel];
    }
    SampleEditRequest work = rq;
    if (rq.verb == SampleVerb::Normalize) {
        // One gain for every channel edited, so a stereo image keeps its balance.
        float peak = 0.0f;
        for (const auto& e : edited) {
            if (!e.empty()) peak = std::max(peak, peak_of(e, lead, static_cast<std::size_t>(count)));
        }
        if (peak < 1e-9f) {
            error = "the stretch is silent; there is nothing to normalise" + nothing;
            return false;
        }
        work.verb = SampleVerb::Gain;
        work.db = rq.db - 20.0f * std::log10(peak);
    }
    for (auto& e : edited) {
        if (e.empty()) continue;
        std::string why;
        if (!process_samples(work, e, lead, static_cast<std::size_t>(count), tail, rq.from, why)) {
            error = why + nothing;
            return false;
        }
    }

    // Asked of the model first, with the copy's size, so a refusal costs no write.
    model::AddSource add;
    add.source.id = now.next_id;
    add.source.path = std::string(session_files::kAudioDir) + "/pending.wav";
    add.source.channels = static_cast<std::uint16_t>(nch);
    add.source.length = total;
    model::SetRegionSources sources;
    sources.playlist = playlist->id;
    sources.region = region->id;
    for (std::size_t c = 0; c < nch; ++c) {
        sources.channels.push_back({add.source.id, static_cast<std::uint16_t>(c)});
    }
    sources.source_offset = region->source_offset - s0;
    sources.gain_line = shifted_line(region->gain_line, s0);
    {
        model::Snapshot with, after;
        std::string why;
        if (!model::apply(now, add, with, why) || !model::apply(with, sources, after, why)) {
            error = "the session would refuse the edit: " + why + nothing;
            return false;
        }
    }

    const std::string dir = session_dir + "/" + session_files::kAudioDir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir, ec)) {
        error = "cannot create " + dir + nothing;
        return false;
    }
    const WavWriter::Format format = now.settings.audio_format == model::AudioFormat::Float32
                                         ? WavWriter::Format::Float32
                                         : WavWriter::Format::Int24;
    const model::Source* first_src = now.sources->find(region->channels[0].source);
    const std::int64_t origin = std::max<std::int64_t>(0, first_src->time_reference + s0);
    WavWriter writer;
    writer.set_description(std::string("Sample edit: ") + label_of(rq.verb));
    std::string file;
    bool opened = false;
    // open() refuses an existing name with EEXIST and leaves it untouched.
    for (int n = 1; n <= 9999 && !opened; ++n) {
        char digits[16];
        std::snprintf(digits, sizeof digits, "%02d", n);
        file = file_safe(track->name) + "_edit_" + digits + ".wav";
        opened = writer.open(dir + "/" + file, now.settings.sample_rate,
                             static_cast<std::uint16_t>(nch), format,
                             static_cast<uint64_t>(origin));
        if (!opened && errno != EEXIST) {
            error = "cannot create " + file + ": " + std::strerror(errno) + nothing;
            return false;
        }
    }
    if (!opened) {
        error = "no free sample-edit file name for " + track->name + nothing;
        return false;
    }
    const std::string full = dir + "/" + file;
    const auto abandon = [&](const std::string& why) {
        writer.close();
        std::remove(full.c_str());
        error = why + nothing;
        return false;
    };
    const bool clamp = format == WavWriter::Format::Int24;
    std::vector<float> mix(static_cast<std::size_t>(kBlock) * nch);
    for (model::Frames b0 = 0; b0 < total; b0 += kBlock) {
        const model::Frames n = std::min(kBlock, total - b0);
        for (std::size_t c = 0; c < nch; ++c) {
            const model::SourceRef& ref = region->channels[c];
            WavReader& rd = *readers[ref.source];
            raw.resize(static_cast<std::size_t>(n) * rd.channels());
            if (rd.read(s0 + b0, n, raw.data()) != n) {
                return abandon(now.sources->find(ref.source)->path + " cannot be read");
            }
            for (model::Frames i = 0; i < n; ++i) {
                float v = raw[static_cast<std::size_t>(i) * rd.channels() + ref.channel];
                const model::Frames w = b0 + i;
                if (!edited[c].empty() && w >= a && w < b) {
                    v = edited[c][lead + static_cast<std::size_t>(w - a)];
                }
                if (clamp) v = std::clamp(v, -1.0f, 1.0f);
                mix[static_cast<std::size_t>(i) * nch + c] = v;
            }
        }
        if (!writer.write(mix.data(), static_cast<std::size_t>(n))) {
            return abandon("cannot write " + file + ": " + writer.failure());
        }
    }
    // Synced, with its folder, before the document may name it.
    if (!writer.checkpoint() || !writer.close()) {
        return abandon("cannot write " + file + ": " +
                       (writer.failure().empty() ? "the disk refused it" : writer.failure()));
    }

    // The numbers come from the file, as for every source.
    WavReader written;
    std::string why;
    if (!written.open(full, why)) {
        std::remove(full.c_str());
        error = file + ": " + why + nothing;
        return false;
    }
    add.source.path = std::string(session_files::kAudioDir) + "/" + file;
    add.source.channels = written.channels();
    add.source.length = written.frames();
    add.source.format = written.encoding() == WavReader::Encoding::Float32
                            ? model::AudioFormat::Float32
                            : model::AudioFormat::Int24;
    add.source.time_reference = static_cast<std::int64_t>(written.time_reference());
    add.source.kind = model::SourceKind::Imported;
    written.close();

    model::Compound c;
    c.label = std::string("Sample Edit: ") + label_of(rq.verb);
    c.ops.push_back(add);
    c.ops.push_back(sources);
    model::ReplicationUpdate u;
    if (!document.apply(c, u, why)) {
        error = "the session refused the edited file, which stays in " +
                std::string(session_files::kAudioDir) + " but not in the session: " + why;
        return false;
    }
    updates.push_back(std::move(u));
    return true;
}

}  // namespace sonora
