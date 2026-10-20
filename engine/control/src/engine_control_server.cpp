// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/engine_control_server.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>
#include <variant>
#include <vector>

#include "sonora/api/wire.h"
#include "sonora/audio_backend.h"
#include "sonora/closed_take.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"
#include "sonora/playback_controller.h"
#include "sonora/plugin_hosting.h"
#include "sonora/plugin_scanner.h"
#include "sonora/session.h"

namespace sonora {

namespace {

uint64_t steady_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

api::ModelUpdate to_wire(const model::ReplicationUpdate& u) {
    api::ModelUpdate m;
    switch (u.kind) {
        case model::ReplicationUpdate::Kind::Op: m.kind = api::ModelUpdateKind::Op; break;
        case model::ReplicationUpdate::Kind::Undo: m.kind = api::ModelUpdateKind::Undo; break;
        case model::ReplicationUpdate::Kind::Redo: m.kind = api::ModelUpdateKind::Redo; break;
        case model::ReplicationUpdate::Kind::Snapshot:
            m.kind = api::ModelUpdateKind::Snapshot;
            break;
    }
    m.revision = u.revision;
    m.undo_floor = u.undo_floor;
    m.next_id = u.next_id;
    m.bytes = u.bytes;
    m.label = u.label;
    return m;
}

// How long the control thread sleeps when nothing arrives: the period of
// the take-failure and fatal checks, as the terminal loop's frame was.
constexpr auto kIdle = std::chrono::milliseconds(50);
// While the controller waits on the disk reader (a plan, a locate, a roll
// or a take): a roll asked for starts within this of its window becoming
// resident.
constexpr auto kBusy = std::chrono::milliseconds(2);

// The commands whose reply waits for the residency gate (ADR-0009, update
// "playback on the M4/M7 control server", item 4): an Ok for a take that
// never started is a lost take the operator was told is recording.
bool starts_a_take(const std::string& id) {
    return id == "transport.record" || id == "transport.toggle_record";
}

bool changes_arming(const std::string& id) {
    return id == "track.toggle_arm" || id == "track.arm_all" || id == "track.disarm_all";
}

std::vector<model::Id> track_order(const model::Snapshot& s) {
    std::vector<model::Id> ids;
    ids.reserve(s.edit->tracks.size());
    for (const auto& t : s.edit->tracks) ids.push_back(t->id);
    return ids;
}

// Notices replayed to a client that attaches later. A device notice comes
// first and matters most; a flood after it is not worth a restarted
// interface's screen.
constexpr std::size_t kMaxStandingNotices = 32;

static_assert(api::kInsertSlots == model::kInsertSlots,
              "the state block has a slot for every insert slot the model has");

// This engine hosts no plugin until engine/plugin_client lands (ADR-0012
// checklist 4). It says so rather than accept an insert it would play dry
// or silent with nothing on screen to say why: an operation that would
// start a host is NotImplemented, its catalog is empty, every insert a
// loaded session holds shows faulted, and the health flag is up.
const char* const kNoHosting =
    "plugin hosting is not in this engine yet (engine/plugin_client, ADR-0012 checklist 4)";

// ADR-0012, amendment of 2026-10-19, item 6: an insert a client adds names
// a plugin the catalog does not have yet, while the first scan of this run
// has not completed.
const char* const kScanRunning = "the plug-in scan is still running";

// Q3 (owner, 2026-10-19): never during a take.
const char* const kNoRescanWhileRecording =
    "not during a take: a plug-in scan runs plug-in code; stop the transport, then rescan";

// By index and by ordered key, not by a walk of the whole catalog: a
// compound of 256 inserts against thousands of plugins is one turn
// (rt-engineer on #95).
const api::PluginInfo* catalog_find(const PluginScanner::Catalog& c, model::PluginFormat format,
                                    const std::string& id) {
    return c.find_plugin(static_cast<api::PluginFormat>(format), id);
}

std::string host_id(const model::PluginRef& p) {
    return p.format == model::PluginFormat::Vst3 ? "vst3:" + p.id : p.id;
}

// §6.1's "as scanned": the session file stores the catalog's words, never
// the client's. From the catalog sent, or, as catalog_refusal accepts it,
// from a probe this run finished (finding 4 on #95: during a first cold
// scan every accepted insert is one of those).
void as_scanned(model::PluginRef& ref, const PluginScanner::Catalog& c) {
    if (const api::PluginInfo* p = catalog_find(c, ref.format, ref.id)) {
        ref.name = p->name;
        ref.version = p->version;
        return;
    }
    if (const plugin::probe::Record* best = c.any_probe(host_id(ref))) {
        ref.name = best->name.empty() ? ref.id : best->name;
        ref.version = best->version;
    }
}

void as_scanned(model::Operation& op, const PluginScanner::Catalog& c) {
    if (auto* a = std::get_if<model::AddInsert>(&op)) as_scanned(a->plugin, c);
    if (auto* r = std::get_if<model::ReplaceInsert>(&op)) as_scanned(r->plugin, c);
    if (auto* k = std::get_if<model::Compound>(&op)) {
        for (model::Member& m : k->ops) {
            if (auto* a = std::get_if<model::AddInsert>(&m)) as_scanned(a->plugin, c);
            if (auto* r = std::get_if<model::ReplaceInsert>(&m)) as_scanned(r->plugin, c);
        }
    }
}

// What a chain could run on `channels`: as many in as out, the track's, or
// a mono plugin replicated (the host's rule, plugins/host chain.h).
bool fits(uint32_t in, uint32_t out, uint16_t channels) {
    return in == out && (in == channels || in == 1);
}

std::string fit_refusal(const std::string& name, uint32_t in, uint32_t out, uint16_t channels) {
    return name + " has " + std::to_string(in) + " in and " + std::to_string(out) + " out; the track has " +
           std::to_string(channels) + (channels == 1 ? " channel" : " channels");
}

uint64_t steady_ms_now() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

}  // namespace

EngineControlServer::EngineControlServer(Session& session, backend::AudioBackend* audio,
                                         Options options)
    : session_(session), audio_(audio), opts_(std::move(options)) {
    const auto n = static_cast<uint32_t>(session.tracks().size());
    if (opts_.state_memory != nullptr) {
        block_ = opts_.state_memory;
        block_bytes_ = opts_.state_memory_bytes;
    } else {
        own_block_ = api::StateBlockMemory(n);
        block_ = own_block_.data();
        block_bytes_ = own_block_.bytes();
    }
    state_ready_ = writer_.init(block_, block_bytes_, n, n, opts_.publish_period_ns);
    if (!state_ready_) {
        // Memory the caller got wrong: the server still runs, on a block of
        // its own, rather than writing past the end of theirs.
        own_block_ = api::StateBlockMemory(n);
        block_ = own_block_.data();
        block_bytes_ = own_block_.bytes();
        writer_.init(block_, block_bytes_, n, n, opts_.publish_period_ns);
    }
    publisher_ = std::make_unique<StatePublisher>(session_, audio_, writer_,
                                                  opts_.publish_period_ns);

    // From the folder, not from 1: systemd restarts the engine every time it
    // exits, and a counter that started over numbered the next take like the
    // last session's first one.
    take_ = session_.next_free_take();
    publisher_->set_take(take_);
    document_ = std::make_unique<model::Document>(
        opts_.initial_document ? *opts_.initial_document : engine_document(session_));
    playback_ = std::make_unique<PlaybackController>(session_, session_.config().dir);
    playback_->set_input_monitoring(opts_.input_monitoring);
    // Read as the take starts, which with the residency gate is in pump(),
    // on this thread: the range then is the one the operator last set.
    install_session_commands(registry_, session_, take_, quit_, opts_.may_record,
                             playback_.get(), [this](PunchRange& range) {
                                 return document_punch(document_->current(), range);
                             });
    if (!opts_.plugin_host.empty()) {
        // For the engine process's life (amendment of 2026-10-19, item 3):
        // its catalog outlives a backend activation.
        PluginScannerConfig sc;
        sc.host_path = opts_.plugin_host;
        sc.bundles = opts_.plugin_bundles;
        sc.cache_path = opts_.plugin_cache;
        sc.build_id = opts_.plugin_build_id;
        sc.scan_at_start = opts_.plugin_scan_at_start;
        sc.max_probes = opts_.plugin_probe_slots;
        scanner_ = std::make_unique<PluginScanner>(sc);
        registry_.install("plugin.rescan", ExplainingHandler([this](const CommandContext&, std::string& why) {
                              // Item 5 and Q3: answered when queued, not when
                              // it ends, and never during a take.
                              if (session_.transport().state() == TransportState::Recording ||
                                  playback_->record_requested()) {
                                  why = kNoRescanWhileRecording;
                                  return false;
                              }
                              scanner_->rescan();
                              return true;
                          }));
    }
    for (const auto& spec : command_catalog()) {
        if (spec.domain == CommandDomain::Engine && registry_.implemented(spec.id)) {
            implemented_.push_back(spec.id);
        }
    }
    // The plan is published once the reader has its window, in the
    // control thread's first turns.
    show_document(true);
    publisher_->set_health(api::kHealthPluginsDisabled, true);
    show_inserts(true);
    // With no backend (tests), at once; otherwise once the period is known.
    start_hosting();
}

EngineControlServer::~EngineControlServer() {
    stop();
    // Hosts before the plans that name their slots are freed.
    hosting_.reset();
    // Every probe child killed and reaped before its thread is joined.
    scanner_.reset();
    // Its destructor waits for the reader to let go of the plans this side
    // still owns, so it goes while the session's reader is certainly there.
    playback_.reset();
}

void EngineControlServer::after_deactivate() {
    if (running()) return;
    // One backend activation (amendment item 4): no callback runs now, so
    // the runner, the hosts and their regions may go.
    playback_->set_chain_resolver(ChainResolver());
    hosting_.reset();
    // The next activation builds hosting again, for its own period.
    hosting_tried_ = false;
    playback_->after_deactivate();
}

void EngineControlServer::start_hosting() {
    if (hosting_tried_ || !scanner_) return;
    const uint32_t frames = audio_ != nullptr ? audio_->buffer_frames() : opts_.plugin_block_frames;
    if (frames == 0) return;
    hosting_tried_ = true;
    PluginHostingConfig hc;
    hc.host_path = opts_.plugin_host;
    hc.bundles = opts_.plugin_bundles;
    hc.session_dir = session_.config().dir;
    hc.sample_rate = session_.config().sample_rate;
    hc.block_frames = frames;
    hc.max_block_frames = session_.config().playback.max_block_frames;
    hc.max_stores = session_.config().playback.max_param_stores;
    hosting_ = std::make_unique<PluginHosting>(session_, hc, *scanner_);
    if (!hosting_->ok()) {
        api::EngineEvent e;
        e.kind = api::EventKind::Notice;
        e.text = hosting_->why();
        post_event(e);
    }
    publisher_->set_health(api::kHealthPluginsDisabled, !hosting_->ok());
    playback_->set_chain_resolver([this](const model::TrackEdit& t, uint32_t engine_track, ChainBinding& b) {
        hosting_->resolve(t, engine_track, b);
    });
    hosting_->show(document_->current(), true);
    open_deadline_ms_ = steady_ms_now() + opts_.plugin_open_wait_ms;
    show_document(false);
    show_inserts(false);
}

bool EngineControlServer::plugins_loading() const {
    return hosting_ && hosting_->loading() > 0 && steady_ms_now() < open_deadline_ms_;
}

void EngineControlServer::release_plays(bool cancelled) {
    std::vector<HeldPlay> held;
    held.swap(held_plays_);
    bool played = false;
    for (HeldPlay& p : held) {
        if (cancelled) {
            reply(p.to, p.seq, DispatchResult::Failed, "cancelled");
            continue;
        }
        // One roll for every press that waited.
        std::string why;
        const DispatchResult d = played ? DispatchResult::Ok : registry_.dispatch(p.command, p.ctx, why);
        played = played || d == DispatchResult::Ok;
        reply(p.to, p.seq, d, why);
    }
}

void EngineControlServer::apply_hosting_ops(std::vector<model::Operation>& ops) {
    for (const model::Operation& op : ops) {
        model::ReplicationUpdate u;
        std::string why;
        if (document_->apply(op, u, why)) {
            send_update(u);
        } else {
            api::EngineEvent e;
            e.kind = api::EventKind::Notice;
            e.text = "a value moved in a plugin's editor could not be recorded: " + why;
            post_event(e);
        }
    }
}

void EngineControlServer::send_update(const model::ReplicationUpdate& update) {
    std::string out;
    api::wire::encode(to_wire(update), out);
    send(out);
}

void EngineControlServer::show_document(bool mix_changed) {
    const model::Snapshot& doc = document_->current();
    if (mix_changed) apply_mix();
    std::string why;
    if (playback_->show(doc, why)) {
        if (hosting_) hosting_->built(playback_->last_built_epoch());
    } else {
        // What plays does not change; the operator must know the edit is
        // not what is heard.
        api::EngineEvent e;
        e.kind = api::EventKind::Notice;
        e.text = "playback: the session as edited cannot be played: " + why;
        post_event(e);
    }
}

void EngineControlServer::show_inserts(bool new_session) {
    const auto say = [this](uint64_t insert, const std::string& text) {
        if (text.empty()) fault_text_.erase(insert);
        else fault_text_[insert] = text;
        api::EngineEvent e;
        e.kind = api::EventKind::PluginFault;
        e.insert = insert;
        e.text = text;
        post_event(e);
    };
    if (new_session) {
        // Copied: post_event() erases from the map as each is cleared.
        std::vector<uint64_t> standing;
        for (const auto& [insert, bytes] : plugin_faults_) standing.push_back(insert);
        for (const uint64_t insert : standing) say(insert, "");
    }

    const model::Snapshot& doc = document_->current();
    const auto& tracks = doc.edit->tracks;
    static const std::vector<model::Insert> kNoInserts;
    std::set<uint64_t> in_chain;
    for (std::size_t t = 0; t < session_.tracks().size(); ++t) {
        uint32_t flags[api::kInsertSlots] = {};
        const auto& inserts = t < tracks.size() ? tracks[t]->inserts : kNoInserts;
        for (const model::Insert& in : inserts) {
            const model::InsertMix* m = doc.mix.find_insert(in.id);
            // Without hosting every insert shows faulted (§5.5); with it, an
            // active one shows what its host says, an inactive one nothing.
            uint32_t hosted = 0;
            if (!hosting_) hosted = api::kInsertFaulted;
            else if (in.active) hosted = hosting_->insert_flags(in.id);
            flags[in.slot] = api::kInsertPresent | hosted | (in.active ? api::kInsertActive : 0u) |
                             (m != nullptr && m->bypass ? api::kInsertBypassed : 0u);
            if (!in.active) continue;
            // One fault stands per insert, the latest replacing the last
            // (ADR-0012 §8); from the hosts when there are any.
            std::string text;
            if (!hosting_) {
                text = in.plugin.name + " (" + tracks[t]->name + ", slot " +
                       std::string(1, static_cast<char>('A' + in.slot)) + ") is not running: " + kNoHosting;
            } else if (auto f = hosting_->faults().find(in.id); f != hosting_->faults().end()) {
                text = f->second;
            }
            if (text.empty()) continue;
            in_chain.insert(in.id);
            if (auto shown = fault_text_.find(in.id); shown != fault_text_.end() && shown->second == text) continue;
            say(in.id, text);
        }
        for (uint32_t k = 0; k < api::kInsertSlots; ++k) {
            publisher_->set_insert_flags(static_cast<uint32_t>(t), k, flags[k]);
        }
    }
    std::vector<uint64_t> gone;
    for (const auto& [insert, bytes] : plugin_faults_) {
        if (in_chain.count(insert) == 0) gone.push_back(insert);
    }
    for (const uint64_t insert : gone) say(insert, "");
}

bool EngineControlServer::flush_plugin_state(const std::string& which, std::string& why) {
    // Nothing runs (kNoHosting): every insert's last state is the one the
    // document already holds, so there is nothing to record. What remains
    // is to refuse what names no insert, as a hosting engine will.
    if (which.empty()) return true;
    uint64_t id = 0;
    for (const char c : which) {
        if (c < '0' || c > '9' || id > (UINT64_MAX - 9) / 10) {
            why = "\"" + which + "\" is not an insert id";
            return false;
        }
        id = id * 10 + static_cast<uint64_t>(c - '0');
    }
    for (const auto& t : document_->current().edit->tracks) {
        for (const model::Insert& in : t->inserts) {
            if (in.id == id) return true;
        }
    }
    why = "insert " + which + " is not on any track";
    return false;
}

void EngineControlServer::follow_transport() {
    // Closed means every file of the take is, with its length final
    // (timeline-model.md: "take closed, length N" before RecordTake).
    for (const Session::ClosedTake& take : session_.closed_takes()) {
        const TakeAdded added = add_closed_take(*document_, session_, take);
        for (const auto& u : added.updates) send_update(u);
        std::vector<std::string> notices = added.track_notices;
        if (!added.cut_short.empty()) notices.push_back(added.cut_short);
        // A refused take is also a Notice, which stays for the session: the
        // alert below clears on the next clean take, which while rolling can
        // be the next tick, and this take is still not on the timeline then.
        if (!added.refused.empty()) notices.push_back(added.refused);
        for (const std::string& notice : notices) {
            api::EngineEvent e;
            e.kind = api::EventKind::Notice;
            e.text = notice;
            post_event(e);
        }
        if (!added.refused.empty()) {
            // An error, not a side line: shown as a failed take, until a
            // later take records cleanly, as a take that stopped writing is.
            failed_take_ = take.take;
            take_failed_shown_ = true;
            publisher_->set_health(api::kHealthTakeFailed, true);
            api::EngineEvent e;
            e.kind = api::EventKind::TakeFailed;
            e.frame = static_cast<uint64_t>(take.take);
            e.text = added.refused;
            post_event(e);
        }
        if (!added.updates.empty()) show_document(false);
    }
}

void EngineControlServer::resolve_records() {
    if (held_records_.empty() || playback_->record_requested()) return;
    DispatchResult result = DispatchResult::Ok;
    std::string message = playback_->take_record_error();
    if (!message.empty()) {
        result = DispatchResult::Failed;
    } else if (session_.transport().state() != TransportState::Recording) {
        // Withdrawn by a second press or a stop before the gate opened.
        result = DispatchResult::Failed;
        message = "cancelled";
    }
    for (const HeldReply& h : held_records_) reply(h.to, h.seq, result, message);
    held_records_.clear();
}

void EngineControlServer::pump_playback() {
    // A session whose reader has no thread (tests, tools) still plays: its
    // pass runs here, off the audio thread, as settle() does.
    if (!session_.reader().running()) session_.reader().run_pass();
    playback_->pump();
    if (playback_->plans_published() != compensation_shown_for_) {
        compensation_shown_for_ = playback_->plans_published();
        const PlanReport& r = playback_->playing_report();
        publisher_->set_compensation(r.compensation_frames, r.compensation_shortfall_frames);
        for (std::size_t t = 0; t < r.track_latency.size(); ++t) {
            publisher_->set_track_latency(static_cast<uint32_t>(t), r.track_latency[t]);
        }
        for (std::size_t t = 0; t < r.track_shortfall.size(); ++t) {
            publisher_->set_track_shortfall(static_cast<uint32_t>(t), r.track_shortfall[t]);
        }
    }
    // A take asked for behind the gate starts inside pump().
    publisher_->set_take(take_);
    follow_transport();
    resolve_records();
}

bool EngineControlServer::playback_busy() const {
    return playback_->pending() || playback_->locating() || playback_->play_requested() ||
           playback_->record_requested() || !held_records_.empty() || !held_plays_.empty();
}

void EngineControlServer::remember(const api::EngineEvent& event, const std::string& bytes) {
    switch (event.kind) {
        case api::EventKind::Notice:
            if (notices_.size() < kMaxStandingNotices) notices_.push_back(bytes);
            break;
        case api::EventKind::PluginFault:
            if (event.text.empty()) {
                plugin_faults_.erase(event.insert);
            } else {
                plugin_faults_[event.insert] = bytes;
            }
            break;
        case api::EventKind::InputsMissing:
            inputs_missing_ = event.text.empty() ? std::string() : bytes;
            break;
        case api::EventKind::TakeFailed:
            take_failed_ = event.text.empty() ? std::string() : bytes;
            break;
        case api::EventKind::Fatal: fatal_ = bytes; break;
    }
}

void EngineControlServer::post_event(const api::EngineEvent& event) {
    std::string bytes;
    api::wire::encode(event, bytes);
    remember(event, bytes);
    send(bytes);
}

void EngineControlServer::post_transient(const api::EngineEvent& event) {
    std::string bytes;
    api::wire::encode(event, bytes);
    send(bytes);
}

void EngineControlServer::set_inputs_connected(uint32_t count, bool none_is_a_problem) {
    publisher_->set_inputs_connected(count);
    publisher_->set_health(api::kHealthNoInputs, count == 0 && none_is_a_problem);
}

void EngineControlServer::send_to(Client& client, const std::string& bytes) {
    if (!client.out) return;
    // Cannot fail for what the server sends: a Snapshot is trimmed to
    // kMaxModelBytes, under kMaxMessageBytes.
    std::vector<std::string> frames;
    std::string error;
    if (!api::wire::split_frames(bytes, frames, error)) return;
    for (std::string& f : frames) client.out(std::move(f));
}

void EngineControlServer::send_to(ClientId id, const std::string& bytes) {
    const auto it = clients_.find(id);
    if (it != clients_.end()) send_to(it->second, bytes);
}

void EngineControlServer::send(const std::string& bytes) {
    for (auto& [id, client] : clients_) send_to(client, bytes);
}

void EngineControlServer::greet(Client& client) {
    api::Hello hello;
    hello.protocol_version = opts_.protocol_version;
    hello.state_layout_version = opts_.state_layout_version;
    hello.sample_rate = session_.config().sample_rate;
    hello.buffer_frames = audio_ != nullptr ? audio_->buffer_frames() : 0;
    hello.track_count = static_cast<uint32_t>(session_.tracks().size());
    for (const auto& t : session_.tracks()) hello.track_names.push_back(t->name());
    hello.implemented_commands = implemented_;
    std::string bytes;
    api::wire::encode(hello, bytes);
    send_to(client, bytes);

    // A replica starts from the whole document and its undo window, so a
    // reconnecting interface undoes and redoes in step with the engine.
    model::ReplicationUpdate snap;
    std::string why;
    if (document_->snapshot(snap, why, api::wire::kMaxModelBytes)) {
        api::wire::encode(to_wire(snap), bytes);
        send_to(client, bytes);
    }
    send_catalog(client);
    for (const std::string& e : notices_) send_to(client, e);
    for (const auto& [insert, e] : plugin_faults_) send_to(client, e);
    for (const std::string* e : {&inputs_missing_, &take_failed_, &fatal_}) {
        if (!e->empty()) send_to(client, *e);
    }
}

void EngineControlServer::send_catalog(Client& client) {
    std::string bytes, why;
    if (!scanner_) {
        // No host: nothing can be offered (kNoHosting).
        api::PluginCatalog empty;
        empty.generation = 1;
        if (api::wire::encode(empty, bytes, why)) send_to(client, bytes);
        return;
    }
    // Generation 0 and empty until the worker has read its cache. Encoded
    // by the worker, never here (rt-engineer 2 on #95).
    const auto& c = scanner_->catalog();
    if (c->api_wire) send_to(client, *c->api_wire);
    else if (api::wire::encode(c->api, bytes, why)) send_to(client, bytes);
}

void EngineControlServer::follow_scanner() {
    if (!scanner_) return;
    // Change D: no catalog probe starts while the transport records, or is
    // about to.
    const bool recording =
        session_.transport().state() == TransportState::Recording || playback_->record_requested();
    scanner_->exchange(recording);
    for (const std::string& text : scanner_->take_notices()) {
        api::EngineEvent e;
        e.kind = api::EventKind::Notice;
        e.text = text;
        post_event(e);
    }
    const auto& c = scanner_->catalog();
    if (c->generation != 0 && c->generation != catalog_sent_) {
        // Item 5: again whenever a scan completes with a different result.
        catalog_sent_ = c->generation;
        if (c->api_wire) send(*c->api_wire);
    }
}

std::string EngineControlServer::catalog_refusal(const model::Snapshot& doc, model::MemberRef member) const {
    const model::PluginRef* ref = nullptr;
    const model::TrackEdit* on = nullptr;
    if (const auto* a = member.get_if<model::AddInsert>()) {
        ref = &a->plugin;
        for (const auto& t : doc.edit->tracks)
            if (t->id == a->track) on = t.get();
    } else if (const auto* r = member.get_if<model::ReplaceInsert>()) {
        ref = &r->plugin;
        for (const auto& t : doc.edit->tracks)
            for (const model::Insert& in : t->inserts)
                if (in.id == r->insert) on = t.get();
    } else {
        return {};
    }
    // apply() names a track or insert that is not there.
    if (on == nullptr) return {};
    if (!scanner_) return kNoHosting;
    const auto& c = scanner_->catalog();
    if (!c->disabled.empty()) return c->disabled;
    if (const api::PluginInfo* p = catalog_find(*c, ref->format, ref->id)) {
        // The catalog's refusal words (§8).
        if (!p->usable) return p->name + " cannot be used: " + p->refusal;
        if (!fits(p->inputs, p->outputs, on->channels)) return fit_refusal(p->name, p->inputs, p->outputs, on->channels);
        return {};
    }
    // Not in the catalog sent: a probe this run finished for it (a chain
    // start, or the scan running) is as good.
    if (const plugin::probe::Record* rec = c->any_probe(host_id(*ref))) {
        const std::string name = rec->name.empty() ? ref->id : rec->name;
        if (!rec->ok()) return name + " cannot be used: " + rec->reason;
        if (!fits(rec->audio_in, rec->audio_out, on->channels))
            return fit_refusal(name, rec->audio_in, rec->audio_out, on->channels);
        return {};
    }
    if (!c->scanned) return kScanRunning;
    return "no plugin " + ref->id + " is installed";
}

void EngineControlServer::start_threads() {
    // Something on the block before anyone reads it, so the first look at
    // the heartbeat is not "never published".
    publisher_->publish_once();
    publisher_->start();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
    }
    next_housekeeping_ns_ = 0;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this]() { run(); });
}

