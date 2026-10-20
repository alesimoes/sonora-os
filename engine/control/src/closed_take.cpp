// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/closed_take.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <unordered_map>

#include "sonora/frame_text.h"
#include "sonora/model/operations.h"
#include "sonora/session.h"
#include "sonora/wav_reader.h"
#include "sonora/wav_recovery.h"

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

model::FadeShape shape_of(FadeCurve c) {
    switch (c) {
        case FadeCurve::Linear: return model::FadeShape::Linear;
        case FadeCurve::SCurve: return model::FadeShape::SCurve;
        case FadeCurve::EqualPower: break;
    }
    return model::FadeShape::EqualPower;
}

model::Frames frames_of(uint64_t f) {
    return f > static_cast<uint64_t>(model::kMaxInteger) ? model::kMaxInteger
                                                         : static_cast<model::Frames>(f);
}

const model::Playlist* playlist_by_id(const model::EditState& edit, model::Id id) {
    for (const auto& t : edit.tracks) {
        for (const auto& p : t->playlists) {
            if (p->id == id) return p.get();
        }
    }
    return nullptr;
}

// What the region's shortest channel has, as the model measures handle.
model::Frames shortest_source(const model::Snapshot& s, const model::Region& r) {
    model::Frames shortest = model::kMaxInteger;
    for (const model::SourceRef& c : r.channels) {
        const model::Source* src = s.sources->find(c.source);
        shortest = std::min(shortest, src ? src->length : model::Frames{0});
    }
    return shortest;
}

// Cuts `rec`, a whole file at its TimeReference, to the punch range, and
// asks for the crossfades it can have against `document` before the take.
// A crossfade without handle on both sides is not asked for: the model
// refuses rather than shortens it, and losing the take to it is worse than
// a butt splice. False when nothing was recorded inside the range.
bool cut_to_punch(const model::Snapshot& document, const PunchRange& punch,
                  model::RecordTake& rec) {
    model::Region& n = rec.region;
    const model::Frames origin = n.position;
    const model::Frames file_end = n.end();
    const model::Frames a = frames_of(punch.monitor_in);
    const model::Frames b = frames_of(punch.monitor_out);
    const model::Frames from = std::max(a, origin);
    const model::Frames to = std::min(b, file_end);
    if (to <= from) return false;
    n.position = from;
    n.length = to - from;
    n.source_offset = from - origin;

    const model::Playlist* p = playlist_by_id(*document.edit, rec.playlist);
    if (punch.crossfade == 0 || p == nullptr) return true;
    model::Crossfade x;
    x.length = punch.crossfade;
    x.align = model::Align::Centre;
    x.shape = shape_of(punch.shape);
    model::Frames before = 0, after = 0;
    model::crossfade_extent(x, before, after);

    // A take that started late, or stopped early, meets what is beneath at
    // a point the operator did not set as a punch point: no crossfade there.
    if (from == a) {
        for (const model::Region& r : p->regions) {
            if (r.position >= a || r.end() < a) continue;
            const model::Frames kept = a - r.position;
            const model::Frames handle = shortest_source(document, r) - r.source_offset - kept;
            if (before <= kept && after <= n.length && handle >= after &&
                n.source_offset >= before) {
                x.left = r.id;
                x.right = n.id;
                rec.crossfades.push_back(x);
            }
            break;
        }
    }
    if (to == b) {
        for (const model::Region& r : p->regions) {
            if (r.position > b || r.end() <= b) continue;
            // Spanning the whole take it is split, and its right part is
            // the new id; otherwise it keeps its id and starts at B.
            const model::Id right = r.position < from ? rec.split_right_id : r.id;
            const model::Frames right_offset = r.source_offset + (b - r.position);
            if (before <= n.length && after <= r.end() - b && file_end - to >= after &&
                right_offset >= before) {
                x.left = n.id;
                x.right = right;
                rec.crossfades.push_back(x);
            }
            break;
        }
    }
    return true;
}

