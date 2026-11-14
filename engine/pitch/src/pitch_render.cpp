// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/pitch_render.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <utility>

#include "sonora/model/operations.h"
#include "sonora/session.h"
#include "sonora/session_files/session_folder.h"
#include "sonora/wav_reader.h"
#include "sonora/wav_writer.h"

namespace sonora {

namespace {

const char* const kNothing = "; nothing was edited";
constexpr model::Frames kBlock = 4096;
// Header, chunk padding and the file system's own rounding.
constexpr std::uint64_t kFileSlackBytes = 1u << 20;

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
                if (r.id == id) f = Found{t.get(), p.get(), &r};
            }
        }
    }
    return f;
}

std::string file_safe(std::string name) {
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':') c = '_';
    }
    return name;
}

// The gain line's level at source frame `src`, linear in amplitude between
// points and held outside them: the engine's reading, as sample.edit's.
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

// The gain line in the copy's frames, which start `s0` later: the points
// before the copy fold into one at frame 0 holding their level there.
std::vector<model::GainPoint> shifted_line(const std::vector<model::GainPoint>& line,
                                           model::Frames s0) {
    std::vector<model::GainPoint> out;
    if (line.empty()) return out;
    if (s0 > 0 && line.front().at < s0) out.push_back({0, line_db_at(line, s0)});
    for (const model::GainPoint& p : line) {
        if (p.at < s0 || (p.at == s0 && !out.empty())) continue;
        out.push_back({p.at - s0, p.db});
    }
    return out;
}

// Every note inside the selection, which is inside the region.
bool check_notes(const PitchRenderRequest& rq, const model::Region& region, std::string& why) {
    if (rq.from < region.position || rq.to > region.end() || rq.to <= rq.from) {
        why = "select a stretch inside the region first";
        return false;
    }
    if (!api::validate_pitch_spec(rq.spec, why) ||
        !api::pitch_notes_fit(rq.spec, static_cast<std::uint64_t>(region.length), why)) {
        return false;
    }
    const auto lo = static_cast<std::uint64_t>(rq.from - region.position);
    const auto hi = static_cast<std::uint64_t>(rq.to - region.position);
    for (std::size_t i = 0; i < rq.spec.notes.size(); ++i) {
        const api::PitchNote& n = rq.spec.notes[i];
        if (n.start < lo || n.end > hi) {
            why = "pitch: note " + std::to_string(i + 1) + " lies outside the selection";
            return false;
        }
    }
    return true;
}

using Readers = std::map<model::Id, std::unique_ptr<WavReader>>;

bool open_readers(const model::Snapshot& now, const model::Region& region,
                  const std::string& session_dir, Readers& readers, std::string& why) {
    for (const model::SourceRef& ref : region.channels) {
        const model::Source* src = now.sources->find(ref.source);
        if (src == nullptr) {
            why = "region \"" + region.name + "\": its source is not in the pool";
            return false;
        }
        if (readers.count(ref.source) != 0) continue;
        auto rd = std::make_unique<WavReader>();
        if (!rd->open(session_dir + "/" + src->path, why)) {
            why = src->path + ": " + why;
            return false;
        }
        if (rd->sample_rate() != now.settings.sample_rate) {
            why = src->path + " is at " + std::to_string(rd->sample_rate()) +
                  " Hz, and the session runs at " + std::to_string(now.settings.sample_rate) + " Hz";
            return false;
        }
        if (ref.channel >= rd->channels()) {
            why = src->path + " has no channel " + std::to_string(ref.channel + 1);
            return false;
        }
        readers[ref.source] = std::move(rd);
    }
    return true;
}

// The copy's source frames [s0, s0 + total): the region and a handle either
// side, as far as every channel's source goes.
void copy_window(const model::Snapshot& now, const model::Region& region, model::Frames& s0,
                 model::Frames& total) {
    const model::Frames handle =
        static_cast<model::Frames>(kPitchHandleSeconds * now.settings.sample_rate);
    model::Frames limit = region.source_offset + region.length + handle;
    for (const model::SourceRef& ref : region.channels) {
        limit = std::min(limit, now.sources->find(ref.source)->length);
    }
    s0 = std::max<model::Frames>(0, region.source_offset - handle);
    total = std::max(limit, region.source_offset + region.length) - s0;
}