void EngineControlServer::start() {
    if (running()) return;
    start_threads();
}

void EngineControlServer::start(Outbound out) {
    if (running()) return;
    Client& c = clients_[kFirstClient];
    c.out = std::move(out);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        backlog_[kFirstClient] = 0;
    }
    client_count_.store(clients_.size(), std::memory_order_release);
    greet(c);
    start_threads();
}

void EngineControlServer::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        inbound_.clear();
        backlog_.clear();
    }
    wake_.notify_all();
    room_.notify_all();
    if (thread_.joinable()) thread_.join();
    publisher_->stop();
    running_.store(false, std::memory_order_release);
    // No pump will open the gate now. Withdrawn, so a server started again
    // does not start a take nobody is waiting for.
    playback_->cancel_record();
    for (const HeldReply& h : held_records_) {
        reply(h.to, h.seq, DispatchResult::Failed, "cancelled");
    }
    held_records_.clear();
    clients_.clear();
    client_count_.store(0, std::memory_order_release);
}

EngineControlServer::ClientId EngineControlServer::attach(Outbound out, Refuse refuse) {
    Inbound in;
    in.kind = Inbound::Kind::Attach;
    in.out = std::move(out);
    in.refuse = std::move(refuse);
    ClientId id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = next_client_++;
        in.client = id;
        if (stopping_ || !running()) return id;
        backlog_[id] = 0;
        inbound_.push_back(std::move(in));
    }
    wake_.notify_one();
    return id;
}

