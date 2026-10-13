// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/take_document.h"

#include "sonora/model/operations.h"
#include "sonora/session.h"
#include "sonora/wav_reader.h"

namespace sonora {

TakeDocument::TakeDocument(const Session& session)
    : dir_(session.config().dir), history_([&session] {
          model::SessionSettings s;
          s.name = session.config().name;
          s.sample_rate = session.config().sample_rate;
          s.audio_format = session.config().format == WavWriter::Format::Float32
                               ? model::AudioFormat::Float32
                               : model::AudioFormat::Int24;
          return model::new_session(s);
      }()) {
    for (std::size_t i = 0; i < session.tracks().size(); ++i) {
        const Track& t = *session.tracks()[i];
        model::AddTrack op;
        op.track = history_.current().next_id;
        op.playlist = op.track + 1;
        op.name = t.name();
        op.playlist_name = t.name() + ".01";
        op.channels = t.channels();
        for (uint16_t c = 0; c < t.channels(); ++c) {
            op.mix.input.push_back(static_cast<uint16_t>(i));
        }
        std::string error;
        // Cannot fail: fresh ids, valid names, one or two channels.
        history_.apply(op, error);
        playlists_.push_back(op.playlist);
    }
}

bool TakeDocument::place_file(const std::string& relative_path, std::size_t track,
                              std::string& error) {
    if (track >= playlists_.size()) {
        error = "there is no track " + std::to_string(track + 1);
        return false;
    }
    WavReader file;
    if (!file.open(dir_ + "/" + relative_path, error)) return false;
    if (file.frames() == 0) {
        error = relative_path + ": no audio";
        return false;
    }
    const model::TrackEdit& t = *history_.current().edit->tracks[track];
    if (file.channels() < t.channels) {
        error = relative_path + ": " + std::to_string(file.channels()) + " channels for a " +
                std::to_string(t.channels) + "-channel track";
        return false;
    }
    const model::Id id = history_.current().next_id;
    model::AddSource src;
    src.source.id = id;
    src.source.path = relative_path;
    src.source.channels = file.channels();
    src.source.length = file.frames();
    src.source.format = file.encoding() == WavReader::Encoding::Float32
                            ? model::AudioFormat::Float32
                            : model::AudioFormat::Int24;
    src.source.time_reference = static_cast<int64_t>(file.time_reference());
    src.source.kind = model::SourceKind::Imported;
    if (!history_.apply(src, error)) return false;

    model::PlaceRegion place;
    place.playlist = playlists_[track];
    place.region.id = id + 1;
    place.region.name = relative_path;
    place.region.position = static_cast<model::Frames>(file.time_reference());
    place.region.length = file.frames();
    for (uint16_t c = 0; c < t.channels; ++c) place.region.channels.push_back({id, c});
    // Needed only when the take lands inside one region; harmless otherwise.
    place.split_right_id = id + 2;
    return history_.apply(place, error);
}

bool TakeDocument::add_closed_take(const Session& session, std::string& error) {
    const std::string prefix = dir_ + "/";
    for (std::size_t i = 0; i < session.tracks().size(); ++i) {
        Track& t = *session.tracks()[i];
        // Arming is fixed for a take, so the armed tracks are the take's.
        if (!t.rec_armed() || t.writer().frames_written() == 0) continue;
        const std::string& path = t.writer().path();
        if (path.compare(0, prefix.size(), prefix) != 0) continue;
        if (!place_file(path.substr(prefix.size()), i, error)) return false;
    }
    return true;
}

}  // namespace sonora
