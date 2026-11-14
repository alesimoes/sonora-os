// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control server owns one FactoryPresetReader, configured like its scan
// worker, and plugin.rescan lets a preset that crashed its reader be read
// again (ADR-0144, amendment of 2026-11-13).
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "sonora/api/engine_control.h"
#include "sonora/engine_control_in_process.h"
#include "sonora/factory_preset_reader.h"
#include "sonora/fake_backend.h"
#include "sonora/session.h"

using namespace sonora;
namespace fs = std::filesystem;

namespace {
int failures = 0;
#define CHECK(c) \
    do { \
        if (!(c)) { \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            ++failures; \
        } \
    } while (0)

int runs(const std::string& log, const std::string& preset) {
    std::ifstream f(log);
    int n = 0;
    for (std::string line; std::getline(f, line);)
        if (line.find("--read-preset") != std::string::npos && line.find(" " + preset + " ") != std::string::npos) ++n;
    return n;
}
}  // namespace

int main(int argc, char** argv) {
    std::string host;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--host") host = argv[i + 1];
    if (host.empty()) return 2;

    const std::string root = fs::absolute("test_plugin_rescan_presets").string();
    fs::remove_all(root);
    fs::create_directories(root + "/b.lv2");
    std::ofstream(root + "/b.lv2/plugin.so") << "x";
    const std::string lv2 = "urn:fake:amp";
    const std::string crash = "lv2:urn:p#crash";
    std::ofstream(root + "/world") << "plugin urn:mono " << root << "/b.lv2 lv2 1 1 ok\n"
                                   << "preset " << lv2 << " " << crash << " crash\n";
    ::setenv("SONORA_FAKE_HOST_WORLD", (root + "/world").c_str(), 1);
    ::setenv("SONORA_FAKE_HOST_LOG", (root + "/log").c_str(), 1);

    SessionConfig cfg;
    cfg.dir = root + "/session";
    cfg.track_count = 1;
    Session session(cfg);
    session.create_dirs();
    EngineControlServer::Options opts;
    opts.plugin_host = host;
    InProcessEngine engine(session, nullptr, opts);
    std::string error;
    CHECK(engine.connect(error));

    FactoryPresetReader* reader = engine.server().preset_reader();
    CHECK(reader != nullptr);
    if (!reader) return 1;
    CHECK(reader->read(root + "/b.lv2", lv2, crash).outcome == plugin::probe::Outcome::Blocked);
    CHECK(reader->read(root + "/b.lv2", lv2, crash).outcome == plugin::probe::Outcome::Blocked);
    CHECK(reader->blocked() == 1 && runs(root + "/log", crash) == 1);

    api::CommandRequest r;
    r.command = "plugin.rescan";
    r.target = -1;
    const uint64_t seq = engine.send(r);
    api::Inbox in;
    bool answered = false;
    for (int i = 0; i < 100000 && !answered; ++i) {
        engine.poll(in);
        for (const auto& x : in.replies) answered = answered || x.seq == seq;
        if (!answered) ::usleep(1000);
    }
    CHECK(answered);
    CHECK(reader->blocked() == 0);
    CHECK(reader->read(root + "/b.lv2", lv2, crash).outcome == plugin::probe::Outcome::Blocked);
    CHECK(runs(root + "/log", crash) == 2);

    engine.shutdown();
    fs::remove_all(root);
    std::printf(failures ? "FAILED\n" : "ok\n");
    return failures ? 1 : 0;
}
