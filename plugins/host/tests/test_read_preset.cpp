// SPDX-License-Identifier: GPL-3.0-or-later
//
// plugins_host_read_preset (ADR-0029 slice 3): factory presets listed in the
// probe record and read one-shot by `sonora-plugin-host --read-preset`.
//
//   - LV2 pset:Presets from a bundle of their own: a state preset of the
//     state test plugin, a port-value preset of eg-amp; a preset asked of
//     another plugin, one that is not there, one whose Turtle is broken, and
//     one that makes the plugin crash (the host dies by its signal, which
//     is the engine's to report, ADR-0029 §5).
//   - VST3 programs through IUnitInfo and the program-change parameter, then
//     getState (Unverified 3, for the test plugin), and .vstpreset files
//     under $HOME/.vst3/presets: a good one byte for byte, one of another
//     class, one with a chunk cut short, one over the size cap, one the
//     plugin refuses, one outside the preset folders by "..", one by a
//     symlink.
//
//   test_read_preset --host PATH --lv2-bundle DIR [--vst3-bundle DIR]
//
// Everything it writes lives in a fresh directory under $TMPDIR, which is
// also the children's $HOME.

#include "sonora/plugin/host_control.h"
#include "sonora/plugin/probe_record.h"
#include "vstpreset_writer.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fs = std::filesystem;
namespace pr = sonora::plugin::probe;
namespace pc = sonora::plugin::control;
using sonora::plugin_host::test::snts;
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

std::string host, lv2_bundle, vst3_bundle;
fs::path tmp;

struct Run {
    std::string out;
    int exit_code = -1;  // -1 when it died by a signal
    int signal = 0;
};

Run run(std::vector<std::string> args) {
    args.insert(args.begin(), host);
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    int fds[2];
    Run r;
    if (pipe(fds) != 0) return r;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    pid_t pid = 0;
    if (posix_spawn(&pid, host.c_str(), &fa, nullptr, argv.data(), environ) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        close(fds[0]);
        close(fds[1]);
        return r;
    }
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    // A read that never ends is a failure, not a hung test.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    char buf[65536];
    for (;;) {
        pollfd p{fds[0], POLLIN, 0};
        const int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 deadline - std::chrono::steady_clock::now())
                                 .count());
        if (left <= 0 || poll(&p, 1, left) <= 0) {
            kill(pid, SIGKILL);
            std::printf("  %s did not finish within 60 s\n", args[1].c_str());
            break;
        }
        const ssize_t n = read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        r.out.append(buf, std::size_t(n));
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    if (WIFSIGNALED(status)) r.signal = WTERMSIG(status);
    return r;
}

