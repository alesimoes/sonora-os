// SPDX-License-Identifier: GPL-3.0-or-later
//
// factory_preset_reader (ADR-0029 §5, slice 3): one-shot --read-preset
// children against a fake host (fake_plugin_host.cpp), so every rule of
// factory_preset_reader.h is tested where no plugin SDK is installed.

#include "sonora/factory_preset_reader.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

#include "sonora/plugin/host_control.h"

namespace pr = sonora::plugin::probe;
namespace pc = sonora::plugin::control;
using sonora::FactoryPresetReader;
using sonora::FactoryPresetReaderConfig;

namespace {

int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

std::string host;
std::string dir;

// Children the fake host logged for `preset`.
int runs_of(const std::string& preset) {
    std::ifstream f(dir + "/log");
    int n = 0;
    for (std::string line; std::getline(f, line);)
        if (line.find("--read-preset") != std::string::npos && line.find(" " + preset + " ") != std::string::npos) ++n;
    return n;
}

bool says(const pr::PresetRead& r, pr::Outcome o, const char* what) {
    const bool ok = r.outcome == o && r.reason.find(what) != std::string::npos;
    if (!ok) std::printf("  wanted outcome %d saying \"%s\", got %d \"%s\"\n", int(o), what, int(r.outcome), r.reason.c_str());
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--host") host = argv[i + 1];
    if (host.empty()) {
        std::fprintf(stderr, "usage: test_factory_preset_reader --host FAKE_HOST\n");
        return 2;
    }
    char templ[] = "/tmp/sonora-preset-reader-XXXXXX";
    if (!mkdtemp(templ)) return 2;
    dir = templ;
    const std::string lv2 = "urn:fake:amp";
    const std::string vst3 = "vst3:534F4E4F524156535433544553540009";
    std::ofstream(dir + "/world") << "preset " << lv2 << " lv2:urn:p#ok ok\n"
                                  << "preset " << vst3 << " vst3-program:0/1 ok\n"
                                  << "preset " << lv2 << " lv2:urn:p#refuse refuse\n"
                                  << "preset " << lv2 << " lv2:urn:p#crash crash\n"
                                  << "preset " << lv2 << " lv2:urn:p#hang hang\n"
                                  << "preset " << lv2 << " lv2:urn:p#garbage garbage\n"
                                  << "preset " << lv2 << " lv2:urn:p#liar liar\n"
                                  << "preset " << lv2 << " lv2:urn:p#term term\n"
                                  << "preset " << lv2 << " lv2:urn:p#blocked blocked\n";
    setenv("SONORA_FAKE_HOST_WORLD", (dir + "/world").c_str(), 1);
    setenv("SONORA_FAKE_HOST_LOG", (dir + "/log").c_str(), 1);

    FactoryPresetReaderConfig cfg;
    cfg.host_path = host;
    cfg.bundles = {"/extra.lv2"};
    cfg.scratch_dir = dir;
    cfg.timeout_ms = 1500;
    FactoryPresetReader reader(cfg);

    // A preset read is the host's answer, passed on.
    pr::PresetRead r = reader.read("/b.lv2", lv2, "lv2:urn:p#ok");
    CHECK(r.ok() && r.name == "Fake lv2:urn:p#ok" && r.params.size() == 1);
    {
        pc::StateBlob b;
        std::string e;
        CHECK(pc::decode_state_blob(r.state, b, e) && b.id == lv2 && b.sections.size() == 1);
    }
    r = reader.read("/b.vst3", vst3, "vst3-program:0/1");
    CHECK(r.ok());
    // The child ran at a low priority, given the bundles and the engine's pid.
    {
        std::ifstream f(dir + "/log");
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string log = ss.str();
        CHECK(log.find("--read-preset /b.lv2 lv2:urn:p#ok --plugin urn:fake:amp --bundle /extra.lv2") !=
              std::string::npos);
        CHECK(log.find("nice=10") != std::string::npos);
    }
    // Refusals and absences are answers, and not remembered.
    CHECK(says(reader.read("/b.lv2", lv2, "lv2:urn:p#refuse"), pr::Outcome::Refused, "refused it"));
    CHECK(says(reader.read("/b.lv2", lv2, "lv2:urn:p#refuse"), pr::Outcome::Refused, "refused it"));
    CHECK(runs_of("lv2:urn:p#refuse") == 2);
    CHECK(says(reader.read("/b.lv2", lv2, "lv2:urn:p#none"), pr::Outcome::NotInstalled, "no such preset"));
    CHECK(reader.blocked() == 0);

    // A crash is reported, and the preset is not read again until a rescan.
    CHECK(says(reader.read("/b.lv2", lv2, "lv2:urn:p#crash"), pr::Outcome::Blocked, "killed by signal 9"));
    CHECK(says(reader.read("/b.lv2", lv2, "lv2:urn:p#crash"), pr::Outcome::Blocked, "not read again"));
    CHECK(runs_of("lv2:urn:p#crash") == 1);
    // A hang is killed at the timeout, the same way.
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(says(reader.read("/b.lv2", lv2, "lv2:urn:p#hang"), pr::Outcome::Blocked, "did not answer within 1500 ms"));
    const auto took = std::chrono::steady_clock::now() - t0;
    CHECK(took >= std::chrono::milliseconds(1400) && took < std::chrono::seconds(10));
    CHECK(says(reader.read("/b.lv2", lv2, "lv2:urn:p#hang"), pr::Outcome::Blocked, "not read again"));
    CHECK(runs_of("lv2:urn:p#hang") == 1);
    // Garbage, another plugin's state, an outcome only the engine gives.
    CHECK(says(reader.read("/b.lv2", lv2, "lv2:urn:p#garbage"), pr::Outcome::Blocked, "without a preset"));
    CHECK(says(reader.read("/b.lv2", lv2, "lv2:urn:p#liar"), pr::Outcome::Blocked, "urn:someone:else's state"));
    CHECK(says(reader.read("/b.lv2", lv2, "lv2:urn:p#blocked"), pr::Outcome::Blocked, "only the engine says"));
    CHECK(reader.blocked() == 5);
    // Ended by a stop of ours: blocked this once, tried again next time.
    CHECK(says(reader.read("/b.lv2", lv2, "lv2:urn:p#term"), pr::Outcome::Blocked, "killed by signal 15"));
    CHECK(says(reader.read("/b.lv2", lv2, "lv2:urn:p#term"), pr::Outcome::Blocked, "killed by signal 15"));
    CHECK(runs_of("lv2:urn:p#term") == 2);
    CHECK(reader.blocked() == 5);

    // A rescan forgets: the crashing preset is read, and crashes, once more.
    reader.forget_blocked();
    CHECK(says(reader.read("/b.lv2", lv2, "lv2:urn:p#crash"), pr::Outcome::Blocked, "killed by signal 9"));
    CHECK(runs_of("lv2:urn:p#crash") == 2);

    // A host that is not there.
    FactoryPresetReaderConfig none = cfg;
    none.host_path = dir + "/no-such-host";
    FactoryPresetReader absent(none);
    const pr::PresetRead n = absent.read("/b.lv2", lv2, "lv2:urn:p#ok");
    CHECK(n.outcome == pr::Outcome::Blocked && !n.reason.empty());

    std::string cmd = "rm -rf '" + dir + "'";
    (void)std::system(cmd.c_str());
    if (failures) {
        std::printf("test_factory_preset_reader: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_factory_preset_reader: ok\n");
    return 0;
}
