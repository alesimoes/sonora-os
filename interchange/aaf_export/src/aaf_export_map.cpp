// SPDX-License-Identifier: GPL-3.0-or-later
#include "aaf_export_map.h"

#include <map>

namespace sonora::interchange {
namespace {

using model::Frames;

// 64 lower-case hex digits derived from `seed`, the same every run.
std::string generated_umid(const std::string& seed) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::uint64_t k = 0; k < 4; ++k) {
        std::uint64_t h = 1469598103934665603ULL ^ (k * 0x9E3779B97F4A7C15ULL);
        for (const char c : seed) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
        for (int s = 60; s >= 0; s -= 4) out += digits[(h >> s) & 0xF];
    }
    return out;
}

std::string file_name(const std::string& path) {
    const std::size_t k = path.find_last_of('/');
    return k == std::string::npos ? path : path.substr(k + 1);
}

struct Rate {
    std::int64_t num, den;
};

class Mapper {
public:
    Mapper(const model::Snapshot& s, AafDocument& doc, ConversionReport& rep)
        : s_(s), doc_(doc), rep_(rep), rate_{static_cast<std::int64_t>(s.settings.sample_rate), 1} {}

    bool run(std::string& error) {
        if (!s_.edit || !s_.sources || rate_.num <= 0) {
            error = "the session has no edit state or no sample rate";
            return false;
        }
        comp_.mob_id = generated_umid("composition:" + s_.settings.session_id + s_.settings.name);
        comp_.name = s_.settings.name;
        for (const auto& t : s_.edit->tracks) map_track(*t);
        map_markers();
        drop_session_level();
        map_timecode();
        for (auto& kv : source_mobs_) doc_.mobs.push_back(std::move(kv.second));
        doc_.mobs.push_back(std::move(comp_));
        return true;
    }

private:
    void map_track(const model::TrackEdit& t) {
        if (t.kind != model::TrackKind::Audio) {
            rep_.dropped.push_back("track \"" + t.name + "\": MIDI/instrument track");
            return;
        }
        const model::Playlist* pl = nullptr;
        for (const auto& p : t.playlists) {
            if (p->id == t.active_playlist) {
                pl = p.get();
            } else {
                rep_.dropped.push_back("track \"" + t.name + "\": inactive playlist \"" + p->name + "\"");
            }
        }
        AafSlot slot;
        slot.name = t.name;
        slot.edit_rate = {rate_.num, rate_.den};
        if (pl) map_playlist(t, *pl, slot);
        if (!t.inserts.empty()) rep_.dropped.push_back("track \"" + t.name + "\": plugin inserts");
        if (!t.sends.empty()) rep_.dropped.push_back("track \"" + t.name + "\": sends");
        if (!t.automation.empty()) rep_.dropped.push_back("track \"" + t.name + "\": automation");
        if (s_.mix.find(t.id)) {
            rep_.dropped.push_back("track \"" + t.name + "\": fader, pan, mute, solo");
        }
        comp_.slots.push_back(std::move(slot));
    }

    void map_playlist(const model::TrackEdit& t, const model::Playlist& pl, AafSlot& slot) {
        std::map<model::Id, const model::Crossfade*> by_left;
        for (const auto& x : pl.crossfades) by_left[x.left] = &x;
        Frames cursor = 0;
        for (std::size_t i = 0; i < pl.regions.size(); ++i) {
            const model::Region& r = pl.regions[i];
            if (r.position > cursor) slot.clips.push_back(AafFiller{r.position - cursor});
            slot.clips.push_back(region_clip(t, r));
            cursor = r.position + r.length;
            const auto it = by_left.find(r.id);
            if (it == by_left.end()) continue;
            if (i + 1 >= pl.regions.size() || pl.regions[i + 1].id != it->second->right ||
                pl.regions[i + 1].position != cursor) {
                rep_.dropped.push_back("track \"" + t.name +
                                       "\": crossfade between regions that do not touch");
                continue;
            }
            Frames before = 0, after = 0;
            model::crossfade_extent(*it->second, before, after);
            slot.clips.push_back(AafTransition{before + after, before});
        }
    }

    std::string mob_for(const model::SourceRef& ref) {
        const model::Source* src = nullptr;
        for (const auto& c : s_.sources->sources) {
            if (c.id == ref.source) src = &c;
        }
        if (!src) return {};
        std::string id = src->umid;
        if (id.empty()) {
            id = generated_umid("source:" + src->path);
            rep_.approximated.push_back("source \"" + src->path + "\" has no UMID; generated " + id);
        }
        if (!source_mobs_.count(id)) {
            AafMob m;
            m.mob_id = id;
            m.name = file_name(src->path);
            m.is_source = true;
            m.media_path = src->path;
            AafSlot sl;
            sl.edit_rate = {rate_.num, rate_.den};
            sl.clips.push_back(AafFiller{src->length});
            m.slots.push_back(std::move(sl));
            source_mobs_[id] = std::move(m);
        }
        return id;
    }

