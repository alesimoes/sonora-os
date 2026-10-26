// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plugin scan worker (ADR-0012, amendment of 2026-10-19, items 1, 3, 4
// and 5 with changes A to E). One thread for the engine process, owned by
// the control server and not by PluginHosting: the catalog outlives a
// backend activation, so a reopen at another period does not rescan.
//
// What it does, on its own thread, which may block (it is not the control
// thread, and never the audio thread):
//   - at start: `sonora-plugin-host --version` (it must name probe record
//     2, kProbeRecordVersion), the cache read and every cached bundle fingerprinted again, then
//     the catalog published (generation 1);
//   - a scan: `--list --bundles-only` (no plugin code), each bundle
//     fingerprinted; new or changed ones enumerated (`--list --format lv2`,
//     `--list --format vst3 --only-given` for the changed VST3 bundles
//     only), then one `--probe ID --channels N` child per plugin; bundles
//     gone are dropped; the catalog published again, its generation bumped
//     if clients would see a difference; the cache written;
//   - chain-start probes for PluginHosting, ahead of the scan, deduplicated
//     with any probe queued or running for the same (id, channels).
//
// Rules, each with a test (tests/test_plugin_scanner.cpp):
//   - one record per (id, channels); a plugin probed at 2 when it can run
//     stereo, otherwise at its own count (item 1);
//   - a probe that crashes or does not answer within probe_timeout_ms is
//     cached Blocked, with its reason, and not run again until a rescan or
//     a changed fingerprint (item 4); one ended by our own shutdown is never
//     cached (change C);
//   - children run at nice 10 and the idle I/O class, and so does the walk:
//     the thread lowers itself first (changes A, D);
//   - no new catalog child starts while the transport records, the
//     start-up scan included; those in flight finish, and the scan resumes
//     when the take ends; chain-start probes stay allowed (change D);
//   - one probe slot is reserved for chain-start probes (change E);
//   - a scan that lists a bridged bundle creates the Wine prefix first when
//     there is none, before anything bridged runs; if it cannot, that is
//     said and nothing bridged is enumerated, probed or cached (ADR-0017
//     Y4);
//   - the thread lives until the scanner is destroyed, and is joined only
//     after it has killed and reaped every child (change C: the death
//     signal of a child fires when the thread that forked it exits).
//
// Hand-over (item 3): the control thread calls exchange() once per turn,
// which only try_locks. A request made while the lock was busy is kept for
// the next turn, never dropped. The worker holds that mutex only to swap a
// pointer or a queue, never across I/O or a wait.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sonora/api/messages.h"
#include "sonora/plugin/probe_record.h"

namespace sonora {

struct PluginScannerConfig {
    std::string host_path;             // sonora-plugin-host
    std::vector<std::string> bundles;  // --plugin-bundle, passed to every child
    // The cache file (change A: resolved by the entry layer, passed down).
    // Empty: the catalog lives in memory for the run (tests, CI).
    std::string cache_path;
    // The image's build id, in the cache header (change B): an image
    // update revalidates every bundle, reproducible mtimes or not.
    std::string build_id;
    // Probe slots: 0 is half the hardware threads, at least 2. One of them
    // is only ever taken by a chain-start probe (change E).
    uint32_t max_probes = 0;
    uint64_t probe_timeout_ms = 30000;
    // One --list child (VST3 enumeration is one isolated child per bundle
    // inside it, each with its own 120 s).
    uint64_t list_timeout_ms = 600000;
    std::string scratch_dir = "/tmp";
    // Windows plug-ins through yabridge (ADR-0017 Y4), resolved by the
    // entry layer. The Wine prefix they run in, and the command that
    // creates it (wineboot --init, an absolute path: children are not
    // looked up on PATH). When a scan lists a bridged bundle and
    // `<wine_prefix>/system.reg` does not exist, the command runs first,
    // alone, within wine_prefix_timeout_ms; nothing bridged is enumerated
    // or probed until it has. A first Wine start that creates the prefix
    // takes longer than a probe's timeout, which would cache the plug-in
    // Blocked. Empty prefix: no step. Empty command with a prefix: Wine is
    // not installed, and bridged bundles are not scanned, said.
    std::string wine_prefix;
    std::vector<std::string> wine_prefix_command;
    // wineboot ends before the registry is saved; `wineserver -w` (absolute
    // path) returns once it has been, so system.reg exists. Empty: not waited.
    std::vector<std::string> wine_server_wait_command;
    uint64_t wine_prefix_timeout_ms = 120000;
    // Q1: scan at every engine start. Tests of the chains turn it off.
    bool scan_at_start = true;
    int nice = 10;
    bool idle_io = true;
    // Tests only: each bundle's fingerprint takes this much longer, so a
    // walk over many bundles on slow storage can be stood in for.
    uint32_t fingerprint_delay_ms_for_test = 0;
};

class PluginScanner {
public:
    using Key = std::pair<std::string, uint16_t>;  // (host id, channels)

