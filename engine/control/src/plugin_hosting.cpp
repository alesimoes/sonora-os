// SPDX-License-Identifier: GPL-3.0-or-later
// The control thread's plugin side (plugin_hosting.h). Never on the audio
// thread: it allocates, starts processes and reads files.
#include "sonora/plugin_hosting.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <tuple>

#include "sonora/api/state_block.h"
#include "sonora/json/json.h"
#include "sonora/plan_builder.h"
#include "sonora/platform/process.h"
#include "sonora/plugin/host_control.h"
#include "sonora/plugin/probe_record.h"
#include "sonora/plugin_chain_runner.h"
#include "sonora/plugin_client/host_supervisor.h"
#include "sonora/plugin_client/plugin_client.h"
#include "sonora/plugin_scanner.h"
#include "sonora/session.h"
#include "sonora/sha256.h"

namespace sonora {

namespace pc = plugin::control;
using plugin_client::SupervisedChain;

namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;

std::string slot_letter(uint8_t slot) { return std::string(1, static_cast<char>('A' + slot)); }

std::string where(const model::TrackEdit& t, const model::Insert& in) {
    return (in.plugin.name.empty() ? in.plugin.id : in.plugin.name) + " (" + t.name + ", slot " +
           slot_letter(in.slot) + ")";
}

// The id the host takes: an LV2 URI, or "vst3:" and the class id.
std::string host_id(const model::PluginRef& p) {
    return p.format == model::PluginFormat::Vst3 ? "vst3:" + p.id : p.id;
}

uint64_t steady_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

// A value as a document operation: bypass, or the insert's whole list with
// this key set (§8, "Values moved in the plugin's own editor"). Several
// moves of one batch land on one list.
void record_value(std::vector<model::Operation>& ops, const model::Snapshot& doc, model::Id insert, bool bypass,
                  const std::string& key, float value) {
    if (bypass) {
        ops.push_back(model::SetInsertBypass{insert, value >= 0.5f});
        return;
    }
    model::SetInsertParams* set = nullptr;
    for (model::Operation& op : ops) {
        if (auto* s = model::get_if<model::SetInsertParams>(&op); s && s->insert == insert) set = s;
    }
    if (!set) {
        const model::InsertMix* m = doc.mix.find_insert(insert);
        ops.push_back(model::SetInsertParams{insert, m ? m->params : std::vector<model::ParamValue>{}});
        set = model::get_if<model::SetInsertParams>(&ops.back());
    }
    const auto k = std::find_if(set->params.begin(), set->params.end(),
                                [&](const model::ParamValue& p) { return p.key == key; });
    if (k != set->params.end()) k->value = value;
    else set->params.push_back({key, value});
}

// ADR-0029 §6: makePath folders live outside the session folder, under the
// user's state directory, per engine run. Runs whose engine is gone (it
// crashed, or was killed) are removed first; a live engine's stay.
std::string make_run_dir(std::string root, std::string& why) {
    namespace fs = std::filesystem;
    if (root.empty()) {
        const char* state = std::getenv("XDG_STATE_HOME");
        const char* home = std::getenv("HOME");
        // XDG: a relative value is ignored.
        if (state && state[0] == '/') root = std::string(state) + "/sonora/plugin-scratch";
        else if (home && home[0] == '/') root = std::string(home) + "/.local/state/sonora/plugin-scratch";
    }
    if (root.empty()) {
        why = "neither XDG_STATE_HOME nor HOME names a folder";
        return {};
    }
    std::error_code ec;
    const fs::path base = fs::absolute(root, ec).lexically_normal();
    if (ec) {
        why = root + " is not a folder";
        return {};
    }
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const std::size_t dash = name.find('-');
        if (dash == 0 || dash == std::string::npos ||
            name.find_first_not_of("0123456789") != dash)
            continue;
        std::error_code type;
        if (!fs::is_directory(it->symlink_status(type)) || type) continue;
        if (platform::process_gone(std::strtoll(name.substr(0, dash).c_str(), nullptr, 10))) {
            std::error_code rm;
            fs::remove_all(it->path(), rm);
        }
    }
    // The pid and a count: two hostings of one engine (a backend reactivated)
    // never share a folder.
    static std::atomic<uint64_t> serial{0};
    const fs::path run = base / (std::to_string(platform::current_process_id()) + "-" + std::to_string(++serial));
    ec.clear();
    fs::create_directories(run, ec);
    if (ec) {
        why = run.string() + " could not be made: " + ec.message();
        return {};
    }
    return run.string();
}

bool insert_in(const model::Snapshot& doc, model::Id insert) {
    for (const auto& t : doc.edit->tracks)
        for (const model::Insert& in : t->inserts)
            if (in.id == insert) return true;
    return false;
}

}  // namespace

// ---- what the scan says of one plugin at one channel count

struct PluginHosting::Probe {
    // Queued: not yet asked of the scan worker; Running: asked, no answer
    // in its catalog yet.
    enum class State { Queued, Running, Ok, Missing, Refused };
    std::string id;  // host_id()
    uint16_t channels = 1;
    State state = State::Queued;
    std::vector<std::string> keys;  // dense order
    std::vector<float> defaults;
    std::string reason;
    // The record this answer was taken from, encoded: a catalog whose record
    // for the key differs, or has none, makes it stale. Empty when the
    // answer is not the worker's (a malformed id).
    std::string record;

    // The worker's record, as the chain needs it. A Blocked plugin (its
    // probe crashed or hung, §4) is refused for the chain, as before.
    void take(const plugin::probe::Record& r) {
        record = plugin::probe::encode(r);
        keys.clear();
        defaults.clear();
        reason.clear();
        switch (r.outcome) {
            case plugin::probe::Outcome::Ok:
                for (const plugin::probe::Param& p : r.params) {
                    keys.push_back(p.key);
                    defaults.push_back(p.default_value);
                }
                state = State::Ok;
                return;
            case plugin::probe::Outcome::NotInstalled:
                state = State::Missing;
                reason = "not installed";
                return;
            case plugin::probe::Outcome::Refused:
            case plugin::probe::Outcome::Blocked:
                state = State::Refused;
                reason = r.reason.empty() ? "refused" : r.reason;
                return;
        }
    }
};

struct PluginHosting::Member {
    model::Id insert = 0;
    model::PluginRef plugin;
    bool operator==(const Member& o) const { return insert == o.insert && plugin == o.plugin; }
};

struct PluginHosting::Chain {
    model::Id track = 0;
    uint16_t channels = 1;
    // Excluded instances and why (changes B and C), by insert id.
    std::map<model::Id, std::string> missing;
    std::map<model::Id, std::string> refused;
    // Refusals a host reported (by Down with refused()), kept until the
    // document changes that insert or the operator retries.
    std::map<model::Id, std::string> host_refused;

    std::unique_ptr<SupervisedChain> running;
    uint32_t store = kNone, node = kNone;
    std::vector<Member> running_spec;
    // Every active insert the document had when the running chain's spec
    // was chosen, excluded ones included. While the document still says
    // exactly this, the running chain stands whatever the probe answers say
    // now: a rescan that loses or blocks one of its plugins must not touch
    // it (ADR-0012), and the new answer applies at its next start.
    std::vector<Member> running_doc;

    std::unique_ptr<SupervisedChain> pending;
    uint32_t pstore = kNone, pnode = kNone;
    std::vector<Member> pending_spec;
    std::vector<Member> pending_doc;
    // What start_pending seeded the pending store with, by insert: the
    // document's values then. On Swapped it is what the store holds, so
    // what the document changed meanwhile is posted (B2 on #80).
    std::map<model::Id, model::InsertMix> pending_seeded;
    // A start that crashed, timed out or could not get a store, and the
    // spec it was for: not started again until the document wants another
    // spec (a session opened starts every chain again).
    std::string start_failed;
    std::vector<Member> failed_spec;

    bool opened = false;      // started with the session: silent until Ready
    bool ever_ready = false;
    bool nothing_to_run = false;  // every instance missing or refused
    bool restart_swapped = false;
    uint64_t restart_retire_epoch = 0;

    // The first plan built with the running store (0: none yet), and
    // whether resolve() named the store in the plan being built.
    uint64_t store_epoch = 0;
    bool named = false;
    // Values wait for store_epoch: post_values again once it is known.
    bool need_post = false;

    // The values last given to the running chain's store, by insert.
    std::map<model::Id, model::InsertMix> posted;
    // Changes for the running store the session refused, or that wait for
    // its first plan: (instance, param) -> the newest value.
    std::map<std::pair<uint32_t, uint32_t>, float> retry;
    // ADR-0087 §4: the one insert a render leaves playing (0: no override),
    // and the inserts whose bypass in the running store is the override's,
    // not the document's.
    model::Id solo = 0;
    std::set<model::Id> solo_posted;

    // Gestures the running host has open: the last value touched, by
    // (instance, param), with what the document needs to record it.
    struct Touch {
        model::Id insert = 0;
        bool bypass = false;
        std::string key;
        float value = 0.f;
    };
    std::map<std::pair<uint32_t, uint32_t>, Touch> open_touches;

