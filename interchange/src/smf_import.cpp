// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "sonora/interchange/interchange.h"
#include "sonora/midi/notes.h"
#include "sonora/midi/smf.h"
#include "sonora/midi/tempo_map.h"
#include "sonora/model/history.h"
#include "sonora/model/musical_map.h"
#include "sonora/model/operations.h"

namespace sonora::interchange {

namespace {

namespace fs = std::filesystem;
using model::Ticks;

constexpr std::uint32_t kImportRate = 48000;

// Converting a file's ticks to ours rounds once per tick, so a file at 480
// ppq (a divisor of 960000) is exact and one at 1000 ppq is not.
class TickScale {
public:
    explicit TickScale(int ppq) : ppq_(ppq) {}
    bool exact() const { return model::kTicksPerQuarter % ppq_ == 0; }
    // False when the result would pass the timeline's end.
    bool operator()(midi::Tick tick, Ticks& out) const {
        const std::int64_t q = tick / ppq_, r = tick % ppq_;
        if (q > model::kMaxInteger / model::kTicksPerQuarter - 1) return false;
        out = q * model::kTicksPerQuarter + (r * model::kTicksPerQuarter + ppq_ / 2) / ppq_;
        return true;
    }

private:
    std::int64_t ppq_;
};

bool plain_text(const std::string& s) {
    if (s.empty() || s.size() > 80) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= 0x20 && c < 0x7F; });
}

struct Counts {
    std::map<std::string, std::size_t> dropped;
    void add(const std::string& what) { ++dropped[what]; }
};

std::string meta_name(std::uint8_t type) {
    switch (type) {
        case midi::meta::text: return "text events";
        case midi::meta::copyright: return "copyright notices";
        case midi::meta::instrument_name: return "instrument names";
        case midi::meta::lyric: return "lyrics";
        case midi::meta::marker: return "markers";
        case midi::meta::cue_point: return "cue points";
        case midi::meta::key_signature: return "key signatures";
        case midi::meta::smpte_offset: return "SMPTE offsets";
        case midi::meta::sequencer_specific: return "sequencer-specific events";
        case midi::meta::sequence_number: return "sequence numbers";
        case midi::meta::channel_prefix: return "channel prefixes";
        default: return "other meta events";
    }
}

bool tempo_entries(const midi::Smf& smf, const TickScale&, model::SessionSettings& settings,
                   ConversionReport& report) {
    midi::EventList all;
    for (const midi::SmfTrack& t : smf.tracks) {
        for (const midi::Event& e : t.events) {
            if (midi::tempo_of(e) || midi::time_signature_of(e)) all.push_back(e);
        }
    }
    midi::sort_by_tick(all);
    const midi::TempoMap map = midi::TempoMap::from_events(smf.division.ppq(), all);

    std::set<midi::Tick> at;
    at.insert(0);
    for (const midi::Event& e : all) at.insert(e.tick);

    std::vector<model::TempoEntry> entries;
    std::array<std::uint32_t, 2> meter{{4, 4}};
    bool clamped = false, meter_changed = false;
    std::size_t next = 0;
    for (const midi::Tick tick : at) {
        for (; next < all.size() && all[next].tick <= tick; ++next) {
            const auto ts = midi::time_signature_of(all[next]);
            if (!ts) continue;
            std::uint32_t den = ts->denominator;
            if (ts->numerator == 0) {
                meter_changed = true;
                continue;
            }
            if (!model::is_meter_denominator(den)) {
                den = model::nearest_meter_denominator(den);
                meter_changed = true;
            }
            meter = {{ts->numerator, den}};
        }
        model::TempoEntry e;
        e.at = map.ticks_to_samples(tick, kImportRate);
        const float bpm = static_cast<float>(60000000.0 / map.usec_per_quarter_at(tick));
        e.bpm = model::bpm_within_bounds(bpm);
        clamped = clamped || e.bpm != bpm;
        e.meter = meter;
        if (!entries.empty()) {
            if (entries.back().at == e.at) {
                entries.pop_back();   // two changes in one frame: the later wins
            } else if (entries.back().bpm == e.bpm && entries.back().meter == e.meter) {
                continue;
            }
        }
        entries.push_back(e);
    }
    std::string where;
    const std::string why = model::tempo_map_problem(kImportRate, entries, where);
    if (!why.empty()) {
        report.dropped.push_back("the tempo map (" + where + ": " + why + "); 120 bpm 4/4 is used");
        return false;
    }
    if (clamped) report.approximated.push_back("tempos outside 1 to 1000 bpm were clamped");
    if (meter_changed) {
        report.approximated.push_back("time signatures the session cannot hold were adjusted");
    }
    settings.tempo = std::move(entries);
    return true;
}

}  // namespace

bool SmfImporter::can_read(const std::string& path) const {
    std::ifstream in(path, std::ios::binary);
    char magic[4] = {};
    in.read(magic, 4);
    return in.gcount() == 4 && std::string(magic, 4) == "MThd";
}

bool SmfImporter::read(const std::string& path, model::Snapshot& out, ConversionReport& report,
                       std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        error = path + ": the file is not there";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = path + ": it cannot be read";
        return false;
    }
    std::ostringstream s;
    s << in.rdbuf();
    return read_bytes(s.str(), fs::path(path).stem().string(), out, report, error);
}

