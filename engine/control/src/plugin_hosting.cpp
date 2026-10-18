// SPDX-License-Identifier: GPL-3.0-or-later
// The control thread's plugin side (plugin_hosting.h). Never on the audio
// thread: it allocates, starts processes and reads files.
#include "sonora/plugin_hosting.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#include <tuple>

#include "sonora/api/state_block.h"
#include "sonora/json/json.h"
#include "sonora/platform/process.h"
#include "sonora/plugin/host_control.h"
#include "sonora/plugin_chain_runner.h"
#include "sonora/plugin_client/host_supervisor.h"
#include "sonora/plugin_client/plugin_client.h"
#include "sonora/session.h"

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

}  // namespace

// ---- what the scan says of one plugin at one channel count

struct PluginHosting::Probe {
    enum class State { Queued, Running, Ok, Missing, Refused };
    std::string id;  // host_id()
    uint16_t channels = 1;
    State state = State::Queued;
    std::unique_ptr<platform::ChildProcess> child;
    std::string out_path;
    uint64_t started_ms = 0;
    std::vector<std::string> keys;  // dense order
    std::vector<float> defaults;
    std::string reason;
};

struct Member {
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

    std::unique_ptr<SupervisedChain> pending;
    uint32_t pstore = kNone, pnode = kNone;
    std::vector<Member> pending_spec;
    std::string start_failed;  // a first start that crashed or timed out

    bool opened = false;      // started with the session: silent until Ready
    bool ever_ready = false;
    bool nothing_to_run = false;  // every instance missing or refused
    bool restart_swapped = false;
    uint64_t restart_retire_epoch = 0;

    // The values last given to the running chain's store, by insert.
    std::map<model::Id, model::InsertMix> posted;
    // Changes the session's ring refused: (store, instance, param) -> value.
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, float> retry;
};

PluginHosting::PluginHosting(Session& session, PluginHostingConfig config)
    : session_(session), config_(std::move(config)) {
    if (config_.max_probes == 0) config_.max_probes = std::max(1u, std::thread::hardware_concurrency());
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
}

PluginHosting::~PluginHosting() {
    // The runner leaves the session first; the caller has deactivated the
    // backend or seen a callback since (item 4), so no callback holds it.
    session_.set_insert_chains(nullptr);
    for (auto& [track, c] : chains_) {
        if (c->pending) c->pending->request_stop(0);
        if (c->running) c->running->request_stop(0);
    }
    for (Retiring& r : retiring_) r.chain->request_stop(0);
    chains_.clear();
    retiring_.clear();
    runner_.reset();
    supervisor_.reset();
    client_.reset();
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

// ---- probes (amendment item 3)

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
        }
    }
    return p.get();
}

void PluginHosting::start_probes(uint64_t now_ms) {
    uint32_t running = 0;
    for (auto& [key, p] : probes_) running += p->state == Probe::State::Running ? 1 : 0;
    for (auto& [key, p] : probes_) {
        if (running >= config_.max_probes) return;
        if (p->state != Probe::State::Queued) continue;
        p->out_path = config_.scratch_dir + "/sonora-probe-" + std::to_string(platform::current_process_id()) + "-" +
                      std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()) + ".json";
        std::vector<std::string> argv = {config_.host_path, "--probe", p->id, "--channels", std::to_string(p->channels)};
        for (const std::string& b : config_.bundles) {
            argv.push_back("--bundle");
            argv.push_back(b);
        }
        platform::ChildProcess::Options o;
        o.stdout_path = p->out_path;
        std::string error;
        p->child = platform::ChildProcess::spawn(argv, o, error);
        if (!p->child) {
            p->state = Probe::State::Refused;
            p->reason = "the probe could not start: " + error;
            continue;
        }
        p->state = Probe::State::Running;
        p->started_ms = now_ms;
        ++running;
    }
}

