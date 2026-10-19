// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plugin scan worker and its cache (ADR-0012, amendment of 2026-10-19,
// items 1 to 5 with changes A to E), against fake_plugin_host: every rule
// is checked here, where no plugin SDK is installed.
//
//   catalog_channels   2 when a plugin can run stereo, else its own count
//   cold_then_warm     a cold scan probes each plugin once, refused and
//                      crashed ones in the catalog unusable with the reason;
//                      a warm start probes nothing, a crashed one stays
//                      blocked, the catalog is the same generation
//   changed_ctime      a file rewritten with its mtime set back (cp -p,
//                      touch -d) is a changed bundle, through a symlink too;
//                      only that bundle is probed again
//   cache_header       another image build, another host line, an unreadable
//                      or an over-cap file: discarded whole, said once
//   bundle_gone        a bundle no longer installed leaves the catalog
//   rescan             blocked plugins are probed again, nothing else
//   recording          no catalog child starts during a take; chain-start
//                      probes do; the scan resumes when the take ends
//   reserved_slot      a chain-start probe starts while the catalog fills
//                      every other slot
//   dedup              one probe for one (id, channels), however often asked
//   shutdown           the destructor kills and reaps a hung probe, and it
//                      is never cached as blocked
//   timeout            a probe that does not answer is cached blocked
//   kept_on_busy_lock  a request made while the exchange lock is busy goes
//                      at the next turn
//   priority           every child at nice 10 and the idle I/O class
//   version            a host of another record version disables the scan
//   vst3_list_fail     a VST3 bundle whose scan crashed is said, and not
//                      enumerated again while it is unchanged
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "sonora/api/wire.h"
#include "sonora/plugin_catalog_cache.h"
#include "sonora/plugin_scanner.h"

using namespace sonora;
namespace fs = std::filesystem;
namespace pr = sonora::plugin::probe;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::string g_host;

uint64_t now_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

struct Fixture {
    std::string root, world, log, cache;
    explicit Fixture(const std::string& name) {
        root = fs::absolute("test_plugin_scanner_" + name).string();
        fs::remove_all(root);
        fs::create_directories(root);
        world = root + "/world";
        log = root + "/log";
        cache = root + "/cache/plugin-catalog.json";
        ::setenv("SONORA_FAKE_HOST_WORLD", world.c_str(), 1);
        ::setenv("SONORA_FAKE_HOST_LOG", log.c_str(), 1);
        set_world({});
    }
    ~Fixture() { fs::remove_all(root); }

    // A bundle directory with one binary in it, as a VST3's Contents/.
    std::string bundle(const std::string& name, const std::string& bytes = "binary") {
        const std::string b = root + "/" + name;
        fs::create_directories(b + "/Contents");
        std::ofstream(b + "/Contents/plugin.so") << bytes;
        return b;
    }
    void set_world(const std::vector<std::string>& lines) {
        std::ofstream f(world);
        for (const std::string& l : lines) f << l << "\n";
    }
    std::vector<std::string> log_lines(const std::string& has = "") const {
        std::vector<std::string> out;
        std::ifstream f(log);
        for (std::string l; std::getline(f, l);)
            if (has.empty() || l.find(has) != std::string::npos) out.push_back(l);
        return out;
    }
    void clear_log() const { std::ofstream(log, std::ios::trunc); }

    PluginScannerConfig config(bool with_cache = true) const {
        PluginScannerConfig c;
        c.host_path = g_host;
        c.cache_path = with_cache ? cache : std::string();
        c.build_id = "build-1";
        c.scratch_dir = root;
        c.max_probes = 2;
        c.probe_timeout_ms = 20000;
        return c;
    }
};

