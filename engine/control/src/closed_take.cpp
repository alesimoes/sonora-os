// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/closed_take.h"

#include <algorithm>

#include "sonora/model/operations.h"
#include "sonora/session.h"
#include "sonora/wav_reader.h"

namespace sonora {

namespace {

// The file as a source with id `id`, its numbers read from disk.
bool source_of(const model::Document& document, const std::string& session_dir,
               const std::string& relative_path, uint16_t track_channels, model::Id id,
               model::Source& out, std::string& error) {
    WavReader file;
    if (!file.open(session_dir + "/" + relative_path, error)) return false;
    if (file.frames() == 0) {
        error = relative_path + ": no audio";
        return false;
    }
    // Nothing resamples on the way to the bus: a file at another rate would
    // play at the wrong speed and pitch, and out of place.
    const uint32_t rate = document.current().settings.sample_rate;
    if (file.sample_rate() != rate) {
        error = relative_path + ": recorded at " + std::to_string(file.sample_rate()) +
                " Hz, and the session runs at " + std::to_string(rate) + " Hz";
        return false;
    }
    if (file.channels() < track_channels) {
        error = relative_path + ": " + std::to_string(file.channels()) + " channels for a " +
                std::to_string(track_channels) + "-channel track";
        return false;
    }
    out = model::Source{};
    out.id = id;
    out.path = relative_path;
    out.channels = file.channels();
    out.length = file.frames();
    out.format = file.encoding() == WavReader::Encoding::Float32 ? model::AudioFormat::Float32
                                                                 : model::AudioFormat::Int24;
    out.time_reference = static_cast<int64_t>(file.time_reference());
    return true;
}

bool apply(model::Document& document, const model::Operation& op,
           std::vector<model::ReplicationUpdate>& updates, std::string& error) {
    model::ReplicationUpdate u;
    if (!document.apply(op, u, error)) return false;
    updates.push_back(std::move(u));
    return true;
}

const model::TrackEdit* track_at(const model::Document& document, std::size_t track,
                                 std::string& error) {
    const auto& tracks = document.current().edit->tracks;
    if (track >= tracks.size()) {
        error = "there is no track " + std::to_string(track + 1) + " in the session";
        return nullptr;
    }
    return tracks[track].get();
}

model::Region region_of(const model::Source& source, model::Id id, const std::string& name,
                        uint16_t channels) {
    model::Region r;
    r.id = id;
    r.name = name;
    r.position = static_cast<model::Frames>(source.time_reference);
    r.length = source.length;
    for (uint16_t c = 0; c < channels; ++c) r.channels.push_back({source.id, c});
    return r;
}

// "Audio Files/Audio 1_03.wav" names its region "Audio 1_03".
std::string stem(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    std::string s = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = s.rfind('.');
    if (dot != std::string::npos && dot > 0) s.erase(dot);
    return s;
}

}  // namespace

model::Snapshot engine_document(const Session& session) {
    model::SessionSettings settings;
    settings.name = session.config().name;
    settings.sample_rate = session.config().sample_rate;
    settings.audio_format = session.config().format == WavWriter::Format::Float32
                                ? model::AudioFormat::Float32
                                : model::AudioFormat::Int24;
    model::Snapshot s = model::new_session(settings);
    for (std::size_t i = 0; i < session.tracks().size(); ++i) {
        const Track& t = *session.tracks()[i];
        model::AddTrack op;
        op.track = s.next_id;
        op.playlist = s.next_id + 1;
        op.name = t.name();
        op.playlist_name = t.name() + ".01";
        op.channels = t.channels();
        op.mix.input.assign(t.channels(), static_cast<uint16_t>(i));
        model::Snapshot next;
        std::string error;
        // Cannot fail for fresh ids and the engine's own names; if it ever
        // does, the track is left out rather than the session refused, as
        // new_session_with_tracks() does.
        if (model::apply(s, op, next, error)) s = std::move(next);
    }
    // The tracks are where the session starts, not edits to undo.
    s.revision = 0;
    return s;
}

bool place_file(model::Document& document, const std::string& session_dir,
                const std::string& relative_path, std::size_t track,
                std::vector<model::ReplicationUpdate>& updates, std::string& error) {
    const model::TrackEdit* t = track_at(document, track, error);
    if (!t) return false;
    model::AddSource src;
    if (!source_of(document, session_dir, relative_path, t->channels,
                   document.current().next_id, src.source, error)) {
        return false;
    }
    src.source.kind = model::SourceKind::Imported;
    // Copied before the document moves on: `t` belongs to the old state.
    const model::Id playlist = t->active_playlist;
    const uint16_t channels = t->channels;
    if (!apply(document, src, updates, error)) return false;

    model::PlaceRegion place;
    place.playlist = playlist;
    place.region = region_of(src.source, document.current().next_id, stem(relative_path),
                             channels);
    // Needed only when the file lands inside one region; harmless otherwise.
    place.split_right_id = place.region.id + 1;
    return apply(document, place, updates, error);
}

ClosedTake add_closed_take(model::Document& document, const Session& session, int take) {
    ClosedTake out;
    const std::string dir = session.config().dir;
    const std::string prefix = dir + "/";
    const std::string which = "take " + std::to_string(take);

    // Every file is read before any operation exists, so a file that does
    // not read costs its own track and is not a refusal of the take.
    // Three ids per track, reserved whether or not the overwrite splits.
    std::vector<model::AddSource> sources;
    std::vector<model::RecordTake> records;
    model::Id next = document.current().next_id;
    for (std::size_t i = 0; i < session.tracks().size(); ++i) {
        // writer() has no const overload; nothing here changes the track.
        Track& engine_track = *session.tracks()[i];
        // Arming is fixed for a take (session_commands.cpp), so the armed
        // tracks are the take's.
        if (!engine_track.rec_armed() || engine_track.writer().frames_written() == 0) continue;
        const std::string& path = engine_track.writer().path();
        if (path.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string relative = path.substr(prefix.size());

        std::string why;
        const model::TrackEdit* t = track_at(document, i, why);
        model::AddSource src;
        if (t == nullptr || !source_of(document, dir, relative, t->channels, next, src.source,
                                       why)) {
            out.track_notices.push_back(
                which + " on " + (t != nullptr ? t->name : engine_track.name()) +
                " is on disk but not in the session: " + why);
            continue;
        }
        src.source.kind = model::SourceKind::Recorded;
        src.source.take = take;
        src.source.recorded_on_track = t->id;
        model::RecordTake rec;
        rec.track = t->id;
        rec.playlist = t->active_playlist;
        rec.region = region_of(src.source, next + 1, stem(relative), t->channels);
        rec.split_right_id = next + 2;
        next += 3;
        sources.push_back(std::move(src));
        records.push_back(std::move(rec));
    }
    // Two members a track. Past the compound limit the take is split, in
    // track order, one undo step per part: a take that does not fit must
    // still land whole, not fall back to the pool for being large. The ids
    // above rise across the parts, so each part's are still fresh.
    constexpr std::size_t kTracksPerCompound = model::kMaxCompoundMembers / 2;
    for (std::size_t first = 0; first < sources.size(); first += kTracksPerCompound) {
        const std::size_t last = std::min(sources.size(), first + kTracksPerCompound);
        model::Compound c;
        c.label = "Record";
        for (std::size_t k = first; k < last; ++k) {
            c.ops.push_back(sources[k]);
            c.ops.push_back(records[k]);
        }
        std::string why;
        model::ReplicationUpdate u;
        if (document.apply(model::Operation{std::move(c)}, u, why)) {
            out.updates.push_back(std::move(u));
            continue;
        }

        // The engine built these operations, so a refusal is a bug in one
        // of them or in the model. The audio must not be lost to it: into
        // the pool it goes, where a save records it, and the operator is
        // told.
        if (!out.refused.empty()) out.refused += "; ";
        out.refused += which + " is on disk and in the session's audio, but not on the timeline: " +
                       why;
        for (std::size_t k = first; k < last; ++k) {
            std::string error;
            if (!apply(document, sources[k], out.updates, error)) {
                out.refused += "; and " + sources[k].source.path +
                               " could not be added either: " + error;
            }
        }
    }
    return out;
}

}  // namespace sonora