void write_file(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

// One read, decoded; `want` the outcome, `says` a piece of its reason.
pr::PresetRead read(const std::string& bundle, const std::string& preset, const std::string& plugin,
                    pr::Outcome want, const char* says = "", std::vector<std::string> more = {}) {
    std::vector<std::string> args = {"--read-preset", bundle, preset, "--plugin", plugin};
    args.insert(args.end(), more.begin(), more.end());
    const Run r = run(args);
    pr::PresetRead got;
    std::string e;
    const bool decoded = pr::decode_preset_read(r.out, got, e);
    const bool ok = decoded && got.outcome == want && got.reason.find(says) != std::string::npos &&
                    got.plugin == plugin && got.preset == preset &&
                    r.exit_code == (want == pr::Outcome::Ok ? 0 : 5);
    if (!ok)
        std::printf("  read %s of %s: exit %d signal %d, decoded %d (%s), outcome %d, reason \"%s\"\n",
                    preset.c_str(), plugin.c_str(), r.exit_code, r.signal, int(decoded), e.c_str(), int(got.outcome),
                    got.reason.c_str());
    CHECK(ok);
    return got;
}

pr::Record probe(const std::string& id, std::vector<std::string> bundles) {
    std::vector<std::string> args = {"--probe", id};
    for (const std::string& b : bundles) {
        args.push_back("--bundle");
        args.push_back(b);
    }
    const Run r = run(args);
    pr::Record rec;
    std::string e;
    if (!pr::decode(r.out, rec, e)) std::printf("  probe %s: %s\n  %s\n", id.c_str(), e.c_str(), r.out.c_str());
    CHECK(rec.ok());
    return rec;
}

bool lists(const pr::Record& r, const std::string& id, const std::string& name) {
    for (const pr::Preset& p : r.presets)
        if (p.id == id && p.name == name) return true;
    std::printf("  %s lists no %s \"%s\"; it lists:", r.uri.c_str(), id.c_str(), name.c_str());
    for (const pr::Preset& p : r.presets) std::printf(" [%s \"%s\"]", p.id.c_str(), p.name.c_str());
    std::printf("\n");
    return false;
}

bool lists_id(const pr::Record& r, const std::string& id) {
    for (const pr::Preset& p : r.presets)
        if (p.id == id) return true;
    return false;
}

pc::StateBlob blob_of(const pr::PresetRead& r) {
    pc::StateBlob b;
    std::string e;
    if (!pc::decode_state_blob(r.state, b, e)) std::printf("  the state does not decode: %s\n", e.c_str());
    return b;
}

// ---- LV2

constexpr const char* kState = "urn:sonora:test#state";
constexpr const char* kCrash = "urn:sonora:test#crash";
constexpr const char* kAmp = "http://lv2plug.in/plugins/eg-amp";

std::string lv2_presets_bundle() {
    const fs::path b = tmp / "sonora-test-presets.lv2";
    write_file(b / "manifest.ttl", R"(@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
@prefix pset: <http://lv2plug.in/ns/ext/presets#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .

<urn:sonora:test:presets#count-1000> a pset:Preset ; lv2:appliesTo <urn:sonora:test#state> ; rdfs:seeAlso <presets.ttl> .
<urn:sonora:test:presets#quiet> a pset:Preset ; lv2:appliesTo <http://lv2plug.in/plugins/eg-amp> ; rdfs:seeAlso <presets.ttl> .
<urn:sonora:test:presets#broken> a pset:Preset ; lv2:appliesTo <urn:sonora:test#state> ; rdfs:seeAlso <broken.ttl> .
<urn:sonora:test:presets#boom> a pset:Preset ; lv2:appliesTo <urn:sonora:test#crash> ; rdfs:seeAlso <presets.ttl> .
)");
    write_file(b / "presets.ttl", R"(@prefix atom: <http://lv2plug.in/ns/ext/atom#> .
@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
@prefix pset: <http://lv2plug.in/ns/ext/presets#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
@prefix state: <http://lv2plug.in/ns/ext/state#> .
@prefix xsd:  <http://www.w3.org/2001/XMLSchema#> .

<urn:sonora:test:presets#count-1000> a pset:Preset ;
    lv2:appliesTo <urn:sonora:test#state> ;
    rdfs:label "Count 1000" ;
    state:state [ <urn:sonora:test#blocks> "1000"^^xsd:long ] .

<urn:sonora:test:presets#quiet> a pset:Preset ;
    lv2:appliesTo <http://lv2plug.in/plugins/eg-amp> ;
    rdfs:label "Quiet" ;
    lv2:port [ lv2:symbol "gain" ; pset:value -6.0 ] .

<urn:sonora:test:presets#boom> a pset:Preset ;
    lv2:appliesTo <urn:sonora:test#crash> ;
    rdfs:label "Boom" ;
    lv2:port [ lv2:symbol "crash" ; pset:value 1.0 ] .
)");
    // Not Turtle at all, and a gigantic literal: what a hostile or damaged
    // preset file looks like to lilv.
    write_file(b / "broken.ttl", "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n<urn:sonora:test:presets#broken> "
                                 "lv2:appliesTo [[[ \"" +
                                     std::string(1 << 20, 'x') + "\n");
    return b.string();
}

void lv2() {
    const std::string presets = lv2_presets_bundle();
    const pr::Record state = probe(kState, {lv2_bundle, presets});
    CHECK(lists(state, "lv2:urn:sonora:test:presets#count-1000", "Count 1000"));
    const pr::Record amp = probe(kAmp, {presets});
    CHECK(lists(amp, "lv2:urn:sonora:test:presets#quiet", "Quiet"));
    CHECK(!lists_id(amp, "lv2:urn:sonora:test:presets#count-1000"));

    // A state preset: the state as the host saves one, no port values.
    const pr::PresetRead count = read(presets, "lv2:urn:sonora:test:presets#count-1000", kState, pr::Outcome::Ok, "",
                                      {"--bundle", lv2_bundle});
    CHECK(count.name == "Count 1000" && count.params.empty());
    const pc::StateBlob b = blob_of(count);
    CHECK(b.format == pc::Format::Lv2 && b.id == kState && b.sections.size() == 1);
    CHECK(!b.sections.empty() && b.sections[0].find("urn:sonora:test#blocks") != std::string::npos &&
          b.sections[0].find("1000") != std::string::npos);

    // A port-value preset of a plugin with no state of its own.
    const pr::PresetRead quiet = read(presets, "lv2:urn:sonora:test:presets#quiet", kAmp, pr::Outcome::Ok);
    CHECK(quiet.name == "Quiet" && quiet.state.empty() && quiet.params.size() == 1 && !quiet.params.empty() &&
          quiet.params[0].key == "gain" && quiet.params[0].value == -6.f);

    // Another plugin's preset, one that is not there, a VST3 kind, a plugin
    // that is not installed.
    read(presets, "lv2:urn:sonora:test:presets#count-1000", kAmp, pr::Outcome::NotInstalled, "no preset");
    read(presets, "lv2:urn:sonora:test:presets#absent", kState, pr::Outcome::NotInstalled, "no preset",
         {"--bundle", lv2_bundle});
    read(presets, "vst3-file:a/b.vstpreset", kState, pr::Outcome::Refused, "not a preset id",
         {"--bundle", lv2_bundle});
    read(presets, "lv2:urn:sonora:test:presets#quiet", "urn:sonora:test#absent", pr::Outcome::NotInstalled,
         "not installed");

    // Broken Turtle: an answer, whatever it is, and never a crash or a hang.
    {
        const Run r = run({"--read-preset", presets, "lv2:urn:sonora:test:presets#broken", "--plugin", kState,
                           "--bundle", lv2_bundle});
        pr::PresetRead got;
        std::string e;
        const bool ok = r.signal == 0 && pr::decode_preset_read(r.out, got, e);
        if (!ok) std::printf("  broken: exit %d signal %d: %s\n", r.exit_code, r.signal, e.c_str());
        CHECK(ok);
    }
    // A preset that crashes the plugin ends the host by its signal, before
    // it writes anything: the engine reports that (factory_preset_reader).
    {
        const Run r = run({"--read-preset", presets, "lv2:urn:sonora:test:presets#boom", "--plugin", kCrash,
                           "--bundle", lv2_bundle});
        pr::PresetRead got;
        std::string e;
        CHECK(r.signal == SIGSEGV);
        CHECK(!pr::decode_preset_read(r.out, got, e));
    }
}

// ---- VST3

constexpr const char* kVstState = "vst3:534F4E4F524156535433544553540009";
constexpr const char* kVstStateHex = "534F4E4F524156535433544553540009";
constexpr const char* kVstCrashHex = "534F4E4F524156535433544553540001";

void vst3() {
    const fs::path root = tmp / "home/.vst3/presets";
    const fs::path dir = root / "SonoraOS tests" / "Sonora Test State";
    write_file(dir / "Warm.vstpreset", vstpreset(kVstStateHex, {{"Comp", snts(42)}, {"Info", "<MetaInfo/>"}}));
    write_file(dir / "Strings/Other.vstpreset", vstpreset(kVstCrashHex, {{"Comp", snts(7)}}));
    {
        std::string cut = vstpreset(kVstStateHex, {{"Comp", snts(9)}});
        // The chunk says 12 bytes; move the list so they run past it.
        cut[40] = static_cast<char>(48 + 4);
        cut.erase(48 + 4, 8);
        write_file(dir / "Cut.vstpreset", cut);
    }
    write_file(dir / "Refused.vstpreset", vstpreset(kVstStateHex, {{"Comp", "XXXX12345678"}}));
    {
        // A sparse file over the cap: refused by its size, never read.
        write_file(dir / "Big.vstpreset", vstpreset(kVstStateHex, {{"Comp", snts(1)}}));
        fs::resize_file(dir / "Big.vstpreset", std::uintmax_t{64} << 20);
    }
    write_file(tmp / "home/outside.vstpreset", vstpreset(kVstStateHex, {{"Comp", snts(5)}}));
    fs::create_symlink(tmp / "home/outside.vstpreset", dir / "Link.vstpreset");

    const pr::Record rec = probe(kVstState, {vst3_bundle});
    CHECK(lists(rec, "vst3-program:0/0", "Count 0"));
    CHECK(lists(rec, "vst3-program:0/2", "Count 2000"));
    CHECK(lists(rec, "vst3-file:SonoraOS tests/Sonora Test State/Warm.vstpreset", "Warm"));
    CHECK(!lists_id(rec, "vst3-file:SonoraOS tests/Sonora Test State/Strings/Other.vstpreset"));
    CHECK(!lists_id(rec, "vst3-file:SonoraOS tests/Sonora Test State/Big.vstpreset"));
    CHECK(!lists_id(rec, "vst3-file:SonoraOS tests/Sonora Test State/Link.vstpreset"));

    // Unverified 3, for the test plugin: the program selected through its
    // program-change parameter, a few blocks, then getState, is that
    // program's state.
    const pr::PresetRead prog = read(vst3_bundle, "vst3-program:0/2", kVstState, pr::Outcome::Ok);
    CHECK(prog.name == "Count 2000");
    {
        const pc::StateBlob b = blob_of(prog);
        CHECK(b.format == pc::Format::Vst3 && b.id == kVstStateHex && b.sections.size() == 2);
        int64_t n = -1;
        if (!b.sections.empty() && b.sections[0].size() == 12 && b.sections[0].compare(0, 4, "SNTS") == 0)
            std::memcpy(&n, b.sections[0].data() + 4, sizeof n);
        if (n < 2000 || n >= 2100) std::printf("  program 2's count is %lld\n", static_cast<long long>(n));
        CHECK(n >= 2000 && n < 2100);
        bool has_program = false, has_unused = false;
        for (const pr::PresetValue& v : prog.params) {
            has_program = has_program || v.key == "1";
            has_unused = has_unused || v.key == "0";
        }
        CHECK(!has_program && has_unused);
    }
    read(vst3_bundle, "vst3-program:0/7", kVstState, pr::Outcome::NotInstalled, "no such program");
    read(vst3_bundle, "vst3-program:0", kVstState, pr::Outcome::Refused, "not <unit id>/<program index>");
    read(vst3_bundle, "vst3-program:0/x", kVstState, pr::Outcome::Refused, "not <unit id>/<program index>");

    // A .vstpreset: its chunks, byte for byte, once the plugin took them.
    const pr::PresetRead warm =
        read(vst3_bundle, "vst3-file:SonoraOS tests/Sonora Test State/Warm.vstpreset", kVstState, pr::Outcome::Ok);
    {
        const pc::StateBlob b = blob_of(warm);
        CHECK(warm.name == "Warm" && b.sections.size() == 2 && !b.sections.empty() && b.sections[0] == snts(42) &&
              b.sections[1].empty());
    }
    read(vst3_bundle, "vst3-file:SonoraOS tests/Sonora Test State/Strings/Other.vstpreset", kVstState,
         pr::Outcome::Refused, "another plugin's");
    read(vst3_bundle, "vst3-file:SonoraOS tests/Sonora Test State/Cut.vstpreset", kVstState, pr::Outcome::Refused,
         "outside its data");
    read(vst3_bundle, "vst3-file:SonoraOS tests/Sonora Test State/Big.vstpreset", kVstState, pr::Outcome::Refused,
         "a preset may be");
    read(vst3_bundle, "vst3-file:SonoraOS tests/Sonora Test State/Refused.vstpreset", kVstState,
         pr::Outcome::Refused, "refused it");
    read(vst3_bundle, "vst3-file:../outside.vstpreset", kVstState, pr::Outcome::NotInstalled, "leaves");
    read(vst3_bundle, "vst3-file:SonoraOS tests/Sonora Test State/Link.vstpreset", kVstState,
         pr::Outcome::NotInstalled, "leads out");
    read(vst3_bundle, "vst3-file:SonoraOS tests/Sonora Test State/None.vstpreset", kVstState,
         pr::Outcome::NotInstalled, "no VST3 preset folder holds");
    read(vst3_bundle, "lv2:urn:x", kVstState, pr::Outcome::Refused, "not a preset id");
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--host") host = argv[i + 1];
        else if (k == "--lv2-bundle") lv2_bundle = argv[i + 1];
        else if (k == "--vst3-bundle") vst3_bundle = argv[i + 1];
    }
    if (host.empty() || lv2_bundle.empty()) {
        std::fprintf(stderr, "usage: test_read_preset --host PATH --lv2-bundle DIR [--vst3-bundle DIR]\n");
        return 2;
    }
    std::string templ = (fs::temp_directory_path() / "sonora-read-preset-XXXXXX").string();
    if (!mkdtemp(templ.data())) {
        std::perror("mkdtemp");
        return 2;
    }
    tmp = templ;
    fs::create_directories(tmp / "home");
    // The children's $HOME: ~/.vst3/presets is the one this test wrote.
    setenv("HOME", (tmp / "home").c_str(), 1);
    lv2();
    if (!vst3_bundle.empty()) vst3();
    else std::printf("test_read_preset: built without VST3, its half not run\n");
    std::error_code ec;
    fs::remove_all(tmp, ec);
    if (failures) {
        std::printf("test_read_preset: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_read_preset: ok\n");
    return 0;
}
