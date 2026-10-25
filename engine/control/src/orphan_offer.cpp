// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/orphan_offer.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <tuple>

#include "sonora/closed_take.h"
#include "sonora/model/operations.h"
#include "sonora/wav_reader.h"

namespace sonora {

namespace {

constexpr const char* kAudioDir = "Audio Files";
// What every new session is called until its first save names it.
constexpr const char* kDefaultName = "Untitled";
// An offer is read on screen: past this many lines the rest are counted.
constexpr std::size_t kMaxOfferLines = 24;

// Where a take goes: the track at display index `track`, or the pool with
// `why`.
struct Destination {
    int track = -1;
    std::string why;
};

Destination destination(const model::Snapshot& document, const OrphanTake& o) {
    Destination d;
    if (o.said.track_id == 0) {
        d.why = "its header names no track";
        return d;
    }
    const auto& tracks = document.edit->tracks;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const model::TrackEdit& t = *tracks[i];
        if (t.id != o.said.track_id) continue;
        if (t.kind != model::TrackKind::Audio) {
            d.why = "its track \"" + t.name + "\" is not an audio track now";
            return d;
        }
        if (o.channels < t.channels) {
            d.why = "it has " + std::to_string(o.channels) + " channels and \"" + t.name +
                    "\" has " + std::to_string(t.channels);
            return d;
        }
        // Over [A, B) or not at all: never part of a punch over what is there.
        if (o.said.punched &&
            (o.time_reference > o.said.from || o.time_reference + o.frames < o.said.to)) {
            d.why = "it does not cover its punch range";
            return d;
        }
        d.track = static_cast<int>(i);
        return d;
    }
    d.why = "its track is not in this session";
    return d;
}

std::string seconds(uint64_t frames, uint32_t rate) {
    char text[32];
    std::snprintf(text, sizeof text, "%.3f s",
                  rate ? static_cast<double>(frames) / rate : 0.0);
    return text;
}

std::string file_name(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string offer_line(const model::Snapshot& document, const OrphanTake& o) {
    const uint32_t rate = document.settings.sample_rate;
    std::string line = file_name(o.path) + ": take " + std::to_string(o.said.take);
    const Destination d = destination(document, o);
    if (d.track < 0) {
        return line + ", " + seconds(o.frames, rate) + ", into the audio pool only: " + d.why;
    }
    const std::string track = "\"" + document.edit->tracks[static_cast<std::size_t>(d.track)]->name + "\"";
    const std::string range = seconds(o.said.from, rate) + " to " + seconds(o.said.to, rate);
    if (o.said.punched) return line + ", punched on " + track + " from " + range;
    if (o.said.looped) {
        const std::size_t passes = loop_passes(o.loop.wraps, o.frames, o.loop.from, o.loop.to,
                                               o.time_reference, o.loop.latency)
                                       .size();
        line += ", loop on " + track + ": " + std::to_string(passes) +
                (passes == 1 ? " pass" : " passes") + " from " + range + " on new playlists";
        if (o.loop.by_length) line += ", placed by loop length";
        return line;
    }
    return line + " on " + track + " at " + seconds(o.time_reference, rate) + ", " +
           seconds(o.frames, rate) + " long";
}

model::Frames frames_of(uint64_t f) {
    return f > static_cast<uint64_t>(model::kMaxInteger) ? model::kMaxInteger
                                                         : static_cast<model::Frames>(f);
}

}  // namespace

bool take_is_of_session(const TakeDescription& said, const model::SessionSettings& settings) {
    if (!said.session_id.empty()) return said.session_id == settings.session_id;
    return said.session == settings.name && settings.name != kDefaultName;
}

bool leave_orphan_takes(model::SessionSettings& settings, const OrphanOffer& offer) {
    std::set<std::string> left(settings.left_takes.begin(), settings.left_takes.end());
    const std::size_t before = left.size();
    for (const OrphanTake& o : offer.takes) left.insert(o.path);
    if (left.size() == before) return false;
    settings.left_takes.assign(left.begin(), left.end());
    return true;
}

OrphanOffer find_orphan_takes(const model::Snapshot& document, const std::string& session_dir) {
    OrphanOffer offer;
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(session_dir) / kAudioDir;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return offer;
    std::set<std::string> named;
    for (const model::Source& s : document.sources->sources) named.insert(s.path);
    named.insert(document.settings.left_takes.begin(), document.settings.left_takes.end());
    std::vector<std::string> names;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".wav") == 0) {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        OrphanTake o;
        o.path = std::string(kAudioDir) + "/" + name;
        if (named.count(o.path) != 0) continue;
        const std::string full = session_dir + "/" + o.path;
        WavReader file;
        std::string error;
        if (!file.open(full, error) || file.frames() <= 0) continue;
        if (file.sample_rate() != document.settings.sample_rate) continue;
        if (!parse_take_description(file.description(), o.said)) continue;
        if (!take_is_of_session(o.said, document.settings)) continue;
        o.time_reference = file.time_reference();
        o.frames = static_cast<uint64_t>(file.frames());
        o.channels = file.channels();
        o.format = file.encoding() == WavReader::Encoding::Float32 ? model::AudioFormat::Float32
                                                                  : model::AudioFormat::Int24;
        file.close();
        if (o.said.looped && !recover_loop_wraps(full, o.loop)) continue;
        offer.takes.push_back(std::move(o));
    }
    std::size_t lines = 0;
    for (const OrphanTake& o : offer.takes) {
        if (lines == kMaxOfferLines) {
            offer.text += "\nand " + std::to_string(offer.takes.size() - lines) + " more";
            break;
        }
        if (lines++ != 0) offer.text += "\n";
        offer.text += offer_line(document, o);
    }
    return offer;
}