    // Saved states not restored (ADR-0012 §6.2): the plugin runs with its
    // defaults plus its params, and the operator is told. By insert, with
    // the state the document named then: the fault stands while it still
    // names that one. The pending start's become the running chain's at
    // its swap, with whatever its host's Loaded refused.
    struct LostState {
        model::StateRef ref;
        std::string why;
    };
    std::map<model::Id, LostState> state_lost;
    std::map<model::Id, LostState> pending_state_lost;

    // Inserts already told, this run, that their worker refused messages
    // (ADR-0012 change G, as for non-finite output): one Notice per run.
    // A run is one build of the chain; a restart of it is the same run.
    std::set<model::Id> work_noticed;
    // The same for touches of a parameter the session cannot record, per
    // insert, and for touches lost on the way, per chain (the host's touch
    // queue is the chain's, not one plugin's).
    std::set<model::Id> unmapped_noticed;
    // The same for path calls the host refused (Fault{Plugin} with a count).
    std::set<model::Id> paths_noticed;
    bool lost_noticed = false;
};

PluginHosting::PluginHosting(Session& session, PluginHostingConfig config, PluginScanner& scanner)
    : session_(session), config_(std::move(config)), scanner_(scanner) {
    plugin_client::ClientConfig cc;
    cc.block_frames = config_.block_frames;
    cc.max_stores = config_.max_stores;
    cc.max_nodes = config_.max_stores;
    cc.max_channels = 2 * config_.max_stores + 8;  // make-before-break doubles a chain briefly
    cc.max_chain_channels = 2;                     // item 4: the session's stereo planes
    client_ = std::make_unique<plugin_client::PluginClient>(cc);
    plugin_client::SupervisorConfig sc;
    sc.host_path = config_.host_path;
    sc.bundles = config_.bundles;
    sc.sample_rate = config_.sample_rate;
    sc.block_frames = config_.block_frames;
    sc.max_frames = std::max(config_.block_frames, config_.max_block_frames);
    sc.ready_timeout_ms = config_.ready_timeout_ms;
    sc.scratch_dir = config_.scratch_dir;
    supervisor_ = std::make_unique<plugin_client::HostSupervisor>(sc, *client_);
    ok_ = supervisor_->ok();
    why_ = supervisor_->why();
    for (uint32_t i = config_.max_stores; i-- > 0;) {
        free_stores_.push_back(i);
        free_nodes_.push_back(i);
    }
    runner_ = std::make_unique<PluginChainRunner>(*client_);
    session_.set_insert_chains(runner_.get());
    std::string why;
    run_dir_ = make_run_dir(config_.plugin_scratch_root, why);
    if (run_dir_.empty() && ok_)
        hold(0, "plug-ins cannot make files of their own this run (their scratch folder: " + why +
                    "); a plug-in that needs one may not work");
    followed_dir_ = session_dir();
}

PluginHosting::~PluginHosting() {
    // The runner leaves the session first; the caller has deactivated the
    // backend or seen a callback since (item 4), so no callback holds it.
    session_.set_insert_chains(nullptr);
    std::vector<SupervisedChain*> all;
    for (auto& [track, c] : chains_) {
        if (c->pending) all.push_back(c->pending.get());
        if (c->running) all.push_back(c->running.get());
    }
    for (Retiring& r : retiring_) all.push_back(r.chain.get());
    for (auto& s : stopping_) all.push_back(s.get());
    // Every stop asked first, then one grace for the whole set (M5 on #80):
    // N hung hosts cost 2 s, not 2 s each. stopped() signals the kill past
    // the grace without waiting; the loop's last half second lets the
    // kills be reaped, so the destructors below find nothing running.
    const uint64_t t0 = steady_ms();
    for (SupervisedChain* s : all) s->request_stop(t0);
    for (;;) {
        bool done = true;
        for (SupervisedChain* s : all) done = s->stopped(steady_ms()) && done;
        if (supervisor_) done = supervisor_->reap(steady_ms()) && done;
        if (done || steady_ms() - t0 >= 2500) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    chains_.clear();
    stopping_.clear();
    retiring_.clear();
    runner_.reset();
    supervisor_.reset();
    client_.reset();
    // Every host is gone: a scratch file a saved state needed was copied
    // into the session at its flush, so nothing here is the session's.
    if (!run_dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(run_dir_, ec);
    }
}

std::string PluginHosting::session_dir() const {
    const std::shared_ptr<const std::string> folder = session_.folder();
    if (!folder || folder->empty()) return {};
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(*folder, ec);
    return ec ? std::string() : abs.lexically_normal().string();
}

std::string PluginHosting::scratch_for(model::Id insert, const std::string& who) {
    if (run_dir_.empty()) return {};
    const std::filesystem::path dir =
        std::filesystem::path(run_dir_) / std::to_string(session_serial_) / std::to_string(insert);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec || !std::filesystem::is_directory(std::filesystem::symlink_status(dir, ec))) {
        // It runs without makePath, and the operator is told once.
        if (scratch_failed_.insert(insert).second)
            hold(insert, who + ": its scratch folder " + dir.string() +
                             " could not be made, so it cannot make files of its own");
        return {};
    }
    return dir.string();
}

void PluginHosting::follow_session_dir() {
    const std::string dir = session_dir();
    if (dir == followed_dir_) return;
    followed_dir_ = dir;
    for (auto& [track, c] : chains_) {
        if (c->running) c->running->set_session_dir(dir);
        if (c->pending) c->pending->set_session_dir(dir);
    }
}

uint32_t PluginHosting::take_store() {
    if (free_stores_.empty()) return kNone;
    const uint32_t s = free_stores_.back();
    free_stores_.pop_back();
    return s;
}

uint32_t PluginHosting::take_node() {
    if (free_nodes_.empty()) return kNone;
    const uint32_t n = free_nodes_.back();
    free_nodes_.pop_back();
    return n;
}

void PluginHosting::retire(std::unique_ptr<SupervisedChain> chain, uint32_t store, uint32_t node) {
    if (!chain) {
        if (store != kNone) free_stores_.push_back(store);
        if (node != kNone) free_nodes_.push_back(node);
        return;
    }
    retiring_.push_back(Retiring{std::move(chain), store, node, 0});
}

void PluginHosting::abandon_pending(Chain& c) {
    if (c.pending) {
        // Never bound: no plan names it, so it needs no epoch, only a stop
        // the control thread does not wait for (M5 on #80).
        c.pending->request_stop(steady_ms());
        stopping_.push_back(std::move(c.pending));
    }
    retire(nullptr, c.pstore, c.pnode);
    c.pstore = c.pnode = kNone;
    c.pending_spec.clear();
    c.pending_doc.clear();
    c.pending_seeded.clear();
    c.pending_state_lost.clear();
}

void PluginHosting::drop_chain(Chain& c) {
    fail_state_requests(c.track, "its chain was removed or rebuilt before it answered");
    abandon_pending(c);
    retire(std::move(c.running), c.store, c.node);
    c.store = c.node = kNone;
    c.running_spec.clear();
    c.running_doc.clear();
    c.posted.clear();
    c.retry.clear();
    c.solo = 0;
    c.solo_posted.clear();
    c.open_touches.clear();
    c.store_epoch = 0;
    c.named = false;
    c.need_post = false;
    c.restart_swapped = false;
    c.restart_retire_epoch = 0;
    c.state_lost.clear();
    c.work_noticed.clear();
    c.unmapped_noticed.clear();
    c.paths_noticed.clear();
    c.lost_noticed = false;
}

// ---- probes: the scan worker's (amendment of 2026-10-19, item 3)

PluginHosting::Probe* PluginHosting::probe_for(const model::PluginRef& plugin, uint16_t channels) {
    const std::string id = host_id(plugin);
    auto& p = probes_[{id, channels}];
    if (!p) {
        p = std::make_unique<Probe>();
        p->id = id;
        p->channels = channels;
        const pc::Format f = plugin.format == model::PluginFormat::Vst3 ? pc::Format::Vst3 : pc::Format::Lv2;
        if (!pc::valid_plugin_id(f, plugin.id)) {
            p->state = Probe::State::Refused;
            p->reason = "not a plugin id";
        } else if (const plugin::probe::Record* r = scanner_.catalog()->find(id, channels)) {
            // Scanned already, this run or an earlier one: no probe.
            p->take(*r);
        }
    }
    return p.get();
}

void PluginHosting::start_probes() {
    for (auto& [key, p] : probes_) {
        if (p->state != Probe::State::Queued) continue;
        // Deduplicated in the worker with any probe queued or running for
        // the same key, and run ahead of its scan.
        scanner_.probe(p->id, p->channels);
        p->state = Probe::State::Running;
    }
}

