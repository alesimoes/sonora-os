// SPDX-License-Identifier: GPL-3.0-or-later
// The plugin scan worker (plugin_scanner.h). Its own thread: it starts and
// reaps children, reads and writes the cache and walks bundles. Nothing
// here runs on the control thread but the few calls the header names, and
// nothing on the audio thread.
#include "sonora/plugin_scanner.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <deque>
#include <fstream>
#include <random>
#include <sstream>
#include <tuple>

#include "sonora/api/wire.h"
#include "sonora/json/json.h"
#include "sonora/platform/process.h"
#include "sonora/plugin/protocol.h"
#include "sonora/plugin_catalog_cache.h"

namespace sonora {

namespace pr = plugin::probe;
using Key = PluginScanner::Key;

static_assert(uint32_t(pr::kParamAutomatable) == api::kParamAutomatable && uint32_t(pr::kParamToggle) == api::kParamToggle &&
                  uint32_t(pr::kParamEnum) == api::kParamEnum &&
                  uint32_t(pr::kParamLogarithmic) == api::kParamLogarithmic &&
                  uint32_t(pr::kParamPastCap) == api::kParamPastCap && pr::kKnownParamFlags == api::kKnownParamFlags,
              "the probe record and the catalog spell parameter flags alike");

namespace {

uint64_t steady_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

constexpr uint64_t kVersionTimeoutMs = 5000;
constexpr uint64_t kSaveEveryMs = 30000;
constexpr auto kTurn = std::chrono::milliseconds(20);
// A catalog is a copy of every record: during a cold scan it is published
// at most this often, unless a chain start waits on it.
constexpr uint64_t kPublishEveryMs = 250;

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        if (end > start) out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::string format_of(const std::string& host_id) { return host_id.rfind("vst3:", 0) == 0 ? "vst3" : "lv2"; }

bool same_key(const pr::Record& r, const Key& k) { return r.uri == k.first && r.channels == k.second; }

std::string ended(platform::ChildProcess& c) {
    return c.exited_normally() ? "exited with code " + std::to_string(c.exit_code())
                               : "was killed by signal " + std::to_string(c.signal_number());
}

}  // namespace

uint16_t catalog_channels(uint32_t in, uint32_t out) {
    if (in == out && (in == 1 || in == 2)) return 2;
    if (in == out && in > 0) return uint16_t(std::min<uint32_t>(in, 64));
    return uint16_t(std::max<uint32_t>(1, std::min<uint32_t>(std::max(in, out), 64)));
}

api::PluginInfo catalog_entry(const pr::Record& r) {
    api::PluginInfo p;
    const bool vst3 = r.format == "vst3";
    p.format = vst3 ? api::PluginFormat::Vst3 : api::PluginFormat::Lv2;
    p.id = vst3 && r.uri.rfind("vst3:", 0) == 0 ? r.uri.substr(5) : r.uri;
    p.name = r.name.empty() ? p.id : r.name;
    p.vendor = r.vendor;
    p.version = r.version;
    p.categories = r.categories;
    if (p.categories.size() > api::wire::kMaxPluginCategories) p.categories.resize(api::wire::kMaxPluginCategories);
    p.inputs = r.audio_in;
    p.outputs = r.audio_out;
    p.usable = r.ok();
    p.refusal = r.ok() ? std::string() : r.reason;
    p.has_editor = r.has_editor;
    for (const pr::Param& q : r.params) {
        if (p.params.size() >= api::wire::kMaxPluginParams) break;
        // A key past the catalog's cap could not be named by a client.
        if (q.key.size() > api::wire::kMaxParamKeyBytes) continue;
        api::PluginParam a;
        a.index = uint32_t(p.params.size());
        a.key = q.key;
        a.name = q.name;
        a.min = q.min;
        a.max = q.max;
        a.default_value = q.default_value;
        a.flags = q.flags;
        a.unit = q.unit;
        a.steps = q.steps;
        if (q.labels.size() <= api::wire::kMaxParamLabels) a.labels = q.labels;
        p.params.push_back(std::move(a));
    }
    return p;
}

// ---- the worker's own state

struct PluginScanner::Job {
    enum class Kind { Bundles, ListLv2, ListVst3, Probe };
    Kind kind = Kind::Probe;
    Key key;
    bool chain = false;      // a chain-start probe, or joined by one
    std::string bundle;      // where the enumeration found it
    std::vector<std::string> given;  // ListVst3: the bundles asked about
    std::unique_ptr<platform::ChildProcess> child;
    std::string out_path;
    uint64_t started_ms = 0;
    uint64_t timeout_ms = 0;
    bool catalog() const { return !(kind == Kind::Probe && chain); }
};

struct PluginScanner::Worker {
    PluginScanner& s;
    const PluginScannerConfig& cfg;