void EngineControlServer::detach(ClientId client) {
    Inbound in;
    in.kind = Inbound::Kind::Detach;
    in.client = client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        backlog_.erase(client);
        if (stopping_ || !running()) return;
        inbound_.push_back(std::move(in));
    }
    wake_.notify_one();
    room_.notify_all();
}

void EngineControlServer::deliver(ClientId client, std::string frame) {
    Inbound in;
    in.kind = Inbound::Kind::Frame;
    in.client = client;
    const std::size_t n = frame.size();
    in.bytes = std::move(frame);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !running()) return;
        const auto b = backlog_.find(client);
        if (b != backlog_.end()) {
            b->second += n;
            if (b->second > peak_backlog_.load(std::memory_order_relaxed)) {
                peak_backlog_.store(b->second, std::memory_order_release);
            }
        }
        inbound_.push_back(std::move(in));
    }
    wake_.notify_one();
}

EngineControlServer::Room EngineControlServer::wait_for_room(ClientId client, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto settled = [this, client]() {
        if (stopping_) return true;
        const auto b = backlog_.find(client);
        return b == backlog_.end() || b->second < opts_.max_client_backlog_bytes;
    };
    if (!room_.wait_for(lock, std::chrono::milliseconds(timeout_ms), settled)) return Room::Full;
    if (stopping_ || backlog_.find(client) == backlog_.end()) return Room::Gone;
    return Room::Ok;
}