bool read_window(const model::Region& region, Readers& readers, model::Frames s0,
                 model::Frames total, pitch::Planar& out) {
    const std::size_t nch = region.channels.size();
    out.assign(nch, std::vector<float>(static_cast<std::size_t>(total)));
    std::vector<float> raw;
    for (model::Frames b0 = 0; b0 < total; b0 += kBlock) {
        const model::Frames n = std::min(kBlock, total - b0);
        for (std::size_t c = 0; c < nch; ++c) {
            const model::SourceRef& ref = region.channels[c];
            WavReader& rd = *readers[ref.source];
            raw.resize(static_cast<std::size_t>(n) * rd.channels());
            if (rd.read(s0 + b0, n, raw.data()) != n) return false;
            for (model::Frames i = 0; i < n; ++i) {
                out[c][static_cast<std::size_t>(b0 + i)] =
                    raw[static_cast<std::size_t>(i) * rd.channels() + ref.channel];
            }
        }
    }
    return true;
}

bool write_window(WavWriter& writer, const pitch::Planar& audio, bool clamp) {
    const std::size_t nch = audio.size();
    const std::size_t total = audio[0].size();
    std::vector<float> mix(static_cast<std::size_t>(kBlock) * nch);
    for (std::size_t b0 = 0; b0 < total; b0 += kBlock) {
        const std::size_t n = std::min<std::size_t>(kBlock, total - b0);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t c = 0; c < nch; ++c) {
                const float v = audio[c][b0 + i];
                mix[i * nch + c] = clamp ? std::clamp(v, -1.0f, 1.0f) : v;
            }
        }
        if (!writer.write(mix.data(), n)) return false;
    }
    return true;
}

// Opens "<track>_pitch_NN.wav" with exclusive create: an existing name is
// never touched, the next number is tried.
bool open_copy(WavWriter& writer, const std::string& dir, const std::string& track,
               const model::Snapshot& now, std::uint16_t nch, WavWriter::Format format,
               std::uint64_t origin, std::string& file, std::string& why) {
    for (int n = 1; n <= 9999; ++n) {
        char digits[16];
        std::snprintf(digits, sizeof digits, "%02d", n);
        file = file_safe(track) + "_pitch_" + digits + ".wav";
        if (writer.open(dir + "/" + file, now.settings.sample_rate, nch, format, origin)) return true;
        if (errno != EEXIST) {
            why = "cannot create " + file + ": " + std::strerror(errno);
            return false;
        }
    }
    why = "no free pitch file name for " + track;
    return false;
}

}  // namespace

std::uint64_t free_bytes_on_disk(const std::string& dir) {
    std::error_code ec;
    const std::filesystem::space_info s = std::filesystem::space(dir, ec);
    if (ec) return std::numeric_limits<std::uint64_t>::max();
    return static_cast<std::uint64_t>(s.available);
}