    std::string host_line;
    std::string disabled;
    PluginCatalogCache cache;
    std::map<Key, pr::Record> run_only;  // not installed; blocked with no bundle known
    bool dirty = false;
    uint64_t saved_ms = 0;

    std::deque<Key> chain_queue;
    struct Pending {
        Key key;
        std::string bundle;
    };
    std::deque<Pending> catalog_queue;
    std::vector<std::unique_ptr<Job>> jobs;

    enum class Phase { Idle, Bundles, Enumerate, Probing };
    Phase phase = Phase::Idle;
    bool scan_wanted = false;
    bool rescan = false;           // this scan probes blocked plugins again
    bool scanned_once = false;
    std::map<std::string, std::string> listed;           // bundle -> format, this scan
    std::set<std::string> changed_lv2, changed_vst3;
    // Bundles enumerated whose probes are not all in: the new fingerprint
    // is cached only once they are, so a scan cut short re-enumerates them.
    std::map<std::string, std::pair<BundleFingerprint, uint32_t>> settling;
    std::set<Key> probed_this_scan;

    bool stopping = false;
    bool publish_wanted = false;
    bool urgent = false;  // a chain start or a scan's end waits on the publish
    uint64_t published_ms = 0;
    std::set<Key> chain_asked;
    api::PluginCatalog api;
    std::string api_bytes;
    uint64_t generation = 0;
    uint64_t serial = 0;
    bool ready = false;

    explicit Worker(PluginScanner& scanner) : s(scanner), cfg(scanner.config_) {}

    void notice(const std::string& text) {
        std::lock_guard<std::mutex> l(s.exchange_mu_);
        s.out_notices_.push_back(text);
    }

    std::string scratch(const char* what) {
        return cfg.scratch_dir + "/sonora-scan-" + what + "-" + std::to_string(platform::current_process_id()) + "-" +
               std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()) + ".out";
    }

    // `skip_vst3`: --list --only-given scans every VST3 bundle it is given,
    // so an unchanged extra one is left out.
    void add_bundles(std::vector<std::string>& argv, bool skip_vst3 = false) const {
        for (const std::string& b : cfg.bundles) {
            if (skip_vst3 && b.size() > 5 && b.compare(b.size() - 5, 5, ".vst3") == 0) continue;
            argv.push_back("--bundle");
            argv.push_back(b);
        }
        argv.push_back("--engine-pid");
        argv.push_back(std::to_string(platform::current_process_id()));
    }

    bool spawn(Job& j, std::vector<std::string> argv) {
        platform::ChildProcess::Options o;
        o.stdout_path = j.out_path;
        o.nice = cfg.nice;
        o.idle_io = cfg.idle_io;
        // Finding 1 on #95: a Ctrl+C or a signal to the engine's group is
        // not the probe's to receive.
        o.own_group = true;
        std::string error;
        j.child = platform::ChildProcess::spawn(argv, o, error);
        if (!j.child) {
            std::remove(j.out_path.c_str());
            notice("the plug-in scan could not start " + cfg.host_path + ": " + error);
            return false;
        }
        j.started_ms = steady_ms();
        const uint32_t n = uint32_t(jobs.size()) + 1;
        if (n > s.peak_children_.load(std::memory_order_relaxed)) s.peak_children_.store(n, std::memory_order_release);
        return true;
    }

    uint32_t catalog_jobs() const {
        uint32_t n = 0;
        for (const auto& j : jobs) n += j->catalog() ? 1 : 0;
        return n;
    }

    // ---- start-up

    void check_version() {
        const std::string out = scratch("version");
        platform::ChildProcess::Options o;
        o.stdout_path = out;
        o.own_group = true;
        std::string error;
        auto child = platform::ChildProcess::spawn({cfg.host_path, "--version"}, o, error);
        if (!child) {
            disabled = "the plug-in scan is disabled: " + error;
            return;
        }
        if (!child->wait(int(kVersionTimeoutMs))) {
            child->kill();
            child->wait(-1);
            std::remove(out.c_str());
            disabled = "the plug-in scan is disabled: " + cfg.host_path + " --version did not answer";
            return;
        }
        const std::vector<std::string> l = lines_of(read_file(out));
        std::remove(out.c_str());
        host_line = l.empty() ? std::string() : l.front();
        uint32_t v = 0;
        if (!pr::parse_probe_version(host_line, v)) {
            disabled = "the plug-in scan is disabled: " + cfg.host_path + " names no probe record version (" +
                       host_line + "); this engine reads version " + std::to_string(pr::kProbeRecordVersion);
        } else if (v != pr::kProbeRecordVersion) {
            disabled = "the plug-in scan is disabled: " + cfg.host_path + " writes probe record " +
                       std::to_string(v) + "; this engine reads version " + std::to_string(pr::kProbeRecordVersion);
        }
    }

