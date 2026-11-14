// SPDX-License-Identifier: GPL-3.0-or-later
//
// plugins_host_vstpreset_fuzz (ADR-0029 slice 3): the .vstpreset reader
// (src/vstpreset.h) on untrusted bytes. A well-formed file reads back
// exactly; every truncation, every hostile offset, size and count, and a
// few hundred thousand random mutations are refused or read, never a
// crash, a hang or a read outside the buffer (the sanitizer job runs this
// too). A file over the size cap is refused by its size before it is read;
// a class id in COM byte order is the same class. The Info chunk's Name
// (ADR-0144) is read from hostile XML, and a bad Info leaves the file name.

#include "vstpreset.h"
#include "vstpreset_writer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace vp = sonora::plugin_host::vstpreset;
namespace fs = std::filesystem;
using sonora::plugin_host::test::put_le;
using sonora::plugin_host::test::vstpreset;

namespace {

int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

constexpr const char* kHex = "534F4E4F524156535433544553540009";

bool refused(const std::string& bytes, const char* says) {
    vp::File f;
    std::string e;
    const bool ok = !vp::parse(bytes, f, e) && e.find(says) != std::string::npos;
    if (!ok) std::printf("  wanted a refusal saying \"%s\", got \"%s\"\n", says, e.c_str());
    return ok;
}

void set_u64(std::string& s, std::size_t at, uint64_t v) {
    for (int k = 0; k < 8; ++k) s[at + std::size_t(k)] = static_cast<char>((v >> (8 * k)) & 0xFF);
}

void well_formed() {
    const std::string good = vstpreset(kHex, {{"Comp", "component"}, {"Cont", "controller"}, {"Info", "<x/>"}});
    vp::File f;
    std::string e;
    CHECK(vp::parse(good, f, e) && f.class_id == kHex && f.component == "component" && f.has_controller &&
          f.controller == "controller");
    // Lower-case hex is the same id; no 'Cont' is a single-component plugin.
    std::string lower = vstpreset("534f4e4f524156535433544553540009", {{"Comp", ""}});
    CHECK(vp::parse(lower, f, e) && f.class_id == kHex && f.component.empty() && !f.has_controller);
    CHECK(vp::same_class(f.class_id, "vst3:534F4E4F524156535433544553540009"));
    // COM order: the first 4, 2 and 2 bytes reversed.
    CHECK(vp::same_class("4F4E4F534152535654335445535400" "09", "vst3:534F4E4F524156535433544553540009"));
    CHECK(!vp::same_class(kHex, "vst3:534F4E4F524156535433544553540001"));
    CHECK(!vp::same_class(kHex, "urn:sonora:test#state"));
    CHECK(!vp::same_class(kHex, "vst3:534F"));
    std::string id;
    CHECK(vp::read_header(good.substr(0, vp::kHeaderBytes), id, e) && id == kHex);
}

void crafted() {
    const std::string good = vstpreset(kHex, {{"Comp", "component"}, {"Cont", "controller"}});
    const std::size_t list = good.size() - 8 - 2 * 20;
    CHECK(refused("", "shorter than its header"));
    CHECK(refused(good.substr(0, 47), "shorter than its header"));
    std::string s = good;
    s[0] = 'X';
    CHECK(refused(s, "no 'VST3'"));
    CHECK(refused(vstpreset(kHex, {{"Comp", "c"}}, 2), "format version 2"));
    CHECK(refused(vstpreset("534F4E4F52415653543354455354000G", {{"Comp", "c"}}), "not hexadecimal"));
    // The list offset: inside the header, past the end, negative, max.
    for (uint64_t off : {uint64_t{0}, uint64_t{47}, uint64_t(good.size()), uint64_t(good.size() - 7),
                         ~uint64_t{0}, uint64_t{1} << 63, (uint64_t{1} << 63) - 1}) {
        s = good;
        set_u64(s, 40, off);
        CHECK(refused(s, "chunk list offset"));
    }
    s = good;
    s[list] = 'X';
    CHECK(refused(s, "no 'List'"));
    // Entry counts: negative, over the cap, more than the bytes hold.
    for (uint32_t n : {0xFFFFFFFFu, 0x80000000u, 65u, 3u}) {
        s = good;
        for (int k = 0; k < 4; ++k) s[list + 4 + std::size_t(k)] = static_cast<char>((n >> (8 * k)) & 0xFF);
        CHECK(refused(s, n == 3u ? "cut short" : "chunks; at most"));
    }
    // A chunk's offset and size: inside the header, overlapping the list,
    // overflowing when added, the int64 extremes.
    const std::size_t e0 = list + 8;
    for (auto [off, size] : std::initializer_list<std::pair<uint64_t, uint64_t>>{
             {0, 4}, {47, 4}, {48, list}, {uint64_t(list), 1}, {48, ~uint64_t{0}}, {~uint64_t{0}, 1},
             {uint64_t{1} << 63, uint64_t{1} << 63}, {48, (uint64_t{1} << 63) + 1}}) {
        s = good;
        set_u64(s, e0 + 4, off);
        set_u64(s, e0 + 12, size);
        CHECK(refused(s, "outside its data"));
    }
    // An empty chunk at the very end of the data is still inside it.
    s = good;
    set_u64(s, e0 + 4, list);
    set_u64(s, e0 + 12, 0);
    vp::File f;
    std::string e;
    CHECK(vp::parse(s, f, e) && f.component.empty());
    CHECK(refused(vstpreset(kHex, {{"Comp", "a"}, {"Comp", "b"}}), "two 'Comp'"));
    CHECK(refused(vstpreset(kHex, {{"Cont", "a"}}), "no component state"));
    CHECK(refused(vstpreset(kHex, {}), "no component state"));
    // Every truncation of a good file is refused.
    for (std::size_t n = 0; n < good.size(); ++n) {
        if (!(n < vp::kHeaderBytes || !vp::parse(good.substr(0, n), f, e))) {
            std::printf("  a file cut to %zu bytes was read\n", n);
            CHECK(false);
        }
    }
}

void fuzz() {
    const std::string good = vstpreset(kHex, {{"Comp", std::string(64, 'c')}, {"Cont", "ctl"}, {"Info", "<x/>"}});
    std::mt19937_64 rng(29);
    vp::File f;
    std::string e;
    const auto t0 = std::chrono::steady_clock::now();
    int read_back = 0;
    for (int i = 0; i < 200000; ++i) {
        std::string s = good;
        const int edits = 1 + int(rng() % 6);
        for (int k = 0; k < edits; ++k) {
            const std::size_t at = rng() % s.size();
            switch (rng() % 5) {
                case 0: s[at] = static_cast<char>(rng()); break;
                case 1: s.erase(at, 1 + rng() % 16); break;
                case 2: s.insert(at, 1 + rng() % 16, static_cast<char>(rng())); break;
                case 3:
                    // An 8-byte field anywhere set to an extreme.
                    if (at + 8 <= s.size()) set_u64(s, at, rng() % 2 ? ~uint64_t{0} >> (rng() % 64) : rng() % 512);
                    break;
                default: s.resize(rng() % (s.size() + 1)); break;
            }
            if (s.empty()) break;
        }
        if (vp::parse(s, f, e)) {
            ++read_back;
            // What was read lies inside what was given.
            CHECK(f.component.size() + f.controller.size() <= s.size());
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("test_vstpreset: 200000 mutated files, %d read, %.0f ms\n", read_back, ms);
}

// The Info chunk as the SDK's PresetFile writes it, with `attrs` inside.
std::string meta(const std::string& attrs) {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<MetaInfo>\n"
           "\t<Attr id=\"MediaType\" value=\"VstPreset\" type=\"string\" flags=\"writeProtected\"></Attr>\n" +
           attrs + "</MetaInfo>\n";
}

std::string named(const std::string& value) {
    return meta("\t<Attr id=\"MusicalCategory\" value=\"Pad\" type=\"string\"/>\n\t<Attr id=\"Name\" value=\"" + value +
                "\" type=\"string\"/>\n");
}

bool name_is(const std::string& xml, const std::string& want) {
    std::string n;
    const bool ok = vp::info_name(xml, n) && n == want;
    if (!ok) std::printf("  wanted the name \"%s\", got \"%s\"\n", want.c_str(), n.c_str());
    return ok;
}

bool no_name(const std::string& xml) {
    std::string n = "untouched";
    return !vp::info_name(xml, n) && n == "untouched";
}

// ADR-0144: the Name attribute and nothing else, from untrusted XML.
void info() {
    CHECK(name_is(named("Warm Pad"), "Warm Pad"));
    CHECK(name_is(named("Bass &amp; Drums &lt;3&gt; &quot;A&quot; &apos;B&apos;"), "Bass & Drums <3> \"A\" 'B'"));
    CHECK(name_is(named("Caf&#233; &#x2014; \xE6\x9A\x96"), "Caf\xC3\xA9 \xE2\x80\x94 \xE6\x9A\x96"));
    CHECK(name_is(named("  two\tlines\nhere  "), "two lines here"));
    CHECK(name_is(meta("<Attr value='Single' id='Name'/>"), "Single"));
    // A commented-out Name does not count; the real one does.
    CHECK(name_is(meta("<!-- <Attr id=\"Name\" value=\"Old\"/> -->\n<Attr id=\"Name\" value=\"New\"/>"), "New"));
    // Without one.
    CHECK(no_name(meta("")));
    CHECK(no_name("<MetaInfo/>"));
    CHECK(no_name(""));
    CHECK(no_name(meta("<Attr id=\"Name\" type=\"string\"/>")));
    CHECK(no_name(named("")));
    CHECK(no_name(named("   ")));
    // Two names: which is meant is not ours to guess.
    CHECK(no_name(meta("<Attr id=\"Name\" value=\"A\"/><Attr id=\"Name\" value=\"B\"/>")));
    CHECK(no_name(meta("<Attr id=\"Name\" id=\"Name\" value=\"A\"/>")));
    // Truncated: every cut of a good chunk that loses the Name's tag.
    const std::string good = named("Warm Pad");
    const std::size_t tag_end = good.find("/>", good.find("\"Name\"")) + 2;
    for (std::size_t n = 0; n < tag_end; ++n)
        if (!no_name(good.substr(0, n))) {
            std::printf("  an Info cut to %zu bytes named a preset\n", n);
            CHECK(false);
        }
    CHECK(no_name(meta("<!-- never closed <Attr id=\"Name\" value=\"A\"/>")));
    // Over the cap: the chunk is not scanned; a long name is cut on a
    // character boundary.
    CHECK(no_name(named("A") + std::string(vp::kMaxInfoBytes, ' ')));
    std::string long_name;
    for (int k = 0; k < 100; ++k) long_name += "\xC3\xA9";  // 200 bytes of 2-byte characters
    std::string n;
    CHECK(vp::info_name(named("x" + long_name), n) && n.size() <= vp::kMaxNameBytes && n.size() == 127 &&
          n.substr(0, 3) == "x\xC3\xA9");
    // An entity bomb: the DOCTYPE is refused before any entity is looked up.
    std::string bomb = "<?xml version=\"1.0\"?>\n<!DOCTYPE MetaInfo [\n<!ENTITY a0 \"aaaaaaaaaa\">\n";
    for (int k = 1; k < 10; ++k) {
        bomb += "<!ENTITY a" + std::to_string(k) + " \"";
        for (int j = 0; j < 10; ++j) bomb += "&a" + std::to_string(k - 1) + ";";
        bomb += "\">\n";
    }
    bomb += "]>\n<MetaInfo><Attr id=\"Name\" value=\"&a9;\"/></MetaInfo>\n";
    CHECK(no_name(bomb));
    CHECK(no_name(named("&a9;")));
    CHECK(no_name(named("&#0;")) && no_name(named("&#xD800;")) && no_name(named("&#x110000;")));
    CHECK(no_name(named("&#12345678;")) && no_name(named("&amp")) && no_name(named("&;")));
    CHECK(no_name(meta("<!ENTITY x \"y\"><Attr id=\"Name\" value=\"A\"/>")));
    // Invalid UTF-8: a lone continuation, an overlong '/', a surrogate, a
    // cut sequence, past U+10FFFF; and control characters.
    for (const char* bad : {"a\x80", "\xC0\xAF", "\xED\xA0\x80", "\xE6\x9A", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80",
                            "a\x01" "b", "a\x7F", "\xC2\x85"})
        if (!no_name(named(bad))) {
            std::printf("  the name \"%s\" was taken\n", bad);
            CHECK(false);
        }
    // Raw '<' in a value, a tag that never closes, text with a bare '<'.
    CHECK(no_name(meta("<Attr id=\"Name\" value=\"a<b\"/>")));
    CHECK(no_name(meta("<Attr id=\"Name\" value=\"A\"")));
    CHECK(no_name(meta("< Attr id=\"Name\" value=\"A\"/>")));
}

void info_fuzz() {
    const std::string good = named("Caf&#233; &amp; Pad");
    std::mt19937_64 rng(144);
    int named_count = 0;
    for (int i = 0; i < 100000; ++i) {
        std::string s = good;
        const int edits = 1 + int(rng() % 4);
        for (int k = 0; k < edits && !s.empty(); ++k) {
            const std::size_t at = rng() % s.size();
            switch (rng() % 4) {
                case 0: s[at] = static_cast<char>(rng()); break;
                case 1: s.erase(at, 1 + rng() % 8); break;
                case 2: s.insert(at, 1 + rng() % 8, "<>&;\"'!-?#x/="[rng() % 13]); break;
                default: s.resize(rng() % (s.size() + 1)); break;
            }
        }
        std::string n;
        if (vp::info_name(s, n)) {
            ++named_count;
            CHECK(!n.empty() && n.size() <= vp::kMaxNameBytes && n.front() != ' ' && n.back() != ' ');
        }
    }
    std::printf("test_vstpreset: 100000 mutated Info chunks, %d named\n", named_count);
}

void files() {
    std::string templ = (fs::temp_directory_path() / "sonora-vstpreset-XXXXXX").string();
    if (!mkdtemp(templ.data())) {
        CHECK(false);
        return;
    }
    const fs::path dir = templ;
    const fs::path big = dir / "big.vstpreset";
    std::ofstream(big, std::ios::binary) << vstpreset(kHex, {{"Comp", "c"}});
    // Sparse: 8 GiB costs nothing on disk, and must cost nothing to refuse.
    fs::resize_file(big, std::uintmax_t{8} << 30);
    std::string bytes, e;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!vp::load(big.string(), bytes, e) && e.find("a preset may be") != std::string::npos && bytes.empty());
    CHECK(!vp::load_header(big.string(), bytes, e));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
    CHECK(!vp::load(dir.string(), bytes, e) && e.find("not a regular file") != std::string::npos);
    CHECK(!vp::load((dir / "none").string(), bytes, e));
    const fs::path ok = dir / "ok.vstpreset";
    const std::string good = vstpreset(kHex, {{"Comp", "c"}});
    std::ofstream(ok, std::ios::binary) << good;
    CHECK(vp::load(ok.string(), bytes, e) && bytes == good);
    // resolve(): relative paths inside a root only.
    const fs::path root = dir / "root";
    fs::create_directories(root / "V" / "P");
    std::ofstream(root / "V" / "P" / "a.vstpreset", std::ios::binary) << good;
    std::string path;
    CHECK(vp::resolve({root.string()}, "V/P/a.vstpreset", path, e) && fs::path(path).filename() == "a.vstpreset");
    CHECK(!vp::resolve({root.string()}, "/etc/passwd", path, e));
    CHECK(!vp::resolve({root.string()}, "V/../../ok.vstpreset", path, e) && e.find("leaves") != std::string::npos);
    CHECK(!vp::resolve({root.string()}, "V//P/a.vstpreset", path, e) || path.find(root.string()) == 0);
    CHECK(!vp::resolve({root.string()}, "V/P/a.txt", path, e) && e.find("not a .vstpreset") != std::string::npos);
    CHECK(!vp::resolve({root.string()}, "", path, e));
    // list(): the plugin's class only; a vendor that is a path, nothing.
    std::ofstream(root / "V" / "P" / "other.vstpreset", std::ios::binary)
        << vstpreset("534F4E4F524156535433544553540001", {{"Comp", "c"}});
    const auto listed = vp::list({root.string()}, "V", "P", std::string("vst3:") + kHex, 100);
    CHECK(listed.size() == 1 && !listed.empty() && listed[0].relative == "V/P/a.vstpreset" && listed[0].name == "a");
    // The Info chunk's Name, when it has a usable one; the file name when
    // it has none, a bad one, two Info chunks or one over the cap.
    const fs::path pd = root / "V" / "P";
    std::ofstream(pd / "b.vstpreset", std::ios::binary) << vstpreset(kHex, {{"Comp", "c"}, {"Info", named("Bright")}});
    std::ofstream(pd / "c.vstpreset", std::ios::binary) << vstpreset(kHex, {{"Info", meta("")}, {"Comp", "c"}});
    std::ofstream(pd / "d.vstpreset", std::ios::binary)
        << vstpreset(kHex, {{"Comp", "c"}, {"Info", named("One")}, {"Info", named("Two")}});
    std::ofstream(pd / "e.vstpreset", std::ios::binary)
        << vstpreset(kHex, {{"Comp", "c"}, {"Info", named("Big") + std::string(vp::kMaxInfoBytes, ' ')}});
    std::ofstream(pd / "f.vstpreset", std::ios::binary)
        << vstpreset(kHex, {{"Comp", "c"}, {"Info", named("&lol;")}});
    {
        // The Info entry points past the list.
        std::string s = vstpreset(kHex, {{"Comp", "c"}, {"Info", named("Out")}});
        set_u64(s, s.size() - 20 + 4, s.size());
        std::ofstream(pd / "g.vstpreset", std::ios::binary) << s;
    }
    std::string info;
    CHECK(vp::load_info((pd / "b.vstpreset").string(), info, e) && info == named("Bright"));
    CHECK(!vp::load_info((pd / "a.vstpreset").string(), info, e) && e.find("no 'Info'") != std::string::npos);
    CHECK(!vp::load_info((pd / "d.vstpreset").string(), info, e) && e.find("two 'Info'") != std::string::npos);
    CHECK(!vp::load_info((pd / "e.vstpreset").string(), info, e) && e.find("over") != std::string::npos);
    CHECK(!vp::load_info((pd / "g.vstpreset").string(), info, e) && e.find("outside") != std::string::npos);
    CHECK(!vp::load_info(big.string(), info, e) && e.find("a preset may be") != std::string::npos);
    const auto with_info = vp::list({root.string()}, "V", "P", std::string("vst3:") + kHex, 100);
    const std::vector<std::pair<std::string, std::string>> want = {
        {"V/P/a.vstpreset", "a"}, {"V/P/b.vstpreset", "Bright"}, {"V/P/c.vstpreset", "c"},
        {"V/P/d.vstpreset", "d"}, {"V/P/e.vstpreset", "e"},      {"V/P/f.vstpreset", "f"},
        {"V/P/g.vstpreset", "g"}};
    CHECK(with_info.size() == want.size());
    for (std::size_t k = 0; k < std::min(want.size(), with_info.size()); ++k)
        if (with_info[k].relative != want[k].first || with_info[k].name != want[k].second) {
            std::printf("  listed [%s \"%s\"], wanted [%s \"%s\"]\n", with_info[k].relative.c_str(),
                        with_info[k].name.c_str(), want[k].first.c_str(), want[k].second.c_str());
            CHECK(false);
        }
    CHECK(vp::list({root.string()}, "..", "P", std::string("vst3:") + kHex, 100).empty());
    CHECK(vp::list({root.string()}, "V/P", "..", std::string("vst3:") + kHex, 100).empty());
    std::error_code ec;
    fs::remove_all(dir, ec);
}

}  // namespace

int main() {
    well_formed();
    crafted();
    fuzz();
    info();
    info_fuzz();
    files();
    if (failures) {
        std::printf("test_vstpreset: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_vstpreset: ok\n");
    return 0;
}
