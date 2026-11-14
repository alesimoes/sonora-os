#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "sonora/loops/loop_scan.h"

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);      \
            std::exit(1);                                                \
        }                                                                \
    } while (0)

using namespace sonora::loops;

namespace {

struct FakeFs : ILoopFileSystem {
    struct File { std::string data; std::int64_t mtime = 1; };
    std::map<std::string, File> files;  // full path
    bool list(const std::string& dir, std::vector<FsEntry>& out) const override {
        std::map<std::string, FsEntry> seen;
        const std::string pre = dir + "/";
        for (const auto& kv : files) {
            if (kv.first.compare(0, pre.size(), pre) != 0) continue;
            std::string rest = kv.first.substr(pre.size());
            auto slash = rest.find('/');
            FsEntry e;
            e.name = rest.substr(0, slash);
            e.is_dir = slash != std::string::npos;
            e.size = kv.second.data.size();
            e.mtime = kv.second.mtime;
            seen[e.name] = e;
        }
        for (auto& kv : seen) out.push_back(kv.second);
        return true;
    }
    bool read(const std::string& p, std::size_t max, std::string& out) const override {
        auto it = files.find(p);
        if (it == files.end()) return false;
        out = it->second.data.substr(0, max);
        return true;
    }
};

void put32(std::string& s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 0xff);
}
void put16(std::string& s, std::uint16_t v) {
    s += static_cast<char>(v & 0xff);
    s += static_cast<char>(v >> 8);
}

std::string wav(bool with_acid, float tempo) {
    std::string fmt;
    put16(fmt, 1); put16(fmt, 2); put32(fmt, 44100); put32(fmt, 44100 * 4);
    put16(fmt, 4); put16(fmt, 16);
    std::string body = "WAVE";
    body += "fmt "; put32(body, 16); body += fmt;
    if (with_acid) {
        std::string a;
        put32(a, 0); put16(a, 0); put16(a, 0); put32(a, 0);
        put32(a, 8);            // beats
        put16(a, 4); put16(a, 4);
        std::uint32_t raw;
        std::memcpy(&raw, &tempo, 4);
        put32(a, raw);
        body += "acid"; put32(body, static_cast<std::uint32_t>(a.size())); body += a;
    }
    body += "data"; put32(body, 4000);  // 1000 stereo 16-bit frames
    body += std::string(64, '\0');       // truncated read of the sample data
    std::string out = "RIFF";
    put32(out, static_cast<std::uint32_t>(body.size()));
    return out + body;
}

}  // namespace

int main() {
    FakeFs fs;
    fs.files["lib/Drums/Funk_100bpm_Cmin.wav"] = {wav(false, 0)};
    fs.files["lib/Synth/Pad.wav"] = {wav(true, 90.0f)};
    fs.files["lib/Synth/Pad.json"] = {R"({"name":"Warm Pad","tags":["ambient","soft"],"key":"Am"})"};
    fs.files["lib/Synth/notes.txt"] = {"x"};

    LoopScanner sc(fs, "lib");
    sc.refresh();
    CHECK(sc.count() == 2);
    const LoopEntry* d = sc.at(0);
    CHECK(d->category == "Drums");
    CHECK(d->bpm == 100.0);           // filename token
    CHECK(d->key == "Cmin");
    CHECK(d->sample_rate == 44100);
    CHECK(d->frames == 1000);
    const LoopEntry* p = sc.at(1);
    CHECK(p->bpm == 90.0);            // acid chunk
    CHECK(p->beats == 8);
    CHECK(p->name == "Warm Pad");     // sidecar
    CHECK(p->tags.size() == 2);
    CHECK(p->key == "Am");
    LoopQuery q;
    q.text = "soft";
    CHECK(sc.search(q).size() == 1);

    // Unchanged files come from the cache.
    CHECK(sc.files_read() == 2);
    sc.refresh();
    CHECK(sc.files_read() == 2);
    // A touched sidecar re-reads only that loop.
    fs.files["lib/Synth/Pad.json"] = {R"({"name":"Cold Pad"})", 5};
    sc.refresh();
    CHECK(sc.files_read() == 3);
    CHECK(sc.at(1)->name == "Cold Pad");
    // A deleted file leaves the catalog.
    fs.files.erase("lib/Drums/Funk_100bpm_Cmin.wav");
    sc.refresh();
    CHECK(sc.count() == 1);
    // Garbage sidecar does not break the scan.
    fs.files["lib/Synth/Pad.json"] = {"{not json", 9};
    sc.refresh();
    CHECK(sc.count() == 1);
    std::printf("loops_scan ok\n");
    return 0;
}