bool PluginHosting::poll_probes() {
    bool finished = false;
    const auto& catalog = scanner_.catalog();
    if (catalog->serial != catalog_serial_) {
        // A rescan can change a plugin under the same id (an upgrade, a
        // bundle replaced): the answer taken from its old record is dropped
        // and taken again from the new one, or asked of the worker when the
        // catalog no longer has it. Running chains are not touched: each
        // keeps the table its host loaded until its next start.
        catalog_serial_ = catalog->serial;
        for (auto it = probes_.begin(); it != probes_.end();) {
            const Probe& p = *it->second;
            const plugin::probe::Record* r = p.record.empty() ? nullptr : catalog->find(p.id, p.channels);
            if (p.record.empty() || (r && plugin::probe::encode(*r) == p.record)) {
                ++it;
                continue;
            }
            it = probes_.erase(it);
            ++answers_dropped_;
            finished = true;
        }
    }
    for (auto& [key, p] : probes_) {
        if (p->state != Probe::State::Running) continue;
        const plugin::probe::Record* r = catalog->find(p->id, p->channels);
        if (!r) continue;
        p->take(*r);
        finished = true;
    }
    return finished;
}

std::vector<PluginHosting::Member> PluginHosting::document_members(const model::TrackEdit& track) {
    std::vector<const model::Insert*> active;
    for (const model::Insert& in : track.inserts)
        if (in.active) active.push_back(&in);
    std::sort(active.begin(), active.end(), [](const model::Insert* a, const model::Insert* b) { return a->slot < b->slot; });
    std::vector<Member> out;
    for (const model::Insert* in : active) out.push_back({in->id, in->plugin});
    return out;
}

bool PluginHosting::probe_answer_for_test(const std::string& host_id, std::vector<std::string>& keys) const {
    for (auto it = probes_.lower_bound({host_id, 0}); it != probes_.end() && it->first.first == host_id; ++it) {
        if (it->second->state != Probe::State::Ok) continue;
        keys = it->second->keys;
        return true;
    }
    return false;
}

// ---- the document

bool PluginHosting::desired_spec(const model::TrackEdit& track, const model::Snapshot& doc, Chain& c) {
    (void)doc;
    c.missing.clear();
    c.refused.clear();
    bool complete = true;
    std::vector<const model::Insert*> active;
    for (const model::Insert& in : track.inserts)
        if (in.active) active.push_back(&in);
    std::sort(active.begin(), active.end(), [](const model::Insert* a, const model::Insert* b) { return a->slot < b->slot; });
    for (const model::Insert* in : active) {
        Probe* p = probe_for(in->plugin, track.channels);
        switch (p->state) {
            case Probe::State::Queued:
            case Probe::State::Running: complete = false; break;
            case Probe::State::Missing: c.missing[in->id] = where(track, *in) + " is not installed"; break;
            case Probe::State::Refused:
                c.refused[in->id] = where(track, *in) + " was refused: " + p->reason;
                break;
            case Probe::State::Ok:
                if (auto it = c.host_refused.find(in->id); it != c.host_refused.end()) c.refused[in->id] = it->second;
                break;
        }
    }
    return complete;
}

void PluginHosting::start_pending(Chain& c, const model::Snapshot& doc, const std::vector<Member>& want) {
    const model::TrackEdit* track = nullptr;
    for (const auto& t : doc.edit->tracks)
        if (t->id == c.track) track = t.get();
    if (!track) return;
    std::vector<Member> spec_members;
    std::map<model::Id, model::InsertMix> seeded;
    std::map<model::Id, Chain::LostState> lost;
    plugin_client::ChainSpec spec;
    spec.channels = track->channels;
    // The folder the saved states' files resolve under (ADR-0029 §6).
    spec.session_dir = session_dir();
    std::vector<const model::Insert*> active;
    for (const model::Insert& in : track->inserts)
        if (in.active && !c.missing.count(in.id) && !c.refused.count(in.id)) active.push_back(&in);
    std::sort(active.begin(), active.end(), [](const model::Insert* a, const model::Insert* b) { return a->slot < b->slot; });
    for (const model::Insert* in : active) {
        Probe* p = probe_for(in->plugin, track->channels);
        spec.plugins.push_back(host_id(in->plugin));
        spec.param_counts.push_back(uint32_t(p->keys.size()));
        // §5.1: seeded from the scan's defaults, then the document's values
        // by stable key, so the host instantiates with them.
        std::vector<float> values = p->defaults;
        const model::InsertMix* m = doc.mix.find_insert(in->id);
        if (m) {
            for (const model::ParamValue& v : m->params) {
                const auto k = std::find(p->keys.begin(), p->keys.end(), v.key);
                if (k != p->keys.end()) values[std::size_t(k - p->keys.begin())] = v.value;
            }
        }
        spec.values.insert(spec.values.end(), values.begin(), values.end());
        spec.bypass.push_back(m && m->bypass ? 1.f : 0.f);
        std::string state;
        if (m && m->state) {
            // The last state saved (§6.1), restored in Load; a file that is
            // gone or does not match loads the plugin with defaults plus its
            // params, as at open. Sized first: a file past one state's cap
            // is not one, and reading it would stall the control loop.
            // Each way it fails is said (BACKLOG Track 4 item 8).
            const std::string path = *session_.folder() + "/" + m->state->file;
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            std::string why;
            if (ec) {
                why = std::filesystem::exists(path, ec) ? "its file could not be read" : "its file is missing";
            } else if (size > pc::kMaxStateBytes) {
                why = "its file is " + std::to_string(size) + " bytes, over the " +
                      std::to_string(pc::kMaxStateBytes >> 20) + " MiB a plugin state may be";
            } else {
                std::ifstream f(path, std::ios::binary);
                std::ostringstream ss;
                ss << f.rdbuf();
                if (!f && size != 0) why = "its file could not be read";
                else if (!pc::state_matches(ss.str(), m->state->sha256)) why = "its file does not match its checksum";
                else state = ss.str();
            }
            if (why.empty()) {
                // Hashed when the session opened (verify_plugin_files), so a
                // restart during a take reads no sample library again.
                const std::string file_why = plugin_files_.problem(*session_.folder(), m->state->files);
                if (!file_why.empty()) {
                    why = "a file it names: " + file_why;
                    state.clear();
                }
            }
            if (!why.empty()) lost[in->id] = Chain::LostState{*m->state, std::move(why)};
        }
        spec.states.push_back(std::move(state));
        spec.scratch_dirs.push_back(scratch_for(in->id, where(*track, *in)));
        spec_members.push_back({in->id, in->plugin});
        seeded[in->id] = m ? *m : model::InsertMix{in->id, false, {}, {}};
    }
    abandon_pending(c);
    if (spec_members.empty()) return;
    c.pending_state_lost = std::move(lost);
    c.pstore = take_store();
    c.pnode = take_node();
    if (c.pstore == kNone || c.pnode == kNone) {
        retire(nullptr, c.pstore, c.pnode);
        c.pstore = c.pnode = kNone;
        c.start_failed = "no parameter store left for this chain";
        c.failed_spec = want;
        return;
    }
    c.pending = std::make_unique<SupervisedChain>(*supervisor_, std::move(spec), c.pstore, c.pnode);
    c.pending_spec = std::move(spec_members);
    c.pending_doc = document_members(*track);
    c.pending_seeded = std::move(seeded);
    std::string error;
    if (c.pending->begin(error)) return;
    // Not even spawned. A state over its cap is that instance's refusal
    // (change C: shown, the rest started again); anything else is a failed
    // start, not tried again for this spec.
    const int32_t r = c.pending->refused() ? c.pending->refused_instance() : -1;
    if (r >= 0 && uint32_t(r) < c.pending_spec.size()) {
        const Member& m = c.pending_spec[uint32_t(r)];
        c.host_refused[m.insert] = (m.plugin.name.empty() ? m.plugin.id : m.plugin.name) + " was refused: " + error;
        reshow_ = true;
    } else {
        c.start_failed = error;
        c.failed_spec = want;
    }
    abandon_pending(c);
}