    void load() {
        cache.host = host_line;
        cache.build = cfg.build_id;
        if (cfg.cache_path.empty()) return;
        std::string why;
        switch (load_plugin_cache(cfg.cache_path, host_line, cfg.build_id, cache, why)) {
            case CacheLoad::Loaded: break;
            case CacheLoad::Missing: break;
            case CacheLoad::Discarded:
                // Said once; the scan that follows is a full one.
                notice(why + "; it is discarded and every plug-in is scanned again");
                dirty = true;
                break;
        }
        // A cached bundle whose files changed since is dropped, so no chain
        // starts from what it used to be (the scan probes it). One bundle
        // at a time, with chain-start requests served between them (finding
        // 5 on #95): a chain asking for a plugin of a bundle not yet checked
        // has that bundle checked at once.
        unvalidated.swap(cache.bundles);
        while (!unvalidated.empty() && !s.stop_.load(std::memory_order_acquire)) {
            validate(unvalidated.begin()->first);
            service();
        }
        unvalidated.clear();
    }

    // Cached bundles not yet fingerprinted again since start-up.
    std::map<std::string, CachedBundle> unvalidated;

    void validate(const std::string& path) {
        const auto it = unvalidated.find(path);
        if (it == unvalidated.end()) return;
        if (it->second.fingerprint.matches(fingerprint(path))) cache.bundles.insert(*it);
        else dirty = true;
        unvalidated.erase(it);
    }

