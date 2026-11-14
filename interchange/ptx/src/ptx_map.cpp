// SPDX-License-Identifier: GPL-3.0-or-later
#include "ptx_map.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "sonora/model/history.h"
#include "sonora/model/musical_map.h"
#include "sonora/model/operations.h"

namespace sonora::interchange {
namespace {

using model::Frames;

bool plain_name(const std::string& s) {
    if (s.empty() || s.size() > 80) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= 0x20 && c < 0x7F; });
}

std::string safe_name(const std::string& s, const std::string& fallback, ConversionReport& rep) {
    if (plain_name(s)) return s;
    rep.approximated.push_back("name \"" + s + "\" replaced by \"" + fallback + "\"");
    return fallback;
}

// The session wants "Audio Files/<file>"; the stored folders are not ours.
std::string session_path(const std::string& stored, const std::string& fallback) {
    const std::size_t cut = stored.find_last_of("/\\");
    std::string base = cut == std::string::npos ? stored : stored.substr(cut + 1);
    if (base == "." || base == ".." || base.find(':') != std::string::npos ||
        !plain_name(base)) {
        base = fallback;
    }
    return "Audio Files/" + base;
}

class Scaler {
public:
    Scaler(std::uint32_t from, std::uint32_t to) : from_(from), to_(to) {}
    Frames operator()(std::uint64_t v, bool& rounded) const {
        if (from_ == to_) return static_cast<Frames>(v);
        const long double exact = static_cast<long double>(v) * to_ / from_;
        const Frames r = static_cast<Frames>(std::llround(exact));
        if (static_cast<long double>(r) != exact) rounded = true;
        return r;
    }

private:
    std::uint32_t from_, to_;
};

float gain_to_db(float linear, bool& approx) {
    if (!(linear > 0.0f) || !std::isfinite(linear)) {
        approx = true;
        return model::kMinGainDb;
    }
    const float db = 20.0f * std::log10(linear);
    if (db < model::kMinGainDb || db > model::kMaxGainDb) {
        approx = true;
        return std::clamp(db, model::kMinGainDb, model::kMaxGainDb);
    }
    return db;
}

struct Mapper {
    const PtxDocument& doc;
    Scaler scale;
    ConversionReport& rep;
    std::vector<model::Id> source_ids;
    std::vector<Frames> source_lengths;
    model::History history;
    bool rounded = false, clamped_gain = false, trimmed = false;
    std::size_t empty_regions = 0;

    Mapper(const PtxDocument& d, std::uint32_t rate, ConversionReport& r, model::Snapshot start)
        : doc(d), scale(d.sample_rate, rate), rep(r), history(std::move(start)) {}

    bool add_sources(std::string& error) {
        for (std::size_t i = 0; i < doc.sources.size(); ++i) {
            const PtxAudioSource& s = doc.sources[i];
            model::AddSource op;
            op.source.id = history.current().next_id;
            op.source.path = session_path(s.path, "source " + std::to_string(i + 1) + ".wav");
            op.source.length = scale(s.length_samples, rounded);
            op.source.kind = model::SourceKind::Imported;
            std::string why;
            if (!history.apply(op, why)) {
                error = "source \"" + s.name + "\": " + why;
                return false;
            }
            source_ids.push_back(op.source.id);
            source_lengths.push_back(op.source.length);
        }
        return true;
    }

    bool region_of(const PtxTrack& t, const PtxRegion& r, model::Region& out, bool& keep,
                   std::string& error) {
        keep = false;
        if (r.source_index >= doc.sources.size()) {
            error = "track \"" + t.name + "\": a region names source " +
                    std::to_string(r.source_index) + " and the file has " +
                    std::to_string(doc.sources.size());
            return false;
        }
        if (r.length_samples == 0) {
            ++empty_regions;
            return true;
        }
        out.position = scale(r.start_samples, rounded);
        out.source_offset = scale(r.offset_samples, rounded);
        out.length = std::max<Frames>(1, scale(r.length_samples, rounded));
        const Frames room = source_lengths[r.source_index] - out.source_offset;
        if (room <= 0) {
            error = "track \"" + t.name + "\": a region starts past the end of its source";
            return false;
        }
        if (out.length > room) {
            out.length = room;
            trimmed = true;
        }
        out.channels.push_back({source_ids[r.source_index], 0});
        out.gain_db = gain_to_db(r.gain, clamped_gain);
        out.mute = r.muted;
        keep = true;
        return true;
    }