void PluginHosting::show(const model::Snapshot& doc, bool opened) {
    shown_ = doc;
    if (opened) {
        // A session opened, even the one already open (B3 on #80): ids are
        // per session, so no host of the document it replaces is kept, and
        // every chain starts from its saved state, silent until Ready (item
        // 1). The old hosts play until the first plan without them and
        // leave by epoch; their open gestures belong to the old document.
        for (auto& [track, c] : chains_) drop_chain(*c);
        chains_.clear();
        loading_total_ = 0;
        // Keyed by insert id too: the new document's insert of the same id
        // is another insert, with nothing refused yet (#115 review).
        work_refused_.clear();
        param_unmapped_.clear();
        touches_lost_.clear();
        notices_.clear();
        // Held notices are the session's, by insert id: none of the last
        // one's is replayed to an interface attaching to this one. The run's
        // own (key 0) stays.
        for (auto it = held_.begin(); it != held_.end();) it = it->first != 0 ? held_.erase(it) : std::next(it);
        // The new session's insert ids are its own: their scratch folders
        // are new ones, never the last session's files.
        ++session_serial_;
        scratch_failed_.clear();
    }
    follow_session_dir();
    // An insert the document no longer has keeps no held notice.
    for (auto it = held_.begin(); it != held_.end();)
        it = it->first != 0 && !insert_in(doc, it->first) ? held_.erase(it) : std::next(it);
    if (opened) verify_plugin_files(doc);
    std::set<model::Id> seen;
    for (const auto& tp : doc.edit->tracks) {
        const model::TrackEdit& t = *tp;
        const bool any_active =
            std::any_of(t.inserts.begin(), t.inserts.end(), [](const model::Insert& in) { return in.active; });
        if (!any_active) continue;
        seen.insert(t.id);
        auto& slot = chains_[t.id];
        if (!slot) {
            slot = std::make_unique<Chain>();
            slot->track = t.id;
            slot->opened = opened;
            if (opened) ++loading_total_;
        }
        Chain& c = *slot;
        c.channels = t.channels;
        // A refusal a host reported stands until the document changes that
        // insert (change C); an insert gone from the chain forgets it.
        for (auto it = c.host_refused.begin(); it != c.host_refused.end();) {
            const bool still = std::any_of(t.inserts.begin(), t.inserts.end(),
                                           [&](const model::Insert& in) { return in.id == it->first && in.active; });
            it = still ? std::next(it) : c.host_refused.erase(it);
        }
        if (!ok_) continue;
        // The running chain follows the document for the inserts it has,
        // whatever starts beside it, and while a probe runs too.
        post_values(c, doc);
        if (c.running && !c.running->down() && !c.running_doc.empty() && c.running_doc == document_members(t)) {
            // The document still describes the running chain: it stands,
            // whatever a rescan made of its plugins since (code-reviewer on
            // #104). Answers dropped meanwhile are asked again, for its next
            // start; a start the document no longer wants is abandoned
            // (change H).
            for (const Member& m : c.running_doc) probe_for(m.plugin, t.channels);
            abandon_pending(c);
            c.start_failed.clear();
            c.failed_spec.clear();
            continue;
        }
        if (!desired_spec(t, doc, c)) continue;  // a probe still runs
        std::vector<Member> want;
        std::vector<const model::Insert*> active;
        for (const model::Insert& in : t.inserts)
            if (in.active && !c.missing.count(in.id) && !c.refused.count(in.id)) active.push_back(&in);
        std::sort(active.begin(), active.end(), [](const model::Insert* a, const model::Insert* b) { return a->slot < b->slot; });
        for (const model::Insert* in : active) want.push_back({in->id, in->plugin});
        c.nothing_to_run = want.empty();
        if (want.empty()) {
            // No instance left: no chain, dry, its faults shown (change B).
            close_gestures(c, doc, deferred_ops_);
            drop_chain(c);
            c.start_failed.clear();
            c.failed_spec.clear();
            continue;
        }
        const bool running_matches = c.running && !c.running->down() && c.running_spec == want;
        if (running_matches) {
            // An edit undone before its new host was Ready: that start is
            // abandoned, the old binding never stopped (change H).
            abandon_pending(c);
            c.start_failed.clear();
            c.failed_spec.clear();
            continue;
        }
        if (c.pending && c.pending_spec == want) continue;
        // Down after its restarts (§5.3) stays down, silent at its last
        // latency, until the document wants another chain: never respawned
        // by the next document change.
        if (c.running && c.running->down() && c.running_spec == want) continue;
        // A start that failed for this very spec is not tried again on
        // every document change or probe (M4 on #80): its fault stands until
        // the document wants another spec.
        if (!c.start_failed.empty() && c.failed_spec == want) continue;
        c.start_failed.clear();
        c.failed_spec.clear();
        start_pending(c, doc, want);
    }
    // Tracks with no active insert left, or gone: their chains leave by
    // epoch.
    for (auto it = chains_.begin(); it != chains_.end();) {
        if (seen.count(it->first)) {
            ++it;
            continue;
        }
        Chain& c = *it->second;
        close_gestures(c, doc, deferred_ops_);
        drop_chain(c);
        it = chains_.erase(it);
    }
    recompute_faults(doc);
}

uint64_t PluginHosting::post_epoch(const Chain& c) const {
    if (c.store == kNone || c.store_epoch == 0) return 0;
    // The store changes only with a swap, which starts store_epoch again,
    // so every plan from store_epoch on names it. The playing plan when it
    // is one of them: the next callback takes the change. Before that, the
    // first plan with the store, published or not: held until the audio
    // thread adopts it (PR #77 RT audit M1).
    const PlaybackPlan* p = session_.plans().current();
    return std::max(c.store_epoch, p ? p->epoch : 0);
}

void PluginHosting::post(Chain& c, uint32_t instance, uint32_t param, float value) {
    const uint64_t epoch = post_epoch(c);
    if (epoch != 0 && session_.post_plugin_param(epoch, c.store, instance, param, value)) {
        // A newer value went: an older one waiting must not follow it.
        c.retry.erase({instance, param});
        return;
    }
    // A full ring, or no plan with the store yet: coalesced, the newest
    // value kept, never lost (#68 review item 3).
    c.retry[{instance, param}] = value;
}

void PluginHosting::flush_retry(Chain& c) {
    if (!c.running || c.running->down() || post_epoch(c) == 0) return;
    const uint64_t epoch = post_epoch(c);
    for (auto it = c.retry.begin(); it != c.retry.end();) {
        const auto [instance, param] = it->first;
        it = session_.post_plugin_param(epoch, c.store, instance, param, it->second) ? c.retry.erase(it)
                                                                                   : std::next(it);
    }
}

void PluginHosting::post_values(Chain& c, const model::Snapshot& doc, bool force) {
    if (!c.running || c.running->down() || !c.running->host()) return;
    if (post_epoch(c) == 0) {
        // The store is in no plan yet (just swapped in): posted once built()
        // names the plan that has it, against what it was seeded with.
        c.need_post = true;
        return;
    }
    c.need_post = false;
    const pc::Loaded& l = c.running->host()->loaded();
    for (uint32_t i = 0; i < c.running_spec.size() && i < l.instances.size(); ++i) {
        const model::Id id = c.running_spec[i].insert;
        const model::InsertMix* m = doc.mix.find_insert(id);
        const model::InsertMix now = m ? *m : model::InsertMix{id, false, {}, {}};
        model::InsertMix& was = c.posted[id];
        const bool silenced = c.solo != 0 && id != c.solo;
        const bool bypass = now.bypass || silenced;
        const bool was_bypass = was.bypass || c.solo_posted.count(id) != 0;
        if (force || bypass != was_bypass) post(c, i, plugin::kBypassParam, bypass ? 1.f : 0.f);
        if (silenced) {
            c.solo_posted.insert(id);
        } else {
            c.solo_posted.erase(id);
        }
        for (const model::ParamValue& v : now.params) {
            const auto old = std::find_if(was.params.begin(), was.params.end(),
                                          [&](const model::ParamValue& p) { return p.key == v.key; });
            if (!force && old != was.params.end() && old->value == v.value) continue;
            const auto& keys = l.instances[i].params;
            const auto d = std::find_if(keys.begin(), keys.end(), [&](const pc::ParamDesc& p) { return p.key == v.key; });
            if (d == keys.end()) continue;
            post(c, i, uint32_t(d - keys.begin()), std::min(std::max(v.value, d->min), d->max));
        }
        was = now;
    }
}

bool PluginHosting::take_messages(Chain& c, const model::Snapshot& doc, std::vector<model::Operation>& ops) {
    if (!c.running) return false;
    bool latency = false;
    for (const std::string& m : c.running->take_host_messages()) {
        pc::Type t{};
        if (pc::peek_type(m, t) && t == pc::Type::LatencyChanged) {
            latency = c.running->refresh_latency() || latency;
            continue;
        }
        handle_host_message(c.track, m, doc, ops);
    }
    return latency;
}

void PluginHosting::close_gestures(Chain& c, const model::Snapshot& doc, std::vector<model::Operation>& ops) {
    for (const auto& [key, t] : c.open_touches)
        if (insert_in(doc, t.insert)) record_value(ops, doc, t.insert, t.bypass, t.key, t.value);
    c.open_touches.clear();
}

// ---- the loop