    BundleFingerprint fingerprint(const std::string& path) const {
        if (cfg.fingerprint_delay_ms_for_test)
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.fingerprint_delay_ms_for_test));
        return fingerprint_bundle(path);
    }

    // Between the bundles of a walk (finding 5 on #95, change E): chain-start
    // requests taken, chain probes reaped and started, their answers
    // published. Nothing of the catalog scan moves here.
    void service() {
        std::vector<Key> probes;
        {
            std::lock_guard<std::mutex> l(s.exchange_mu_);
            probes.swap(s.in_probes_);
        }
        for (const Key& k : probes) enqueue_chain(k);
        reap(true);
        start_chain_probes();
        if (publish_wanted && urgent) publish();
    }

    // ---- what the control thread sees

    const pr::Record* known(const Key& k) const {
        if (auto it = run_only.find(k); it != run_only.end()) return &it->second;
        for (const auto& [path, b] : cache.bundles)
            for (const pr::Record& r : b.plugins)
                if (same_key(r, k)) return &r;
        return nullptr;
    }

    void rebuild_api() {
        // One entry per plugin: the record at its catalog channel count,
        // else the widest one known.
        std::map<std::pair<std::string, std::string>, const pr::Record*> pick;
        for (const auto& [path, b] : cache.bundles) {
            for (const pr::Record& r : b.plugins) {
                if (r.outcome == pr::Outcome::NotInstalled) continue;
                const pr::Record*& p = pick[{r.format, r.uri}];
                const uint16_t want = catalog_channels(r.audio_in, r.audio_out);
                if (!p || (r.channels == want && p->channels != want) ||
                    (p->channels != want && r.channels > p->channels))
                    p = &r;
            }
        }
        api::PluginCatalog c;
        for (const auto& [k, r] : pick) {
            if (c.plugins.size() >= api::wire::kMaxCatalogPlugins) break;
            c.plugins.push_back(catalog_entry(*r));
        }
        std::sort(c.plugins.begin(), c.plugins.end(), [](const api::PluginInfo& a, const api::PluginInfo& b) {
            return std::tie(a.name, a.id) < std::tie(b.name, b.id);
        });
        std::string bytes, e;
        c.generation = 0;
        if (!api::wire::encode(c, bytes, e)) {
            notice("the plug-in catalog cannot be sent: " + e);
            c.plugins.clear();
            api::wire::encode(c, bytes, e);
        }
        if (generation == 0 || bytes != api_bytes) {
            ++generation;
            api_bytes = std::move(bytes);
            api = std::move(c);
        }
        api.generation = generation;
        publish_wanted = true;
        urgent = true;
    }

    void publish() {
        auto c = std::make_shared<Catalog>();
        c->serial = ++serial;
        c->generation = generation;
        c->ready = ready;
        c->scanned = scanned_once;
        c->disabled = disabled;
        c->api = api;
        for (const auto& [path, b] : cache.bundles)
            for (const pr::Record& r : b.plugins) c->probes[{r.uri, uint16_t(r.channels)}] = r;
        for (const auto& [k, r] : run_only) c->probes[k] = r;
        {
            std::lock_guard<std::mutex> l(s.exchange_mu_);
            s.out_catalog_ = std::move(c);
        }
        publish_wanted = false;
        urgent = false;
        published_ms = steady_ms();
    }

    // ---- results

    void store(const Key& k, pr::Record r, const std::string& bundle_hint) {
        // Run-only: a plugin not installed has no bundle to be cached under
        // (a rescan forgets it), nor has a blocked one whose bundle no
        // enumeration named.
        if (chain_asked.erase(k)) urgent = true;
        std::string bundle = r.bundle.empty() ? bundle_hint : r.bundle;
        if (r.outcome == pr::Outcome::NotInstalled || bundle.empty()) {
            run_only[k] = std::move(r);
            publish_wanted = true;
            return;
        }
        run_only.erase(k);
        CachedBundle& b = cache.bundles[bundle];
        if (b.format.empty()) {
            b.format = r.format;
            // Found by a chain-start probe before any scan listed it.
            if (!settling.count(bundle)) b.fingerprint = fingerprint_bundle(bundle);
        }
        b.plugins.erase(std::remove_if(b.plugins.begin(), b.plugins.end(),
                                       [&](const pr::Record& x) { return same_key(x, k); }),
                        b.plugins.end());
        b.plugins.push_back(std::move(r));
        dirty = true;
        publish_wanted = true;
    }

    pr::Record blocked(const Key& k, const std::string& why) const {
        pr::Record r;
        r.host = host_line;
        r.uri = k.first;
        r.format = format_of(k.first);
        r.channels = k.second;
        r.outcome = pr::Outcome::Blocked;
        r.reason = why;
        return r;
    }

    void settle_one(const std::string& bundle) {
        const auto it = settling.find(bundle);
        if (it == settling.end()) return;
        if (it->second.second > 0) --it->second.second;
        if (it->second.second == 0) {
            cache.bundles[bundle].fingerprint = it->second.first;
            settling.erase(it);
            dirty = true;
        }
    }

    // A probe that ended because something asked processes to stop (our
    // own shutdown, a Ctrl+C, systemd's stop of the whole cgroup) said
    // nothing about the plugin (change C, finding 1 on #95).
    bool stopped_by_signal(platform::ChildProcess& c) const {
        if (c.exited_normally()) return false;
        const int sig = c.signal_number();
        return sig == SIGINT || sig == SIGTERM || sig == SIGHUP || sig == SIGQUIT;
    }

    void finish_probe(Job& j, bool timed_out) {
        pr::Record r;
        std::string why;
        const std::string out = read_file(j.out_path);
        std::remove(j.out_path.c_str());
        if (!timed_out && (s.stop_.load(std::memory_order_acquire) || stopped_by_signal(*j.child))) {
            // Never cached: refused for this run only, and its bundle left
            // unsettled, so the next start enumerates and probes it again.
            store(j.key, blocked(j.key, "its probe " + ended(*j.child) + " before it answered"), std::string());
            settling.erase(j.bundle);
            return;
        }
        if (timed_out) {
            const uint64_t t = cfg.probe_timeout_ms;
            r = blocked(j.key, "its probe did not answer within " +
                                   (t % 1000 == 0 ? std::to_string(t / 1000) + " s" : std::to_string(t) + " ms"));
        } else {
            const std::vector<std::string> l = lines_of(out);
            if (l.empty() || !pr::decode(l.front(), r, why)) {
                // A crash, or an answer that is not a record: §4's
                // "blocked", with the reason, and never run again at boot.
                r = blocked(j.key, "its probe " + ended(*j.child) + " without an answer" +
                                       (why.empty() || l.empty() ? std::string() : " (" + why + ")"));
            } else if (!same_key(r, j.key)) {
                r = blocked(j.key, "its probe answered for " + r.uri + " at " + std::to_string(r.channels) +
                                       " channels");
            }
        }
        store(j.key, std::move(r), j.bundle);
        settle_one(j.bundle);
    }

    void finish_bundles(Job& j, bool failed) {
        const std::string out = read_file(j.out_path);
        std::remove(j.out_path.c_str());
        if (failed) {
            notice("the plug-in scan could not list the installed bundles: " + cfg.host_path + " --list " +
                   (j.child->running() ? std::string("did not answer") : ended(*j.child)));
            end_scan();
            return;
        }
        listed.clear();
        for (const std::string& line : lines_of(out)) {
            json::Value v;
            json::Error e;
            if (!json::parse(line, v, e) || !v.is_object()) continue;
            const json::Value* b = v.find("bundle");
            const json::Value* f = v.find("format");
            if (!b || !b->is_string() || !f || !f->is_string() || b->text().empty()) continue;
            listed[b->text()] = f->text();
        }
        changed_lv2.clear();
        changed_vst3.clear();
        for (const auto& [path, format] : listed) {
            service();
            const BundleFingerprint fp = fingerprint(path);
            auto it = cache.bundles.find(path);
            // A rescan retries a bundle whose enumeration failed.
            if (it != cache.bundles.end() && it->second.format == format && it->second.fingerprint.matches(fp) &&
                !settling.count(path) && !(rescan && !it->second.failed.empty())) {
                // Unchanged. A rescan runs what it blocked again.
                if (rescan) {
                    for (const pr::Record& r : it->second.plugins)
                        if (r.outcome == pr::Outcome::Blocked) enqueue_catalog({r.uri, uint16_t(r.channels)}, path);
                }
                continue;
            }
            // New or changed: what it said before is gone; enumerated again.
            CachedBundle& cb = cache.bundles[path];
            cb.format = format;
            cb.failed.clear();
            cb.plugins.clear();
            cb.fingerprint = BundleFingerprint{};
            cb.fingerprint.complete = false;
            settling[path] = {fp, 0};
            (format == "vst3" ? changed_vst3 : changed_lv2).insert(path);
            dirty = true;
            publish_wanted = true;
        }
        // Bundles gone from the paths leave the catalog.
        for (auto it = cache.bundles.begin(); it != cache.bundles.end();) {
            if (listed.count(it->first)) {
                ++it;
            } else {
                settling.erase(it->first);
                it = cache.bundles.erase(it);
                dirty = true;
                publish_wanted = true;
            }
        }
        phase = Phase::Enumerate;
    }

    void finish_list(Job& j, bool failed) {
        const std::string out = read_file(j.out_path);
        std::remove(j.out_path.c_str());
        const std::set<std::string>& changed = j.kind == Job::Kind::ListLv2 ? changed_lv2 : changed_vst3;
        std::map<std::string, uint32_t> found;
        for (const std::string& line : lines_of(out)) {
            json::Value v;
            json::Error e;
            if (!json::parse(line, v, e) || !v.is_object()) continue;
            const json::Value* b = v.find("bundle");
            if (!b || !b->is_string()) continue;
            const std::string bundle = b->text();
            if (!changed.count(bundle)) continue;
            const json::Value* uri = v.find("uri");
            if (!uri || !uri->is_string()) {
                // A VST3 bundle whose scan crashed or hung: no plugin of it
                // can be named, so the bundle carries the reason, said once.
                const json::Value* reason = v.find("reason");
                CachedBundle& cb = cache.bundles[bundle];
                cb.failed = reason && reason->is_string() ? reason->text() : "it could not be scanned";
                notice("the plug-in bundle " + bundle + " could not be scanned: " + cb.failed);
                continue;
            }
            int64_t in = 0, outs = 0;
            const json::Value* ai = v.find("audio_in");
            const json::Value* ao = v.find("audio_out");
            if (ai) ai->to_int64(in);
            if (ao) ao->to_int64(outs);
            enqueue_catalog({uri->text(), catalog_channels(uint32_t(std::max<int64_t>(0, in)),
                                                           uint32_t(std::max<int64_t>(0, outs)))},
                            bundle);
            ++found[bundle];
        }
        for (const std::string& bundle : changed) {
            if (failed && !found.count(bundle)) {
                // The listing itself failed: left unsettled, enumerated
                // again by the next scan.
                settling.erase(bundle);
                continue;
            }
            auto st = settling.find(bundle);
            if (st == settling.end()) continue;
            st->second.second += found.count(bundle) ? found[bundle] : 0;
            if (st->second.second == 0) settle_one(bundle);
        }
        if (failed)
            notice("the plug-in scan could not enumerate the " +
                   std::string(j.kind == Job::Kind::ListLv2 ? "LV2" : "VST3") + " bundles: " + cfg.host_path +
                   " --list " + (j.child->running() ? std::string("did not answer") : ended(*j.child)));
    }

    // ---- queues

    bool in_flight(const Key& k) const {
        for (const auto& j : jobs)
            if (j->kind == Job::Kind::Probe && j->key == k) return true;
        return false;
    }

    void enqueue_catalog(const Key& k, const std::string& bundle) {
        if (probed_this_scan.count(k) || in_flight(k)) return;
        for (const Pending& p : catalog_queue)
            if (p.key == k) return;
        if (std::find(chain_queue.begin(), chain_queue.end(), k) != chain_queue.end()) return;
        // Cached and valid already, from a bundle not being enumerated.
        if (known(k) && !rescan && !settling.count(bundle)) return;
        catalog_queue.push_back({k, bundle});
    }

    void enqueue_chain(const Key& k) {
        for (auto it = unvalidated.begin(); it != unvalidated.end(); ++it) {
            const bool has = std::any_of(it->second.plugins.begin(), it->second.plugins.end(),
                                         [&](const pr::Record& r) { return same_key(r, k); });
            if (has) {
                validate(it->first);
                break;
            }
        }
        if (known(k)) {
            // Answered already: the next publish has it.
            publish_wanted = true;
            urgent = true;
            return;
        }
        chain_asked.insert(k);
        for (auto& j : jobs)
            if (j->kind == Job::Kind::Probe && j->key == k) {
                j->chain = true;  // joins the probe in flight
                return;
            }
        if (std::find(chain_queue.begin(), chain_queue.end(), k) != chain_queue.end()) return;
        // Ahead of the scan: a catalog probe queued for it moves here.
        std::string bundle;
        for (auto it = catalog_queue.begin(); it != catalog_queue.end(); ++it)
            if (it->key == k) {
                bundle = it->bundle;
                catalog_queue.erase(it);
                break;
            }
        if (!bundle.empty()) chain_bundles[k] = bundle;
        chain_queue.push_back(k);
    }
    std::map<Key, std::string> chain_bundles;

    // A probe that cannot run is refused, not blocked: nothing of the
    // plugin's ran, and the next start may succeed.
    void refuse(const Key& k, const std::string& why, const std::string& bundle) {
        pr::Record r = blocked(k, why);
        r.outcome = pr::Outcome::Refused;
        store(k, std::move(r), std::string());
        settle_one(bundle);
    }

    void start_probe(const Key& k, bool chain, const std::string& bundle) {
        if (!disabled.empty()) {
            refuse(k, disabled, bundle);
            return;
        }
        auto j = std::make_unique<Job>();
        j->kind = Job::Kind::Probe;
        j->key = k;
        j->chain = chain;
        j->bundle = bundle;
        j->out_path = scratch("probe");
        j->timeout_ms = cfg.probe_timeout_ms;
        std::vector<std::string> argv = {cfg.host_path, "--probe", k.first, "--channels", std::to_string(k.second)};
        add_bundles(argv);
        if (!spawn(*j, argv)) {
            refuse(k, "its probe could not start", bundle);
            return;
        }
        probed_this_scan.insert(k);
        s.probes_started_.fetch_add(1, std::memory_order_acq_rel);
        if (!chain) s.catalog_probes_.fetch_add(1, std::memory_order_acq_rel);
        jobs.push_back(std::move(j));
    }

    // Change E: a catalog child never takes the last free slot, which is
    // kept for a chain-start probe.
    bool catalog_slot() const { return catalog_jobs() + 1 < s.slots_ && jobs.size() < s.slots_; }

    // Change D, read now: a walk just before may have taken seconds.
    bool recording_now() const { return s.recording_.load(std::memory_order_acquire); }

    bool start_list(Job::Kind kind) {
        if (recording_now() || stopping) return false;
        auto j = std::make_unique<Job>();
        j->kind = kind;
        j->out_path = scratch("list");
        j->timeout_ms = cfg.list_timeout_ms;
        std::vector<std::string> argv = {cfg.host_path, "--list", "--json"};
        if (kind == Job::Kind::Bundles) {
            argv.push_back("--bundles-only");
        } else if (kind == Job::Kind::ListLv2) {
            argv.insert(argv.end(), {"--format", "lv2"});
        } else {
            // Only the changed VST3 bundles: listing one runs its code.
            argv.insert(argv.end(), {"--format", "vst3", "--only-given"});
            for (const std::string& b : changed_vst3) {
                argv.push_back("--bundle");
                argv.push_back(b);
            }
        }
        add_bundles(argv, kind == Job::Kind::ListVst3);
        if (!spawn(*j, argv)) {
            if (kind == Job::Kind::Bundles) end_scan();
            return false;
        }
        s.lists_started_.fetch_add(1, std::memory_order_acq_rel);
        jobs.push_back(std::move(j));
        return true;
    }

    // ---- the loop

    // `chain_only`: between the bundles of a walk, only chain-start probes.
    void reap(bool chain_only = false) {
        // Once stopping, what ends is shutdown()'s, never a result.
        if (s.stop_.load(std::memory_order_acquire)) return;
        const uint64_t now = steady_ms();
        // The finished ones leave `jobs` first: handling one may walk
        // bundles and serve chain starts (service()), which reaps and starts
        // jobs of its own.
        std::vector<std::pair<std::unique_ptr<Job>, bool>> done;
        for (auto it = jobs.begin(); it != jobs.end();) {
            Job& j = **it;
            const bool running = (chain_only && j.catalog()) ? true : j.child->running();
            const bool timed_out = running && !(chain_only && j.catalog()) && now - j.started_ms >= j.timeout_ms;
            if (running && !timed_out) {
                ++it;
                continue;
            }
            if (timed_out) {
                j.child->kill();
                j.child->wait(-1);
            }
            done.emplace_back(std::move(*it), timed_out);
            it = jobs.erase(it);
        }
        for (auto& [jp, timed_out] : done) {
            Job& j = *jp;
            switch (j.kind) {
                case Job::Kind::Probe: finish_probe(j, timed_out); break;
                case Job::Kind::Bundles:
                    finish_bundles(j, timed_out || !j.child->exited_normally() || j.child->exit_code() != 0);
                    break;
                case Job::Kind::ListLv2:
                case Job::Kind::ListVst3:
                    finish_list(j, timed_out || !j.child->exited_normally() || j.child->exit_code() != 0);
                    break;
            }
        }
    }

    void end_scan() {
        phase = Phase::Idle;
        rescan = false;
        catalog_queue.clear();
        probed_this_scan.clear();
        scanned_once = true;
        rebuild_api();
        save(true);
    }

    void advance() {
        const bool list_running = std::any_of(jobs.begin(), jobs.end(), [](const auto& j) {
            return j->kind != Job::Kind::Probe;
        });
        // Change D: nothing of the catalog scan starts during a take. Read
        // here and again before every spawn (recording_now()).
        const bool may_start = !recording_now() && !stopping;
        switch (phase) {
            case Phase::Idle:
                if (scan_wanted && may_start) {
                    if (!disabled.empty()) {
                        scan_wanted = false;
                        end_scan();
                        break;
                    }
                    if (rescan) run_only.clear();
                    probed_this_scan.clear();
                    if (start_list(Job::Kind::Bundles)) {
                        scan_wanted = false;
                        phase = Phase::Bundles;
                    }
                }
                break;
            case Phase::Bundles: break;  // finish_bundles moves it on
            case Phase::Enumerate:
                if (list_running) break;
                if (!may_start) break;
                if (!changed_lv2.empty() && !listed_lv2 && catalog_slot())
                    listed_lv2 = start_list(Job::Kind::ListLv2);
                if (!changed_vst3.empty() && !listed_vst3 && catalog_slot())
                    listed_vst3 = start_list(Job::Kind::ListVst3);
                if ((changed_lv2.empty() || listed_lv2) && (changed_vst3.empty() || listed_vst3) &&
                    !std::any_of(jobs.begin(), jobs.end(), [](const auto& j) { return j->kind != Job::Kind::Probe; })) {
                    phase = Phase::Probing;
                    listed_lv2 = listed_vst3 = false;
                }
                break;
            case Phase::Probing: {
                while (!recording_now() && !stopping && !catalog_queue.empty() && catalog_slot()) {
                    const Pending p = catalog_queue.front();
                    catalog_queue.pop_front();
                    start_probe(p.key, false, p.bundle);
                }
                const bool catalog_running =
                    std::any_of(jobs.begin(), jobs.end(), [](const auto& j) { return j->catalog(); });
                if (catalog_queue.empty() && !catalog_running) {
                    // Bundles whose plugins all answered are settled; any
                    // left (a probe that never ran) are scanned next time.
                    end_scan();
                }
                break;
            }
        }
        start_chain_probes();
        s.idle_.store(phase == Phase::Idle && !scan_wanted && jobs.empty() && chain_queue.empty() && ready,
                      std::memory_order_release);
    }
    bool listed_lv2 = false, listed_vst3 = false;

    // Chain-start probes: any free slot, the reserved one included.
    void start_chain_probes() {
        while (!chain_queue.empty() && jobs.size() < s.slots_ && !stopping) {
            const Key k = chain_queue.front();
            chain_queue.pop_front();
            std::string bundle;
            if (auto b = chain_bundles.find(k); b != chain_bundles.end()) {
                bundle = b->second;
                chain_bundles.erase(b);
            }
            start_probe(k, true, bundle);
        }
    }

    void save(bool now) {
        if (!dirty || cfg.cache_path.empty()) return;
        const uint64_t t = steady_ms();
        if (!now && t - saved_ms < kSaveEveryMs) return;
        saved_ms = t;
        // A bundle still settling keeps its incomplete fingerprint on
        // disk: enumerated again at the next start.
        std::string why;
        if (save_plugin_cache(cfg.cache_path, cache, why)) dirty = false;
        else notice("the plug-in catalog cache could not be written: " + why);
    }

    void take_inbox() {
        std::vector<Key> probes;
        bool want_rescan = false;
        {
            std::lock_guard<std::mutex> l(s.exchange_mu_);
            probes.swap(s.in_probes_);
            want_rescan = s.in_rescan_;
            s.in_rescan_ = false;
        }
        if (want_rescan) {
            if (phase == Phase::Idle) {
                scan_wanted = true;
                rescan = true;
            } else if (!rescan) {
                // Joins the scan running: what it blocked is probed again
                // before it ends.
                rescan = true;
                run_only.clear();
                for (const auto& [path, b] : cache.bundles)
                    for (const pr::Record& r : b.plugins)
                        if (r.outcome == pr::Outcome::Blocked && listed.count(path))
                            enqueue_catalog({r.uri, uint16_t(r.channels)}, path);
            }
        }
        for (const Key& k : probes) enqueue_chain(k);
    }

    void shutdown() {
        stopping = true;
        // Killed and reaped before the thread ends (change C); none of
        // these is cached, blocked or otherwise: we ended it.
        for (auto& j : jobs) j->child->kill();
        for (auto& j : jobs) {
            j->child->wait(-1);
            std::remove(j->out_path.c_str());
        }
        jobs.clear();
        save(true);
    }

    void run() {
        std::string why;
        // The walk, the cache and every child at nice 10 and the idle I/O
        // class (changes A, D); children inherit it from birth.
        if (!platform::lower_current_thread(cfg.nice, cfg.idle_io, why))
            notice("the plug-in scan runs at normal priority: " + why);
        check_version();
        if (!disabled.empty()) notice(disabled);
        load();
        ready = true;
        rebuild_api();
        publish();
        scan_wanted = cfg.scan_at_start;
        while (!s.stop_.load(std::memory_order_acquire)) {
            take_inbox();
            reap();
            advance();
            if (publish_wanted && (urgent || steady_ms() - published_ms >= kPublishEveryMs)) publish();
            if (phase == Phase::Idle) save(false);
            std::unique_lock<std::mutex> l(s.sleep_mu_);
            s.sleep_cv_.wait_for(l, kTurn, [&] { return s.stop_.load(std::memory_order_acquire); });
        }
        shutdown();
    }
};