void EngineControlServer::apply_mix() {
    playback_->apply_mix(document_->current());
    riders_.clear();
}

void EngineControlServer::forget(ClientId id) {
    // A ride its client left before releasing (ADR-0009 §4 item 3): only
    // the tracks it was the last to ride, so a ride another client holds
    // stays where it is.
    for (auto it = riders_.begin(); it != riders_.end();) {
        if (it->second == id) {
            playback_->apply_gain(document_->current(), it->first);
            it = riders_.erase(it);
        } else {
            ++it;
        }
    }
    large_.erase(id);
    handled_.erase(id);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        backlog_.erase(id);
    }
    room_.notify_all();
}

namespace {

// The length of the message a frame belongs to, from its header (wire.h).
std::size_t message_length(const std::string& frame) {
    if (frame.size() < api::wire::kFrameHeaderBytes) return 0;
    std::size_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::size_t>(static_cast<uint8_t>(frame[i])) << (8 * i);
    }
    return v;
}

}  // namespace

void EngineControlServer::frame(ClientId id, std::string bytes) {
    const auto it = clients_.find(id);
    if (it == clients_.end()) return;
    Client& c = it->second;
    const bool needs_slot = c.reassembler.idle() &&
                            message_length(bytes) > opts_.large_message_bytes &&
                            large_.size() >= opts_.max_large_in_flight;
    if (c.parked.empty() && !needs_slot) {
        feed(id, c, bytes);
        return;
    }
    // Behind its own parked frames too, so the client's order holds.
    if (c.parked.empty()) parked_.push_back(id);
    c.parked.push_back(std::move(bytes));
}

