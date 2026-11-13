// SPDX-License-Identifier: GPL-3.0-or-later
#include "omf_map.h"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace sonora::interchange {
namespace {

using model::Frames;

bool path_ok(const std::string& p) {
    if (p.empty() || p.front() == '/' || p.find(':') != std::string::npos) return false;
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = p.find('/', start);
        const std::string part =
            p.substr(start, slash == std::string::npos ? slash : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (const char c : part) {
            if (static_cast<unsigned char>(c) < 0x20) return false;
        }
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
}

class Mapper {
public:
    Mapper(const OmfDocument& d, std::uint32_t rate, ConversionReport& r, std::string& e)
        : doc_(d), rate_(rate), report_(r), error_(e) {}

    bool run(model::Snapshot& out) {
        for (const std::string& u : doc_.unsupported) {
            report_.dropped.push_back("not modelled: " + u);
        }
        if (doc_.sample_rate <= 0) {
            error_ = "the OMF file has no sample rate";
            return false;
        }
        if (doc_.tracks.empty()) {
            error_ = "OMF file holds no track: nothing to import";
            return false;
        }
        build_sources();
        std::vector<std::shared_ptr<const model::TrackEdit>> tracks;
        for (std::size_t t = 0; t < doc_.tracks.size(); ++t) {
            if (!map_track(doc_.tracks[t], t, tracks)) return false;
        }
        return assemble(out, std::move(tracks));
    }

private:
    const OmfDocument& doc_;
    std::uint32_t rate_;
    ConversionReport& report_;
    std::string& error_;
    model::Id next_id_ = 1;
    std::vector<model::Source> sources_;
    model::MixState mix_;

    model::Id take_id() { return next_id_++; }

    bool assemble(model::Snapshot& out,
                  std::vector<std::shared_ptr<const model::TrackEdit>> tracks) {
        model::SessionSettings settings;
        settings.name = "Imported OMF";
        settings.sample_rate = rate_;
        settings.session_id = model::new_session_id();
        model::Snapshot snap = model::new_session(settings);
        auto edit = std::make_shared<model::EditState>();
        edit->tracks = std::move(tracks);
        edit->markers = std::make_shared<const model::MarkerList>();
        snap.edit = std::move(edit);
        auto pool = std::make_shared<model::SourcePool>();
        pool->sources = sources_;
        snap.sources = std::move(pool);
        snap.mix = std::move(mix_);
        snap.next_id = next_id_;
        std::string why;
        if (!model::check_invariants(snap, why)) {
            error_ = "the OMF maps to an invalid session: " + why;
            return false;
        }
        out = std::move(snap);
        return true;
    }

    // Nearest sample at the session rate, no report; false if not representable.
    bool convert(std::int64_t v, Frames& out, bool& exact) const {
        const std::int64_t from = doc_.sample_rate, to = rate_;
        exact = true;
        if (v < 0) return false;
        if (from == to) {
            out = v;
            return true;
        }
        const std::int64_t q = v / from, r = v % from;
        if (q > INT64_MAX / to / 2) return false;
        const std::int64_t rem = r * to;
        out = q * to + (rem + from / 2) / from;
        exact = rem % from == 0;
        return true;
    }

    bool rescale(std::int64_t v, const std::string& what, Frames& out) {
        bool exact = true;
        if (!convert(v, out, exact)) {
            error_ = what + " is negative or too large to convert";
            return false;
        }
        if (!exact) {
            report_.approximated.push_back(what + ": " + std::to_string(v) + " samples at " +
                                           std::to_string(doc_.sample_rate) + " Hz rounded to " +
                                           std::to_string(out) + " at " + std::to_string(rate_) +
                                           " Hz");
        }
        return true;
    }

    void build_sources() {
        if (doc_.sample_rate != static_cast<std::int64_t>(rate_)) {
            report_.approximated.push_back("sample rate " + std::to_string(doc_.sample_rate) +
                                           " Hz in the file, session at " + std::to_string(rate_) +
                                           " Hz: every position is converted");
        }
        std::size_t n = 0;
        for (const OmfMediaSource& m : doc_.sources) {
            ++n;
            model::Source s;
            s.id = take_id();
            s.kind = model::SourceKind::Imported;
            s.path = source_path(m, n);
            Frames len = 0;
            bool exact = true;
            s.length = convert(m.length, len, exact) ? len : 0;
            sources_.push_back(std::move(s));
        }
    }

    std::string source_path(const OmfMediaSource& m, std::size_t n) {
        std::string p = m.path;
        std::replace(p.begin(), p.end(), '\\', '/');
        if (!m.embedded && path_ok(p)) return p;
        const std::size_t slash = p.find_last_of('/');
        const std::string base = slash == std::string::npos ? p : p.substr(slash + 1);
        const std::string fixed =
            "Audio Files/" +
            (!base.empty() && path_ok(base) ? base : "omf-source-" + std::to_string(n) + ".wav");
        const std::string label = "source " + (m.name.empty() ? std::to_string(n) : m.name);
        report_.approximated.push_back(
            label + (m.embedded ? ": embedded media must be extracted to \""
                                : ": path is not inside the session folder, now \"") +
            fixed + "\"");
        return fixed;
    }

    struct Placed {
        const OmfClip* clip = nullptr;
        Frames start = 0, length = 0, offset = 0;
    };

    bool convert_clip(const OmfClip& c, const std::string& what, Placed& p) {
        p.clip = &c;
        return rescale(c.start, what + " position", p.start) &&
               rescale(c.length, what + " length", p.length) &&
               rescale(c.source_offset, what + " source offset", p.offset);
    }

    bool collect_clips(const OmfTrack& ot, const std::string& label, std::vector<Placed>& placed) {
        for (std::size_t i = 0; i < ot.clips.size(); ++i) {
            const OmfClip& c = ot.clips[i];
            const std::string what = label + " clip " + std::to_string(i);
            if (c.source < 0 || static_cast<std::size_t>(c.source) >= sources_.size()) {
                report_.dropped.push_back(
                    what + ": it names a source the file does not hold; left as a gap");
                continue;
            }
            Placed p;
            if (!convert_clip(c, what, p)) return false;
            if (p.length <= 0) {
                report_.dropped.push_back(what + ": it is empty");
                continue;
            }
            placed.push_back(p);
        }
        std::stable_sort(placed.begin(), placed.end(),
                         [](const Placed& a, const Placed& b) { return a.start < b.start; });
        for (std::size_t i = 1; i < placed.size(); ++i) {
            if (placed[i].start < placed[i - 1].start + placed[i - 1].length) {
                error_ = label + ": overlapping clips at sample " + std::to_string(placed[i].start);
                return false;
            }
        }
        return true;
    }

    bool map_track(const OmfTrack& ot, std::size_t index,
                   std::vector<std::shared_ptr<const model::TrackEdit>>& tracks) {
        const std::string label =
            "track " + (ot.name.empty() ? std::to_string(index + 1) : ot.name);
        std::vector<Placed> placed;
        if (!collect_clips(ot, label, placed)) return false;
        model::Playlist pl;
        pl.id = take_id();
        pl.name = "Main";
        for (const Placed& p : placed) pl.regions.push_back(make_region(p, label));

        auto track = std::make_shared<model::TrackEdit>();
        track->id = take_id();
        track->name = ot.name.empty() ? "Track " + std::to_string(index + 1) : ot.name;
        track->colour = model::auto_colour(tracks.size());
        track->channels = 1;
        track->active_playlist = pl.id;
        track->playlists.push_back(std::make_shared<const model::Playlist>(std::move(pl)));
        model::TrackMix tm;
        tm.track = track->id;
        tm.input.push_back(0);
        mix_.tracks.push_back(std::move(tm));
        tracks.push_back(std::move(track));
        return true;
    }

    model::Region make_region(const Placed& p, const std::string& label) {
        model::Source& src = sources_[static_cast<std::size_t>(p.clip->source)];
        model::Region r;
        r.id = take_id();
        r.name = src.path.substr(src.path.find_last_of('/') + 1);
        r.position = p.start;
        r.length = p.length;
        r.source_offset = p.offset;
        r.channels.push_back({src.id, 0});
        r.mute = p.clip->mute;
        src.length = std::max(src.length, p.offset + p.length);
        apply_gain(r, p.clip->gain_db, label);
        fit_fades(r, *p.clip, label);
        return r;
    }

    void apply_gain(model::Region& r, double db, const std::string& label) {
        if (!(db == db)) {
            report_.approximated.push_back(label + ": gain is not a number, now 0 dB");
            return;
        }
        const double clamped =
            std::min<double>(model::kMaxGainDb, std::max<double>(model::kMinGainDb, db));
        if (clamped != db) {
            report_.approximated.push_back(label + ": gain " + std::to_string(db) +
                                           " dB clamped to " + std::to_string(clamped) + " dB");
        }
        r.gain_db = static_cast<float>(clamped);
    }

    void fit_fades(model::Region& r, const OmfClip& c, const std::string& label) {
        Frames fi = 0, fo = 0;
        bool e1 = true, e2 = true;
        if (!convert(c.fade_in, fi, e1) || !convert(c.fade_out, fo, e2)) {
            report_.dropped.push_back(label + ": a fade is negative or too large; removed");
            return;
        }
        const Frames in0 = fi, out0 = fo;
        fi = std::min(fi, r.length);
        fo = std::min(fo, r.length - fi);
        if (fi != in0 || fo != out0 || !e1 || !e2) {
            report_.approximated.push_back(label + ": fades rounded or shortened to fit the clip");
        }
        r.fade_in.length = fi;
        r.fade_out.length = fo;
    }
};

}  // namespace

bool map_omf(const OmfDocument& doc, std::uint32_t sample_rate, model::Snapshot& out,
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
