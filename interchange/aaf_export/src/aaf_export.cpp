// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/interchange/aaf_export.h"

#include <cstdio>
#include <set>

#include "aaf_export_map.h"
#include "aaf_libaaf_writer.h"
#include "sonora/audio_export.h"
#include "sonora/model/model.h"

namespace sonora::interchange {

namespace {

std::string dir_of(const std::string& p) {
    const auto i = p.find_last_of("/\\");
    return i == std::string::npos ? std::string() : p.substr(0, i + 1);
}

std::string stem_of(const std::string& p) {
    const auto i = p.find_last_of("/\\");
    std::string n = i == std::string::npos ? p : p.substr(i + 1);
    const auto dot = n.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? n : n.substr(0, dot);
}

bool exists(const std::string& p) {
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// Copies each external source beside the .aaf as 32-bit float WAV and points
// the mob at the copy. A missing source is an error naming it.
bool copy_essence(AafDocument& doc, const std::string& aaf_path, std::string& error) {
    std::set<std::string> used;
    for (auto& mob : doc.mobs) {
        if (!mob.is_source || mob.media_path.empty()) continue;
        if (!exists(mob.media_path)) {
            error = "source file missing: " + mob.media_path;
            return false;
        }
        const std::string stem = stem_of(mob.media_path);
        std::string name = stem;
        for (int n = 2; used.count(name); ++n) name = stem + "-" + std::to_string(n);
        used.insert(name);
        const std::string dst = dir_of(aaf_path) + name + ".wav";
        ExportSpec spec;
        spec.container = ExportContainer::Wav;
        spec.bits = 32;
        std::string why;
        if (!export_audio(mob.media_path, dst, spec, ExportMeta{}, 0.0, why)) {
            error = "could not copy " + mob.media_path + ": " + why;
            return false;
        }
        mob.media_path = dst;
    }
    return true;
}

class OwningExporter : public Exporter {
public:
    OwningExporter() : writer_(make_libaaf_writer()), inner_(writer_.get()) {}
    bool write(const model::Snapshot& s, const std::string& p, ConversionReport& r,
               std::string& e) override {
        return inner_.write(s, p, r, e);
    }

private:
    std::unique_ptr<AafWriter> writer_;
    AafExporter inner_;
};

}  // namespace

bool AafExporter::write(const model::Snapshot& s, const std::string& path,
                        ConversionReport& report, std::string& error) {
    if (!writer_) {
        error = "AAF export unavailable: no AAF writer (built without libaaf)";
        return false;
    }
    if (!s.edit || s.edit->tracks.empty()) {
        error = "nothing to export: the session has no tracks";
        return false;
    }
    AafDocument doc;
    if (!map_to_aaf(s, doc, report, error)) return false;
    if (!copy_essence(doc, path, error)) return false;
    return writer_->write(doc, path, error);
}

std::unique_ptr<Exporter> make_aaf_exporter() {
    return std::make_unique<OwningExporter>();
}

}  // namespace sonora::interchange