PluginHosting::PollResult PluginHosting::poll(uint64_t now_ms, const model::Snapshot& doc, uint64_t oldest_live_epoch) {
    PollResult result;
    result.ops.swap(deferred_ops_);
    // A Save that renamed or moved the folder: a restart from now on names
    // the new one.
    follow_session_dir();
    // A probe that finished lets the chains waiting on it start.
    const bool probes_moved = poll_probes();
    start_probes();

    bool reshow = reshow_;
    reshow_ = false;
    for (auto& [track, cp] : chains_) {
        Chain& c = *cp;
        if (c.pending) {
            switch (c.pending->poll(now_ms)) {
                case SupervisedChain::Event::Swapped: {
                    // The new host is Ready: it replaces the old binding in
                    // the next plan; the old one leaves by epoch, after what
                    // it sent and the gestures it leaves open are taken.
                    if (take_messages(c, doc, result.ops)) result.replan = true;
                    close_gestures(c, doc, result.ops);
                    fail_state_requests(c.track, "its chain was rebuilt (an edit) before it answered");
                    retire(std::move(c.running), c.store, c.node);
                    c.running = std::move(c.pending);
                    c.store = c.pstore;
                    c.node = c.pnode;
                    c.pstore = c.pnode = kNone;
                    c.running_spec = std::move(c.pending_spec);
                    c.pending_spec.clear();
                    c.running_doc = std::move(c.pending_doc);
                    c.pending_doc.clear();
                    c.ever_ready = true;
                    c.start_failed.clear();
                    c.failed_spec.clear();
                    c.restart_swapped = false;
                    c.restart_retire_epoch = 0;
                    // What the store was seeded with, not the document now:
                    // whatever moved while the host started is posted once a
                    // plan has the store (B2 on #80).
                    c.posted = std::move(c.pending_seeded);
                    c.pending_seeded.clear();
                    // A new store, seeded from the document: a standing
                    // override is posted to it with the rest (need_post).
                    c.solo_posted.clear();
                    c.state_lost = std::move(c.pending_state_lost);
                    c.pending_state_lost.clear();
                    c.work_noticed.clear();
                    c.unmapped_noticed.clear();
                    c.paths_noticed.clear();
                    c.lost_noticed = false;
                    note_refused_states(c, doc);
                    c.retry.clear();
                    c.store_epoch = 0;
                    c.named = false;
                    c.need_post = true;
                    result.replan = true;
                    break;
                }
                case SupervisedChain::Event::Down: {
                    if (c.pending->refused() && c.pending->refused_instance() >= 0 &&
                        uint32_t(c.pending->refused_instance()) < c.pending_spec.size()) {
                        // Change C: the refused instance stays out, the rest
                        // starts again.
                        const Member& m = c.pending_spec[uint32_t(c.pending->refused_instance())];
                        c.host_refused[m.insert] = (m.plugin.name.empty() ? m.plugin.id : m.plugin.name) +
                                                   " was refused: " + c.pending->why();
                        reshow = true;
                    } else {
                        c.start_failed = c.pending->why();
                        c.failed_spec = c.pending_spec;
                        result.replan = true;
                    }
                    abandon_pending(c);
                    break;
                }
                default: break;
            }
        }
        if (c.running) {
            bool host_lost = false;
            switch (c.running->poll(now_ms)) {
                case SupervisedChain::Event::Swapped:
                    // A restart is Ready (§5.3): the next plan names the new
                    // slot; the dead one leaves by epoch.
                    c.restart_swapped = true;
                    c.restart_retire_epoch = 0;
                    result.replan = true;
                    break;
                case SupervisedChain::Event::Restarting: host_lost = true; break;
                case SupervisedChain::Event::Down:
                    host_lost = true;
                    result.replan = true;
                    break;
                default: break;
            }
            if (take_messages(c, doc, result.ops)) result.replan = true;
            // The host died mid-gesture: its touches are in the store (kept
            // across the restart, change F), so the document gets them too.
            if (host_lost) {
                close_gestures(c, doc, result.ops);
                // What it answered is in hand; the rest never will be.
                fail_state_requests(c.track, "its plug-in host stopped before it answered");
            }
            if (c.need_post) post_values(c, doc);
            flush_retry(c);
            if (c.restart_swapped && c.restart_retire_epoch != 0 && oldest_live_epoch >= c.restart_retire_epoch) {
                c.running->retired();
                c.restart_swapped = false;
                c.restart_retire_epoch = 0;
            }
        }
    }

    follow_state_requests(now_ms);

    // A change the audio thread dropped: posted for a plan it replaced
    // before the change was drained (the plan moved between the post and
    // the next callback, or a plan built was cancelled), or for a store no
    // runnable node of its plan names. The running chains' document values,
    // every one and the bypass included, posted again under the playing
    // plan, at most every 100 ms. Held changes for a plan not yet adopted
    // are not drops, so this does not fire while a plan waits for residency.
    // It cannot churn on its own: a re-post is dropped only if yet another
    // plan replaces the playing one before the next callback, so it repeats
    // only while plans keep being published, at 10 Hz at most.
    const uint64_t dropped = session_.plugin_params_dropped();
    if (dropped != dropped_seen_ && now_ms - last_repost_ms_ >= 100) {
        dropped_seen_ = dropped;
        last_repost_ms_ = now_ms;
        for (auto& [track, cp] : chains_) post_values(*cp, doc, true);
    }

    // Retiring by epoch (item 2).
    for (auto it = retiring_.begin(); it != retiring_.end();) {
        Retiring& r = *it;
        if (r.first_epoch_without == 0 || oldest_live_epoch < r.first_epoch_without) {
            // Its hosts are still in watch(), Readable and, while frames
            // wait, Writable: one left unserviced would end every sleep of
            // the control thread at once (the #198 review's spin; ADR-0213).
            r.chain->poll_parked(now_ms);
            ++it;
            continue;
        }
        r.chain->request_stop(now_ms);
        if (!r.chain->stopped(now_ms)) {
            ++it;
            continue;
        }
        r.chain.reset();
        if (r.store != kNone) free_stores_.push_back(r.store);
        if (r.node != kNone) free_nodes_.push_back(r.node);
        it = retiring_.erase(it);
    }
    // Starts let go: reaped once they have exited, never waited for.
    for (auto it = stopping_.begin(); it != stopping_.end();)
        it = (*it)->stopped(now_ms) ? stopping_.erase(it) : std::next(it);
    // Hosts of a chain destroyed without being asked first; none on the
    // paths above, which ask, but reaped here rather than at teardown.
    if (supervisor_) supervisor_->reap(now_ms);

    if (reshow || probes_moved) show(doc, false);
    else recompute_faults(doc);
    // Gestures show() closed belong to this turn's operations.
    result.ops.insert(result.ops.end(), std::make_move_iterator(deferred_ops_.begin()),
                      std::make_move_iterator(deferred_ops_.end()));
    deferred_ops_.clear();
    return result;
}

void PluginHosting::resolve(const model::TrackEdit& track, uint32_t, ChainBinding& b) {
    const auto it = chains_.find(track.id);
    if (!ok_) {
        // §5.5: hosting disabled plays every chain silent, faulted.
        b.channel_slot = kSilentChannelSlot;
        b.latency_frames = 0;
        return;
    }
    if (it == chains_.end()) return;  // unbound
    Chain& c = *it->second;
    if (c.running && !c.running->down()) {
        // Running, or restarting on the dead binding (change A).
        const plugin_client::InsertChainRef r = c.running->node();
        b.channel_slot = r.channel_slot;
        b.channel_generation = r.channel_generation;
        b.param_store = r.param_store;
        b.node_state = r.node_state;
        b.latency_frames = c.running->latency_frames();
        c.named = true;
        return;
    }
    if (c.running) {
        // Down: silent at its last checked latency (change A).
        b.channel_slot = kSilentChannelSlot;
        b.node_state = c.node;
        b.latency_frames = c.running->latency_frames();
        return;
    }
    if (c.opened && !c.ever_ready && !c.nothing_to_run) {
        // Starting with the session (its probes or its host), or its start
        // failed: silent, nothing known yet (item 1).
        b.channel_slot = kSilentChannelSlot;
        b.latency_frames = 0;
        return;
    }
    if (!c.start_failed.empty()) {
        b.channel_slot = kSilentChannelSlot;
        b.latency_frames = 0;
    }
    // New on the track, or nothing left of it: unbound, dry and aligned.
}

bool PluginHosting::resolve_lane(const model::TrackEdit& track, model::Id insert, const std::string& key,
                                 LaneTarget& target) const {
    if (!ok_) return false;
    const auto it = chains_.find(track.id);
    if (it == chains_.end()) return false;
    const Chain& c = *it->second;
    // What resolve() binds a store for; a down chain plays silent, with no
    // store a change could reach.
    if (!c.running || c.running->down() || !c.running->host()) return false;
    const pc::Loaded& l = c.running->host()->loaded();
    for (uint32_t i = 0; i < c.running_spec.size() && i < l.instances.size(); ++i) {
        if (c.running_spec[i].insert != insert) continue;
        const auto& keys = l.instances[i].params;
        const auto d = std::find_if(keys.begin(), keys.end(), [&](const pc::ParamDesc& p) { return p.key == key; });
        if (d == keys.end()) return false;
        target.instance = i;
        target.param = uint32_t(d - keys.begin());
        target.min = d->min;
        target.max = d->max;
        target.stepped = d->stepped;
        return true;
    }
    return false;
}

void PluginHosting::built(uint64_t epoch) {
    last_built_epoch_ = epoch;
    for (Retiring& r : retiring_)
        if (r.first_epoch_without == 0) r.first_epoch_without = epoch;
    for (auto& [track, c] : chains_) {
        if (c->restart_swapped && c->restart_retire_epoch == 0) c->restart_retire_epoch = epoch;
        // The first plan with the running store: changes for it are tagged
        // from here on (post_epoch).
        if (c->named && c->store_epoch == 0) c->store_epoch = epoch;
        c->named = false;
    }
}