bool PluginHosting::poll_probes(uint64_t now_ms) {
    bool finished = false;
    for (auto& [key, p] : probes_) {
        if (p->state != Probe::State::Running) continue;
        if (p->child->running()) {
            if (now_ms - p->started_ms < config_.probe_timeout_ms) continue;
            finished = true;
            // §4: a probe that hangs blocks the plugin, with the reason.
            p->child->kill();
            p->state = Probe::State::Refused;
            p->reason = "its probe did not answer within " + std::to_string(config_.probe_timeout_ms / 1000) + " s";
            std::remove(p->out_path.c_str());
            continue;
        }
        finished = true;
        std::string line;
        {
            std::ifstream f(p->out_path);
            std::getline(f, line);
        }
        std::remove(p->out_path.c_str());
        json::Value v;
        json::Error je;
        if (!json::parse(line, v, je) || !v.is_object()) {
            p->state = Probe::State::Refused;
            p->reason = "its probe " +
                        (p->child->exited_normally() ? "exited with code " + std::to_string(p->child->exit_code())
                                                     : "was killed by signal " + std::to_string(p->child->signal_number())) +
                        " without an answer";
            continue;
        }
        const json::Value* ok = v.find("ok");
        const json::Value* reason = v.find("reason");
        if (!ok || !ok->is_bool() || !ok->as_bool()) {
            p->reason = reason && reason->is_string() ? reason->text() : "refused";
            p->state = p->reason == "not installed" ? Probe::State::Missing : Probe::State::Refused;
            continue;
        }
        p->keys.clear();
        p->defaults.clear();
        if (const json::Value* params = v.find("params"); params && params->is_array()) {
            for (const json::Value& d : params->items()) {
                const json::Value* k = d.find("key");
                const json::Value* def = d.find("default");
                float x = 0.f;
                if (!k || !k->is_string() || !def || !def->to_float(x)) continue;
                p->keys.push_back(k->text());
                p->defaults.push_back(x);
            }
        }
        p->state = Probe::State::Ok;
    }
    return finished;
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

void PluginHosting::start_pending(Chain& c, const model::Snapshot& doc) {
    const model::TrackEdit* track = nullptr;
    for (const auto& t : doc.edit->tracks)
        if (t->id == c.track) track = t.get();
    if (!track) return;
    std::vector<Member> spec_members;
    plugin_client::ChainSpec spec;
    spec.channels = track->channels;
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
            // params, as at open.
            std::ifstream f(config_.session_dir + "/" + m->state->file, std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            if (f && pc::state_matches(ss.str(), m->state->sha256)) state = ss.str();
        }
        spec.states.push_back(std::move(state));
        spec_members.push_back({in->id, in->plugin});
    }
    if (c.pending) {
        c.pending->request_stop(0);
        c.pending.reset();
        retire(nullptr, c.pstore, c.pnode);
        c.pstore = c.pnode = kNone;
    }
    c.pending_spec.clear();
    if (spec_members.empty()) return;
    c.pstore = take_store();
    c.pnode = take_node();
    if (c.pstore == kNone || c.pnode == kNone) {
        retire(nullptr, c.pstore, c.pnode);
        c.pstore = c.pnode = kNone;
        c.start_failed = "no parameter store left for this chain";
        return;
    }
    c.pending = std::make_unique<SupervisedChain>(*supervisor_, std::move(spec), c.pstore, c.pnode);
    c.pending_spec = std::move(spec_members);
    std::string error;
    if (!c.pending->begin(error)) c.start_failed = error;
}

void PluginHosting::show(const model::Snapshot& doc, bool opened) {
    if (opened) loading_total_ = 0;
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
            if (c.pending) {
                c.pending->request_stop(0);
                c.pending.reset();
                retire(nullptr, c.pstore, c.pnode);
                c.pstore = c.pnode = kNone;
                c.pending_spec.clear();
            }
            if (c.running) {
                retire(std::move(c.running), c.store, c.node);
                c.store = c.node = kNone;
                c.running_spec.clear();
            }
            continue;
        }
        const bool running_matches = c.running && !c.running->down() && c.running_spec == want;
        if (running_matches) {
            // An edit undone before its new host was Ready: that start is
            // abandoned, the old binding never stopped (change H).
            if (c.pending) {
                c.pending->request_stop(0);
                c.pending.reset();
                retire(nullptr, c.pstore, c.pnode);
                c.pstore = c.pnode = kNone;
                c.pending_spec.clear();
            }
            post_values(c, doc);
            continue;
        }
        if (c.pending && c.pending_spec == want) continue;
        c.start_failed.clear();
        start_pending(c, doc);
    }
    // Tracks with no active insert left, or gone: their chains leave by
    // epoch.
    for (auto it = chains_.begin(); it != chains_.end();) {
        if (seen.count(it->first)) {
            ++it;
            continue;
        }
        Chain& c = *it->second;
        if (c.pending) {
            c.pending->request_stop(0);
            c.pending.reset();
            retire(nullptr, c.pstore, c.pnode);
        }
        retire(std::move(c.running), c.store, c.node);
        it = chains_.erase(it);
    }
    recompute_faults(doc);
}