void EngineControlServer::feed(ClientId id, Client& c, const std::string& bytes) {
    handled_[id] += bytes.size();
    if (c.reassembler.idle() && message_length(bytes) > opts_.large_message_bytes) {
        large_.insert(id);
        if (large_.size() > peak_large_.load(std::memory_order_relaxed)) {
            peak_large_.store(large_.size(), std::memory_order_release);
        }
    }
    std::string message, error;
    // `c` may be gone once refuse() or handle() returns.
    switch (c.reassembler.add(bytes, message, error)) {
        case api::wire::Reassembler::Result::Partial: return;
        case api::wire::Reassembler::Result::Error:
            large_.erase(id);
            refuse(id, "a frame out of place: " + error);
            return;
        case api::wire::Reassembler::Result::Complete:
            large_.erase(id);
            handle(id, message);
            return;
    }
}

void EngineControlServer::unpark() {
    while (!parked_.empty() && large_.size() < opts_.max_large_in_flight) {
        const ClientId id = parked_.front();
        bool waits = false;
        for (;;) {
            const auto it = clients_.find(id);
            if (it == clients_.end() || it->second.parked.empty()) break;
            Client& c = it->second;
            if (c.reassembler.idle() &&
                message_length(c.parked.front()) > opts_.large_message_bytes &&
                large_.size() >= opts_.max_large_in_flight) {
                waits = true;
                break;
            }
            const std::string bytes = std::move(c.parked.front());
            c.parked.pop_front();
            feed(id, c, bytes);
        }
        if (waits) break;
        parked_.pop_front();
    }
}