    bool regions_of(const PtxTrack& t, std::vector<model::Region>& regions, std::string& error) {
        for (const PtxRegion& r : t.regions) {
            model::Region out;
            bool keep = false;
            if (!region_of(t, r, out, keep, error)) return false;
            if (keep) regions.push_back(std::move(out));
        }
        std::stable_sort(regions.begin(), regions.end(),
                         [](const model::Region& a, const model::Region& b) {
                             return a.position < b.position;
                         });
        for (std::size_t i = 1; i < regions.size(); ++i) {
            if (regions[i].position < regions[i - 1].end()) {
                error = "track \"" + t.name + "\": regions overlap at frame " +
                        std::to_string(regions[i].position);
                return false;
            }
        }
        return true;
    }

    bool add_track(const PtxTrack& t, std::size_t index, std::string& error) {
        const std::string name = safe_name(t.name, "Track " + std::to_string(index + 1), rep);
        std::vector<model::Region> regions;
        if (!regions_of(t, regions, error)) return false;
        model::AddTrack add;
        model::Id next = history.current().next_id;
        add.track = next++;
        add.playlist = next++;
        add.name = name;
        add.playlist_name = name + ".01";
        add.channels = 1;
        add.mix.input = {0};
        std::string why;
        if (!history.apply(add, why)) {
            error = "track \"" + name + "\": " + why;
            return false;
        }
        for (model::Region& r : regions) {
            model::PlaceRegion place;
            place.playlist = add.playlist;
            r.id = history.current().next_id;
            r.name = name;
            place.region = std::move(r);
            if (!history.apply(place, why)) {
                error = "track \"" + name + "\": " + why;
                return false;
            }
        }
        return true;
    }

    void report_totals() {
        if (empty_regions > 0) {
            rep.dropped.push_back(std::to_string(empty_regions) + " region(s) of zero length");
        }
        if (rounded) rep.approximated.push_back("positions rounded to whole frames at the new rate");
        if (clamped_gain) {
            rep.approximated.push_back("region gain outside -144..+24 dB, or zero, was limited");
        }
        if (trimmed) rep.approximated.push_back("region(s) trimmed to the end of their source");
    }
};

}  // namespace

bool map_ptx_document(const PtxDocument& doc, std::uint32_t session_rate, model::Snapshot& out,
                      ConversionReport& report, std::string& error) {
    if (doc.sample_rate == 0 || doc.sample_rate > model::kMaxSampleRate) {
        error = "ptx: sample rate " + std::to_string(doc.sample_rate) + " Hz is not usable";
        return false;
    }
    const std::uint32_t rate = session_rate == 0 ? doc.sample_rate : session_rate;
    if (rate > model::kMaxSampleRate) {
        error = "ptx: session rate " + std::to_string(rate) + " Hz is not usable";
        return false;
    }
    if (rate != doc.sample_rate) {
        report.approximated.push_back(
            "file is " + std::to_string(doc.sample_rate) + " Hz, session is " +
            std::to_string(rate) + " Hz: positions rescaled, audio files not resampled");
    }
    model::SessionSettings settings;
    settings.name = "Imported";
    settings.sample_rate = rate;
    settings.session_id = model::new_session_id();
    Mapper m(doc, rate, report, model::new_session(settings));
    if (!m.add_sources(error)) return false;
    for (std::size_t i = 0; i < doc.tracks.size(); ++i) {
        if (!m.add_track(doc.tracks[i], i, error)) return false;
    }
    for (const auto& u : doc.unsupported) report.dropped.push_back(u);
    m.report_totals();
    out = m.history.current();
    std::string why;
    if (!model::check_invariants(out, why)) {
        error = "the converted session breaks the format: " + why;
        return false;
    }
    return true;
}

}  // namespace sonora::interchange