bool SmfImporter::read_bytes(const std::string& bytes, const std::string& name,
                             model::Snapshot& out, ConversionReport& report,
                             std::string& error) {
    const midi::SmfReadResult parsed =
        midi::read_smf(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    if (!parsed.ok()) {
        error = "not a readable MIDI file: " + parsed.error.message + " (byte " +
                std::to_string(parsed.error.offset) + ")";
        return false;
    }
    const midi::Smf& smf = *parsed.smf;
    if (smf.division.is_smpte()) {
        error = "this file counts time in SMPTE frames, which is not supported";
        return false;
    }
    if (smf.format == 2) {
        error = "format 2 files (independent sequences) are not supported";
        return false;
    }
    const TickScale scale(smf.division.ppq());
    if (!scale.exact()) {
        report.approximated.push_back(std::to_string(smf.division.ppq()) +
                                      " ticks per quarter does not divide 960000; ticks are rounded");
    }

    model::SessionSettings settings;
    settings.name = plain_text(name) ? name : "Imported";
    settings.session_id = model::new_session_id();
    tempo_entries(smf, scale, settings, report);

    model::History history(model::new_session(settings));
    Counts counts;
    std::size_t unmatched = 0, unterminated = 0, empty_tracks = 0, made = 0;

    for (std::size_t t = 0; t < smf.tracks.size(); ++t) {
        const midi::NoteExtraction x = midi::extract_notes(smf.tracks[t].events);
        unmatched += x.unmatched_note_offs;
        unterminated += x.unterminated_notes;

        model::MidiLists lists;
        std::string track_name;
        Ticks last = 0, end_of_track = 0;
        const auto place = [&](midi::Tick tick, Ticks& at) {
            if (scale(tick, at)) return true;
            error = "an event lies past the end of the timeline";
            return false;
        };
        for (const midi::Note& n : x.notes) {
            midi::Note m = n;
            Ticks end = 0;
            if (!place(n.start, m.start) || !place(n.start + n.length, end)) return false;
            m.length = end - m.start;
            lists.notes.push_back(m);
            last = std::max(last, end);
        }
        midi::sort_notes(lists.notes);
        for (const midi::Event& e : x.other) {
            Ticks at = 0;
            if (!place(e.tick, at)) return false;
            if (e.is_end_of_track()) {
                end_of_track = at;
            } else if (e.is_meta(midi::meta::track_name)) {
                if (track_name.empty() && midi::text_of(e)) track_name = *midi::text_of(e);
            } else if (e.is_meta(midi::meta::tempo) || e.is_meta(midi::meta::time_signature)) {
                // the tempo map above
            } else if (e.status == midi::status::meta) {
                counts.add(meta_name(e.data1));
            } else if (e.status == midi::status::sysex) {
                model::MidiSysEx sx;
                sx.tick = at;
                sx.bytes.push_back(0xF0);
                sx.bytes.insert(sx.bytes.end(), e.payload.begin(), e.payload.end());
                if (sx.bytes.back() != 0xF7) sx.bytes.push_back(0xF7);
                lists.sysex.push_back(std::move(sx));
                last = std::max(last, at + 1);
            } else if (e.is_channel() && e.status >= 0xA0) {
                model::MidiEvent me;
                me.tick = at;
                me.status = e.status;
                me.data1 = e.data1;
                me.data2 = e.data2;
                lists.events.push_back(me);
                last = std::max(last, at + 1);
            } else {
                counts.add("events the model has no place for");
            }
        }
        std::stable_sort(lists.events.begin(), lists.events.end(),
                         [](const model::MidiEvent& a, const model::MidiEvent& b) {
                             return a.tick < b.tick;
                         });
        std::stable_sort(lists.sysex.begin(), lists.sysex.end(),
                         [](const model::MidiSysEx& a, const model::MidiSysEx& b) {
                             return a.tick < b.tick;
                         });
        if (lists.empty()) {
            ++empty_tracks;
            continue;
        }
        ++made;
        if (!plain_text(track_name)) {
            if (!track_name.empty()) {
                report.approximated.push_back("track name \"" + track_name +
                                              "\" replaced: only plain ASCII names are kept");
            }
            track_name = "Track " + std::to_string(t + 1);
        }

        model::Id next_id = history.current().next_id;
        const auto ids = [&next_id] { return next_id++; };
        model::AddTrack add;
        add.track = ids();
        add.playlist = ids();
        add.name = track_name;
        add.playlist_name = track_name + ".01";
        add.kind = model::TrackKind::Midi;
        add.channels = 0;
        model::PlaceMidiRegion place_region;
        place_region.playlist = add.playlist;
        place_region.region.id = ids();
        place_region.region.name = track_name;
        place_region.region.position = 0;
        place_region.region.length = std::max<Ticks>({end_of_track, last, 1});
        place_region.region.data = std::make_shared<const model::MidiSequence>(std::move(lists));
        std::string why;
        if (!history.apply(add, why) || !history.apply(place_region, why)) {
            error = "track \"" + track_name + "\": " + why;
            return false;
        }
    }
    if (made == 0) {
        error = "the file holds no notes or events";
        return false;
    }
    if (empty_tracks > 0) {
        report.dropped.push_back(std::to_string(empty_tracks) +
                                 " track(s) with no notes or events (conductor data is kept as the tempo map)");
    }
    if (unmatched > 0) {
        report.dropped.push_back(std::to_string(unmatched) + " note off(s) with no note on");
    }
    if (unterminated > 0) {
        report.approximated.push_back(std::to_string(unterminated) +
                                      " note(s) never ended, closed at the end of their track");
    }
    for (const auto& [what, n] : counts.dropped) {
        report.dropped.push_back(std::to_string(n) + " " + what);
    }
    std::string why;
    out = history.current();
    if (!model::check_invariants(out, why)) {
        error = "the converted session breaks the format: " + why;
        return false;
    }
    return true;
}

}  // namespace sonora::interchange