void EngineControlServer::release_handled() {
    if (handled_.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, n] : handled_) {
            const auto b = backlog_.find(id);
            if (b != backlog_.end()) b->second -= std::min(n, b->second);
        }
    }
    handled_.clear();
    room_.notify_all();
}

void EngineControlServer::refuse(ClientId id, const std::string& why) {
    const auto it = clients_.find(id);
    if (it == clients_.end()) return;
    refused_.fetch_add(1, std::memory_order_acq_rel);
    // A client with no transport to close (InProcessEngine) loses the
    // message and stays; one on a socket is dropped, since a peer that
    // frames wrongly cannot be resynchronised.
    if (!it->second.refuse) return;
    const Refuse r = std::move(it->second.refuse);
    forget(id);
    clients_.erase(it);
    client_count_.store(clients_.size(), std::memory_order_release);
    r(why);
}

void EngineControlServer::take(Inbound& in) {
    switch (in.kind) {
        case Inbound::Kind::Attach: {
            Client& c = clients_[in.client];
            c.out = std::move(in.out);
            c.refuse = std::move(in.refuse);
            client_count_.store(clients_.size(), std::memory_order_release);
            greet(c);
            return;
        }
        case Inbound::Kind::Detach:
            forget(in.client);
            clients_.erase(in.client);
            client_count_.store(clients_.size(), std::memory_order_release);
            return;
        case Inbound::Kind::Frame: frame(in.client, std::move(in.bytes)); return;
    }
}

void EngineControlServer::run() {
    std::deque<Inbound> batch;
    std::vector<std::deque<Inbound>> lanes;
    std::map<ClientId, std::size_t> lane_of;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, playback_busy() ? kBusy : kIdle,
                           [this]() { return stopping_ || !inbound_.empty(); });
            if (stopping_) return;
            batch.swap(inbound_);
        }
        // One item per client in turn: a client with much queued cannot
        // hold another's request behind all of it. Each client's own order
        // is kept.
        lanes.clear();
        lane_of.clear();
        for (Inbound& in : batch) {
            const auto [it, fresh] = lane_of.emplace(in.client, lanes.size());
            if (fresh) lanes.emplace_back();
            lanes[it->second].push_back(std::move(in));
        }
        batch.clear();
        for (bool more = true; more;) {
            more = false;
            for (std::deque<Inbound>& lane : lanes) {
                if (lane.empty()) continue;
                take(lane.front());
                lane.pop_front();
                more = more || !lane.empty();
            }
        }
        unpark();
        release_handled();
        tick();
    }
}

