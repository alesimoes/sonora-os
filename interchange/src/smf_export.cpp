// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <filesystem>
#include <fstream>

#include "sonora/interchange/interchange.h"
#include "sonora/midi/notes.h"
#include "sonora/midi/smf.h"
#include "sonora/midi/tempo_map.h"
#include "sonora/model/musical_map.h"

namespace sonora::interchange {

namespace {

constexpr int kExportPpq = 960;
constexpr model::Ticks kTicksPerSmfTick = model::kTicksPerQuarter / kExportPpq;

midi::Tick to_smf(model::Ticks t) { return (t + kTicksPerSmfTick / 2) / kTicksPerSmfTick; }

std::string plural(std::size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

}  // namespace

bool SmfExporter::encode(const model::Snapshot& s, std::string& bytes, ConversionReport& report,
                         std::string& error) {
    model::MusicalMap map;
    if (!map.build(s.settings, error)) return false;

    midi::Smf smf;
    smf.format = 1;
    smf.division = midi::Division::ticks_per_quarter(kExportPpq);

    midi::SmfTrack conductor;
    conductor.events.push_back(midi::track_name(0, s.settings.name.substr(0, 80)));
    const auto& entries = map.entries();
    for (std::size_t i = 0; i < entries.size() && i < s.settings.tempo.size(); ++i) {
        const model::TempoEntry& t = s.settings.tempo[i];
        const midi::Tick at = to_smf(entries[i].tick);
        conductor.events.push_back(
            midi::tempo(at, midi::TempoMap::usec_from_bpm(static_cast<double>(t.bpm))));
        conductor.events.push_back(midi::time_signature(
            at, static_cast<std::uint8_t>(t.meter[0]), static_cast<std::uint8_t>(t.meter[1])));
    }
    smf.tracks.push_back(std::move(conductor));

    std::size_t audio = 0, off_grid = 0;
    for (const auto& track : s.edit->tracks) {
        if (track->kind == model::TrackKind::Audio) {
            ++audio;
            continue;
        }
        const model::MidiPlaylist* playlist = nullptr;
        for (const auto& p : track->midi_playlists) {
            if (p->id == track->active_playlist) playlist = p.get();
        }
        if (playlist != nullptr && track->midi_playlists.size() > 1) {
            report.dropped.push_back("track \"" + track->name + "\": " +
                                     plural(track->midi_playlists.size() - 1, "other playlist",
                                            "other playlists"));
        }

        midi::NoteList notes;
        midi::EventList others;
        if (playlist != nullptr) {
            for (const model::MidiRegion& r : playlist->regions) {
                if (r.mute) {
                    report.dropped.push_back("track \"" + track->name + "\": muted region \"" +
                                             r.name + "\"");
                    continue;
                }
                // A region is a window: what starts in [offset, offset + length)
                // sounds, placed at `position`.
                const model::Ticks shift = r.position - r.offset;
                const model::Ticks from = r.offset, to = r.offset + r.length;
                for (const midi::Note& n : r.data->notes()) {
                    if (n.start < from || n.start >= to) continue;
                    midi::Note m = n;
                    m.start = to_smf(n.start + shift);
                    m.length = std::max<midi::Tick>(to_smf(n.start + shift + n.length) - m.start,
                                                    n.length > 0 ? 1 : 0);
                    off_grid += (n.start + shift) % kTicksPerSmfTick != 0 ||
                                n.length % kTicksPerSmfTick != 0;
                    notes.push_back(m);
                }
                for (const model::MidiEvent& e : r.data->events()) {
                    if (e.tick < from || e.tick >= to) continue;
                    midi::Event ev;
                    ev.tick = to_smf(e.tick + shift);
                    ev.status = e.status;
                    ev.data1 = e.data1;
                    ev.data2 = e.data2;
                    off_grid += (e.tick + shift) % kTicksPerSmfTick != 0;
                    others.push_back(std::move(ev));
                }
                for (const model::MidiSysEx& x : r.data->sysex()) {
                    if (x.tick < from || x.tick >= to) continue;
                    midi::Event ev;
                    ev.tick = to_smf(x.tick + shift);
                    ev.status = midi::status::sysex;
                    ev.payload.assign(x.bytes.begin() + 1, x.bytes.end());
                    off_grid += (x.tick + shift) % kTicksPerSmfTick != 0;
                    others.push_back(std::move(ev));
                }
            }
        }
        midi::sort_notes(notes);
        midi::sort_by_tick(others);
        midi::SmfTrack out;
        out.events.push_back(midi::track_name(0, track->name.substr(0, 80)));
        const midi::EventList merged =
            midi::merge_events(midi::notes_to_events(notes), others);
        out.events.insert(out.events.end(), merged.begin(), merged.end());
        smf.tracks.push_back(std::move(out));
    }
    if (smf.tracks.size() == 1) {
        error = "the session has no MIDI or instrument track to export";
        return false;
    }
    if (off_grid > 0) {
        report.approximated.push_back(plural(off_grid, "position", "positions") +
                                      " rounded to the file's 960 ticks per quarter");
    }
    if (audio > 0) {
        report.dropped.push_back(plural(audio, "audio track", "audio tracks") +
                                 " (a MIDI file holds no audio)");
    }
    if (!s.edit->markers->markers.empty()) {
        report.dropped.push_back(plural(s.edit->markers->markers.size(), "marker", "markers"));
    }
    report.dropped.push_back("mix, routing, plugins and automation");

    const midi::SmfWriteResult w = midi::write_smf(smf);
    if (!w.ok()) {
        error = "the file could not be written: " + w.error.message;
        return false;
    }
    bytes.assign(w.bytes.begin(), w.bytes.end());
    return true;
}

bool SmfExporter::write(const model::Snapshot& s, const std::string& path,
                        ConversionReport& report, std::string& error) {
    std::string bytes;
    if (!encode(s, bytes, report, error)) return false;
    // Beside the destination and renamed over it: a failed export leaves no
    // half file where a good one was.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            error = tmp + ": writing failed";
            std::error_code ignore;
            std::filesystem::remove(tmp, ignore);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        error = path + ": " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace sonora::interchange