// One notice per fade beside a punch that its crossfade shortened: Pro
// Tools truncates such a fade and never refuses the record, and so does the
// model (RecordTake fits fades to its crossfades), but the operator should
// hear what changed. Only the regions a crossfade of `rec` joins the take
// to are looked at, so a fade a plain overwrite clamped (no crossfade) is
// never blamed on one. Only an edge the take did not cut is compared: the
// fade the overwrite dropped at the cut, where the crossfade now sits, is
// not news. The right part of a region the take split is compared with
// that region, whose fade-out it carries.
void say_shortened_fades(const model::EditState& before, const model::Snapshot& after,
                         const model::RecordTake& rec, int track, const std::string& which,
                         std::vector<std::string>& out) {
    if (rec.crossfades.empty()) return;
    const model::Playlist* was = playlist_by_id(before, rec.playlist);
    const model::Playlist* now = playlist_by_id(*after.edit, rec.playlist);
    if (was == nullptr || now == nullptr) return;
    const model::Region& take = rec.region;
    std::unordered_map<model::Id, const model::Region*> old;
    const model::Region* split = nullptr;
    for (const model::Region& o : was->regions) {
        old.emplace(o.id, &o);
        if (o.position < take.position && o.end() > take.end()) split = &o;
    }
    std::unordered_map<model::Id, const model::Region*> current;
    for (const model::Region& r : now->regions) current.emplace(r.id, &r);

    const double rate = after.settings.sample_rate;
    const auto ms = [rate](model::Frames f) {
        char text[32];
        std::snprintf(text, sizeof text, "%.1f ms", rate > 0 ? 1000.0 * f / rate : 0.0);
        return std::string(text);
    };
    for (const model::Crossfade& x : rec.crossfades) {
        const model::Id id = x.left == take.id ? x.right : x.left;
        const auto at = current.find(id);
        if (at == current.end()) continue;
        const model::Region& r = *at->second;
        const auto was_at = old.find(id);
        const model::Region* origin = was_at != old.end() ? was_at->second
                                      : id == rec.split_right_id ? split
                                                                 : nullptr;
        if (origin == nullptr) continue;
        const auto say = [&](const char* end, const model::Fade& from, const model::Fade& to) {
            if (to.length >= from.length) return;
            out.push_back(which + " shortened the " + end + " of \"" + r.name + "\" on track " +
                          std::to_string(track) + " from " + ms(from.length) + " to " +
                          ms(to.length) + " to make room for its crossfade");
        };
        if (r.position == origin->position) say("fade-in", origin->fade_in, r.fade_in);
        if (r.end() == origin->end()) say("fade-out", origin->fade_out, r.fade_out);
    }
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

namespace {

// Why a punched take holds no frame: from where its first block began and
// where it was ended.
std::string why_nothing(const Session::ClosedTake& take) {
    const auto frame = frame_text;
    const PunchRange& p = take.punch;
    if (take.origin == Session::kNoOrigin) {
        return "it ended before the audio thread ran a block of it";
    }
    // The #91 race: start_punch() checked the playhead before B, and by the
    // time the files were open the transport had rolled past B + H.
    if (take.origin >= p.capture_to) {
        return "it began at " + frame(take.origin) + ", after its capture ended at " +
               frame(p.capture_to) + " (the punch-out at " + frame(p.monitor_out) +
               ", plus its handle): the transport passed the punch range while the take's"
               " files were being opened";
    }
    if (take.end <= p.capture_from) {
        return "it ended before " + frame(p.capture_from) + ", where its capture was to begin"
               " (the punch-in at " + frame(p.monitor_in) + ", less its handle)";
    }
    // Rolled into the range and recorded nothing: every frame was lost.
    const uint64_t from = take.origin > p.capture_from ? take.origin : p.capture_from;
    return "nothing reached its files from " + frame(from) +
           " on: the capture or boundary ring overran";
}

}  // namespace

TakeAdded add_closed_take(model::Document& document, const Session& session,
                          const Session::ClosedTake& take, std::size_t files_per_compound) {
    TakeAdded out;
    const std::string dir = session.config().dir;
    const std::string prefix = dir + "/";
    const std::string which = "take " + std::to_string(take.take);
    std::string first_failure;
    int more_failures = 0;

    // Every file is read before any operation exists, so a file that does
    // not read costs its own track and is not a refusal of the take. Three
    // ids per file, reserved whether or not the overwrite splits.
    std::vector<model::AddSource> sources;
    std::vector<model::RecordTake> records;
    // False for a punched file with nothing inside the punch range: its
    // source goes in alone.
    std::vector<bool> placed;
    std::vector<int> numbers;   // the track each file is on, from 1
    model::Id next = document.current().next_id;
    for (const DiskThread::ClosedFile& f : take.files) {
        // A failed file is still valid up to its last whole frame
        // (bwf-broadcast-wave.md), so what was recorded before the failure
        // is placed, and the failure is reported rather than hidden
        // (timeline-model.md, "Closed takes").
        if (!f.failure.empty()) {
            if (first_failure.empty()) {
                first_failure = f.path + ": " + f.failure +
                                (f.frames == 0 ? ", with nothing recorded before it"
                                               : ", placed up to the failure");
            } else {
                ++more_failures;
            }
        }
        if (f.frames == 0) continue;
        if (f.path.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string relative = f.path.substr(prefix.size());

        std::string why;
        const model::TrackEdit* t = track_at(document, static_cast<std::size_t>(f.track), why);
        // Arming refuses these tracks, but one can come to sit on an armed
        // engine track (an add or a reorder during the take). Its file is
        // the capture channel's audio, which is not what the track records.
        if (t != nullptr && t->kind != model::TrackKind::Audio) {
            out.track_notices.push_back(
                which + " is on disk but not in the session: track " +
                std::to_string(f.track + 1) + " is a " +
                (t->kind == model::TrackKind::Midi ? "MIDI" : "instrument") +
                " track, which records no audio, " + f.path);
            continue;
        }
        model::AddSource src;
        if (t == nullptr ||
            !source_of(document, dir, relative, t->channels, next, src.source, why)) {
            out.track_notices.push_back(which + " is on disk but not in the session: track " +
                                        std::to_string(f.track + 1) + ", " + f.path + ": " +
                                        why);
            continue;
        }
        src.source.kind = model::SourceKind::Recorded;
        src.source.take = take.take;
        src.source.recorded_on_track = t->id;
        model::RecordTake rec;
        rec.track = t->id;
        rec.playlist = t->active_playlist;
        rec.region = region_of(src.source, next + 1, stem(relative), t->channels);
        rec.split_right_id = next + 2;
        next += 3;
        const model::Frames origin = rec.region.position;
        const bool in_range = !take.punched || cut_to_punch(document.current(), take.punch, rec);
        // start_punch() refuses a punch-out behind the playhead, but the
        // files open after that check while the audio thread rolls on, so a
        // press just before B can capture from B on. Stopped before A is
        // the operator's own choice and needs no word.
        if (!in_range && origin >= frames_of(take.punch.monitor_out)) {
            out.track_notices.push_back(
                which + " is in the session's audio, not on the timeline: track " +
                std::to_string(f.track + 1) + " started recording at " +
                frame_text(static_cast<uint64_t>(origin)) + ", after the punch-out at " +
                frame_text(take.punch.monitor_out));
        }
        placed.push_back(in_range);
        sources.push_back(std::move(src));
        records.push_back(std::move(rec));
        numbers.push_back(f.track + 1);
    }
    // A punch that ends with nothing in any file would otherwise vanish
    // without a word: no source, no region, no undo step. A failed file is
    // said by cut_short instead.
    const bool captured = std::any_of(take.files.begin(), take.files.end(),
                                      [](const DiskThread::ClosedFile& f) { return f.frames != 0; });
    if (take.punched && !captured && first_failure.empty()) {
        out.track_notices.push_back(which + " captured nothing: " + why_nothing(take));
    }
    if (!first_failure.empty()) {
        out.cut_short = which + " was cut short: " + first_failure;
        if (more_failures > 0) {
            out.cut_short += " (and " + std::to_string(more_failures) + " more track" +
                             (more_failures == 1 ? "" : "s") + ")";
        }
    }

    // Two members a file. Past the compound limit the take is split, in
    // track order, one undo step per part: a take that does not fit must
    // still land whole, not fall back to the pool for being large. The ids
    // above rise across the parts, so each part's are still fresh.
    const std::size_t per_part =
        std::clamp<std::size_t>(files_per_compound, 1, model::kMaxCompoundMembers / 2);
    for (std::size_t first = 0; first < sources.size(); first += per_part) {
        const std::size_t last = std::min(sources.size(), first + per_part);
        const auto part_of = [&](bool crossfades) {
            model::Compound c;
            c.label = "Record";
            for (std::size_t k = first; k < last; ++k) {
                c.ops.push_back(sources[k]);
                if (!placed[k]) continue;
                model::RecordTake rec = records[k];
                if (!crossfades) rec.crossfades.clear();
                c.ops.push_back(std::move(rec));
            }
            return model::Operation{std::move(c)};
        };
        const auto say_fades = [&](const model::EditState& before) {
            for (std::size_t k = first; k < last; ++k) {
                if (!placed[k]) continue;
                say_shortened_fades(before, document.current(), records[k], numbers[k], which,
                                    out.fade_notices);
            }
        };
        const std::shared_ptr<const model::EditState> before = document.current().edit;
        std::string why;
        model::ReplicationUpdate u;
        if (document.apply(part_of(true), u, why)) {
            out.updates.push_back(std::move(u));
            if (take.punched) say_fades(*before);
            continue;
        }
        // A punch's crossfades are checked against the model's rules here,
        // but a rule this side does not know of must cost the crossfades,
        // not the take.
        const bool had_crossfades =
            std::any_of(records.begin() + static_cast<std::ptrdiff_t>(first),
                        records.begin() + static_cast<std::ptrdiff_t>(last),
                        [](const model::RecordTake& r) { return !r.crossfades.empty(); });
        std::string plain_why;
        if (had_crossfades && document.apply(part_of(false), u, plain_why)) {
            out.updates.push_back(std::move(u));
            out.track_notices.push_back(which + " was punched in without its crossfades: " +
                                        why);
            continue;
        }

        // The engine built these operations, so a refusal is a bug in one
        // of them or in the model. One bad member must not cost the other
        // tracks their takes (PR #100 review): file by file, each that the
        // model takes lands as its own step, and the audio of each it
        // refuses goes into the pool, where a save records it, and the
        // operator is told by track.
        // Each landed file is its own undo step, so undoing this take takes
        // several: the model names no member when it refuses a compound, so
        // a compound of the survivors could be refused again for a reason
        // nobody here knows, and that would cost them all. This path is a
        // model bug; landing the takes outweighs a one-step undo.
        std::vector<std::size_t> failed;
        std::string first_why;
        for (std::size_t k = first; k < last; ++k) {
            const auto one = [&](bool crossfades) {
                model::Compound c;
                c.label = "Record";
                c.ops.push_back(sources[k]);
                if (placed[k]) {
                    model::RecordTake rec = records[k];
                    if (!crossfades) rec.crossfades.clear();
                    c.ops.push_back(std::move(rec));
                }
                return model::Operation{std::move(c)};
            };
            std::string alone;
            const std::shared_ptr<const model::EditState> was = document.current().edit;
            if (document.apply(one(true), u, alone)) {
                out.updates.push_back(std::move(u));
                if (take.punched && placed[k]) {
                    say_shortened_fades(*was, document.current(), records[k], numbers[k], which,
                                        out.fade_notices);
                }
                continue;
            }
            if (!records[k].crossfades.empty() && document.apply(one(false), u, plain_why)) {
                out.updates.push_back(std::move(u));
                out.track_notices.push_back(which + " (track " + std::to_string(numbers[k]) +
                                            ") was punched in without its crossfades: " + alone);
                continue;
            }
            failed.push_back(k);
            if (first_why.empty()) first_why = alone;
            std::string error;
            if (!apply(document, sources[k], out.updates, error)) {
                first_why += "; and " + sources[k].source.path +
                             " could not be added either: " + error;
            }
        }
        if (failed.empty()) continue;
        std::string part = which;
        if (failed.size() == last - first && per_part < sources.size()) {
            part += " (tracks " + std::to_string(numbers[first]) + " to " +
                    std::to_string(numbers[last - 1]) + ")";
        } else if (failed.size() < sources.size()) {
            part += failed.size() == 1 ? " (track " : " (tracks ";
            for (std::size_t i = 0; i < failed.size(); ++i) {
                part += (i == 0 ? "" : ", ") + std::to_string(numbers[failed[i]]);
            }
            part += ")";
        }
        if (!out.refused.empty()) out.refused += "; ";
        out.refused += part + " is on disk and in the session's audio, but not on the timeline: " +
                       first_why;
    }
    return out;
}

bool may_arm_track(const model::Snapshot& document, std::size_t engine_track, std::string& why) {
    const auto& tracks = document.edit->tracks;
    if (engine_track >= tracks.size()) return true;
    const model::TrackEdit& t = *tracks[engine_track];
    if (t.kind == model::TrackKind::Audio) return true;
    why = "\"" + t.name + "\" is a " + (t.kind == model::TrackKind::Midi ? "MIDI" : "instrument") +
          " track; this version records audio only";
    return false;
}

bool document_punch(const model::Snapshot& document, PunchRange& out) {
    const auto& t = document.settings.transport;
    const model::Frames a = t.punch[0];
    const model::Frames b = t.punch[1];
    if (a < 0 || b <= a) return false;
    // H = max(punch_crossfade, 1 s): timeline-model.md, "Punch in".
    const model::Frames x = std::clamp<model::Frames>(t.punch_crossfade, 0, UINT32_MAX);
    const model::Frames h =
        std::max<model::Frames>(x, static_cast<model::Frames>(document.settings.sample_rate));
    out = PunchRange{};
    out.capture_from = static_cast<uint64_t>(a > h ? a - h : 0);
    out.capture_to = static_cast<uint64_t>(b + h);
    out.monitor_in = static_cast<uint64_t>(a);
    out.monitor_out = static_cast<uint64_t>(b);
    out.crossfade = static_cast<uint32_t>(x);
    // The shape session-format.md gives the punch crossfade.
    out.shape = FadeCurve::EqualPower;
    return true;
}

std::vector<std::string> recover_unclosed_takes(const std::string& session_dir,
                                                uint32_t sample_rate) {
    std::vector<std::string> notices;
    for (const TakeRecovery& r : recover_takes(session_dir + "/Audio Files")) {
        const std::string name = std::filesystem::path(r.path).filename().string();
        switch (r.outcome) {
            case TakeRecovery::Outcome::Repaired: {
                // Killed before any audio: one per armed track, up to 18,
                // and nothing to keep. The header is repaired all the same.
                if (r.frames == 0) break;
                char seconds[32];
                std::snprintf(seconds, sizeof seconds, "%.1f",
                              sample_rate ? static_cast<double>(r.frames) / sample_rate : 0.0);
                notices.push_back("recovered " + name +
                                  ": the engine stopped before closing it; " + seconds +
                                  " s were on disk and are kept. It is in Audio Files, "
                                  "not on the timeline");
                break;
            }
            case TakeRecovery::Outcome::Short:
                notices.push_back(name + " is shorter than its header (" + r.why +
                                  "); the part on disk plays");
                break;
            default:
                notices.push_back("could not recover " + name + ": " + r.why);
                break;
        }
    }
    return notices;
}

}  // namespace sonora