void EngineControlServer::reply(ClientId to, uint64_t seq, DispatchResult result,
                                const std::string& message) {
    api::CommandReply r;
    r.seq = seq;
    r.result = result;
    r.message = message;
    std::string bytes;
    api::wire::encode(r, bytes);
    send_to(to, bytes);
}

void EngineControlServer::handle(ClientId from, const std::string& bytes) {
    api::wire::Type type{};
    std::string error;
    // A message the codec refuses has no trustworthy seq to answer. It is a
    // client bug or a hostile peer, and the engine must not fall over
    // either way: the client is refused, the session never sees it.
    if (!api::wire::peek_type(bytes, type)) {
        refuse(from, "an empty message");
        return;
    }

    if (type == api::wire::Type::CommandRequest) {
        api::CommandRequest r;
        if (!api::wire::decode(bytes, r, error)) {
            refuse(from, "a command request that does not decode: " + error);
            return;
        }
        if (find_command(r.command) == nullptr) {
            reply(from, r.seq, DispatchResult::UnknownCommand, "");
            return;
        }
        if (!is_engine_command(r.command)) {
            reply(from, r.seq, DispatchResult::NotImplemented,
                  r.command + " is the interface's own command; the engine does not run it");
            return;
        }
        CommandContext ctx;
        ctx.target = r.target;
        ctx.argument = r.argument;
        // Play from a session just opened waits for its plug-ins, at most
        // the bound (the owner's decision of 2026-10-18); a second press, a
        // stop or a take withdraws it. Record never waits: capture is
        // pre-insert.
        const bool would_play = (r.command == "transport.play" || r.command == "transport.toggle_play") &&
                                session_.transport().state() == TransportState::Stopped &&
                                !playback_->play_requested() && !playback_->record_requested();
        if (!held_plays_.empty()) {
            if (r.command == "transport.play") {
                // Pressed again while one waits: both answered together.
                held_plays_.push_back({from, r.seq, r.command, ctx});
                return;
            }
            // Only a second space, a stop or a take withdraws the waiting
            // play (the owner's rule); any other transport command (a
            // return to zero, a locate) runs and the play still waits.
            if (r.command == "transport.toggle_play") {
                release_plays(true);
                reply(from, r.seq, DispatchResult::Ok, "");
                return;
            }
            if (r.command == "transport.stop" || starts_a_take(r.command)) release_plays(true);
        }
        if (would_play && plugins_loading()) {
            held_plays_.push_back({from, r.seq, r.command, ctx});
            // Said once per wait, to the clients attached now, never kept
            // for one that attaches later: it is not standing (#83 review
            // item 2). The count is bounded by the total, whatever a Load
            // reset meanwhile.
            api::EngineEvent e;
            e.kind = api::EventKind::Notice;
            const uint32_t loading = hosting_->loading();
            const uint32_t total = std::max(hosting_->loading_total(), loading);
            e.text = "Loading plug-ins (" + std::to_string(total - loading) + " of " + std::to_string(total) +
                     "): play starts when they are ready";
            post_transient(e);
            return;
        }
        std::string why;
        const DispatchResult d = registry_.dispatch(r.command, ctx, why);
        publisher_->set_take(take_);
        // A stop just dispatched closed a take: placed now, so the reply
        // follows its RecordTake updates. Placement comes from the disk
        // thread's report of each file, not from arming.
        follow_transport();
        if (d == DispatchResult::Ok && changes_arming(r.command)) {
            playback_->apply_monitoring(document_->current());
        }
        if (d == DispatchResult::Ok && starts_a_take(r.command) &&
            playback_->record_requested()) {
            held_records_.push_back({from, r.seq});
        } else {
            reply(from, r.seq, d, why);
        }
        // A second press or a stop may have withdrawn a take still waiting.
        resolve_records();
        return;
    }

    if (type == api::wire::Type::ParameterChange) {
        api::ParameterChange c;
        if (!api::wire::decode(bytes, c, error)) {
            refuse(from, "a parameter change that does not decode: " + error);
            return;
        }
        if (c.track >= session_.tracks().size()) {
            reply(from, c.seq, DispatchResult::Failed,
                  "there is no track " + std::to_string(c.track + 1));
            return;
        }
        // Written as an atomic the audio thread already reads; a NaN would
        // reach every sample of the track, so it is refused here.
        if (!(c.value >= 0.0f && c.value <= 4.0f)) {
            reply(from, c.seq, DispatchResult::Failed, "gain must be from 0 to 4 (linear)");
            return;
        }
        session_.tracks()[c.track]->set_gain(c.value);
        riders_[c.track] = from;
        reply(from, c.seq, DispatchResult::Ok, "");
        return;
    }

    if (type == api::wire::Type::PluginParameterChange) {
        api::PluginParameterChange c;
        if (!api::wire::decode(bytes, c, error)) {
            refuse(from, "a plugin parameter change that does not decode: " + error);
            return;
        }
        if (!hosting_) {
            reply(from, c.seq, DispatchResult::NotImplemented, kNoHosting);
            return;
        }
        // Resolved by key against the running instance's own table (§8).
        std::string why;
        const bool ok = hosting_->set_param(c.insert, c.key, c.value, why);
        reply(from, c.seq, ok ? DispatchResult::Ok : DispatchResult::Failed, ok ? "" : why);
        return;
    }

    if (type == api::wire::Type::ModelRequest) {
        api::ModelRequest m;
        if (!api::wire::decode(bytes, m, error)) {
            refuse(from, "a model request that does not decode: " + error);
            return;
        }
        model::ReplicationUpdate u;
        std::string why;
        bool ok = false;
        DispatchResult refusal = DispatchResult::Failed;
        // Faders are written from the document only when it changed what
        // they hold, whichever request changed it: a fader ride sent as a
        // ParameterChange must not snap back on an unrelated edit. A change
        // of track order or of the track set counts too: engine track i
        // plays the document's track i in display order.
        bool mix_changed = false;
        const std::vector<model::Id> order_before = track_order(document_->current());
        const model::MixState mix_before = document_->current().mix;
        switch (m.action) {
            case api::ModelAction::Apply: {
                model::Operation op;
                if (!model::decode_operation(m.op, op, why)) break;
                if (scanner_) as_scanned(op, *scanner_->catalog());
                // Asked of each member of a compound with the state the
                // members before it made (ADR-0009 update 2026-10-14).
                bool no_host = false;
                const model::MemberCheck check = [&](const model::Snapshot& at,
                                                     model::MemberRef member) {
                    if (std::string refusal = catalog_refusal(at, member); !refusal.empty()) {
                        no_host = !scanner_;
                        return refusal;
                    }
                    // Even one that leaves the document as it was: it ends a
                    // ride whose last ParameterChange need not be its value.
                    if (member.holds<model::SetMix>() || member.holds<model::SetMonitorBus>()) {
                        mix_changed = true;
                    }
                    return std::string();
                };
                ok = document_->apply(op, u, why, check);
                if (!ok && no_host) refusal = DispatchResult::NotImplemented;
                break;
            }
            case api::ModelAction::Undo: ok = document_->undo(u, why); break;
            case api::ModelAction::Redo: ok = document_->redo(u, why); break;
            case api::ModelAction::Resync:
                ok = document_->snapshot(u, why, api::wire::kMaxModelBytes);
                break;
            case api::ModelAction::Load:
                ok = document_->load(m.op, session_.config().sample_rate, u, why,
                                     api::wire::kMaxModelBytes);
                mix_changed = true;
                break;
            case api::ModelAction::FlushPluginState:
                // No update to send: see flush_plugin_state().
                ok = flush_plugin_state(m.op, why);
                reply(from, m.seq, ok ? DispatchResult::Ok : DispatchResult::Failed,
                      ok ? "" : why);
                return;
        }
        reply(from, m.seq, ok ? DispatchResult::Ok : refusal, ok ? "" : why);
        if (ok) {
            std::string out;
            api::wire::encode(to_wire(u), out);
            // A resync is the asker's alone; every other replica is in step.
            if (m.action == api::ModelAction::Resync) {
                send_to(from, out);
                // The asker may be resyncing after a lost link, with a ride's
                // last step still in the gain atomics; the plan is unchanged.
                apply_mix();
            } else {
                send(out);
                // The faders' part of the mix only: an insert's bypass or
                // parameters are not what the Track atomics hold.
                const model::MixState& mix_after = document_->current().mix;
                mix_changed = mix_changed || !(mix_after.bus == mix_before.bus) ||
                              !(mix_after.tracks == mix_before.tracks) ||
                              track_order(document_->current()) != order_before;
                // Chains from the document diff (undo, redo, active, values
                // included), before the plan is built from it.
                if (hosting_) hosting_->show(document_->current(), m.action == api::ModelAction::Load);
                if (m.action == api::ModelAction::Load) {
                    open_deadline_ms_ = steady_ms_now() + opts_.plugin_open_wait_ms;
                }
                show_document(mix_changed);
                show_inserts(m.action == api::ModelAction::Load);
            }
        }
        return;
    }

    // Hello, a reply, an event or an update: the engine's own messages,
    // which no client sends.
    refuse(from, "a message only the engine sends (type " +
                     std::to_string(static_cast<unsigned>(type)) + ")");
}