void PluginHosting::post_values(Chain& c, const model::Snapshot& doc) {
    if (!c.running || c.running->down() || !c.running->host()) return;
    const pc::Loaded& l = c.running->host()->loaded();
    for (uint32_t i = 0; i < c.running_spec.size() && i < l.instances.size(); ++i) {
        const model::Id id = c.running_spec[i].insert;
        const model::InsertMix* m = doc.mix.find_insert(id);
        const model::InsertMix now = m ? *m : model::InsertMix{id, false, {}, {}};
        model::InsertMix& was = c.posted[id];
        if (now.bypass != was.bypass) {
            if (!session_.post_plugin_param(c.store, i, plugin::kBypassParam, now.bypass ? 1.f : 0.f))
                c.retry[{c.store, i, plugin::kBypassParam}] = now.bypass ? 1.f : 0.f;
        }
        for (const model::ParamValue& v : now.params) {
            const auto old = std::find_if(was.params.begin(), was.params.end(),
                                          [&](const model::ParamValue& p) { return p.key == v.key; });
            if (old != was.params.end() && old->value == v.value) continue;
            const auto& keys = l.instances[i].params;
            const auto d = std::find_if(keys.begin(), keys.end(), [&](const pc::ParamDesc& p) { return p.key == v.key; });
            if (d == keys.end()) continue;
            const auto dense = uint32_t(d - keys.begin());
            const float x = std::min(std::max(v.value, d->min), d->max);
            if (!session_.post_plugin_param(c.store, i, dense, x)) c.retry[{c.store, i, dense}] = x;
        }
        was = now;
    }
}

// ---- the loop

