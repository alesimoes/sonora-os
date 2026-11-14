// SPDX-License-Identifier: GPL-3.0-or-later
//
// The builder of Import Session Data's Compound (ADR-0123 decision 2): what
// each source track becomes in the destination, as ordinary operations.
#include <algorithm>
#include <cctype>

#include "session_import_detail.h"
#include "sonora/session_files/session_folder.h"

namespace sonora::session_files::import_detail {

namespace {

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

const char* lane_name(model::AutomationParam p) {
    switch (p) {
        case model::AutomationParam::Volume: return "volume";
        case model::AutomationParam::Pan: return "pan";
        case model::AutomationParam::Plugin: return "plug-in";
        case model::AutomationParam::VolumeTrim: return "volume trim";
        case model::AutomationParam::Mute: return "mute";
        case model::AutomationParam::SendLevel: return "send level";
        case model::AutomationParam::SendPan: return "send pan";
    }
    return "an";
}

std::string bus_name(const model::EditState& e, model::Id bus) {
    for (const model::Bus& b : e.buses) {
        if (b.id == bus) return b.name;
    }
    return "bus " + std::to_string(bus);
}

bool holds(const std::vector<model::Id>& members, model::Id id) {
    return std::find(members.begin(), members.end(), id) != members.end();
}

}  // namespace

std::string with_suffix(const std::string& name, int n) {
    const std::size_t dot = name.rfind('.');
    const std::string number = " " + std::to_string(n);
    if (dot == std::string::npos || dot == 0) return name + number;
    return name.substr(0, dot) + number + name.substr(dot);
}

std::string fold(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string basename_of(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

const model::Playlist* active_playlist(const model::TrackEdit& t) {
    for (const auto& p : t.playlists) {
        if (p->id == t.active_playlist) return p.get();
    }
    return nullptr;
}

Builder::Builder(const model::Snapshot& dest, const model::Snapshot& src,
                 const SessionImportRequest& request)
    : dest_(dest), src_(src), request_(request), next_(dest.next_id) {
    for (const model::Source& s : dest.sources->sources) paths_.insert(fold(s.path));
    for (const model::MidiSource& s : dest.sources->midi_sources) paths_.insert(fold(s.path));
    // A take the operator chose to leave is still a file in Audio Files/.
    for (const std::string& t : dest.settings.left_takes) {
        paths_.insert(fold(t));
        paths_.insert(fold(std::string(kAudioDir) + "/" + basename_of(t)));
    }
    for (const auto& t : dest.edit->tracks) names_.insert(t->name);
}

std::string Builder::unique_track_name(const std::string& name) {
    std::string out = name;
    for (int n = 2; names_.count(out) != 0; ++n) out = with_suffix(name, n);
    names_.insert(out);
    return out;
}

std::string Builder::unique_audio_path(const std::string& source_path) {
    const std::string base = std::string(kAudioDir) + "/" + basename_of(source_path);
    std::string out = base;
    for (int n = 2; paths_.count(fold(out)) != 0; ++n) out = with_suffix(base, n);
    paths_.insert(fold(out));
    return out;
}

bool Builder::shifted(model::Frames at, model::Frames& out) {
    if (at > model::kMaxInteger - request_.offset) {
        invalid = "the offset moves frame " + std::to_string(at) + " past the end of the timeline";
        return false;
    }
    out = at + request_.offset;
    return true;
}

void Builder::add_sources(const std::vector<const model::TrackEdit*>& tracks,
                          std::vector<std::size_t>& introduced) {
    introduced.assign(tracks.size(), 0);
    if (!request_.material) return;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const model::TrackEdit& t = *tracks[i];
        const model::Playlist* p = t.kind == model::TrackKind::Audio ? active_playlist(t) : nullptr;
        if (p == nullptr) continue;
        for (const model::Region& r : p->regions) {
            for (const model::SourceRef& c : r.channels) {
                if (sources_.count(c.source) != 0) continue;
                const model::Source* s = src_.sources->find(c.source);
                if (s == nullptr) continue;   // check_invariants refused this already
                const model::Source* same = nullptr;
                for (const model::Source& d : dest_.sources->sources) {
                    if (!s->umid.empty() && d.umid == s->umid && d.channels == s->channels &&
                        d.length >= s->length) {
                        same = &d;
                        break;
                    }
                }
                if (same != nullptr) {
                    sources_[c.source] = same->id;
                    continue;
                }
                model::Source copy = *s;
                copy.id = take_id();
                copy.path = unique_audio_path(s->path);
                // The take belonged to the other session's numbering and its
                // track; here it is audio brought in.
                copy.kind = model::SourceKind::Imported;
                copy.take = 0;
                copy.recorded_on_track = 0;
                copy.latency_compensation = 0;
                if (basename_of(copy.path) != basename_of(s->path)) {
                    approximated.push_back("file " + quoted(basename_of(s->path)) + " is " +
                                           quoted(basename_of(copy.path)) + " here");
                }
                FileCopy f;
                f.source_path = request_.source_folder + "/" + s->path;
                f.dest_path = copy.path;
                f.source = copy.id;
                copies.push_back(std::move(f));
                sources_[c.source] = copy.id;
                ops.push_back(model::AddSource{std::move(copy)});
                ++introduced[i];
            }
        }
    }
}

model::TrackMix Builder::track_mix(const model::TrackEdit& t, model::Id id) const {
    model::TrackMix mix;
    if (const model::TrackMix* m = src_.mix.find(t.id)) mix = *m;
    if (mix.input.size() != t.channels) {
        mix.input.clear();
        for (std::uint16_t c = 0; c < t.channels; ++c) mix.input.push_back(c);
    }
    mix.track = id;
    // A soloed import would silence the rest of the session the moment it
    // lands.
    mix.solo = false;
    if (!request_.mix) {
        mix.gain_db = 0.0f;
        mix.pan = 0.0f;
        mix.mute = false;
    }
    return mix;
}

std::size_t Builder::add_track(const model::TrackEdit& t) {
    if (t.kind != model::TrackKind::Audio) {
        dropped.push_back("track " + quoted(t.name) + ": " +
                          (t.kind == model::TrackKind::Midi ? "MIDI" : "instrument") +
                          " tracks are not imported in this version");
        return 0;
    }
    const std::size_t before = ops.size();
    const std::string name = unique_track_name(t.name);
    if (name != t.name) {
        approximated.push_back("track " + quoted(t.name) + " is named " + quoted(name) +
                               " here: the session has a track of that name");
    }
    model::AddTrack add;
    add.track = take_id();
    add.playlist = take_id();
    add.name = name;
    add.channels = t.channels;
    const model::Playlist* p = active_playlist(t);
    add.playlist_name = p != nullptr ? p->name : name;
    if (name != t.name && add.playlist_name.compare(0, t.name.size(), t.name) == 0) {
        add.playlist_name = name + add.playlist_name.substr(t.name.size());
    }
    add.mix = track_mix(t, add.track);
    const model::Id track = add.track;
    const model::Id playlist = add.playlist;
    ops.push_back(std::move(add));
    // AddTrack colours by position; the source's own colour is kept, as
    // the operator chose it.
    if (t.colour != model::auto_colour(dest_.edit->tracks.size() + tracks_added_)) {
        ops.push_back(model::SetTrackColour{track, t.colour});
    }
    ++tracks_added_;
    if (request_.material) add_material(t, playlist, name);
    if (request_.inserts) add_inserts(t, track, name);
    if (request_.automation) add_automation(t, track, name);
    report_structure(t, name);
    return ops.size() - before;
}

void Builder::add_material(const model::TrackEdit& t, model::Id playlist,
                           const std::string& name) {
    const model::Playlist* p = active_playlist(t);
    if (t.playlists.size() > 1) {
        dropped.push_back("track " + quoted(name) + ": " + std::to_string(t.playlists.size() - 1) +
                          " playlist(s) other than the active one");
    }
    if (p == nullptr) return;
    if (p->freeze) {
        approximated.push_back("track " + quoted(name) +
                               ": a frozen playlist arrives as plain audio");
    }
    // Ids first, in region order: a clip group is named by one of its
    // members, which may come later on the playlist.
    std::map<model::Id, model::Id> regions;
    for (const model::Region& r : p->regions) regions[r.id] = take_id();
    for (const model::Region& r : p->regions) {
        model::PlaceRegion place;
        place.playlist = playlist;
        place.region = r;
        place.region.id = regions[r.id];
        model::Frames end = 0;
        if (!shifted(r.position, place.region.position) || !shifted(r.end(), end)) return;
        for (model::SourceRef& c : place.region.channels) c.source = sources_[c.source];
        const auto group = regions.find(r.clip_group);
        place.region.clip_group = group != regions.end() ? group->second : 0;
        if (place.region.clip_group == 0) place.region.loop = 0;
        ops.push_back(std::move(place));
    }
    for (const model::Crossfade& x : p->crossfades) {
        model::Crossfade c = x;
        c.left = regions[x.left];
        c.right = regions[x.right];
        ops.push_back(model::SetCrossfade{playlist, c});
    }
}

void Builder::add_inserts(const model::TrackEdit& t, model::Id track, const std::string& name) {
    for (const model::Insert& in : t.inserts) {
        model::AddInsert add;
        add.track = track;
        add.insert = take_id();
        add.slot = in.slot;
        add.plugin = in.plugin;
        const model::InsertMix* values = src_.mix.find_insert(in.id);
        if (values != nullptr) add.params = values->params;
        if (values != nullptr && values->state) {
            // A state names files in the other session's Plugin State/.
            approximated.push_back("track " + quoted(name) + ": insert " + quoted(in.plugin.name) +
                                   " arrives without its plug-in state");
        }
        const model::Id id = add.insert;
        ops.push_back(std::move(add));
        if (!in.active) ops.push_back(model::SetInsertActive{id, false});
        if (values != nullptr && values->bypass) ops.push_back(model::SetInsertBypass{id, true});
    }
}

void Builder::add_automation(const model::TrackEdit& t, model::Id track,
                             const std::string& name) {
    for (const auto& lane : t.automation) {
        if (lane->param != model::AutomationParam::Volume &&
            lane->param != model::AutomationParam::Pan) {
            dropped.push_back("track " + quoted(name) + ": the " + lane_name(lane->param) +
                              " automation lane");
            continue;
        }
        model::EditAutomation edit;
        edit.track = track;
        edit.param = lane->param;
        edit.from = 0;
        edit.to = model::kMaxInteger;
        edit.points = lane->points;
        for (model::AutomationPoint& point : edit.points) {
            if (!shifted(point.at, point.at)) return;
            if (point.at >= model::kMaxInteger) {
                invalid = "the offset moves an automation point past the end of the timeline";
                return;
            }
        }
        ops.push_back(std::move(edit));
    }
}

void Builder::report_structure(const model::TrackEdit& t, const std::string& name) {
    const model::EditState& e = *src_.edit;
    for (const model::Send& s : t.sends) {
        dropped.push_back("track " + quoted(name) + ": the send to " + quoted(bus_name(e, s.bus)));
    }
    if (t.output != model::kMasterOutput) {
        approximated.push_back("track " + quoted(name) + ": its output " +
                               quoted(bus_name(e, t.output)) + " is not imported; it plays to"
                               " the Master");
    }
    for (const model::Group& g : e.groups) {
        if (holds(g.members, t.id)) {
            dropped.push_back("track " + quoted(name) + ": group " + quoted(g.name));
        }
    }
    for (const model::Vca& v : e.vcas) {
        if (holds(v.members, t.id)) {
            dropped.push_back("track " + quoted(name) + ": VCA " + quoted(v.name));
        }
    }
    for (const model::Folder& f : e.folders) {
        if (holds(f.members, t.id)) {
            dropped.push_back("track " + quoted(name) + ": folder " + quoted(f.name));
        }
    }
}

std::size_t Builder::add_markers() {
    if (!request_.markers || !src_.edit->markers) return 0;
    std::size_t n = 0;
    for (const model::Marker& m : src_.edit->markers->markers) {
        model::Marker copy = m;
        copy.id = take_id();
        model::Frames end = 0;
        if (!shifted(m.position, copy.position) || !shifted(m.position + m.length, end)) return n;
        ops.push_back(model::AddMarker{std::move(copy)});
        ++n;
    }
    return n;
}

}  // namespace sonora::session_files::import_detail
