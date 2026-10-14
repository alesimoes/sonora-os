// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/closed_take.h"

#include "sonora/model/operations.h"
#include "sonora/session.h"
#include "sonora/wav_reader.h"

namespace sonora {

namespace {

// The file as a source with the next free id, its numbers read from disk.
bool source_of(const model::Document& document, const std::string& session_dir,
               const std::string& relative_path, uint16_t track_channels,
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
    out.id = document.current().next_id;
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
    if (!source_of(document, session_dir, relative_path, t->channels, src.source, error)) {
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

bool add_closed_take(model::Document& document, const Session& session,
                     const Session::ClosedTake& take,
                     std::vector<model::ReplicationUpdate>& updates, std::string& error) {
    const std::string dir = session.config().dir;
    const std::string prefix = dir + "/";
    for (const DiskThread::ClosedFile& f : take.files) {
        if (f.frames == 0) continue;
        if (f.path.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string relative = f.path.substr(prefix.size());

        const model::TrackEdit* t = track_at(document, static_cast<std::size_t>(f.track), error);
        if (!t) return false;
        model::AddSource src;
        if (!source_of(document, dir, relative, t->channels, src.source, error)) return false;
        src.source.kind = model::SourceKind::Recorded;
        src.source.take = take.take;
        src.source.recorded_on_track = t->id;
        // Copied before the document moves on: `t` belongs to the old state.
        const model::Id track_id = t->id;
        const model::Id playlist = t->active_playlist;
        const uint16_t channels = t->channels;
        if (!apply(document, src, updates, error)) return false;

        model::RecordTake rec;
        rec.track = track_id;
        rec.playlist = playlist;
        rec.region = region_of(src.source, document.current().next_id, stem(relative), channels);
        rec.split_right_id = rec.region.id + 1;
        if (!apply(document, rec, updates, error)) return false;
    }
    return true;
}

}  // namespace sonora
