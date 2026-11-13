// SPDX-License-Identifier: GPL-3.0-or-later
#include "aaf_map.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <unordered_map>

#include "aaf_rational.h"

namespace sonora::interchange {
namespace {

using model::Frames;

bool valid_utf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20) return false;
        std::size_t extra = 0;
        if (c >= 0x80) {
            if ((c >> 5) == 6) extra = 1;
            else if ((c >> 4) == 14) extra = 2;
            else if ((c >> 3) == 30) extra = 3;
            else return false;
        }
        if (extra != 0 && i + extra >= s.size()) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

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

// The "urn:smpte:umid:" prefix, dots, dashes and colons are formatting; what
// is left must be 64 hex digits.
std::string umid_from(const std::string& mob_id) {
    std::string lower;
    for (const char c : mob_id) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string prefix = "urn:smpte:umid:";
    if (lower.compare(0, prefix.size(), prefix) == 0) lower.erase(0, prefix.size());
    std::string out;
    for (const char c : lower) {
        if (c == '.' || c == '-' || c == ':') continue;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
        out += c;
    }
    return out.size() == 64 ? out : std::string();
}

bool path_ok(const std::string& p) {
    if (p.empty() || p.front() == '/' || p.find(':') != std::string::npos || !valid_utf8(p)) {
        return false;
    }
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = p.find('/', start);
        const std::string part =
            p.substr(start, slash == std::string::npos ? slash : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
}

std::int64_t length_of(const AafComponent& c) {
    if (const auto* s = std::get_if<AafSourceClip>(&c)) return s->length;
    if (const auto* f = std::get_if<AafFiller>(&c)) return f->length;
    return 0;
}

// What a slot's regions need to know about each other once placed.
struct SlotState {
    struct Placed {
        bool is_region = false;
        std::size_t region = 0;
    };
    std::vector<Placed> placed;       // per component
    std::vector<Frames> in_x, out_x;  // per region: crossfade frames inside it, -1 none
};

class Mapper {
public:
    Mapper(const AafDocument& d, std::uint32_t rate, ConversionReport& r, std::string& e)
        : doc_(d), rate_(rate), report_(r), error_(e) {}

    bool run(model::Snapshot& out) {
        for (const std::string& u : doc_.unmapped) report_.dropped.push_back(u);
        build_sources();
        std::vector<std::shared_ptr<const model::TrackEdit>> tracks;
        model::MixState mix;
        model::MarkerList markers;
        std::string first_name;
        for (const AafMob& mob : doc_.mobs) {
            if (mob.is_source) continue;
            if (first_name.empty()) first_name = mob.name;
            for (const AafSlot& slot : mob.slots) {
                if (!map_slot(mob, slot, tracks, mix, markers)) return false;
            }
        }
        if (tracks.empty()) {
            error_ = "AAF file holds no composition track: nothing to import";
            return false;
        }
        return assemble(out, first_name, std::move(tracks), std::move(mix), std::move(markers));
    }

private:
    const AafDocument& doc_;
    std::uint32_t rate_;
    ConversionReport& report_;
    std::string& error_;
    model::Id next_id_ = 1;
    std::vector<model::Source> sources_;
    std::unordered_map<std::string, std::size_t> by_mob_;

    model::Id take_id() { return next_id_++; }

    bool assemble(model::Snapshot& out, const std::string& first_name,
                  std::vector<std::shared_ptr<const model::TrackEdit>> tracks,
                  model::MixState mix, model::MarkerList markers) {
        model::SessionSettings settings;
        settings.name = !first_name.empty() && valid_utf8(first_name) ? first_name : "Imported AAF";
        settings.sample_rate = rate_;
        settings.session_id = model::new_session_id();
        map_timecode(settings);
        model::Snapshot snap = model::new_session(settings);
        auto edit = std::make_shared<model::EditState>();
        edit->tracks = std::move(tracks);
        std::stable_sort(markers.markers.begin(), markers.markers.end(),
                         [](const model::Marker& a, const model::Marker& b) {
                             return a.position != b.position ? a.position < b.position : a.id < b.id;
                         });
        edit->markers = std::make_shared<const model::MarkerList>(std::move(markers));
        snap.edit = std::move(edit);
        auto pool = std::make_shared<model::SourcePool>();
        pool->sources = sources_;
        snap.sources = std::move(pool);
        snap.mix = std::move(mix);
        snap.next_id = next_id_;
        std::string why;
        if (!model::check_invariants(snap, why)) {
            error_ = "the AAF maps to an invalid session: " + why;
            return false;
        }
        out = std::move(snap);
        return true;
    }

    // One rounding, one line in the report.
    Frames conv(std::int64_t units, const AafRational& from, const std::string& what, bool& ok) {
        std::int64_t out = 0;
        bool exact = true;
        if (!rescale_units(units, from, rate_, out, exact)) {
            error_ = what + ": edit rate " + std::to_string(from.num) + "/" +
                     std::to_string(from.den) + " cannot be converted";
            ok = false;
            return 0;
        }
        if (!exact) {
            report_.approximated.push_back(
                what + ": " + std::to_string(units) + " units at " + std::to_string(from.num) + "/" +
                std::to_string(from.den) + " rounded to " + std::to_string(out) + " samples at " +
                std::to_string(rate_) + " Hz");
        }
        return out;
    }

    void build_sources() {
        std::size_t n = 0;
        for (const AafMob& mob : doc_.mobs) {
            if (!mob.is_source) continue;
            ++n;
            model::Source s;
            s.id = take_id();
            s.channels = static_cast<std::uint16_t>(std::max<std::size_t>(1, mob.slots.size()));
            s.kind = model::SourceKind::Imported;
            s.umid = umid_from(mob.mob_id);
            const std::string label = "source " + (mob.name.empty() ? std::to_string(n) : mob.name);
            if (s.umid.empty()) {
                s.umid = generated_umid(mob.mob_id + "|" + mob.name + "|" + mob.media_path + "|" +
                                        std::to_string(n));
                report_.approximated.push_back(label + ": no usable UMID, one was generated");
            }
            s.path = source_path(mob, n, label);
            for (const AafSlot& slot : mob.slots) {
                std::int64_t total = 0;
                for (const AafComponent& c : slot.clips) total += length_of(c);
                std::int64_t f = 0;
                bool exact = true;
                // A source's own length is only a bound for handles; a failure
                // here leaves it at what the clips use.
                if (rescale_units(total, slot.edit_rate, rate_, f, exact)) {
                    s.length = std::max<Frames>(s.length, f);
                }
            }
            if (!mob.mob_id.empty()) by_mob_.emplace(mob.mob_id, sources_.size());
            sources_.push_back(std::move(s));
        }
    }

    std::string source_path(const AafMob& mob, std::size_t n, const std::string& label) {
        std::string p = mob.media_path;
        std::replace(p.begin(), p.end(), '\\', '/');
        if (path_ok(p)) return p;
        const std::size_t slash = p.find_last_of('/');
        const std::string base = slash == std::string::npos ? p : p.substr(slash + 1);
        const std::string fallback = !base.empty() && path_ok(base)
                                         ? "Audio Files/" + base
                                         : "Audio Files/aaf-source-" + std::to_string(n) + ".wav";
        report_.approximated.push_back(label + ": media path \"" + mob.media_path +
                                       "\" is not inside the session folder, now \"" + fallback + "\"");
        return fallback;
    }

    void map_timecode(model::SessionSettings& settings) {
        if (!doc_.timecode) return;
        const AafTimecode& t = *doc_.timecode;
        AafRational from{t.fps, 1};
        if (t.fps == 24 && !t.drop) {
            settings.timecode.fps = model::Fps::F24;
        } else if (t.fps == 25 && !t.drop) {
            settings.timecode.fps = model::Fps::F25;
        } else if (t.fps == 30 && t.drop) {
            settings.timecode.fps = model::Fps::F29_97df;
            from = {30000, 1001};
        } else if (t.fps == 30) {
            settings.timecode.fps = model::Fps::F30;
        } else {
            report_.dropped.push_back("timecode at " + std::to_string(t.fps) +
                                      (t.drop ? " fps drop-frame" : " fps") +
                                      " has no model equivalent");
            return;
        }
        bool ok = true;
        const Frames origin = conv(t.start, from, "timecode start", ok);
        if (ok && origin >= 0) settings.timecode.origin = origin;
    }

    bool map_slot(const AafMob& mob, const AafSlot& slot,
                  std::vector<std::shared_ptr<const model::TrackEdit>>& tracks,
                  model::MixState& mix, model::MarkerList& markers) {
        const std::string label = "slot " + (slot.name.empty() ? mob.name : slot.name);
        std::uint16_t channels = 1;
        for (const AafComponent& c : slot.clips) {
            const auto* clip = std::get_if<AafSourceClip>(&c);
            const auto it = clip ? by_mob_.find(clip->mob_id) : by_mob_.end();
            if (it != by_mob_.end()) channels = std::max(channels, sources_[it->second].channels);
        }
        model::Playlist pl;
        pl.id = take_id();
        pl.name = "Main";
        SlotState st;
        st.placed.resize(slot.clips.size());
        if (!place_regions(slot, label, channels, pl, st)) return false;
        st.in_x.assign(pl.regions.size(), -1);
        st.out_x.assign(pl.regions.size(), -1);
        if (!place_crossfades(slot, label, pl, st)) return false;
        if (!place_fades(slot, pl, st)) return false;

        auto track = std::make_shared<model::TrackEdit>();
        track->id = take_id();
        std::string name = !slot.name.empty() ? slot.name : mob.name;
        if (name.empty() || !valid_utf8(name)) name = "Track " + std::to_string(tracks.size() + 1);
        track->name = name;
        track->colour = model::auto_colour(tracks.size());
        track->channels = channels;
        track->active_playlist = pl.id;
        track->playlists.push_back(std::make_shared<const model::Playlist>(std::move(pl)));
        model::TrackMix tm;
        tm.track = track->id;
        for (std::uint16_t c = 0; c < channels; ++c) tm.input.push_back(c);
        mix.tracks.push_back(std::move(tm));
        tracks.push_back(std::move(track));
        return map_markers(slot, label, markers);
    }

    bool map_markers(const AafSlot& slot, const std::string& label, model::MarkerList& markers) {
        for (const AafMarker& m : slot.markers) {
            bool ok = true;
            model::Marker mk;
            mk.position = conv(m.position, slot.edit_rate, label + " marker", ok);
            if (!ok) return false;
            if (mk.position < 0) {
                report_.dropped.push_back(label + " marker \"" + m.name + "\" is before the start");
                continue;
            }
            mk.id = take_id();
            mk.name = valid_utf8(m.name) ? m.name : "Marker";
            markers.markers.push_back(std::move(mk));
        }
        return true;
    }

    bool place_regions(const AafSlot& slot, const std::string& label, std::uint16_t channels,
                       model::Playlist& pl, SlotState& st) {
        std::int64_t pos = 0;
        for (std::size_t i = 0; i < slot.clips.size(); ++i) {
            const std::int64_t len = length_of(slot.clips[i]);
            if (len < 0) {
                error_ = label + ": component " + std::to_string(i) + " has a negative length";
                return false;
            }
            const std::int64_t begin = pos;
            pos += len;
            const auto* clip = std::get_if<AafSourceClip>(&slot.clips[i]);
            if (!clip || len == 0) continue;
            const std::string what = label + " clip " + std::to_string(i);
            const auto it = by_mob_.find(clip->mob_id);
            if (it == by_mob_.end()) {
                report_.dropped.push_back(
                    what + ": " +
                    (clip->mob_id.empty() ? std::string("it names no source mob")
                                          : "source mob " + clip->mob_id + " is not in the file") +
                    "; left as a gap");
                continue;
            }
            bool ok = true;
            const Frames a = conv(begin, slot.edit_rate, what + " position", ok);
            const Frames b = conv(pos, slot.edit_rate, what + " end", ok);
            const Frames off = conv(clip->start, slot.edit_rate, what + " source start", ok);
            if (!ok) return false;
            if (b <= a || a < 0 || off < 0) {
                report_.dropped.push_back(what + ": it rounds to an empty or negative span");
                continue;
            }
            model::Source& src = sources_[it->second];
            model::Region r;
            r.id = take_id();
            r.name = src.path.substr(src.path.find_last_of('/') + 1);
            r.position = a;
            r.length = b - a;
            r.source_offset = off;
            for (std::uint16_t c = 0; c < channels; ++c) {
                r.channels.push_back({src.id, std::min<std::uint16_t>(c, src.channels - 1)});
            }
            if (src.channels != channels) {
                report_.approximated.push_back(
                    what + ": the source has " + std::to_string(src.channels) +
                    " channel(s) and the track " + std::to_string(channels) +
                    "; the last source channel is repeated");
            }
            src.length = std::max(src.length, off + r.length);
            if (clip->gain_db) apply_gain(r, *clip->gain_db, what);
            st.placed[i] = {true, pl.regions.size()};
            pl.regions.push_back(std::move(r));
        }
        return true;
    }

    void apply_gain(model::Region& r, double db, const std::string& what) {
        const double clamped =
            std::min<double>(model::kMaxGainDb, std::max<double>(model::kMinGainDb, db));
        if (clamped != db) {
            report_.approximated.push_back(what + ": gain " + std::to_string(db) +
                                           " dB clamped to " + std::to_string(clamped) + " dB");
        }
        r.gain_db = static_cast<float>(clamped);
    }

    bool place_crossfades(const AafSlot& slot, const std::string& label, model::Playlist& pl,
                          SlotState& st) {
        model::SourcePool pool;
        pool.sources = sources_;
        for (std::size_t i = 0; i < slot.clips.size(); ++i) {
            const auto* t = std::get_if<AafTransition>(&slot.clips[i]);
            if (!t) continue;
            const std::string what = label + " transition " + std::to_string(i);
            if (t->length < 0 || t->cut_point < 0 || t->cut_point > t->length) {
                error_ = what + ": its length or cut point is out of range";
                return false;
            }
            if (i == 0 || i + 1 >= slot.clips.size()) {
                report_.dropped.push_back(what + ": no clip on both sides; no crossfade");
                continue;
            }
            const std::int64_t before_u = t->cut_point, after_u = t->length - t->cut_point;
            if (before_u > length_of(slot.clips[i - 1]) || after_u > length_of(slot.clips[i + 1])) {
                error_ = what + ": overlapping clips: the transition is longer than a clip it joins";
                return false;
            }
            if (!st.placed[i - 1].is_region || !st.placed[i + 1].is_region) {
                report_.dropped.push_back(what + ": a neighbour is a gap; no crossfade");
                continue;
            }
            if (t->length == 0) continue;
            if (!add_crossfade(slot, what, pl, st, st.placed[i - 1].region,
                               st.placed[i + 1].region, before_u, after_u, pool)) {
                return false;
            }
        }
        return true;
    }

    // False only for an error; a crossfade that cannot be placed is a report line.
    bool add_crossfade(const AafSlot& slot, const std::string& what, model::Playlist& pl,
                       SlotState& st, std::size_t li, std::size_t ri, std::int64_t before_u,
                       std::int64_t after_u, const model::SourcePool& pool) {
        bool ok = true;
        const Frames before = conv(before_u, slot.edit_rate, what + " before the cut", ok);
        const Frames after = conv(after_u, slot.edit_rate, what + " after the cut", ok);
        if (!ok) return false;
        const model::Region& l = pl.regions[li];
        const model::Region& r = pl.regions[ri];
        if (l.end() != r.position) {
            report_.dropped.push_back(what + ": the clips do not butt after rounding");
            return true;
        }
        model::Crossfade x;
        x.left = l.id;
        x.right = r.id;
        x.length = before + after;
        if (x.length <= 0) return true;
        if (before == 0) {
            x.align = model::Align::Post;
        } else if (after == 0) {
            x.align = model::Align::Pre;
        } else if (before != x.length / 2) {
            report_.approximated.push_back(what + ": the cut is not central; it is centred");
        }
        Frames hb = 0, ha = 0;
        model::crossfade_handles(l, r, pool, hb, ha);
        const Frames want = x.length;
        if (!model::fit_crossfade(x, hb, ha)) {
            report_.dropped.push_back(what + ": the clips have no spare source for a crossfade");
            return true;
        }
        Frames xb = 0, xa = 0;
        model::crossfade_extent(x, xb, xa);
        if (xb > l.length || xa > r.length ||
            (st.in_x[li] >= 0 && st.in_x[li] + xb > l.length)) {
            report_.dropped.push_back(what + ": its ends would meet inside a clip; no crossfade");
            return true;
        }
        if (x.length != want) {
            report_.approximated.push_back(what + ": crossfade shortened from " +
                                           std::to_string(want) + " to " + std::to_string(x.length) +
                                           " frames to fit the source handles");
        }
        st.out_x[li] = xb;
        st.in_x[ri] = xa;
        pl.crossfades.push_back(x);
        return true;
    }

    bool place_fades(const AafSlot& slot, model::Playlist& pl, SlotState& st) {
        for (std::size_t i = 0; i < slot.clips.size(); ++i) {
            const auto* clip = std::get_if<AafSourceClip>(&slot.clips[i]);
            if (!clip || !st.placed[i].is_region) continue;
            const std::size_t idx = st.placed[i].region;
            model::Region& r = pl.regions[idx];
            const std::string what = "clip fade at " + std::to_string(r.position);
            bool ok = true;
            const bool in_x = st.in_x[idx] >= 0, out_x = st.out_x[idx] >= 0;
            if (clip->fade_in) {
                if (in_x) {
                    report_.approximated.push_back(what + ": fade-in replaced by the crossfade");
                } else {
                    r.fade_in.length = conv(clip->fade_in->length, slot.edit_rate, what + " in", ok);
                }
            }
            if (clip->fade_out) {
                if (out_x) {
                    report_.approximated.push_back(what + ": fade-out replaced by the crossfade");
                } else {
                    r.fade_out.length =
                        conv(clip->fade_out->length, slot.edit_rate, what + " out", ok);
                }
            }
            if (!ok) return false;
            clamp_fades(r, std::max<Frames>(0, st.in_x[idx]), std::max<Frames>(0, st.out_x[idx]),
                        what);
        }
        return true;
    }

    // A fade is what is heard at an end unless a crossfade is there; the two
    // ends must not meet inside the region (model heard_problem).
    void clamp_fades(model::Region& r, Frames in_x, Frames out_x, const std::string& what) {
        const Frames in0 = r.fade_in.length, out0 = r.fade_out.length;
        r.fade_in.length = std::max<Frames>(0, std::min(in0, r.length - out_x));
        r.fade_out.length = std::max<Frames>(0, std::min(out0, r.length - std::max(in_x, r.fade_in.length)));
        if (in0 != r.fade_in.length || out0 != r.fade_out.length) {
            report_.approximated.push_back(what + ": fades shortened to fit the clip");
        }
    }
};

}  // namespace

bool map_aaf(const AafDocument& doc, std::uint32_t sample_rate, model::Snapshot& out,
             ConversionReport& report, std::string& error) {
    if (sample_rate == 0) {
        error = "the session sample rate is 0";
        return false;
    }
    ConversionReport local;
    Mapper m(doc, sample_rate, local, error);
    if (!m.run(out)) return false;
    report.dropped.insert(report.dropped.end(), local.dropped.begin(), local.dropped.end());
    report.approximated.insert(report.approximated.end(), local.approximated.begin(),
                               local.approximated.end());
    return true;
}

}  // namespace sonora::interchange