// The control thread's loop, in miniature: one exchange per 5 ms turn.
bool until(PluginScanner& s, const std::function<bool()>& pred, uint64_t ms, bool recording = false) {
    const uint64_t end = now_ms() + ms;
    while (now_ms() < end) {
        s.exchange(recording);
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    s.exchange(recording);
    return pred();
}

bool scanned(PluginScanner& s, uint64_t ms = 10000) {
    return until(s, [&] { return s.catalog()->scanned && s.idle(); }, ms);
}

const api::PluginInfo* entry(PluginScanner& s, const std::string& id) {
    for (const api::PluginInfo& p : s.catalog()->api.plugins)
        if (p.id == id) return &p;
    return nullptr;
}

std::string api_bytes(PluginScanner& s) {
    api::PluginCatalog c = s.catalog()->api;
    c.generation = 0;
    std::string out, e;
    api::wire::encode(c, out, e);
    return out;
}

std::vector<std::string> notices(PluginScanner& s) {
    s.exchange(false);
    return s.take_notices();
}

bool any_says(const std::vector<std::string>& v, const std::string& what) {
    for (const std::string& n : v)
        if (n.find(what) != std::string::npos) return true;
    return false;
}

// ---- the tests

void test_catalog_channels() {
    std::printf("catalog_channels\n");
    check(catalog_channels(1, 1) == 2, "mono in and out: 2, replicated");
    check(catalog_channels(2, 2) == 2, "stereo: 2");
    check(catalog_channels(6, 6) == 6, "six in, six out: its own");
    check(catalog_channels(1, 2) == 2, "1 in 2 out: probed at 2 and refused there");
    check(catalog_channels(0, 0) == 1, "no audio: 1");
}

struct World3 {
    std::string a, b, c, v;  // bundles
};

World3 three(Fixture& fx) {
    World3 w{fx.bundle("a.lv2"), fx.bundle("b.lv2"), fx.bundle("c.vst3"), ""};
    fx.set_world({"plugin urn:a " + w.a + " lv2 1 1 ok", "plugin urn:b " + w.b + " lv2 2 2 refuse",
                  "plugin vst3:C0000000000000000000000000000000 " + w.c + " vst3 2 2 crash"});
    return w;
}

void test_cold_then_warm() {
    std::printf("cold_then_warm\n");
    Fixture fx("cold");
    const World3 w = three(fx);
    std::string first;
    uint64_t first_gen = 0;
    {
        PluginScanner s(fx.config());
        check(scanned(s), "the first scan completes");
        check(s.probes_started() == 3, "three plugins, three probes (" + std::to_string(s.probes_started()) + ")");
        check(fx.log_lines("--probe urn:a --channels 2").size() == 1, "a mono plugin is probed at 2 (item 1)");
        const api::PluginInfo* a = entry(s, "urn:a");
        const api::PluginInfo* b = entry(s, "urn:b");
        const api::PluginInfo* c = entry(s, "C0000000000000000000000000000000");
        check(a && a->usable && a->name == "Fake urn:a" && a->vendor == "Sonora tests" && a->params.size() == 1 &&
                  a->params[0].unit == "dB",
              "usable, with the record's words and parameters");
        check(b && !b->usable && b->refusal.find("wrote outside") != std::string::npos,
              "a refused plugin is in the catalog, unusable, with the reason");
        check(c && !c->usable && c->refusal.find("killed by signal 9") != std::string::npos && c->format == api::PluginFormat::Vst3,
              "a crashed probe is in the catalog, blocked, with the reason, its id without vst3:");
        const auto vst3_lists = fx.log_lines("--format vst3");
        check(vst3_lists.size() == 1 && vst3_lists[0].find("--only-given --bundle " + w.c) != std::string::npos,
              "VST3 enumerated for its changed bundle only");
        first = api_bytes(s);
        first_gen = s.catalog()->generation;
    }
    check(fs::exists(fx.cache), "the cache is written");
    bool tmp_left = false;
    for (const auto& e : fs::directory_iterator(fs::path(fx.cache).parent_path()))
        tmp_left = tmp_left || e.path().filename().string().find(".tmp-") != std::string::npos;
    check(!tmp_left, "no temporary file is left beside it");
    fx.clear_log();
    {
        PluginScanner s(fx.config());
        check(until(s, [&] { return s.catalog()->ready; }, 5000) && s.catalog()->generation == 1 && api_bytes(s) == first,
              "a warm start publishes the cached catalog at once (generation 1)");
        check(scanned(s), "the warm scan completes");
        check(s.probes_started() == 0, "a warm start probes nothing (" + std::to_string(s.probes_started()) + ")");
        check(fx.log_lines("--list").size() == 1 && fx.log_lines("--bundles-only").size() == 1,
              "only the bundle listing runs, which runs no plugin code");
        check(s.catalog()->generation == 1 && api_bytes(s) == first,
              "the same result keeps the generation (no new catalog sent)");
        (void)first_gen;
    }
}

void test_changed_ctime() {
    std::printf("changed_ctime\n");
    Fixture fx("ctime");
    const World3 w = three(fx);
    // A bundle whose binary is a symlink to outside it (yabridge).
    const std::string outside = fx.root + "/wine-prefix-plugin.so";
    std::ofstream(outside) << "wine";
    const std::string y = fx.root + "/y.vst3";
    fs::create_directories(y + "/Contents");
    fs::create_symlink(outside, y + "/Contents/y.so");
    fx.set_world({"plugin urn:a " + w.a + " lv2 1 1 ok", "plugin urn:b " + w.b + " lv2 2 2 ok",
                  "plugin vst3:Y0000000000000000000000000000000 " + y + " vst3 2 2 ok"});
    {
        PluginScanner s(fx.config());
        check(scanned(s) && s.probes_started() == 3, "cold scan");
    }
    // Rewritten in place, same size, mtime put back (cp -p, rsync -t):
    // only the ctime says so.
    const auto rewrite = [](const std::string& path, const std::string& bytes) {
        struct stat st {};
        ::stat(path.c_str(), &st);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::ofstream(path, std::ios::trunc) << bytes;
        std::system(("touch -m -d @" + std::to_string(st.st_mtim.tv_sec) + "." +
                     std::to_string(st.st_mtim.tv_nsec) + " '" + path + "'")
                        .c_str());
    };
    rewrite(w.a + "/Contents/plugin.so", "BINARY");
    rewrite(outside, "WINE");
    fx.clear_log();
    {
        PluginScanner s(fx.config());
        check(scanned(s), "the scan completes");
        check(s.probes_started() == 2, "two probes (" + std::to_string(s.probes_started()) + ")");
        check(fx.log_lines("--probe urn:a").size() == 1, "the bundle rewritten with its mtime set back is probed again");
        check(fx.log_lines("--probe vst3:Y").size() == 1, "so is the one whose symlink target changed (change B)");
        check(fx.log_lines("--probe urn:b").empty(), "the unchanged bundle is not");
    }
}

void test_cache_header() {
    std::printf("cache_header\n");
    Fixture fx("header");
    three(fx);
    {
        PluginScanner s(fx.config());
        check(scanned(s), "cold scan");
    }
    const auto discarded = [&](const PluginScannerConfig& c, const std::string& says) {
        fx.clear_log();
        PluginScanner s(c);
        const bool done = scanned(s);
        const auto n = notices(s);
        const bool full = s.probes_started() == 3;
        const bool said = any_says(n, says) && any_says(n, "every plug-in is scanned again");
        if (!said)
            for (const std::string& t : n) std::printf("    notice: %s\n", t.c_str());
        return done && full && said;
    };
    PluginScannerConfig c = fx.config();
    c.build_id = "build-2";
    check(discarded(c, "another image build"), "another image build: discarded, a full scan, said (change B)");
    // Another host line: the world's version changes.
    std::string world;
    {
        std::ifstream f(fx.world);
        std::ostringstream ss;
        ss << f.rdbuf();
        world = ss.str();
    }
    std::ofstream(fx.world, std::ios::trunc) << "version sonora-plugin-host layout 1 clock 1 wire 1 probe 1 build 2\n"
                                             << world;
    check(discarded(fx.config(), "another plug-in host"), "another host line: discarded, a full scan");
    std::ofstream(fx.cache, std::ios::trunc) << "{not json";
    check(discarded(fx.config(), "not readable JSON"), "an unreadable file: discarded");
    {
        std::ofstream f(fx.cache, std::ios::trunc);
        f << std::string(kMaxPluginCacheBytes + 1, ' ');
    }
    check(discarded(fx.config(), "MiB cap"), "a file over 32 MiB: discarded");
}

void test_bundle_gone() {
    std::printf("bundle_gone\n");
    Fixture fx("gone");
    const World3 w = three(fx);
    uint64_t gen = 0;
    {
        PluginScanner s(fx.config());
        check(scanned(s) && entry(s, "urn:b"), "cold scan has b");
        gen = s.catalog()->generation;
    }
    fs::remove_all(w.b);
    fx.set_world({"plugin urn:a " + w.a + " lv2 1 1 ok",
                  "plugin vst3:C0000000000000000000000000000000 " + w.c + " vst3 2 2 crash"});
    PluginScanner s(fx.config());
    check(until(s, [&] { return s.catalog()->ready; }, 5000) && !entry(s, "urn:b"),
          "a cached bundle that is gone is not published at start");
    check(scanned(s) && !entry(s, "urn:b") && entry(s, "urn:a"), "nor after the scan; the others stay");
    check(s.catalog()->generation >= 1 && s.probes_started() == 0, "and nothing is probed for it");
    (void)gen;
}

void test_rescan() {
    std::printf("rescan\n");
    Fixture fx("rescan");
    three(fx);
    PluginScanner s(fx.config());
    check(scanned(s) && s.probes_started() == 3, "cold scan");
    fx.clear_log();
    s.rescan();
    check(until(s, [&] { return s.probes_started() == 4 && s.idle(); }, 10000), "a rescan runs one probe");
    check(fx.log_lines("--probe vst3:C").size() == 1 && fx.log_lines("--probe urn:").empty(),
          "the blocked plugin, and only it, is probed again");
}

void test_recording() {
    std::printf("recording\n");
    Fixture fx("recording");
    const std::string a = fx.bundle("a.lv2"), b = fx.bundle("b.lv2");
    fx.set_world({"plugin urn:a " + a + " lv2 1 1 sleep 300", "plugin urn:b " + b + " lv2 1 1 sleep 300",
                  "plugin urn:c " + b + " lv2 1 1 sleep 300", "plugin urn:x " + b + " lv2 1 1 ok"});
    PluginScanner s(fx.config(false));
    // The take is on before the worker's first turn: the start-up scan
    // waits (change D, Q3).
    s.exchange(true);
    until(s, [] { return false; }, 600, true);
    check(s.lists_started() == 0 && s.catalog_probes_started() == 0, "no catalog child starts during a take");
    s.probe("urn:x", 1);
    check(until(s, [&] { return s.catalog()->find("urn:x", 1) != nullptr; }, 5000, true),
          "a chain-start probe runs during a take");
    check(until(s, [&] { return s.catalog_probes_started() >= 1; }, 5000, false), "the take ends: the scan starts");
    // A take again, mid-scan: the probe in flight finishes, no new one. The
    // count is read once the worker has had a turn to see the take.
    s.exchange(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const uint64_t started = s.catalog_probes_started();
    until(s, [] { return false; }, 900, true);
    check(s.catalog_probes_started() == started, "paused mid-scan while recording (" +
                                                     std::to_string(s.catalog_probes_started()) + " started)");
    check(until(s, [&] { return s.catalog()->scanned && s.idle(); }, 10000, false), "and resumes after the take");
}

void test_reserved_slot() {
    std::printf("reserved_slot\n");
    Fixture fx("slot");
    const std::string a = fx.bundle("a.lv2");
    fx.set_world({"plugin urn:s1 " + a + " lv2 1 1 sleep 1500", "plugin urn:s2 " + a + " lv2 1 1 sleep 1500",
                  "plugin urn:s3 " + a + " lv2 1 1 sleep 1500", "plugin urn:x " + a + " lv2 1 1 ok"});
    PluginScanner s(fx.config(false));
    check(s.slots() == 2, "two slots");
    check(until(s, [&] { return s.catalog_probes_started() >= 1; }, 5000), "the catalog scan is probing");
    const uint64_t t0 = now_ms();
    s.probe("urn:x", 1);
    check(until(s, [&] { return s.catalog()->find("urn:x", 1) != nullptr; }, 5000), "the chain-start probe answers");
    check(now_ms() - t0 < 1200, "before the catalog's probe frees its slot (" + std::to_string(now_ms() - t0) + " ms)");
    check(s.peak_children() <= 2, "never more children than slots");
}

void test_dedup() {
    std::printf("dedup\n");
    Fixture fx("dedup");
    const std::string a = fx.bundle("a.lv2");
    fx.set_world({"plugin urn:s " + a + " lv2 1 1 sleep 600"});
    PluginScanner s(fx.config(false));
    check(until(s, [&] { return s.catalog_probes_started() >= 1; }, 5000), "the catalog probe of urn:s runs");
    for (int i = 0; i < 5; ++i) s.probe("urn:s", 2);
    check(until(s, [&] { return s.catalog()->find("urn:s", 2) != nullptr && s.idle(); }, 5000), "answered");
    check(fx.log_lines("--probe urn:s --channels 2").size() == 1, "one probe for one (id, channels)");
}

void test_shutdown() {
    std::printf("shutdown\n");
    Fixture fx("shutdown");
    const std::string a = fx.bundle("a.lv2");
    fx.set_world({"plugin urn:h " + a + " lv2 1 1 hang"});
    long pid = 0;
    uint64_t took = 0;
    {
        auto s = std::make_unique<PluginScanner>(fx.config());
        check(until(*s, [&] { return !fx.log_lines("--probe urn:h").empty(); }, 5000), "the probe hangs");
        const std::string line = fx.log_lines("--probe urn:h").front();
        pid = std::atol(line.c_str() + line.find("pid=") + 4);
        const uint64_t t0 = now_ms();
        s.reset();
        took = now_ms() - t0;
    }
    check(took < 3000, "the destructor returns at once (" + std::to_string(took) + " ms)");
    check(pid > 0 && ::kill(pid_t(pid), 0) != 0, "the probe was killed and reaped (change C)");
    fx.clear_log();
    PluginScanner s(fx.config());
    check(until(s, [&] { return !fx.log_lines("--probe urn:h").empty(); }, 5000),
          "the next start probes it again: our own shutdown is never cached as blocked");
}

void test_timeout() {
    std::printf("timeout\n");
    Fixture fx("timeout");
    const std::string a = fx.bundle("a.lv2");
    fx.set_world({"plugin urn:h " + a + " lv2 1 1 hang"});
    PluginScannerConfig c = fx.config();
    c.probe_timeout_ms = 500;
    {
        PluginScanner s(c);
        check(scanned(s), "the scan completes past the hung probe");
        const api::PluginInfo* h = entry(s, "urn:h");
        check(h && !h->usable && h->refusal.find("did not answer within 500 ms") != std::string::npos,
              "cached blocked, with the reason");
    }
    fx.clear_log();
    PluginScanner s(c);
    check(scanned(s) && s.probes_started() == 0, "and not run again at the next boot (item 4)");
}

void test_kept_on_busy_lock() {
    std::printf("kept_on_busy_lock\n");
    Fixture fx("lock");
    const std::string a = fx.bundle("a.lv2");
    fx.set_world({"plugin urn:x " + a + " lv2 1 1 ok"});
    PluginScannerConfig c = fx.config(false);
    c.scan_at_start = false;
    PluginScanner s(c);
    check(until(s, [&] { return s.catalog()->ready; }, 5000), "ready");
    std::mutex m;
    std::condition_variable cv;
    bool held = false, release = false;
    std::thread holder([&] {
        std::lock_guard<std::mutex> l(s.exchange_mutex_for_test());
        std::unique_lock<std::mutex> g(m);
        held = true;
        cv.notify_all();
        cv.wait(g, [&] { return release; });
    });
    {
        std::unique_lock<std::mutex> g(m);
        cv.wait(g, [&] { return held; });
    }
    s.probe("urn:x", 1);
    check(!s.exchange(false), "the exchange does not wait for a busy lock");
    {
        std::lock_guard<std::mutex> g(m);
        release = true;
    }
    cv.notify_all();
    holder.join();
    check(until(s, [&] { return s.catalog()->find("urn:x", 1) != nullptr; }, 5000),
          "the request made then goes at the next turn (change C)");
}

void test_priority() {
    std::printf("priority\n");
    Fixture fx("priority");
    three(fx);
    PluginScanner s(fx.config(false));
    check(scanned(s), "scanned");
    bool all = true;
    std::size_t n = 0;
    for (const std::string& l : fx.log_lines()) {
        if (l.rfind("--version", 0) == 0) continue;
        ++n;
        all = all && l.find(" nice=10 ") != std::string::npos;
#ifdef __linux__
        all = all && l.find(" io=3") != std::string::npos;
#endif
    }
    check(n >= 5 && all, "every list and probe child runs at nice 10 and the idle I/O class (" + std::to_string(n) + ")");
}

void test_version() {
    std::printf("version\n");
    Fixture fx("version");
    const std::string a = fx.bundle("a.lv2");
    fx.set_world({"version sonora-plugin-host layout 1 clock 1 wire 1 probe 2", "plugin urn:x " + a + " lv2 1 1 ok"});
    PluginScanner s(fx.config(false));
    check(scanned(s), "the scan ends at once");
    check(s.catalog()->disabled.find("probe record 2") != std::string::npos, "disabled, saying why");
    check(any_says(notices(s), "probe record 2"), "and said");
    s.probe("urn:x", 1);
    check(until(s, [&] { return s.catalog()->find("urn:x", 1) != nullptr; }, 5000) &&
              !s.catalog()->find("urn:x", 1)->ok(),
          "a chain-start probe is refused, never guessed at");
    check(s.probes_started() == 0 && s.lists_started() == 0, "nothing ran");
}

void test_vst3_list_fail() {
    std::printf("vst3_list_fail\n");
    Fixture fx("listfail");
    const std::string a = fx.bundle("a.lv2"), v = fx.bundle("v.vst3");
    fx.set_world({"plugin urn:a " + a + " lv2 1 1 ok", "plugin vst3:V0000000000000000000000000000000 " + v + " vst3 2 2 ok",
                  "listfail " + v + " the scan crashed (signal 11, Segmentation fault)"});
    {
        PluginScanner s(fx.config());
        check(scanned(s), "scanned");
        check(any_says(notices(s), "could not be scanned: the scan crashed"), "the bundle is said");
        check(entry(s, "urn:a") && !entry(s, "V0000000000000000000000000000000"), "the rest is in the catalog");
    }
    fx.clear_log();
    PluginScanner s(fx.config());
    check(scanned(s) && fx.log_lines("--format vst3").empty(), "not enumerated again while unchanged");
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--host") g_host = argv[i + 1];
    if (g_host.empty()) {
        std::printf("usage: test_plugin_scanner --host FAKE_HOST\n");
        return 2;
    }
    g_host = fs::absolute(g_host).string();
    test_catalog_channels();
    test_cold_then_warm();
    test_changed_ctime();
    test_cache_header();
    test_bundle_gone();
    test_rescan();
    test_recording();
    test_reserved_slot();
    test_dedup();
    test_shutdown();
    test_timeout();
    test_kept_on_busy_lock();
    test_priority();
    test_version();
    test_vst3_list_fail();
    if (failures) {
        std::printf("test_plugin_scanner: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_plugin_scanner: ok\n");
    return 0;
}