bool PluginHosting::set_param(model::Id insert, const std::string& key, float value, std::string& why) {
    for (auto& [track, cp] : chains_) {
        Chain& c = *cp;
        for (uint32_t i = 0; i < c.running_spec.size(); ++i) {
            if (c.running_spec[i].insert != insert) continue;
            if (!c.running || c.running->down() || !c.running->host()) {
                why = "insert " + std::to_string(insert) + " is not running";
                return false;
            }
            // The running instance's own table (§8), never the catalog's.
            const pc::Loaded& l = c.running->host()->loaded();
            if (i >= l.instances.size()) break;
            const auto& keys = l.instances[i].params;
            const auto d = std::find_if(keys.begin(), keys.end(), [&](const pc::ParamDesc& p) { return p.key == key; });
            if (d == keys.end()) {
                why = "the plugin has no parameter \"" + key + "\"";
                return false;
            }
            if (!(value >= d->min && value <= d->max)) {
                why = key + " must be from " + std::to_string(d->min) + " to " + std::to_string(d->max);
                return false;
            }
            post(c, i, uint32_t(d - keys.begin()), value);
            return true;
        }
    }
    why = "insert " + std::to_string(insert) + " is not running";
    return false;
}

void PluginHosting::handle_host_message(model::Id track, const std::string& bytes, const model::Snapshot& doc,
                                        std::vector<model::Operation>& ops) {
    const auto it = chains_.find(track);
    if (it == chains_.end() || !it->second->running || !it->second->running->host()) return;
    Chain& c = *it->second;
    const pc::Loaded& l = c.running->host()->loaded();
    pc::Type t{};
    std::string e;
    if (!pc::peek_type(bytes, t)) return;
    // What the document needs to record (instance, param): the insert, and
    // the bypass or the parameter's stable key. False for neither.
    const auto describe = [&](uint32_t instance, uint32_t param, Chain::Touch& out) {
        if (instance >= c.running_spec.size() || instance >= l.instances.size()) return false;
        out.insert = c.running_spec[instance].insert;
        out.bypass = param == plugin::kBypassParam;
        if (out.bypass) return true;
        if (param >= l.instances[instance].params.size()) return false;
        out.key = l.instances[instance].params[param].key;
        return true;
    };
    const auto record = [&](uint32_t instance, uint32_t param, float value) {
        Chain::Touch d;
        if (describe(instance, param, d)) record_value(ops, doc, d.insert, d.bypass, d.key, value);
    };
    switch (t) {
        case pc::Type::ParamTouched: {
            pc::ParamTouched m;
            if (!pc::decode(bytes, m, e)) break;
            // The host never applies a touch itself (host_control.h): the
            // value goes to the chain tagged, through the ring, and is
            // remembered until its gesture ends.
            post(c, m.instance, m.param, m.value);
            Chain::Touch d;
            if (describe(m.instance, m.param, d)) {
                d.value = m.value;
                c.open_touches[{m.instance, m.param}] = std::move(d);
            }
            break;
        }
        case pc::Type::GestureEnd: {
            pc::GestureEnd m;
            if (!pc::decode(bytes, m, e)) break;
            post(c, m.instance, m.param, m.value);
            record(m.instance, m.param, m.value);
            c.open_touches.erase({m.instance, m.param});
            break;
        }
        case pc::Type::EditorClosed: {
            pc::EditorClosed m;
            if (!pc::decode(bytes, m, e)) break;
            for (const pc::TouchedValue& v : m.moved) record(m.instance, v.param, v.value);
            for (auto o = c.open_touches.begin(); o != c.open_touches.end();)
                o = o->first.first == m.instance ? c.open_touches.erase(o) : std::next(o);
            break;
        }
        case pc::Type::State: take_state_reply(track, bytes); break;
        case pc::Type::Fault: {
            pc::Fault m;
            if (!pc::decode(bytes, m, e)) break;
            const auto add = [](uint64_t& n, uint64_t count) {
                n = count > std::numeric_limits<uint64_t>::max() - n ? std::numeric_limits<uint64_t>::max()
                                                                     : n + count;
            };
            const model::TrackEdit* t = nullptr;
            for (const auto& tp : doc.edit->tracks)
                if (tp->id == track) t = tp.get();
            if (m.kind == pc::FaultKind::EventsDropped) {
                add(touches_lost_[track], m.count);
                if (c.lost_noticed || !t) break;
                c.lost_noticed = true;
                notices_.push_back(t->name +
                                   ": parameter moves from a plug-in editor on this track were lost on the way to "
                                   "the engine; the plug-in's values were read back and recorded, but one move "
                                   "may have become more than one undo step");
                break;
            }
            // Wire 4: with a count, the plugin's path calls (makePath,
            // mapPath) the host refused; it plays on, but a file it meant to
            // keep may be missing from the session, which the operator must
            // hear before relying on it. Said once a run, with the host's text.
            if (m.kind == pc::FaultKind::Plugin && m.count > 0) {
                if (m.instance < 0 || uint32_t(m.instance) >= c.running_spec.size() || !t) break;
                const model::Id insert = c.running_spec[uint32_t(m.instance)].insert;
                if (!c.paths_noticed.insert(insert).second) break;
                // Held (ADR-0029 §6): the operator must know before relying
                // on the session that a file the plugin keeps may be missing.
                for (const model::Insert& in : t->inserts)
                    if (in.id == insert)
                        hold(insert, where(*t, in) + ": the host refused a file path its plug-in asked for (" + m.text +
                                         "), so a file it keeps may not be saved with the session");
                break;
            }
            if (m.kind != pc::FaultKind::WorkRefused && m.kind != pc::FaultKind::ParamUnmapped) break;
            if (m.instance < 0 || uint32_t(m.instance) >= c.running_spec.size()) break;
            const model::Id insert = c.running_spec[uint32_t(m.instance)].insert;
            const bool work = m.kind == pc::FaultKind::WorkRefused;
            add(work ? work_refused_[insert] : param_unmapped_[insert], m.count);
            // The plugin plays on, so nothing stands: said once a run, and
            // the count carries the rest (ADR-0012 change G).
            if (!(work ? c.work_noticed : c.unmapped_noticed).insert(insert).second || !t) break;
            for (const model::Insert& in : t->inserts) {
                if (in.id != insert) continue;
                notices_.push_back(where(*t, in) +
                                   (work ? ": its worker refused messages (its queue was full), so work the "
                                           "plugin asked for may not have been done"
                                         : ": its editor changed a parameter it does not offer for recording "
                                           "(read-only or undeclared), so the session did not record that "
                                           "change; check it after the session is reopened"));
            }
            break;
        }
        default: break;
    }
}

void PluginHosting::watch(std::vector<platform::WaitSet::Watch>& out) const {
    out.clear();
    if (!supervisor_) return;
    std::vector<plugin_client::HostWatch> hosts;
    supervisor_->watch(hosts);
    for (const plugin_client::HostWatch& h : hosts)
        out.push_back({h.handle, h.writable ? platform::WaitSet::When::Writable : platform::WaitSet::When::Readable});
}

// ---- FlushPluginState (§8)

uint64_t PluginHosting::request_states(model::Id only, const model::Snapshot& doc, bool no_copy) {
    const uint64_t id = next_flush_++;
    Flush& f = flushes_[id];
    f.id = id;
    // ADR-0029 §6: the hosts save the files a state names into the session
    // folder, and name them relative to it.
    f.save_dir = session_dir();
    for (auto& [track, cp] : chains_) {
        Chain& c = *cp;
        const model::TrackEdit* t = nullptr;
        for (const auto& tp : doc.edit->tracks)
            if (tp->id == c.track) t = tp.get();
        for (uint32_t i = 0; i < c.running_spec.size(); ++i) {
            const model::Id insert = c.running_spec[i].insert;
            if (only != 0 && insert != only) continue;
            std::string who = "insert " + std::to_string(insert);
            if (t) {
                for (const model::Insert& in : t->inserts)
                    if (in.id == insert) who = where(*t, in);
            }
            if (!c.running || c.running->down()) {
                f.failed.push_back(who + ": its plug-in host is down, so its state could not be read");
                continue;
            }
            if (c.running->restarting() || !c.running->host()) {
                f.failed.push_back(who + ": its plug-in host is restarting; save again once it runs");
                continue;
            }
            if (next_state_request_ == 0) next_state_request_ = 1;
            const uint32_t request = next_state_request_++;
            pc::GetState ask;
            ask.request = request;
            ask.instance = i;
            ask.save_dir = f.save_dir;
            // During a take the hosts copy and hash nothing (ADR-0029 §6): no
            // bulk I/O competes with the disk thread.
            ask.flags = no_copy ? pc::kGetStateNoCopy : 0u;
            PluginStateRequests::Request r;
            r.flush = id;
            r.insert = insert;
            r.track = c.track;
            r.host = c.running->host();
            r.where = who;
            pc::encode(ask, r.frame);
            // A save that may copy plugin files waits for them (host_control.h)
            // and goes one host at a time (ADR-0151 §1); each deadline starts
            // when its host begins on it (plugin_hosting_state.h).
            r.timeout_ms = pc::state_reply_timeout_ms(ask);
            r.copies = !ask.save_dir.empty() && !(ask.flags & pc::kGetStateNoCopy);
            state_requests_.add(request, std::move(r));
        }
    }
    send_due_states(steady_ms());
    finish_flush_if_done(id);
    return id;
}

