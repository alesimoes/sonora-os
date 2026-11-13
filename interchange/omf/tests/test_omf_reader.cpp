// SPDX-License-Identifier: GPL-3.0-or-later
// OmfReader on a hand-built OMFI fixture, plus every truncation and every
// single-byte corruption of it: an error or a document, never a crash.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "omf_container.h"
#include "sonora/interchange/omf_import.h"

using namespace sonora::interchange;
using Bytes = std::vector<std::uint8_t>;

static int fails = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            ++fails;                                                \
        }                                                           \
    } while (0)

namespace {

void be32(Bytes& b, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(std::uint8_t(v >> s));
}
Bytes i64(std::int64_t v) {
    Bytes b;
    be32(b, std::uint32_t(std::uint64_t(v) >> 32));
    be32(b, std::uint32_t(std::uint64_t(v)));
    return b;
}
Bytes u32(std::uint32_t v) {
    Bytes b;
    be32(b, v);
    return b;
}
Bytes ids(std::vector<std::uint32_t> v) {
    Bytes b;
    for (auto x : v) be32(b, x);
    return b;
}
Bytes str(const std::string& s) { return Bytes(s.begin(), s.end()); }

Bytes wav(int frames) {
    Bytes b;
    auto le32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(std::uint8_t(v >> (8 * i))); };
    auto le16 = [&](std::uint16_t v) { b.push_back(std::uint8_t(v)); b.push_back(std::uint8_t(v >> 8)); };
    const std::uint32_t data = std::uint32_t(frames) * 2;
    for (char c : std::string("RIFF")) b.push_back(std::uint8_t(c));
    le32(36 + data);
    for (char c : std::string("WAVEfmt ")) b.push_back(std::uint8_t(c));
    le32(16); le16(1); le16(1); le32(48000); le32(96000); le16(2); le16(16);
    for (char c : std::string("data")) b.push_back(std::uint8_t(c));
    le32(data);
    b.resize(b.size() + data, 0);
    return b;
}

std::uint32_t cc(const char* s) {
    return (std::uint32_t(std::uint8_t(s[0])) << 24) | (std::uint32_t(std::uint8_t(s[1])) << 16) |
           (std::uint32_t(std::uint8_t(s[2])) << 8) | std::uint32_t(std::uint8_t(s[3]));
}

struct Prop { const char* name; Bytes v; };
struct Obj { std::uint32_t id; const char* cls; std::vector<Prop> props; };

Bytes build(const std::vector<Obj>& objs, std::uint32_t media_class_unused = 0) {
    (void)media_class_unused;
    Bytes data;
    std::vector<std::vector<std::pair<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>>> locs;
    for (const auto& o : objs) {
        locs.emplace_back();
        for (const auto& p : o.props) {
            locs.back().push_back({cc(p.name),
                                   {std::uint32_t(20 + data.size()), std::uint32_t(p.v.size())}});
            data.insert(data.end(), p.v.begin(), p.v.end());
        }
    }
    Bytes toc;
    be32(toc, std::uint32_t(objs.size()));
    for (std::size_t i = 0; i < objs.size(); ++i) {
        be32(toc, objs[i].id);
        be32(toc, cc(objs[i].cls));
        be32(toc, std::uint32_t(locs[i].size()));
        for (auto& l : locs[i]) { be32(toc, l.first); be32(toc, l.second.first); be32(toc, l.second.second); }
    }
    Bytes f = {0xA4, 'C', 'M', 0xA5, 'H', 'd', 'r', 0xD7, 0, 0, 0, 2};
    be32(f, std::uint32_t(20 + data.size()));
    be32(f, std::uint32_t(toc.size()));
    f.insert(f.end(), data.begin(), data.end());
    f.insert(f.end(), toc.begin(), toc.end());
    return f;
}

Bytes fixture() {
    return build({
        {1, "HEAD", {{"RATE", u32(48000)}}},
        {2, "MEDA", {{"NAME", str("kick")}, {"LENG", i64(100)}, {"DATA", wav(100)}}},
        {3, "MEDA", {{"NAME", str("snare")}, {"PATH", str("Audio/snare.wav")}}},
        {4, "SCLP", {{"SRCM", u32(2)}, {"STRT", i64(480)}, {"OFFS", i64(10)}, {"LENG", i64(50)}}},
        {5, "SLOT", {{"NAME", str("Kick")}, {"SEGS", ids({4})}}},
        {6, "MOBS", {{"SLTS", ids({5})}}},
        {7, "EFCT", {}},
    });
}

std::string write_tmp(const std::string& name, const Bytes& b) {
    std::FILE* f = std::fopen(name.c_str(), "wb");
    if (f) { std::fwrite(b.data(), 1, b.size(), f); std::fclose(f); }
    return name;
}

}  // namespace

int main() {
    auto reader = make_omf_reader(".");
    const std::string path = write_tmp("omf_fixture.omf", fixture());
    OmfDocument d;
    std::string err;
    CHECK(reader->read(path, d, err));
    CHECK(d.sample_rate == 48000);
    CHECK(d.sources.size() == 2);
    CHECK(d.sources[0].embedded && d.sources[0].length == 100);
    CHECK(d.sources[1].path == "Audio/snare.wav" && !d.sources[1].embedded);
    CHECK(d.tracks.size() == 1 && d.tracks[0].name == "Kick");
    CHECK(d.tracks[0].clips.size() == 1);
    if (!d.tracks[0].clips.empty()) {
        const auto& c = d.tracks[0].clips[0];
        CHECK(c.source == 0 && c.start == 480 && c.source_offset == 10 && c.length == 50);
    }
    CHECK(d.unsupported.size() == 1);

    const Bytes good = fixture();
    // Truncated at every length: the TOC is last, so each prefix must fail.
    for (std::size_t n = 0; n < good.size(); ++n) {
        Bytes t(good.begin(), good.begin() + n);
        OmfDocument x;
        std::string e;
        const bool ok = reader->read(write_tmp("omf_trunc.omf", t), x, e);
        CHECK(!ok && !e.empty());
    }
    // Every byte flipped three ways: result is free, surviving is the point.
    int errors = 0;
    for (std::size_t i = 0; i < good.size(); ++i)
        for (std::uint8_t mask : {0xFF, 0x80, 0x01}) {
            Bytes t = good;
            t[i] ^= mask;
            OmfDocument x;
            std::string e;
            if (!reader->read(write_tmp("omf_corrupt.omf", t), x, e)) ++errors;
        }
    CHECK(errors > 0);

    // A huge object count and a TOC pointing past the end both error.
    Bytes lie = good;
    lie[16] = lie[17] = lie[18] = lie[19] = 0xFF;
    OmfDocument x;
    std::string e;
    CHECK(!reader->read(write_tmp("omf_lie.omf", lie), x, e));
    OmfContainer c;
    CHECK(!c.parse(Bytes(8, 0), e));
    CHECK(!reader->read("no/such/file.omf", x, e));

    auto imp = make_omf_importer(".");
    CHECK(imp->can_read("a.omf"));
    for (const char* f : {"omf_fixture.omf", "omf_trunc.omf", "omf_corrupt.omf", "omf_lie.omf",
                          "omf_media_2.wav"})
        std::remove(f);
    if (fails == 0) std::printf("omf_reader ok\n");
    return fails ? 1 : 0;
}
