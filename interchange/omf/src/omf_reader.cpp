// SPDX-License-Identifier: GPL-3.0-or-later
// OmfReader over the OMFI container (ADR-0160). Read-only; effects, automation
// and anything not modelled is named in OmfDocument::unsupported, not dropped
// silently. Embedded media is written into the session folder and checked
// with WavReader before the document points at it.
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>

#include "omf_container.h"
#include "sonora/interchange/omf_import.h"
#include "sonora/wav_reader.h"

namespace sonora::interchange {

namespace {

constexpr std::uint32_t kHead = fourcc("HEAD"), kMeda = fourcc("MEDA"), kMobs = fourcc("MOBS"),
                        kSlot = fourcc("SLOT"), kSclp = fourcc("SCLP");
constexpr std::uint32_t pRate = fourcc("RATE"), pName = fourcc("NAME"), pPath = fourcc("PATH"),
                        pLeng = fourcc("LENG"), pData = fourcc("DATA"), pSlts = fourcc("SLTS"),
                        pSegs = fourcc("SEGS"), pSrcm = fourcc("SRCM"), pStrt = fourcc("STRT"),
                        pOffs = fourcc("OFFS");

std::string cls_name(std::uint32_t c) {
    std::string s(4, '?');
    for (int i = 0; i < 4; ++i) {
        const char ch = char((c >> (24 - 8 * i)) & 0xFF);
        if (ch >= 32 && ch < 127) s[i] = ch;
    }
    return s;
}

bool read_file(const std::string& path, std::vector<std::uint8_t>& out, std::string& error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::uint8_t buf[65536];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

class OmfFileReader : public OmfReader {
public:
    explicit OmfFileReader(std::string media_dir) : media_dir_(std::move(media_dir)) {}

    bool read(const std::string& path, OmfDocument& out, std::string& error) override {
        std::vector<std::uint8_t> bytes;
        if (!read_file(path, bytes, error)) return false;
        OmfContainer c;
        if (!c.parse(std::move(bytes), error)) return false;
        OmfDocument d;
        std::map<std::uint32_t, int> source_of;
        if (!header(c, d, error) || !sources(c, d, source_of, error) ||
            !tracks(c, d, source_of, error))
            return false;
        note_unsupported(c, d);
        out = std::move(d);
        return true;
    }

private:
    bool header(const OmfContainer& c, OmfDocument& d, std::string& error) {
        for (const auto& o : c.objects()) {
            if (o.cls != kHead) continue;
            std::uint32_t rate = 0;
            if (!c.prop_u32(o, pRate, rate) || rate == 0 || rate > 768000) {
                error = "OMF header has no valid sample rate";
                return false;
            }
            d.sample_rate = rate;
            return true;
        }
        error = "OMF file has no header object";
        return false;
    }

    bool sources(const OmfContainer& c, OmfDocument& d, std::map<std::uint32_t, int>& index,
                 std::string& error) {
        for (const auto& o : c.objects()) {
            if (o.cls != kMeda) continue;
            OmfMediaSource s;
            c.prop_string(o, pName, s.name);
            std::int64_t len = 0;
            if (c.prop_i64(o, pLeng, len)) {
                if (len < 0) {
                    error = "OMF media has a negative length";
                    return false;
                }
                s.length = len;
            }
            if (const OmfProp* p = o.find(pData)) {
                if (!extract(c, o.id, *p, s, d, error)) return false;
            } else {
                c.prop_string(o, pPath, s.path);
            }
            index[o.id] = int(d.sources.size());
            d.sources.push_back(std::move(s));
        }
        return true;
    }

    bool extract(const OmfContainer& c, std::uint32_t id, const OmfProp& p, OmfMediaSource& s,
                 OmfDocument& d, std::string& error) {
        const std::uint8_t* data = nullptr;
        std::size_t n = 0;
        if (!c.prop_bytes(p, data, n) || n < 12) {
            error = "OMF embedded media is truncated";
            return false;
        }
        const bool wave =
            std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WAVE", 4) == 0;
        const bool aiff = std::memcmp(data, "FORM", 4) == 0 &&
                          (std::memcmp(data + 8, "AIFC", 4) == 0 ||
                           std::memcmp(data + 8, "AIFF", 4) == 0);
        if (!wave && !aiff) {
            error = "OMF embedded media is neither WAVE nor AIFF/AIFC";
            return false;
        }
        const std::string out =
            media_dir_ + "/omf_media_" + std::to_string(id) + (wave ? ".wav" : ".aif");
        std::FILE* f = std::fopen(out.c_str(), "wb");
        if (!f) {
            error = "cannot write extracted media " + out;
            return false;
        }
        const bool ok = std::fwrite(data, 1, n, f) == n;
        std::fclose(f);
        if (!ok) {
            error = "short write extracting media " + out;
            return false;
        }
        if (wave) {
            WavReader w;
            std::string werr;
            if (!w.open(out, werr)) {
                error = "embedded WAVE is corrupt: " + werr;
                return false;
            }
            if (s.length == 0) s.length = w.frames();
        } else {
            d.unsupported.push_back("embedded AIFF/AIFC media extracted but not decoded: " + out);
        }
        s.path = out;
        s.embedded = true;
        return true;
    }

    bool tracks(const OmfContainer& c, OmfDocument& d, const std::map<std::uint32_t, int>& src,
                std::string& error) {
        for (const auto& mob : c.objects()) {
            if (mob.cls != kMobs) continue;
            std::vector<std::uint32_t> slots;
            if (!c.prop_ids(mob, pSlts, slots)) continue;
            for (std::uint32_t sid : slots) {
                const OmfObject* so = c.object(sid);
                if (!so || so->cls != kSlot) {
                    error = "OMF mob refers to a missing slot";
                    return false;
                }
                OmfTrack t;
                c.prop_string(*so, pName, t.name);
                std::vector<std::uint32_t> segs;
                c.prop_ids(*so, pSegs, segs);
                for (std::uint32_t gid : segs)
                    if (!clip(c, gid, src, t, error)) return false;
                d.tracks.push_back(std::move(t));
            }
        }
        return true;
    }

    bool clip(const OmfContainer& c, std::uint32_t id, const std::map<std::uint32_t, int>& src,
              OmfTrack& t, std::string& error) {
        const OmfObject* o = c.object(id);
        std::uint32_t mob = 0;
        OmfClip k;
        if (!o || o->cls != kSclp || !c.prop_u32(*o, pSrcm, mob) ||
            !c.prop_i64(*o, pStrt, k.start) || !c.prop_i64(*o, pOffs, k.source_offset) ||
            !c.prop_i64(*o, pLeng, k.length)) {
            error = "OMF source clip is missing or incomplete";
            return false;
        }
        auto it = src.find(mob);
        if (it == src.end()) {
            error = "OMF source clip refers to missing media";
            return false;
        }
        if (k.start < 0 || k.source_offset < 0 || k.length < 0) {
            error = "OMF source clip has a negative position or length";
            return false;
        }
        k.source = it->second;
        t.clips.push_back(k);
        return true;
    }

    static void note_unsupported(const OmfContainer& c, OmfDocument& d) {
        std::set<std::uint32_t> seen;
        for (const auto& o : c.objects()) {
            if (o.cls == kHead || o.cls == kMeda || o.cls == kMobs || o.cls == kSlot ||
                o.cls == kSclp || !seen.insert(o.cls).second)
                continue;
            d.unsupported.push_back("OMF object class " + cls_name(o.cls) + " not imported");
        }
    }

    std::string media_dir_;
};

class OwningImporter : public Importer {
public:
    explicit OwningImporter(const std::string& dir) : reader_(dir), inner_(&reader_) {}
    bool can_read(const std::string& p) const override { return inner_.can_read(p); }
    bool read(const std::string& p, model::Snapshot& s, ConversionReport& r,
              std::string& e) override {
        return inner_.read(p, s, r, e);
    }

private:
    OmfFileReader reader_;
    OmfImporter inner_;
};

}  // namespace

std::unique_ptr<OmfReader> make_omf_reader(const std::string& media_dir) {
    return std::make_unique<OmfFileReader>(media_dir);
}

std::unique_ptr<Importer> make_omf_importer(const std::string& media_dir) {
    return std::make_unique<OwningImporter>(media_dir);
}

}  // namespace sonora::interchange
