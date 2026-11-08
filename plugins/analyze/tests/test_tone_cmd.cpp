// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-analyze tone and ampcab-tone-profile, end to end through the two
// executables (ADR-0072): the empty library, a fixture library of three
// near and one far capture, a stale card, a missing profile, and the
// generator's determinism and its no-capture exit.
//
// usage: test_analyze_tone_cmd <sonora-analyze> <ampcab-tone-profile> <tiny.nam> <scratch dir>
#include <sys/wait.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "sonora/ampcab/capture_card.h"
#include "sonora/ampcab/capture_card_fake.h"
#include "sonora/analyze/mix_analysis.h"
#include "sonora/analyze/tone_profile.h"
#include "sonora/json/json.h"
#include "sonora/sha256.h"
#include "tone_measure.h"

namespace {

namespace fs = std::filesystem;
using namespace sonora::analyze;

int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

std::string g_analyze, g_gen;
fs::path g_root;

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void spit(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

struct Run {
    int rc = -1;
    std::string out;
};

Run run(const std::string& cmd) {
    const fs::path log = g_root / "run.log";
    const int st = std::system((cmd + " > \"" + log.string() + "\" 2>&1").c_str());
    return {WIFEXITED(st) ? WEXITSTATUS(st) : -1, slurp(log)};
}

void write_wav16(const fs::path& p, const std::vector<float>& x, int rate) {
    std::string d;
    auto u32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) d.push_back(static_cast<char>(v >> (8 * i))); };
    auto u16 = [&](std::uint16_t v) { for (int i = 0; i < 2; ++i) d.push_back(static_cast<char>(v >> (8 * i))); };
    const std::uint32_t bytes = static_cast<std::uint32_t>(x.size() * 2);
    d += "RIFF"; u32(36 + bytes); d += "WAVEfmt "; u32(16); u16(1); u16(1);
    u32(static_cast<std::uint32_t>(rate)); u32(static_cast<std::uint32_t>(rate * 2)); u16(2); u16(16);
    d += "data"; u32(bytes);
    for (float v : x) u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(v * 32767.0f))));
    spit(p, d);
}

std::string card_md(const std::string& id) {
    auto c = sonora::ampcab::valid_capture_card();
    c.id = id;
    c.name = "fixture " + id;
    c.file.name = id + ".nam";
    return sonora::ampcab::format_card(c);
}

std::string manifest(const std::vector<std::string>& ids) {
    std::string m = "{\"entries\":[";
    for (std::size_t i = 0; i < ids.size(); ++i) m += (i ? "," : "") + std::string("{\"id\":\"") + ids[i] + "\"}";
    return m + "]}";
}

std::string tone_cmd(const fs::path& lib, const std::string& extra = "") {
    return "\"" + g_analyze + "\" tone --reference \"" + (g_root / "ref.wav").string() + "\" --captures \"" +
           (lib / "captures").string() + "\" --cards \"" + (lib / "cards").string() + "\" " + extra;
}

void test_empty_library() {
    const fs::path lib = g_root / "empty";
    spit(lib / "captures" / "manifest.json", manifest({}));
    fs::create_directories(lib / "cards");
    const Run r = run(tone_cmd(lib));
    CHECK(r.rc == 0);
    CHECK(r.out.find("no captures installed") != std::string::npos);
    const Run j = run(tone_cmd(lib, "--json"));
    CHECK(j.rc == 0 && j.out.find("no captures installed") != std::string::npos);
    CHECK(run("\"" + g_analyze + "\" tone").rc == 2);
}

// Profiles at known tilts from the reference's own, so the order is known.
void make_library(const fs::path& lib, const ToneProfile& ref, const std::vector<std::pair<std::string, double>>& caps) {
    std::vector<std::string> ids;
    for (const auto& [id, tilt] : caps) {
        ids.push_back(id);
        const std::string md = card_md(id);
        spit(lib / "cards" / (id + ".md"), md);
        CaptureProfile p;
        p.id = id;
        p.card_sha256 = sonora::sha256_hex(md);
        p.stimulus = kToneStimulusId;
        p.profile = ref;
        for (int b = 0; b < kToneBands; ++b) p.profile.bands_db[b] += tilt * (b - 15) / 15.0;
        std::string text, err;
        CHECK(profile_to_json(p, text, err));
        spit(lib / "captures" / "profiles" / (id + ".json"), text);
    }
    spit(lib / "captures" / "manifest.json", manifest(ids));
}