OrphansPlaced place_orphan_takes(model::Document& document, const Session& session,
                                 const OrphanOffer& offer) {
    OrphansPlaced out;
    const std::string dir = *session.folder();
    std::set<std::string> named;
    for (const model::Source& s : document.current().sources->sources) named.insert(s.path);

    // The files of one take, as the disk thread would have closed them:
    // same number, same punch or loop.
    using Key = std::tuple<int, bool, bool, uint64_t, uint64_t>;
    std::map<Key, Session::ClosedTake> takes;
    std::vector<const OrphanTake*> pool;
    for (const OrphanTake& o : offer.takes) {
        if (named.count(o.path) != 0) continue;
        const Destination d = destination(document.current(), o);
        if (d.track < 0) {
            pool.push_back(&o);
            continue;
        }
        Session::ClosedTake& t =
            takes[Key{o.said.take, o.said.punched, o.said.looped, o.said.from, o.said.to}];
        if (t.files.empty()) {
            t.take = o.said.take;
            t.origin = o.time_reference;
            t.latency = o.said.latency;
            if (o.said.punched) {
                t.punched = true;
                t.punch.capture_from = o.time_reference;
                t.punch.capture_to = o.time_reference + o.frames;
                t.punch.monitor_in = o.said.from;
                t.punch.monitor_out = o.said.to;
                // document_punch()'s crossfade: the session's, equal power.
                t.punch.crossfade = static_cast<uint32_t>(std::clamp<model::Frames>(
                    document.current().settings.transport.punch_crossfade, 0, UINT32_MAX));
                t.punch.shape = FadeCurve::EqualPower;
            }
            if (o.said.looped) {
                t.looped = true;
                t.loop.from = o.said.from;
                t.loop.to = o.said.to;
                t.loop.start = o.time_reference;
                t.loop.latency = o.loop.latency;
            }
        }
        DiskThread::ClosedFile f;
        f.track = d.track;
        f.take = o.said.take;
        f.path = dir + "/" + o.path;
        f.frames = o.frames;
        if (o.said.looped) {
            f.wraps = o.loop.wraps;
            t.loop_wraps = std::max<uint32_t>(t.loop_wraps, static_cast<uint32_t>(f.wraps.size()));
            if (o.loop.by_length) {
                out.notices.push_back(
                    "recovered take " + std::to_string(o.said.take) + "'s passes on track " +
                    std::to_string(d.track + 1) +
                    " were placed by loop length: its wrap list was missing or incomplete" +
                    (o.loop.why.empty() ? std::string() : " (" + o.loop.why + ")") +
                    ", so a capture overrun, if there was one, would put them late");
            }
        }
        t.files.push_back(std::move(f));
    }

    for (auto& [key, take] : takes) {
        out.placed += take.files.size();
        TakeAdded added = add_closed_take(document, session, take,
                                          model::kMaxCompoundMembers / 2, kRecoveredLabel);
        for (auto& u : added.updates) out.updates.push_back(std::move(u));
        for (auto& n : added.track_notices) out.notices.push_back("recovered " + n);
        for (auto& n : added.fade_notices) out.notices.push_back("recovered " + n);
        if (!added.refused.empty()) out.notices.push_back("recovered " + added.refused);
    }

    // Unused sources: one step for all of them, in compounds the model takes.
    model::Id next = document.current().next_id;
    std::vector<model::AddSource> adds;
    for (const OrphanTake* o : pool) {
        model::AddSource a;
        a.source.id = next++;
        a.source.path = o->path;
        a.source.channels = o->channels;
        a.source.length = frames_of(o->frames);
        a.source.format = o->format;
        a.source.time_reference = static_cast<int64_t>(o->time_reference);
        a.source.kind = model::SourceKind::Recorded;
        a.source.take = o->said.take;
        a.source.latency_compensation = o->said.latency;
        adds.push_back(std::move(a));
    }
    for (std::size_t first = 0; first < adds.size(); first += model::kMaxCompoundMembers) {
        const std::size_t last = std::min(adds.size(), first + model::kMaxCompoundMembers);
        model::Compound c;
        c.label = kRecoveredLabel;
        for (std::size_t k = first; k < last; ++k) c.ops.push_back(adds[k]);
        model::ReplicationUpdate u;
        std::string why;
        if (document.apply(model::Operation{std::move(c)}, u, why)) {
            out.updates.push_back(std::move(u));
            out.pooled += last - first;
        } else {
            out.notices.push_back("recovered takes could not go into the audio pool: " + why);
        }
    }
    return out;
}

}  // namespace sonora