void PluginHosting::send_due_states(uint64_t now_ms) {
    // A send that fails frees the copying turn, so ask again until none is due.
    for (auto due = state_requests_.due(now_ms); !due.empty(); due = state_requests_.due(now_ms)) {
        for (auto& [request, frame] : due) {
            const PluginStateRequests::Request* r = state_requests_.find(request);
            std::string e;
            if (!r || r->host->send(frame, e)) continue;
            const PluginStateRequests::Request gone = state_requests_.remove(request, now_ms);
            flushes_[gone.flush].failed.push_back(gone.where + ": its state could not be asked for: " + e);
            finish_flush_if_done(gone.flush);
        }
    }
}

void PluginHosting::take_state_reply(model::Id track, const std::string& bytes) {
    pc::State m;
    std::string e;
    if (!pc::decode(bytes, m, e)) return;
    // The supervisor accepted it as the answer to a GetState this host has
    // outstanding; ids are unique across hosts, so it is the one asked.
    PluginStateRequests::Request r;
    if (!state_requests_.answered(m.request, track, steady_ms(), r)) return;
    Flush& f = flushes_[r.flush];
    switch (m.status) {
        case pc::StateStatus::Ok: {
            // The host's list is untrusted: each file is checked against the
            // rule a session file is (model::plugin_file_problem, the byte
            // caps included) before anything is joined to the folder. The
            // saver checks each is there with its size and syncs it.
            StateRead read;
            read.insert = r.insert;
            std::string bad;
            for (const pc::StateFile& file : m.files) {
                read.files.push_back(model::PluginFileRef{file.path, file.sha256, file.bytes});
                const std::string why = model::plugin_file_problem(read.files.back());
                if (!why.empty() && bad.empty()) bad = file.path.substr(0, 256) + ": " + why;
            }
            if (bad.empty()) bad = model::plugin_files_problem(read.files);
            if (!bad.empty()) {
                f.failed.push_back(r.where + ": the plug-in's state names a file the session cannot hold (" + bad +
                                   ")");
                break;
            }
            read.sha256 = sha256_hex(m.bytes);
            read.bytes = std::move(m.bytes);
            f.states.push_back(std::move(read));
            break;
        }
        case pc::StateStatus::Empty: {
            StateRead read;
            read.insert = r.insert;
            f.states.push_back(std::move(read));
            break;
        }
        case pc::StateStatus::Failed:
            f.failed.push_back(r.where + ": the plug-in could not save its state" +
                               (m.text.empty() ? std::string() : " (" + m.text + ")"));
            break;
    }
    // The copying turn this answer frees goes to the next host waiting.
    send_due_states(steady_ms());
    finish_flush_if_done(r.flush);
}

void PluginHosting::fail_state_requests(model::Id track, const std::string& why) {
    std::set<uint64_t> touched;
    for (const PluginStateRequests::Request& r : state_requests_.remove_track(track)) {
        flushes_[r.flush].failed.push_back(r.where + ": " + why);
        touched.insert(r.flush);
    }
    if (!touched.empty()) send_due_states(steady_ms());
    for (const uint64_t f : touched) finish_flush_if_done(f);
}

void PluginHosting::follow_state_requests(uint64_t now_ms) {
    // host_control.h: a GetState unanswered past state_reply_timeout_ms (the
    // one its host is serving; one still queued has not started its clock)
    // is a hung host, killed; the crash path restarts it from the session's
    // last saved state. `host` is still the bound one: anything that unbinds
    // it fails its requests first.
    for (const PluginStateRequests::Request& r : state_requests_.late(now_ms)) {
        r.host->signal_kill();
        fail_state_requests(r.track, "its plug-in did not hand over its state within " +
                                         std::to_string(r.timeout_ms / 1000) + " s, so its host was restarted");
    }
}

void PluginHosting::finish_flush_if_done(uint64_t flush) {
    if (state_requests_.flush_pending(flush)) return;
    const auto it = flushes_.find(flush);
    if (it == flushes_.end()) return;
    flushes_done_.push_back(std::move(it->second));
    flushes_.erase(it);
}

std::vector<PluginHosting::Flush> PluginHosting::take_flushes() {
    std::vector<Flush> out;
    out.swap(flushes_done_);
    return out;
}

std::vector<model::Operation> PluginHosting::states_saved(const Flush& flush, const model::Snapshot& doc) {
    std::vector<model::Operation> ops;
    for (const StateRead& s : flush.states) {
        if (!insert_in(doc, s.insert)) continue;
        std::optional<model::StateRef> want;
        if (!s.sha256.empty())
            want = model::StateRef{model::plugin_state_file(s.sha256), s.sha256, s.bytes.size(), s.files};
        // The host hashed each file as it wrote it, and the saver made them
        // durable: a restart need not hash them again.
        plugin_files_.trust(s.files);
        const model::InsertMix* m = doc.mix.find_insert(s.insert);
        const std::optional<model::StateRef> have = m ? m->state : std::nullopt;
        if (!(have == want)) ops.push_back(model::SetInsertState{s.insert, want});
        // A restart of whichever chain holds the insert now restores this,
        // the state the document is about to name.
        for (auto& [track, cp] : chains_) {
            Chain& c = *cp;
            for (uint32_t i = 0; c.running && i < c.running_spec.size(); ++i)
                if (c.running_spec[i].insert == s.insert) c.running->set_state(i, s.bytes);
            for (uint32_t i = 0; c.pending && i < c.pending_spec.size(); ++i)
                if (c.pending_spec[i].insert == s.insert) c.pending->set_state(i, s.bytes);
        }
    }
    return ops;
}

bool PluginHosting::kill_host_for_test(model::Id track) {
    const auto it = chains_.find(track);
    if (it == chains_.end() || !it->second->running || !it->second->running->host()) return false;
    it->second->running->host()->signal_kill();
    return true;
}

// ---- what the operator is told (item 5)

void PluginHosting::note_refused_states(Chain& c, const model::Snapshot& doc) {
    // host_control.h: a state the plugin refuses does not refuse the chain;
    // Loaded says so per instance, and it is shown as a hash mismatch is.
    if (!c.running || !c.running->host()) return;
    const pc::Loaded& l = c.running->host()->loaded();
    for (uint32_t i = 0; i < c.running_spec.size() && i < l.instances.size(); ++i) {
        if (l.instances[i].state != pc::StateResult::Refused) continue;
        const model::Id insert = c.running_spec[i].insert;
        const model::InsertMix* m = doc.mix.find_insert(insert);
        if (!m || !m->state || c.state_lost.count(insert)) continue;
        c.state_lost[insert] = Chain::LostState{*m->state, "the plugin refused it"};
    }
}

void PluginHosting::recompute_faults(const model::Snapshot& doc) {
    std::map<model::Id, std::string> faults;
    for (const auto& tp : doc.edit->tracks) {
        const model::TrackEdit& t = *tp;
        for (const model::Insert& in : t.inserts) {
            if (!in.active) continue;
            if (!ok_) {
                faults[in.id] = where(t, in) + " is not running: " + why_;
                continue;
            }
            const auto it = chains_.find(t.id);
            if (it == chains_.end()) continue;
            const Chain& c = *it->second;
            if (auto m = c.missing.find(in.id); m != c.missing.end()) {
                faults[in.id] = m->second;
                continue;
            }
            if (auto r = c.refused.find(in.id); r != c.refused.end()) {
                faults[in.id] = r->second;
                continue;
            }
            if (auto r = c.host_refused.find(in.id); r != c.host_refused.end()) {
                faults[in.id] = r->second;
                continue;
            }
            const auto member = [&](const std::vector<Member>& spec) {
                return std::find_if(spec.begin(), spec.end(), [&](const Member& m) { return m.insert == in.id; });
            };
            if (c.running && member(c.running_spec) != c.running_spec.end()) {
                const int32_t culprit = c.running->culprit();
                const auto idx = uint32_t(member(c.running_spec) - c.running_spec.begin());
                if (c.running->down()) {
                    if (culprit < 0 || uint32_t(culprit) == idx || uint32_t(culprit) >= c.running_spec.size()) {
                        faults[in.id] = where(t, in) + ": " + c.running->why();
                    } else {
                        const Member& m = c.running_spec[uint32_t(culprit)];
                        faults[in.id] = where(t, in) + " is down with " +
                                        (m.plugin.name.empty() ? m.plugin.id : m.plugin.name);
                    }
                    continue;
                }
                if (c.running->restarting()) {
                    faults[in.id] = where(t, in) + ": restarting: " + c.running->why();
                    continue;
                }
            }
            if (!c.start_failed.empty() && member(c.failed_spec) != c.failed_spec.end()) {
                // M4 on #80: an edit whose new host failed while the old one
                // plays is said, on every insert of the chain the edit wanted.
                faults[in.id] = where(t, in) + ": " +
                                (c.running && !c.running->down() ? "the edited chain did not start, it plays as before: "
                                                                 : "") +
                                c.start_failed;
                continue;
            }
            // Running without its saved state: said while the document still
            // names the state that was not restored. A later save (a new
            // state) or none named clears it without a restart.
            const auto lost = c.state_lost.find(in.id);
            if (lost == c.state_lost.end() || !c.running || member(c.running_spec) == c.running_spec.end()) continue;
            const model::InsertMix* m = doc.mix.find_insert(in.id);
            if (m && m->state && *m->state == lost->second.ref)
                faults[in.id] = where(t, in) + ": its saved state was not restored (" + lost->second.why +
                                "); it runs with its defaults and its saved parameter values";
        }
    }
    faults_.swap(faults);
}

