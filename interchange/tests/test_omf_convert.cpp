// SPDX-License-Identifier: GPL-3.0-or-later
// sonora-convert on a synthetic OMF: .omf dispatch, the media copied into
// Audio Files/, and a truncated or corrupt file ending in exit 1, no crash.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "sonora/interchange/interchange.h"

namespace fs = std::filesystem;
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
Bytes u32(std::uint32_t v) { Bytes b; be32(b, v); return b; }
Bytes i64(std::int64_t v) {
    Bytes b;
    be32(b, std::uint32_t(std::uint64_t(v) >> 32));
    be32(b, std::uint32_t(std::uint64_t(v)));
    return b;
}
Bytes ids(std::vector<std::uint32_t> v) { Bytes b; for (auto x : v) be32(b, x); return b; }
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

// Same container layout as omf/tests/test_omf_reader.cpp: header, data, TOC.
Bytes build(const std::vector<Obj>& objs) {
    Bytes data;
    std::vector<std::vector<std::pair<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>>> locs;
    for (const auto& o : objs) {
        locs.emplace_back();
        for (const auto& p : o.props) {
            locs.back().push_back({cc(p.name), {std::uint32_t(20 + data.size()), std::uint32_t(p.v.size())}});
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
        {4, "SCLP", {{"SRCM", u32(2)}, {"STRT", i64(480)}, {"OFFS", i64(10)}, {"LENG", i64(50)}}},
        {5, "SLOT", {{"NAME", str("Kick")}, {"SEGS", ids({4})}}},
        {6, "MOBS", {{"SLTS", ids({5})}}},
    });
}

void write(const fs::path& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

int convert(const std::string& cmd, const fs::path& in, const fs::path& out, std::string& so,
            std::string& se) {
    so.clear();
    se.clear();
    return sonora::interchange::run_convert({cmd, in.string(), out.string()}, so, se);
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "sonora_omf_convert";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string so, se;
    const Bytes good = fixture();

    write(dir / "mix.omf", good);
    CHECK(convert("import", dir / "mix.omf", dir / "out" / "s.sonora", so, se) == 0);
    CHECK(fs::exists(dir / "out" / "s.sonora"));
    CHECK(fs::exists(dir / "out" / "Audio Files" / "omf_media_2.wav"));
    CHECK(convert("omf-import", dir / "mix.omf", dir / "out2" / "s.sonora", so, se) == 0);

    // Every proper prefix is refused with a message, never a crash.
    for (std::size_t n : {std::size_t(0), std::size_t(7), good.size() / 2, good.size() - 1}) {
        write(dir / "bad.omf", Bytes(good.begin(), good.begin() + n));
        CHECK(convert("import", dir / "bad.omf", dir / "bad" / "s.sonora", so, se) == 1);
        CHECK(se.find("sonora-convert:") == 0);
        CHECK(!fs::exists(dir / "bad" / "s.sonora"));
    }
    CHECK(convert("import", dir / "missing.omf", dir / "m" / "s.sonora", so, se) == 1);
    CHECK(se.find("sonora-convert:") == 0);

    fs::remove_all(dir);
    if (fails == 0) std::printf("omf_convert ok\n");
    return fails ? 1 : 0;
}