PluginHosting::PollResult PluginHosting::poll(uint64_t now_ms, const model::Snapshot& doc, uint64_t oldest_live_epoch) {
    PollResult result;
    // A probe that finished lets the chains waiting on it start.
    const bool probes_moved = poll_probes(now_ms);
    start_probes(now_ms);

    bool reshow = false;
    for (auto& [track, cp] : chains_) {
        Chain& c = *cp;
        if (c.pending) {
            switch (c.pending->poll(now_ms)) {
                case SupervisedChain::Event::Swapped: {
                    // The new host is Ready: it replaces the old binding in
                    // the next plan; the old one leaves by epoch.
                    retire(std::move(c.running), c.store, c.node);
                    c.running = std::move(c.pending);
                    c.store = c.pstore;
                    c.node = c.pnode;
                    c.pstore = c.pnode = kNone;
                    c.running_spec = std::move(c.pending_spec);
                    c.pending_spec.clear();
                    c.ever_ready = true;
                    c.start_failed.clear();
                    c.restart_swapped = false;
                    c.restart_retire_epoch = 0;
                    // What the store was seeded with.
                    c.posted.clear();
                    for (const Member& mbr : c.running_spec) {
                        const model::InsertMix* m = doc.mix.find_insert(mbr.insert);
                        c.posted[mbr.insert] = m ? *m : model::InsertMix{mbr.insert, false, {}, {}};
                    }
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
                        result.replan = true;
                    }
                    c.pending.reset();
                    retire(nullptr, c.pstore, c.pnode);
                    c.pstore = c.pnode = kNone;
                    c.pending_spec.clear();
                    break;
                }
                default: break;
            }
        }
        if (c.running) {
            switch (c.running->poll(now_ms)) {
                case SupervisedChain::Event::Swapped:
                    // A restart is Ready (§5.3): the next plan names the new
                    // slot; the dead one leaves by epoch.
                    c.restart_swapped = true;
                    c.restart_retire_epoch = 0;
                    result.replan = true;
                    break;
                case SupervisedChain::Event::Down: result.replan = true; break;
                default: break;
            }
            for (const std::string& m : c.running->take_host_messages()) {
                pc::Type t{};
                if (pc::peek_type(m, t) && t == pc::Type::LatencyChanged) {
                    if (c.running->refresh_latency()) result.replan = true;
                    continue;
                }
                handle_host_message(track, m, doc, result.ops);
            }
            for (auto it = c.retry.begin(); it != c.retry.end();) {
                const auto [store, instance, param] = it->first;
                it = session_.post_plugin_param(store, instance, param, it->second) ? c.retry.erase(it) : std::next(it);
            }
            if (c.restart_swapped && c.restart_retire_epoch != 0 && oldest_live_epoch >= c.restart_retire_epoch) {
                c.running->retired();
                c.restart_swapped = false;
                c.restart_retire_epoch = 0;
            }
        }
    }

    // A change the session drained and had no node for (#68 review item 3):
    // the running chains' values posted again, at most every 100 ms.
    const uint64_t dropped = session_.plugin_params_dropped();
    if (dropped != dropped_seen_ && now_ms - last_repost_ms_ >= 100) {
        dropped_seen_ = dropped;
        last_repost_ms_ = now_ms;
        for (auto& [track, cp] : chains_) {
            cp->posted.clear();
            post_values(*cp, doc);
        }
    }

    // Retiring by epoch (item 2).
    for (auto it = retiring_.begin(); it != retiring_.end();) {
        Retiring& r = *it;
        if (r.first_epoch_without == 0 || oldest_live_epoch < r.first_epoch_without) {
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

    if (reshow || probes_moved) show(doc, false);
    else recompute_faults(doc);
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

void PluginHosting::built(uint64_t epoch) {
    last_built_epoch_ = epoch;
    for (Retiring& r : retiring_)
        if (r.first_epoch_without == 0) r.first_epoch_without = epoch;
    for (auto& [track, c] : chains_)
        if (c->restart_swapped && c->restart_retire_epoch == 0) c->restart_retire_epoch = epoch;
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
            const auto dense = uint32_t(d - keys.begin());
            // A full ring keeps the newest value for a retry: coalesced,
            // never lost (#68 review item 3).
            if (!session_.post_plugin_param(c.store, i, dense, value)) c.retry[{c.store, i, dense}] = value;
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
    // The value as a document operation: bypass, or the insert's whole list
    // with this key set (§8, "Values moved in the plugin's own editor").
    const auto record = [&](uint32_t instance, uint32_t param, float value) {
        if (instance >= c.running_spec.size() || instance >= l.instances.size()) return;
        const model::Id id = c.running_spec[instance].insert;
        if (param == plugin::kBypassParam) {
            ops.push_back(model::SetInsertBypass{id, value >= 0.5f});
            return;
        }
        if (param >= l.instances[instance].params.size()) return;
        const std::string& key = l.instances[instance].params[param].key;
        // Several moves of one message (EditorClosed) land on one list.
        model::SetInsertParams* set = nullptr;
        for (model::Operation& op : ops) {
            if (auto* s = std::get_if<model::SetInsertParams>(&op); s && s->insert == id) set = s;
        }
        if (!set) {
            const model::InsertMix* m = doc.mix.find_insert(id);
            ops.push_back(model::SetInsertParams{id, m ? m->params : std::vector<model::ParamValue>{}});
            set = std::get_if<model::SetInsertParams>(&ops.back());
        }
        const auto k = std::find_if(set->params.begin(), set->params.end(),
                                    [&](const model::ParamValue& p) { return p.key == key; });
        if (k != set->params.end()) k->value = value;
        else set->params.push_back({key, value});
    };
    // The host never applies a touch itself (host_control.h): the value
    // goes to the chain tagged, through the ring.
    const auto post = [&](uint32_t instance, uint32_t param, float value) {
        if (!session_.post_plugin_param(c.store, instance, param, value)) c.retry[{c.store, instance, param}] = value;
    };
    switch (t) {
        case pc::Type::ParamTouched: {
            pc::ParamTouched m;
            if (pc::decode(bytes, m, e)) post(m.instance, m.param, m.value);
            break;
        }
        case pc::Type::GestureEnd: {
            pc::GestureEnd m;
            if (!pc::decode(bytes, m, e)) break;
            post(m.instance, m.param, m.value);
            record(m.instance, m.param, m.value);
            break;
        }
        case pc::Type::EditorClosed: {
            pc::EditorClosed m;
            if (!pc::decode(bytes, m, e)) break;
            for (const pc::TouchedValue& v : m.moved) record(m.instance, v.param, v.value);
            break;
        }
        default: break;
    }
}

// ---- what the operator is told (item 5)

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
                } else if (c.running->restarting()) {
                    faults[in.id] = where(t, in) + ": restarting: " + c.running->why();
                }
                continue;
            }
            if (!c.running && !c.start_failed.empty()) faults[in.id] = where(t, in) + ": " + c.start_failed;
        }
    }
    faults_.swap(faults);
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

uint32_t PluginHosting::loading() const {
    // A chain opened with the session is loading until its host is Ready,
    // its start failed, or nothing of it is left to run.
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
    return n;
}

}  // namespace sonora