// ---- the control thread's side

PluginScanner::PluginScanner(PluginScannerConfig config) : config_(std::move(config)) {
    slots_ = config_.max_probes != 0 ? config_.max_probes : std::thread::hardware_concurrency() / 2;
    slots_ = std::max(2u, slots_);
    catalog_ = std::make_shared<Catalog>();
    thread_ = std::thread([this] { run(); });
}

PluginScanner::~PluginScanner() {
    stop_.store(true, std::memory_order_release);
    sleep_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void PluginScanner::run() {
    Worker w(*this);
    w.run();
}

bool PluginScanner::exchange(bool recording) {
    const bool was = recording_.exchange(recording, std::memory_order_acq_rel);
    std::unique_lock<std::mutex> l(exchange_mu_, std::try_to_lock);
    if (!l.owns_lock()) {
        if (was != recording) sleep_cv_.notify_one();
        return false;
    }
    const bool wake = !want_probes_.empty() || want_rescan_ || recording != was;
    in_probes_.insert(in_probes_.end(), want_probes_.begin(), want_probes_.end());
    want_probes_.clear();
    in_rescan_ = in_rescan_ || want_rescan_;
    want_rescan_ = false;
    if (out_catalog_) catalog_ = std::move(out_catalog_);
    notices_.insert(notices_.end(), out_notices_.begin(), out_notices_.end());
    out_notices_.clear();
    l.unlock();
    // Answered keys may be asked again (a rescan dropped them, say).
    for (auto it = asked_.begin(); it != asked_.end();)
        it = catalog_->find(it->first, it->second) ? asked_.erase(it) : std::next(it);
    if (wake) sleep_cv_.notify_one();
    return true;
}

void PluginScanner::probe(const std::string& host_id, uint16_t channels) {
    const Key k{host_id, channels};
    if (catalog_->find(host_id, channels) || !asked_.insert(k).second) return;
    want_probes_.push_back(k);
}

void PluginScanner::rescan() { want_rescan_ = true; }

std::vector<std::string> PluginScanner::take_notices() {
    std::vector<std::string> out;
    out.swap(notices_);
    return out;
}

}  // namespace sonora