void EngineControlServer::tick() {
    pump_playback();

    // The scan worker's catalog and notices (one try_lock), before the
    // chains read its answers.
    follow_scanner();

    // The plugin side: built once the backend's period is known, then
    // polled every turn (probes, hosts, retiring by epoch). Never waits.
    start_hosting();
    if (hosting_) {
        PluginHosting::PollResult r =
            hosting_->poll(steady_ms_now(), document_->current(), playback_->oldest_live_epoch());
        if (!r.ops.empty()) {
            apply_hosting_ops(r.ops);
            hosting_->show(document_->current(), false);
        }
        if (r.replan || !r.ops.empty()) show_document(false);
        show_inserts(false);
    }
    // Every plug-in of the session just opened has loaded or failed, or the
    // bound has passed: the play that waited goes now.
    if (!held_plays_.empty() && !plugins_loading()) release_plays(false);

    // The backend stopped calling the session. Said once; the interface
    // says why and leaves, and the wiring layer closes the take.
    if (audio_ != nullptr && !fatal_said_ && audio_->fatal()) {
        fatal_said_ = true;
        api::EngineEvent e;
        e.kind = api::EventKind::Fatal;
        e.text = audio_->fatal_reason();
        post_event(e);
    }

    // Writing stops at a full disk or the 4 GB limit while the transport
    // still says REC, so the failure is shown the moment the disk thread
    // records it, and until a later take records cleanly.
    if (session_.transport().state() == TransportState::Recording) {
        const std::string failure = session_.take_failure();
        if (!failure.empty() && failed_take_ != take_) {
            failed_take_ = take_;
            take_failed_shown_ = true;
            publisher_->set_health(api::kHealthTakeFailed, true);
            api::EngineEvent e;
            e.kind = api::EventKind::TakeFailed;
            e.frame = static_cast<uint64_t>(take_);
            e.text = "take " + std::to_string(take_) + " failed: " + failure +
                     ". Nothing more is being written; stop the take";
            post_event(e);
        } else if (failure.empty() && take_ != failed_take_ && take_failed_shown_) {
            take_failed_shown_ = false;
            publisher_->set_health(api::kHealthTakeFailed, false);
            api::EngineEvent e;
            e.kind = api::EventKind::TakeFailed;
            e.frame = static_cast<uint64_t>(take_);
            post_event(e);
        }
    }

    if (opts_.housekeeping) {
        const uint64_t now = steady_ns();
        if (now >= next_housekeeping_ns_) {
            next_housekeeping_ns_ = now + opts_.housekeeping_period_ns;
            opts_.housekeeping(*this);
        }
    }
}

}  // namespace sonora
