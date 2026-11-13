// SPDX-License-Identifier: GPL-3.0-or-later
#include "ptx_libptformat_reader.h"

#include <filesystem>
#include <map>

#include <ptformat/ptformat.h>

#include "sonora/audio_import.h"

namespace sonora::interchange {
namespace {

namespace fs = std::filesystem;

const char* load_error(int rc) {
    switch (rc) {
        case -1: return "cannot decrypt the session";
        case -2: return "not a Pro Tools session";
        case -3: return "Pro Tools version not supported by libptformat";
        case -4: return "cannot parse the session";
        default: return "unknown decoder error";
    }
}

// Positions come back scaled to the rate given to load(); load once to learn
// the session's own rate, then again so nothing is rescaled.
bool load_at_native_rate(PTFFormat& ptf, const std::string& path, std::string& error) {
    int rc = ptf.load(path, 48000);
    if (rc == 0 && ptf.sessionrate() != 48000 && ptf.sessionrate() > 0)
        rc = ptf.load(path, ptf.sessionrate());
    if (rc != 0) {
        error = "ptx: '" + path + "': " + load_error(rc);
        return false;
    }
    return true;
}

std::uint64_t pos(std::int64_t v) { return v < 0 ? 0 : static_cast<std::uint64_t>(v); }

}  // namespace

bool LibPtformatReader::read(const std::string& path, PtxDocument& out, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        error = "ptx: cannot open '" + path + "'";
        return false;
    }
    PTFFormat ptf;
    if (!load_at_native_rate(ptf, path, error)) return false;

    out = PtxDocument{};
    out.sample_rate = static_cast<std::uint32_t>(ptf.sessionrate());
    out.version = std::to_string(static_cast<int>(ptf.version()));

    std::map<std::uint16_t, std::uint32_t> source_of;  // wav index -> sources slot
    for (const auto& w : ptf.audiofiles()) {
        source_of[w.index] = static_cast<std::uint32_t>(out.sources.size());
        out.sources.push_back({w.filename, w.filename, pos(w.length)});
    }
    std::map<std::uint16_t, const PTFFormat::region_t*> region_of;
    for (const auto& r : ptf.regions()) region_of[r.index] = &r;

    // libptformat lists one entry per region placement, the track repeated.
    std::map<std::uint16_t, std::size_t> track_slot;
    for (const auto& t : ptf.tracks()) {
        auto slot = track_slot.find(t.index);
        if (slot == track_slot.end()) {
            slot = track_slot.emplace(t.index, out.tracks.size()).first;
            out.tracks.push_back({t.name, {}});
        }
        const auto reg = region_of.find(t.reg.index);
        if (reg == region_of.end() || !source_of.count(reg->second->wave.index)) {
            error = "ptx: track '" + t.name + "' places a region with no audio source";
            return false;
        }
        PtxRegion r;
        r.source_index = source_of[reg->second->wave.index];
        r.start_samples = pos(t.reg.startpos);
        r.offset_samples = pos(reg->second->sampleoffset);
        r.length_samples = pos(reg->second->length);
        out.tracks[slot->second].regions.push_back(r);
    }
    if (!ptf.miditracks().empty() || !ptf.midiregions().empty())
        out.unsupported.push_back("MIDI tracks and regions");
    out.unsupported.push_back("plugins, automation, fades and inactive playlists");
    return true;
}

namespace {

class OwningImporter : public Importer {
public:
    OwningImporter() : importer_(&reader_) {}
    bool can_read(const std::string& path) const override { return importer_.can_read(path); }
    bool read(const std::string& path, model::Snapshot& s, ConversionReport& r,
              std::string& error) override {
        return importer_.read(path, s, r, error);
    }

private:
    LibPtformatReader reader_;
    PtxImporter importer_;
};

}  // namespace

std::unique_ptr<Importer> make_ptx_importer() { return std::make_unique<OwningImporter>(); }

bool copy_ptx_media(PtxDocument& doc, const std::string& session_dir, const std::string& dest_dir,
                    std::string& error) {
    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    if (ec) {
        error = "ptx: cannot create '" + dest_dir + "': " + ec.message();
        return false;
    }
    for (auto& s : doc.sources) {
        const fs::path stored(s.path);
        const fs::path name = stored.filename();
        const fs::path tries[] = {stored, fs::path(session_dir) / stored,
                                  fs::path(session_dir) / "Audio Files" / name};
        fs::path found;
        for (const auto& t : tries) {
            if (fs::is_regular_file(t, ec)) {
                found = t;
                break;
            }
        }
        if (found.empty()) {
            error = "ptx: missing audio file '" + s.path + "'";
            return false;
        }
        AudioInfo info;
        std::string why;
        if (!probe_audio(found.string(), info, why)) {
            error = "ptx: audio file '" + found.string() + "': " + why;
            return false;
        }
        const fs::path dest = fs::path(dest_dir) / name;
        fs::copy_file(found, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "ptx: cannot copy '" + found.string() + "': " + ec.message();
            return false;
        }
        s.path = dest.string();
    }
    return true;
}

}  // namespace sonora::interchange