bool render_pitch(model::Document& document, const Session& session, const std::string& session_dir,
                  const PitchRenderRequest& rq, pitch::PitchShifter& shifter,
                  const FreeBytes& free_bytes, std::vector<model::ReplicationUpdate>& updates,
                  std::string& error) {
    if (session.recording_take() != 0 || session.takes_closing()) {
        error = std::string("stop recording before editing pitch") + kNothing;
        return false;
    }
    const model::Snapshot now = document.current();
    const Found found = find_region(now, rq.region);
    if (found.region == nullptr) {
        error = std::string("no such region") + kNothing;
        return false;
    }
    const model::Region& region = *found.region;
    if (found.track->kind != model::TrackKind::Audio) {
        error = found.track->name + " is not an audio track" + kNothing;
        return false;
    }
    std::string why;
    if (!check_notes(rq, region, why)) {
        error = why + kNothing;
        return false;
    }
    Readers readers;
    if (!open_readers(now, region, session_dir, readers, why)) {
        error = why + kNothing;
        return false;
    }
    model::Frames s0 = 0, total = 0;
    copy_window(now, region, s0, total);
    if (total > kMaxPitchRenderFrames) {
        error = "the region and its handles are " + std::to_string(total) +
                " frames; a pitch edit renders at most " + std::to_string(kMaxPitchRenderFrames) +
                kNothing;
        return false;
    }
    const std::size_t nch = region.channels.size();

    // Asked of the model first, with the copy's size, so a refusal costs no write.
    model::AddSource add;
    add.source.id = now.next_id;
    add.source.path = std::string(session_files::kAudioDir) + "/pending.wav";
    add.source.channels = static_cast<std::uint16_t>(nch);
    add.source.length = total;
    model::SetRegionSources sources;
    sources.playlist = found.playlist->id;
    sources.region = region.id;
    for (std::size_t c = 0; c < nch; ++c) {
        sources.channels.push_back({add.source.id, static_cast<std::uint16_t>(c)});
    }
    sources.source_offset = region.source_offset - s0;
    sources.gain_line = shifted_line(region.gain_line, s0);
    {
        model::Snapshot with, after;
        if (!model::apply(now, add, with, why) || !model::apply(with, sources, after, why)) {
            error = "the session would refuse the edit: " + why + kNothing;
            return false;
        }
    }

    const std::string dir = session_dir + "/" + session_files::kAudioDir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir, ec)) {
        error = "cannot create " + dir + kNothing;
        return false;
    }
    const WavWriter::Format format = now.settings.audio_format == model::AudioFormat::Float32
                                         ? WavWriter::Format::Float32
                                         : WavWriter::Format::Int24;
    const std::uint64_t sample_bytes = format == WavWriter::Format::Float32 ? 4 : 3;
    const std::uint64_t need =
        static_cast<std::uint64_t>(total) * nch * sample_bytes + kFileSlackBytes;
    const std::uint64_t available = free_bytes(dir);
    if (available < need) {
        error = "not enough free space for the pitch copy: " + std::to_string(need >> 20) +
                " MB needed, " + std::to_string(available >> 20) + " MB free" + kNothing;
        return false;
    }

    const model::Source* first_src = now.sources->find(region.channels[0].source);
    const std::int64_t origin = std::max<std::int64_t>(0, first_src->time_reference + s0);
    WavWriter writer;
    writer.set_description("Pitch edit");
    std::string file;
    if (!open_copy(writer, dir, found.track->name, now, static_cast<std::uint16_t>(nch), format,
                   static_cast<std::uint64_t>(origin), file, why)) {
        error = why + kNothing;
        return false;
    }
    const std::string full = dir + "/" + file;
    const auto abandon = [&](const std::string& reason) {
        writer.close();
        std::remove(full.c_str());
        error = reason + kNothing;
        return false;
    };

    pitch::Planar input, output;
    if (!read_window(region, readers, s0, total, input)) {
        return abandon(now.sources->find(region.channels[0].source)->path + " cannot be read");
    }
    // The shifter sees the copy's frames: the notes move by the lead handle.
    api::PitchEditSpec spec = rq.spec;
    const auto lead = static_cast<std::uint64_t>(region.source_offset - s0);
    for (api::PitchNote& n : spec.notes) {
        n.start += lead;
        n.end += lead;
    }
    if (!shifter.shift(input, now.settings.sample_rate, spec, output, why)) return abandon(why);
    input.clear();
    bool same = output.size() == nch;
    for (std::size_t c = 0; same && c < nch; ++c) {
        same = output[c].size() == static_cast<std::size_t>(total);
    }
    if (!same) {
        return abandon("the shifted audio is not the region's length; a pitch edit never "
                       "moves what follows");
    }
    if (!write_window(writer, output, format == WavWriter::Format::Int24)) {
        return abandon("cannot write " + file + ": " + writer.failure());
    }
    // Synced, with its folder, before the document may name it.
    if (!writer.checkpoint() || !writer.close()) {
        return abandon("cannot write " + file + ": " +
                       (writer.failure().empty() ? "the disk refused it" : writer.failure()));
    }

    // The numbers come from the file, as for every source.
    WavReader written;
    if (!written.open(full, why)) {
        std::remove(full.c_str());
        error = file + ": " + why + kNothing;
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
    c.label = "Sample Edit: Pitch";
    c.ops.push_back(add);
    c.ops.push_back(sources);
    model::ReplicationUpdate u;
    if (!document.apply(c, u, why)) {
        error = "the session refused the pitch copy, which stays in " +
                std::string(session_files::kAudioDir) + " but not in the session: " + why;
        return false;
    }
    updates.push_back(std::move(u));
    return true;
}

bool pitch_sample_edit(model::Document& document, const Session& session,
                       const std::string& session_dir, const PitchRenderRequest& request,
                       std::vector<model::ReplicationUpdate>& updates, std::string& error) {
    const std::unique_ptr<pitch::PitchShifter> shifter = make_pitch_shifter();
    if (!shifter) {
        error = std::string("pitch editing is not in this build: the engine was built without "
                            "Rubber Band") + kNothing;
        return false;
    }
    return render_pitch(document, session, session_dir, request, *shifter, free_bytes_on_disk,
                        updates, error);
}

}  // namespace sonora