    AafComponent region_clip(const model::TrackEdit& t, const model::Region& r) {
        const std::string label = "track \"" + t.name + "\" region \"" + r.name + "\"";
        if (r.channels.empty()) {
            rep_.dropped.push_back(label + ": no source; exported as a gap");
            return AafFiller{r.length};
        }
        const std::string id = mob_for(r.channels[0]);
        if (id.empty()) {
            rep_.dropped.push_back(label + ": source missing from the pool; exported as a gap");
            return AafFiller{r.length};
        }
        if (r.mute) {
            rep_.approximated.push_back(label + ": muted, exported as a gap");
            return AafFiller{r.length};
        }
        AafSourceClip c;
        c.mob_id = id;
        c.start = r.source_offset;
        c.length = r.length;
        if (r.gain_db != 0.0f) c.gain_db = r.gain_db;
        if (r.fade_in.length > 0) c.fade_in = AafFade{r.fade_in.length};
        if (r.fade_out.length > 0) c.fade_out = AafFade{r.fade_out.length};
        if ((r.fade_in.length > 0 && r.fade_in.shape != model::FadeShape::Linear) ||
            (r.fade_out.length > 0 && r.fade_out.shape != model::FadeShape::Linear)) {
            rep_.approximated.push_back(label + ": fade shape exported as AAF's default");
        }
        if (r.channels.size() > 1) rep_.dropped.push_back(label + ": channels past the first");
        if (!r.gain_line.empty()) rep_.dropped.push_back(label + ": clip gain envelope");
        if (r.loop != 0) rep_.dropped.push_back(label + ": loop");
        return c;
    }

    void map_markers() {
        if (!s_.edit->markers || s_.edit->markers->markers.empty()) return;
        if (comp_.slots.empty()) {
            rep_.dropped.push_back("markers: no audio track to carry them");
            return;
        }
        for (const model::Marker& m : s_.edit->markers->markers) {
            comp_.slots.front().markers.push_back(AafMarker{m.position, m.name});
            if (m.length > 0) {
                rep_.approximated.push_back("marker \"" + m.name + "\": range exported as a point");
            }
        }
    }

    void drop_session_level() {
        if (!s_.edit->buses.empty()) rep_.dropped.push_back("aux buses");
        if (!s_.edit->vcas.empty()) rep_.dropped.push_back("VCAs");
        if (!s_.edit->folders.empty()) rep_.dropped.push_back("folders");
        if (!s_.edit->groups.empty()) rep_.dropped.push_back("groups");
        if (!s_.edit->master_automation.empty()) rep_.dropped.push_back("master automation");
        if (s_.settings.tempo.size() > 1) rep_.dropped.push_back("tempo map");
    }

    void map_timecode() {
        AafTimecode tc;
        Rate fps{25, 1};
        bool approx = false;
        switch (s_.settings.timecode.fps) {
            case model::Fps::F23_976: tc.fps = 24; fps = {24000, 1001}; approx = true; break;
            case model::Fps::F24: tc.fps = 24; fps = {24, 1}; break;
            case model::Fps::F25: tc.fps = 25; fps = {25, 1}; break;
            case model::Fps::F29_97: tc.fps = 30; fps = {30000, 1001}; approx = true; break;
            case model::Fps::F29_97df: tc.fps = 30; tc.drop = true; fps = {30000, 1001}; break;
            case model::Fps::F30: tc.fps = 30; fps = {30, 1}; break;
        }
        if (approx) rep_.approximated.push_back("timecode rate exported as a whole-number frame rate");
        // frames = origin * fps.num / (fps.den * rate), rounded half up; the
        // origin is a sample count far below 2^50, so the product fits.
        const std::int64_t den = fps.den * rate_.num;
        const std::int64_t num = s_.settings.timecode.origin * fps.num;
        tc.start = (2 * num + den) / (2 * den);
        if (tc.start * den != num) {
            rep_.approximated.push_back("timecode start rounded to a whole frame");
        }
        doc_.timecode = tc;
    }

    const model::Snapshot& s_;
    AafDocument& doc_;
    ConversionReport& rep_;
    Rate rate_;
    AafMob comp_;
    std::map<std::string, AafMob> source_mobs_;
};

}  // namespace

bool map_to_aaf(const model::Snapshot& s, AafDocument& out, ConversionReport& report,
                std::string& error) {
    AafDocument doc;
    Mapper m(s, doc, report);
    if (!m.run(error)) return false;
    out = std::move(doc);
    return true;
}

}  // namespace sonora::interchange