uint64_t PluginHosting::work_refused(model::Id insert) const {
    const auto it = work_refused_.find(insert);
    return it == work_refused_.end() ? 0 : it->second;
}

uint64_t PluginHosting::param_unmapped(model::Id insert) const {
    const auto it = param_unmapped_.find(insert);
    return it == param_unmapped_.end() ? 0 : it->second;
}

uint64_t PluginHosting::touches_lost(model::Id track) const {
    const auto it = touches_lost_.find(track);
    return it == touches_lost_.end() ? 0 : it->second;
}

std::vector<std::string> PluginHosting::take_notices() {
    std::vector<std::string> out;
    out.swap(notices_);
    return out;
}

void PluginHosting::hold(model::Id insert, std::string text) {
    held_[insert] = std::move(text);
    held_new_.push_back(insert);
}

void PluginHosting::verify_plugin_files(const model::Snapshot& doc) {
    plugin_files_.open();
    const std::shared_ptr<const std::string> folder = session_.folder();
    if (!folder || folder->empty()) return;
    for (const auto& tp : doc.edit->tracks) {
        for (const model::Insert& in : tp->inserts) {
            const model::InsertMix* m = doc.mix.find_insert(in.id);
            if (!m || !m->state || m->state->files.empty()) continue;
            const std::string why = plugin_files_.problem(*folder, m->state->files);
            // The chain's start reads the same verdict and loads the plugin
            // without the state, never with the file (ADR-0151 §2).
            if (!why.empty())
                hold(in.id, where(*tp, in) + ": its saved state will not be restored: a file it names, " + why +
                                ". Replace the session's Plugin Files from a good copy before relying on it");
        }
    }
}

std::vector<std::string> PluginHosting::take_held_notices() {
    std::vector<std::string> out;
    for (const model::Id insert : held_new_) {
        const auto it = held_.find(insert);
        if (it != held_.end()) out.push_back(it->second);
    }
    held_new_.clear();
    return out;
}

uint32_t PluginHosting::insert_flags(model::Id insert) const {
    uint32_t flags = 0;
    for (const auto& [track, cp] : chains_) {
        const Chain& c = *cp;
        if (c.missing.count(insert)) flags |= api::kInsertMissing;
        const bool member = std::any_of(c.running_spec.begin(), c.running_spec.end(),
                                        [&](const Member& m) { return m.insert == insert; });
        if (member && c.running) {
            if (c.running->restarting()) flags |= api::kInsertRestarting;
            else if (!c.running->down()) flags |= api::kInsertRunning;
        } else if (c.pending && std::any_of(c.pending_spec.begin(), c.pending_spec.end(),
                                            [&](const Member& m) { return m.insert == insert; })) {
            flags |= api::kInsertRestarting;
        }
    }
    if (faults_.count(insert) && !(flags & api::kInsertMissing)) flags |= api::kInsertFaulted;
    return flags;
}

bool PluginHosting::chain_ready(model::Id track) const {
    if (!ok_) return false;
    const auto it = chains_.find(track);
    if (it == chains_.end()) return false;
    const Chain& c = *it->second;
    if (!c.running || c.pending || c.running->starting() || c.running->restarting() ||
        c.running->down()) {
        return false;
    }
    const plugin_client::InsertChainRef ref = c.running->node();
    if (ref.channel_slot == plugin_client::kNoSlot) return false;
    for (const Member& m : c.running_spec) {
        if (faults_.count(m.insert)) return false;
    }
    return true;
}

bool PluginHosting::render_solo(model::Id track, model::Id insert) {
    const auto it = chains_.find(track);
    if (!ok_ || it == chains_.end()) return false;
    Chain& c = *it->second;
    if (!c.running || c.running->down() || !c.running->host()) return false;
    const auto target = std::find_if(c.running_spec.begin(), c.running_spec.end(),
                                     [&](const Member& m) { return m.insert == insert; });
    // An insert the running host left out (missing, refused) prints nothing.
    if (target == c.running_spec.end()) return false;
    if (c.solo != insert) {
        c.solo = insert;
        post_values(c, shown_);
    }
    flush_retry(c);
    if (c.need_post || !c.retry.empty()) return false;
    plugin_client::HostProcess& h = *c.running->host();
    for (uint32_t i = 0; i < c.running_spec.size(); ++i) {
        if (i >= plugin::kMaxInstances) return false;
        const model::Id id = c.running_spec[i].insert;
        const model::InsertMix* m = shown_.mix.find_insert(id);
        const float want = id != insert || (m != nullptr && m->bypass) ? 1.f : 0.f;
        if (h.header().bypass[i].load(std::memory_order_relaxed) != want) return false;
    }
    return true;
}

void PluginHosting::render_unsolo(model::Id track) {
    const auto it = chains_.find(track);
    if (it == chains_.end() || it->second->solo == 0) return;
    Chain& c = *it->second;
    c.solo = 0;
    // Forced, as for a dropped change: the hosts end where the document
    // says, whatever part of the override reached them.
    post_values(c, shown_, true);
}

uint64_t PluginHosting::chain_misses(model::Id track) const {
    const auto it = chains_.find(track);
    if (it == chains_.end() || !it->second->running) return 0;
    const plugin_client::InsertChainRef ref = it->second->running->node();
    if (ref.channel_slot == plugin_client::kNoSlot) return 0;
    const plugin_client::SlotStatus s = client_->slot_status(ref.channel_slot);
    const plugin_client::NodeStatus n = client_->node_status(ref.node_state);
    return s.misses + s.nonfinite_blocks + n.silent_blocks;
}

uint32_t PluginHosting::loading() const {
    // A chain opened with the session is loading until its host is Ready,
    // its start failed, or nothing of it is left to run. With hosting
    // disabled none will ever start, so none is loading (#83 review item 1).
    if (!ok_) return 0;
    uint32_t n = 0;
    for (const auto& [track, c] : chains_)
        n += c->opened && !c->ever_ready && c->start_failed.empty() && !c->nothing_to_run ? 1 : 0;
    return n;
}

uint32_t PluginHosting::hosts() const {
    uint32_t n = 0;
    const auto count = [](SupervisedChain* s) -> uint32_t {
        if (!s) return 0;
        uint32_t k = 0;
        if (s->host() && s->host()->running()) ++k;
        return k + (s->starting() ? 1u : 0u);
    };
    for (const auto& [track, c] : chains_) n += count(c->running.get()) + count(c->pending.get());
    for (const Retiring& r : retiring_) n += count(r.chain.get());
    for (const auto& s : stopping_) n += count(s.get());
    return n;
}

void PluginHosting::read_telemetry(std::vector<TelemetryRead>& out) {
    for (auto& [track, cp] : chains_) {
        Chain& c = *cp;
        if (!c.running || c.running->down() || !c.running->host()) continue;
        plugin_client::HostProcess& h = *c.running->host();
        if (!h.has_telemetry()) continue;
        for (uint32_t i = 0; i < c.running_spec.size(); ++i) {
            TelemetryRead r;
            if (!h.read_telemetry(i, r.snapshot)) continue;
            r.insert = c.running_spec[i].insert;
            r.plugin = c.running_spec[i].plugin;
            out.push_back(std::move(r));
        }
    }
}

bool PluginHosting::host_value(model::Id insert, const std::string& key, float& value) {
    for (auto& [track, cp] : chains_) {
        Chain& c = *cp;
        for (uint32_t i = 0; i < c.running_spec.size(); ++i) {
            if (c.running_spec[i].insert != insert) continue;
            if (!c.running || c.running->down() || !c.running->host()) return false;
            plugin_client::HostProcess& h = *c.running->host();
            if (key.empty()) {
                if (i >= plugin::kMaxInstances) return false;
                value = h.header().bypass[i].load(std::memory_order_relaxed);
                return true;
            }
            const pc::Loaded& l = h.loaded();
            if (i >= l.instances.size()) return false;
            const auto& keys = l.instances[i].params;
            const auto d = std::find_if(keys.begin(), keys.end(), [&](const pc::ParamDesc& p) { return p.key == key; });
            if (d == keys.end()) return false;
            const int64_t slot = plugin::param_slot(h.map(), i, uint32_t(d - keys.begin()));
            if (slot < 0) return false;
            const auto* table = reinterpret_cast<const std::atomic<float>*>(static_cast<const char*>(h.region()) +
                                                                            h.layout().values_offset);
            value = table[slot].load(std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

}  // namespace sonora