    // Immutable once published: the control thread holds a shared_ptr.
    struct Catalog {
        uint64_t serial = 0;      // every publish
        uint64_t generation = 0;  // what clients are sent; moves when `api` does
        bool ready = false;       // generation 1 is out: the cache was read
        bool scanned = false;     // the first scan of this run has completed
        // Why nothing can be scanned or probed (a host of another record
        // version); empty when it can.
        std::string disabled;
        api::PluginCatalog api;
        // `api` as the wire carries it, encoded and split into frames by the
        // worker: the control thread only hands these to each client, the
        // same immutable frames to all (rt-engineer 2 on #95, and its
        // follow-up: at 18 MB a split and copy per client was 17 ms). Shared
        // by every catalog of one generation. Never null once published.
        using Frames = std::vector<std::shared_ptr<const std::string>>;
        std::shared_ptr<const Frames> api_frames;
        // (format, id) -> index in api.plugins.
        std::map<std::pair<uint32_t, std::string>, std::size_t> api_index;
        // Every record known this run, chain-start probes included.
        std::map<Key, plugin::probe::Record> probes;
        const plugin::probe::Record* find(const std::string& host_id, uint16_t channels) const {
            const auto it = probes.find({host_id, channels});
            return it == probes.end() ? nullptr : &it->second;
        }
        const api::PluginInfo* find_plugin(api::PluginFormat format, const std::string& id) const {
            const auto it = api_index.find({static_cast<uint32_t>(format), id});
            return it == api_index.end() ? nullptr : &api.plugins[it->second];
        }
        // The record a finished probe left for `host_id`, a usable one
        // first, not-installed ones skipped; null when there is none.
        const plugin::probe::Record* any_probe(const std::string& host_id) const {
            const plugin::probe::Record* best = nullptr;
            for (auto it = probes.lower_bound({host_id, 0}); it != probes.end() && it->first.first == host_id; ++it) {
                if (it->second.outcome == plugin::probe::Outcome::NotInstalled) continue;
                if (!best || (it->second.ok() && !best->ok())) best = &it->second;
            }
            return best;
        }
    };

    explicit PluginScanner(PluginScannerConfig config);
    // Kills and reaps every child, writes what finished, then joins.
    ~PluginScanner();
    PluginScanner(const PluginScanner&) = delete;
    PluginScanner& operator=(const PluginScanner&) = delete;

    // --- the control thread ---

    // Once per turn. One try_lock: requests go in, the newest catalog and
    // notices come out. False when the lock was busy; nothing is lost.
    // `recording`: the transport records or is about to. It is stored
    // whether or not the lock is taken, and the worker reads it right
    // before every catalog child it starts (change D).
    bool exchange(bool recording);
    // The newest catalog taken; never null.
    const std::shared_ptr<const Catalog>& catalog() const { return catalog_; }
    // A chain-start probe (PluginHosting): ahead of the scan.
    void probe(const std::string& host_id, uint16_t channels);
    // plugin.rescan: the same walk, and blocked plugins probed again. A
    // request while a scan runs joins it.
    void rescan();
    // What the operator is told once: a discarded cache, a bundle that
    // could not be scanned, a cache that could not be written.
    std::vector<std::string> take_notices();

    // --- any thread: for tests and the log ---
    uint64_t probes_started() const { return probes_started_.load(std::memory_order_acquire); }
    uint64_t catalog_probes_started() const { return catalog_probes_.load(std::memory_order_acquire); }
    uint64_t lists_started() const { return lists_started_.load(std::memory_order_acquire); }
    uint32_t peak_children() const { return peak_children_.load(std::memory_order_acquire); }
    // No scan running and nothing queued.
    bool idle() const { return idle_.load(std::memory_order_acquire); }
    std::mutex& exchange_mutex_for_test() { return exchange_mu_; }
    // Tests only: `c` is handed over as the worker's next publish would be.
    void inject_catalog_for_test(std::shared_ptr<const Catalog> c);
    // Bundles fingerprinted by the worker so far (a walk has begun).
    uint64_t fingerprints() const { return fingerprints_.load(std::memory_order_acquire); }

    // Slots: probes in flight at most, and those a catalog child may take.
    uint32_t slots() const { return slots_; }

    struct Job;
    struct Worker;

private:
    void run();

    const PluginScannerConfig config_;
    uint32_t slots_ = 2;

    // The control thread's own.
    std::shared_ptr<const Catalog> catalog_;
    std::vector<Key> want_probes_;
    std::set<Key> asked_;
    bool want_rescan_ = false;
    std::vector<std::string> notices_;

    // Under exchange_mu_.
    std::mutex exchange_mu_;
    std::vector<Key> in_probes_;
    bool in_rescan_ = false;
    // Not under the lock: a lost try_lock still delivers it.
    std::atomic<bool> recording_{false};
    std::shared_ptr<const Catalog> out_catalog_;
    // Catalogs the control thread let go of: freed by the worker, never on
    // the control thread (rt-engineer 1 on #95).
    std::vector<std::shared_ptr<const Catalog>> retired_;
    std::vector<std::string> out_notices_;

    // The worker sleeps here; the control thread only notifies.
    std::mutex sleep_mu_;
    std::condition_variable sleep_cv_;
    std::atomic<bool> stop_{false};

    std::atomic<uint64_t> probes_started_{0};
    std::atomic<uint64_t> catalog_probes_{0};
    std::atomic<uint64_t> lists_started_{0};
    std::atomic<uint32_t> peak_children_{0};
    std::atomic<uint64_t> fingerprints_{0};
    std::atomic<bool> idle_{false};

    std::thread thread_;
};

// The channel count the catalog probes a plugin at (item 1): 2 when it can
// run stereo (2 in 2 out, or 1 in 1 out replicated), otherwise its own.
uint16_t catalog_channels(uint32_t audio_in, uint32_t audio_out);

// A record as contracts/api describes it to a client (§8): the model's id
// (a VST3 class id without "vst3:"), its words, usable or why not.
api::PluginInfo catalog_entry(const plugin::probe::Record& r);

}  // namespace sonora