void test_fixture_library(const ToneProfile& ref) {
    const fs::path lib = g_root / "lib";
    make_library(lib, ref, {{"fixture-c", 3.0}, {"fixture-a", 1.0}, {"fixture-far", 40.0}, {"fixture-b", 2.0}});
    const Run r = run(tone_cmd(lib));
    CHECK(r.rc == 0);
    const auto a = r.out.find("fixture-a"), b = r.out.find("fixture-b"), c = r.out.find("fixture-c");
    CHECK(a != std::string::npos && b != std::string::npos && c != std::string::npos);
    CHECK(a < b && b < c);
    CHECK(r.out.find("fixture-far") == std::string::npos);
    CHECK(r.out.find(" dB") != std::string::npos);
    std::istringstream in(r.out);
    std::string line;
    int lines = 0;
    while (std::getline(in, line)) ++lines;
    CHECK(lines == 4);  // the reference's file name, then three candidates

    const Run j = run(tone_cmd(lib, "--json"));
    sonora::json::Value v;
    sonora::json::Error e;
    CHECK(j.rc == 0 && sonora::json::parse(j.out, v, e));
    const auto* cands = v.find("candidates");
    CHECK(cands && cands->items().size() == 3);
    CHECK(v.find("reference") && v.find("reference")->text() == "ref.wav");

    const Run tight = run(tone_cmd(lib, "--max-distance 0.0001"));
    CHECK(tight.rc == 0 && tight.out.find("no close capture") != std::string::npos);

    // Editing a card makes its profile stale: exit 1, naming the capture.
    spit(lib / "cards" / "fixture-b.md", card_md("fixture-b") + "\nedited\n");
    const Run stale = run(tone_cmd(lib));
    CHECK(stale.rc == 1 && stale.out.find("fixture-b") != std::string::npos &&
          stale.out.find("stale") != std::string::npos);

    fs::remove(lib / "captures" / "profiles" / "fixture-a.json");
    const Run missing = run(tone_cmd(lib));
    CHECK(missing.rc == 1 && missing.out.find("fixture-a") != std::string::npos);
}

void test_generator(const fs::path& nam) {
    const fs::path lib = g_root / "gen";
    const std::string nam_bytes = slurp(nam);
    auto c = sonora::ampcab::valid_capture_card();
    c.id = "fixture-tiny";
    c.file.name = "tiny.nam";
    c.file.size = nam_bytes.size();
    c.file.sha256 = sonora::sha256_hex(nam_bytes);
    const std::string md = sonora::ampcab::format_card(c);
    spit(lib / "cards" / "fixture-tiny.md", md);
    spit(lib / "captures" / "manifest.json", manifest({"fixture-tiny"}));
    const std::string base = "\"" + g_gen + "\" --captures \"" + (lib / "captures").string() + "\" --cards \"" +
                             (lib / "cards").string() + "\" --out ";

    // The card is there but the capture file is not: nothing is written.
    const Run none = run(base + "\"" + (lib / "none").string() + "\"");
    CHECK(none.rc == 0 && none.out.find("no captures installed") != std::string::npos);
    CHECK(!fs::exists(lib / "none"));

    spit(lib / "captures" / "tiny.nam", nam_bytes);
    CHECK(run(base + "\"" + (lib / "out1").string() + "\"").rc == 0);
    CHECK(run(base + "\"" + (lib / "out2").string() + "\"").rc == 0);
    const std::string p1 = slurp(lib / "out1" / "fixture-tiny.json");
    CHECK(!p1.empty() && p1 == slurp(lib / "out2" / "fixture-tiny.json"));

    CaptureProfile p;
    std::string err;
    CHECK(parse_profile(p1, p, err));
    CHECK(p.id == "fixture-tiny" && p.card_sha256 == sonora::sha256_hex(md) && p.stimulus == kToneStimulusId);
    CHECK(p.ir_id.empty());

    // A tampered capture file is refused, not profiled.
    spit(lib / "captures" / "tiny.nam", nam_bytes + " ");
    CHECK(run(base + "\"" + (lib / "out3").string() + "\"").rc == 1);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) return 2;
    g_analyze = argv[1];
    g_gen = argv[2];
    g_root = argv[4];
    fs::remove_all(g_root);
    fs::create_directories(g_root);

    const std::vector<float> stim = tone_stimulus();
    CHECK(stim == tone_stimulus());  // the stimulus is the same every time
    write_wav16(g_root / "ref.wav", stim, 48000);

    std::vector<std::vector<float>> planar{stim};
    MemorySource src(planar, 48000.0);
    ToneProfile ref;
    std::string err;
    CHECK(measure_tone_profile(src, RunOptions{}, ref, err));
    CHECK(fill_unresolved_bands(ref));
    CHECK(complete(ref));

    test_empty_library();
    test_fixture_library(ref);
    test_generator(argv[3]);
    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
