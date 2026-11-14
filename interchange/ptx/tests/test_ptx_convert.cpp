// SPDX-License-Identifier: GPL-3.0-or-later
// .ptx wiring: importer through the mapper, media copy, the convert glue with
// an injected reader.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "convert_ptx.h"
#include "fake_ptx_reader.h"
#include "ptx_media.h"
#include "sonora/interchange/ptx_import.h"

using namespace sonora::interchange;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c)                                             \
    do {                                                     \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            ++fails;                                         \
        }                                                    \
    } while (0)

namespace {

void write_wav(const fs::path& p, int frames) {
    std::vector<unsigned char> b;
    auto le32 = [&](unsigned v) { for (int i = 0; i < 4; ++i) b.push_back((unsigned char)(v >> (8 * i))); };
    auto le16 = [&](unsigned v) { b.push_back((unsigned char)v); b.push_back((unsigned char)(v >> 8)); };
    auto tag = [&](const char* s) { for (int i = 0; i < 4; ++i) b.push_back((unsigned char)s[i]); };
    const unsigned data = unsigned(frames) * 2;
    tag("RIFF"); le32(36 + data); tag("WAVE"); tag("fmt ");
    le32(16); le16(1); le16(1); le32(48000); le32(96000); le16(2); le16(16);
    tag("data"); le32(data);
    b.resize(b.size() + data, 0);
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), (std::streamsize)b.size());
}

PtxDocument doc_with(const std::string& wav_path) {
    PtxDocument d;
    d.sample_rate = 48000;
    d.sources.push_back({"kick", wav_path, 4800});
    PtxTrack t;
    t.name = "Kick";
    PtxRegion r;
    r.length_samples = 1000;
    t.regions.push_back(r);
    d.tracks.push_back(t);
    d.unsupported.push_back("plug-in: EQ on Kick");
    return d;
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) if (x == s) return true;
    return false;
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "sonora_ptx_convert";
    fs::remove_all(root);
    fs::create_directories(root / "in");
    write_wav(root / "in" / "kick.wav", 4800);

    {  // importer: decode, map, unsupported features reach report.dropped
        FakePtxReader r;
        r.doc = doc_with("kick.wav");
        PtxImporter imp(&r);
        sonora::model::Snapshot s;
        ConversionReport rep;
        std::string err;
        CHECK(imp.read("a.ptx", s, rep, err));
        CHECK(s.edit->tracks.size() == 1);
        CHECK(s.sources->sources.size() == 1);
        CHECK(has(rep.dropped, "plug-in: EQ on Kick"));
    }
    {  // media copy on a real WAV, and the missing-file error naming it
        PtxDocument d = doc_with("kick.wav");
        std::string err;
        CHECK(copy_ptx_media(d, (root / "in").string(), (root / "out" / "Audio Files").string(), err));
        CHECK(fs::is_regular_file(root / "out" / "Audio Files" / "kick.wav"));
        PtxDocument m = doc_with("nope.wav");
        CHECK(!copy_ptx_media(m, (root / "in").string(), (root / "out2").string(), err));
        CHECK(err.find("nope.wav") != std::string::npos);
    }
    {  // glue: fake reader to a session with a copied Audio Files/ entry
        FakePtxReader r;
        r.doc = doc_with("kick.wav");
        const fs::path dest = root / "sess" / "s.sonora";
        fs::create_directories(dest.parent_path());
        // the .ptx sits beside the audio
        sonora::model::Snapshot s;
        ConversionReport rep;
        std::string err;
        CHECK(ptx_import_with(&r, (root / "in" / "a.ptx").string(), dest.string(), s, rep, err));
        CHECK(s.edit->tracks.size() == 1);
        CHECK(fs::is_regular_file(root / "sess" / "Audio Files" / "kick.wav"));
        CHECK(fs::exists(dest));
    }
    {  // null reader fails loudly
        sonora::model::Snapshot s;
        ConversionReport rep;
        std::string err;
        CHECK(!ptx_import_with(nullptr, "a.ptx", (root / "x.sonora").string(), s, rep, err));
        CHECK(err.find("no .ptx reader") != std::string::npos);
    }
    {  // reader failure and missing media are errors
        FakePtxReader r;
        r.succeed = false;
        sonora::model::Snapshot s;
        ConversionReport rep;
        std::string err;
        CHECK(!ptx_import_with(&r, "a.ptx", (root / "y.sonora").string(), s, rep, err));
        CHECK(err == "fake failure");
        r.succeed = true;
        r.doc = doc_with("gone.wav");
        CHECK(!ptx_import_with(&r, (root / "in" / "a.ptx").string(), (root / "y.sonora").string(), s,
                               rep, err));
        CHECK(err.find("gone.wav") != std::string::npos);
    }
    fs::remove_all(root);
    return fails ? 1 : 0;
}
