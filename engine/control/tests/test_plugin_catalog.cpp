// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plugin catalog through the control server (ADR-0012, amendment of
// 2026-10-19, items 5 and 6, Q3), with fake_plugin_host as the scan's host:
//
//   resolution   a client's catalog after the Snapshot, and again when the
//                scan completes with a different result; AddInsert refused
//                while the first scan runs and the plugin is not known yet
//                ("the plug-in scan is still running"), for a plugin not
//                installed, one the scan refused (its words), one whose
//                channels the track cannot carry, and ReplaceInsert alike;
//                an accepted insert stores the catalog's name and version,
//                never the client's; plugin.rescan is announced and Ok
//   no_rescan_while_recording  plugin.rescan refused during a take (Q3)
//   block        plugin.block and plugin.unblock: announced; a blocked plugin
//                is never probed and AddInsert is refused in the operator's
//                words; a malformed or unknown plugin is refused; the list
//                is written to the file the entry layer names
//   shared_frames  every client is sent the catalog's own frames, split once
//                  by the worker: the same immutable bytes, not a copy each
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "sonora/engine_control_in_process.h"
#include "sonora/fake_backend.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"
#include "sonora/plugin_block_list.h"
#include "sonora/session.h"

using namespace sonora;
namespace fs = std::filesystem;

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

model::ReplicationUpdate from_wire(const api::ModelUpdate& m) {
    model::ReplicationUpdate u;
    // The two enums list the same kinds in the same order.
    u.kind = static_cast<model::ReplicationUpdate::Kind>(m.kind);
    u.part_count = m.part_count;
    u.part_bytes = m.part_bytes;
    u.revision = m.revision;
    u.undo_floor = m.undo_floor;
    u.next_id = m.next_id;
    u.bytes = m.bytes;
    u.label = m.label;
    return u;
}

// A client: the inbox, a replica of the document, and the waits.
struct Client {
    explicit Client(api::EngineControl& engine) : e(engine) {}
    api::EngineControl& e;
    api::Inbox in;
    model::Replica replica;
    std::size_t fed = 0;

    void pump() {
        e.poll(in);
        for (; fed < in.model.size(); ++fed) {
            std::string err;
            replica.receive(from_wire(in.model[fed]), err);
        }
    }
    bool until(const std::function<bool()>& pred, uint64_t ms = 10000) {
        const uint64_t end = now_ms() + ms;
        while (now_ms() < end) {
            pump();
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        pump();
        return pred();
    }
    const api::CommandReply* reply(uint64_t seq) {
        if (!until([&] {
                for (const auto& r : in.replies)
                    if (r.seq == seq) return true;
                return false;
            }))
            return nullptr;
        for (const auto& r : in.replies)
            if (r.seq == seq) return &r;
        return nullptr;
    }
    const api::CommandReply* apply(const model::Operation& op) {
        api::ModelRequest m;
        m.action = api::ModelAction::Apply;
        std::string err;
        if (!model::encode_operation(op, m.op, err)) return nullptr;
        return reply(e.submit(m));
    }
    const api::CommandReply* command(const std::string& id, int32_t target = -1, const std::string& argument = "") {
        api::CommandRequest r;
        r.command = id;
        r.target = target;
        r.argument = argument;
        return reply(e.send(r));
    }
};

bool says(const api::CommandReply* r, DispatchResult want, const std::string& text) {
    const bool ok = r && r->result == want && r->message.find(text) != std::string::npos;
    if (!ok) std::printf("    reply: %s\n", r ? r->message.c_str() : "(none)");
    return ok;
}

struct World {
    std::string root;
    explicit World(const std::string& name, const std::vector<std::string>& plugins) {
        root = fs::absolute("test_plugin_catalog_" + name).string();
        fs::remove_all(root);
        fs::create_directories(root + "/b.lv2");
        std::ofstream(root + "/b.lv2/plugin.so") << "x";
        std::ofstream w(root + "/world");
        for (const std::string& p : plugins) w << "plugin " << p.substr(0, p.find(' ')) << " " << root << "/b.lv2 lv2 "
                                               << p.substr(p.find(' ') + 1) << "\n";
        ::setenv("SONORA_FAKE_HOST_WORLD", (root + "/world").c_str(), 1);
        ::unsetenv("SONORA_FAKE_HOST_LOG");
    }
    ~World() { fs::remove_all(root); }
};

model::AddInsert add(model::Id track, model::Id id, const std::string& uri, uint8_t slot = 0) {
    model::AddInsert a;
    a.track = track;
    a.insert = id;
    a.slot = slot;
    a.plugin = model::PluginRef{model::PluginFormat::Lv2, uri, "Wrong", "9"};
    return a;
}

void test_resolution() {
    std::printf("resolution\n");
    World w("resolution", {"urn:mono 1 1 ok", "urn:stereo 2 2 ok", "urn:bad 1 1 refuse", "urn:slow 1 1 sleep 3000"});
    SessionConfig cfg;
    cfg.dir = w.root + "/session";
    cfg.track_count = 2;
    Session session(cfg);
    session.create_dirs();
    EngineControlServer::Options opts;
    opts.plugin_host = g_host;
    opts.plugin_probe_slots = 2;
    // No backend: no hosts start, only the catalog is under test.
    InProcessEngine engine(session, nullptr, opts);
    std::string error;
    check(engine.connect(error), "connected");
    Client c(engine);
    const auto& names = engine.hello().implemented_commands;
    check(std::find(names.begin(), names.end(), "plugin.rescan") != names.end(), "plugin.rescan is announced");
    check(c.until([&] { return c.replica.synced() && !c.in.catalogs.empty(); }), "a catalog after the Snapshot");
    const model::Id track = c.replica.current().edit->tracks.at(0)->id;

    // The scan is still probing urn:slow: a plugin it does not know yet is
    // refused as that, not as not installed.
    check(says(c.apply(add(track, c.replica.current().next_id, "urn:absent")), DispatchResult::Failed,
               "the plug-in scan is still running"),
          "while the first scan runs: \"the plug-in scan is still running\"");

    // Finding 4 on #95: still in the first scan, urn:mono is accepted from
    // its finished probe (the catalog sent does not have it yet), and the
    // document stores the probe's words, not the client's.
    const auto scan_done = [&] {
        for (const api::PluginInfo& p : c.in.catalogs.back().plugins)
            if (p.id == "urn:slow") return true;
        return false;
    };
    const model::Id early = c.replica.current().next_id;
    bool accepted = false;
    for (int i = 0; i < 100 && !accepted && !scan_done(); ++i) {
        const auto* r = c.apply(add(track, early, "urn:mono", 1));
        accepted = r && r->result == DispatchResult::Ok;
        if (!accepted) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(accepted && !scan_done(), "an insert accepted from a probe before the first scan completes");
    check(c.until([&] {
        const auto& ins = c.replica.current().edit->tracks.at(0)->inserts;
        return std::any_of(ins.begin(), ins.end(), [&](const model::Insert& x) { return x.id == early; });
    }),
          "the replica has it");
    for (const model::Insert& x : c.replica.current().edit->tracks.at(0)->inserts)
        if (x.id == early)
            check(x.plugin.name == "Fake urn:mono" && x.plugin.version == "1.0",
                  "stored with the probe's name and version (\"" + x.plugin.name + "\")");

    check(c.until([&] {
        for (const api::PluginInfo& p : c.in.catalogs.back().plugins)
            if (p.id == "urn:slow") return true;
        return false;
    }),
          "the catalog is sent again when the scan completes");
    check(c.in.catalogs.back().generation > c.in.catalogs.front().generation, "with its generation bumped");

    check(says(c.apply(add(track, c.replica.current().next_id, "urn:absent")), DispatchResult::Failed,
               "no plugin urn:absent is installed"),
          "a plugin not in the catalog");
    check(says(c.apply(add(track, c.replica.current().next_id, "urn:bad")), DispatchResult::Failed,
               "cannot be used: it wrote outside its buffers"),
          "a plugin the scan refused, in the catalog's words");
    check(says(c.apply(add(track, c.replica.current().next_id, "urn:stereo")), DispatchResult::Failed,
               "has 2 in and 2 out; the track has 1 channel"),
          "a plugin whose channels the track cannot carry");

    const model::Id id = c.replica.current().next_id;
    const auto* ok = c.apply(add(track, id, "urn:mono"));
    check(ok && ok->result == DispatchResult::Ok, "a usable plugin is inserted");
    const auto find_in = [&](model::Id want) -> const model::Insert* {
        for (const model::Insert& x : c.replica.current().edit->tracks.at(0)->inserts)
            if (x.id == want) return &x;
        return nullptr;
    };
    check(c.until([&] { return find_in(id) != nullptr; }), "the replica has it");
    const model::Insert* in = find_in(id);
    check(in && in->plugin.name == "Fake urn:mono" && in->plugin.version == "1.0",
          "stored with the catalog's name and version, not the client's");

    // ADR-0058: a plug-in with no side-chain input in the catalog takes no key.
    const model::Id bus = c.replica.current().next_id;
    const auto* added = c.apply(model::AddBus{bus, "Key", model::BusMix{}});
    check(added && added->result == DispatchResult::Ok, "a key bus is added");
    check(says(c.apply(model::SetInsertKey{track, id, bus}), DispatchResult::Failed,
               "the plug-in has no side-chain input: Fake urn:mono"),
          "SetInsertKey on a plug-in without a key input is refused, naming it");
    check(says(c.apply(model::SetInsertKey{track, id, 0}), DispatchResult::Ok, ""),
          "unkeying needs no key input");

    model::ReplaceInsert r;
    r.insert = id;
    r.replacement = c.replica.current().next_id;
    r.plugin = model::PluginRef{model::PluginFormat::Lv2, "urn:bad", "x", "x"};
    check(says(c.apply(r), DispatchResult::Failed, "cannot be used"), "ReplaceInsert resolves the same way");

    check(says(c.command("plugin.rescan"), DispatchResult::Ok, ""), "plugin.rescan is Ok while stopped");
    engine.shutdown();
}

void test_block() {
    std::printf("block\n");
    World w("block", {"urn:mono 1 1 ok", "urn:hang 1 1 hang"});
    const std::string log = w.root + "/log";
    ::setenv("SONORA_FAKE_HOST_LOG", log.c_str(), 1);
    const std::string list = w.root + "/config/plugin-blocklist.json";
    fs::create_directories(w.root + "/config");
    std::ofstream(list) << "{\"version\": 1, \"blocked\": [{\"id\": \"urn:hang\", \"name\": \"Hanger\"}]}\n";
    SessionConfig cfg;
    cfg.dir = w.root + "/session";
    cfg.track_count = 2;
    Session session(cfg);
    session.create_dirs();
    EngineControlServer::Options opts;
    opts.plugin_host = g_host;
    opts.plugin_block_list = list;
    InProcessEngine engine(session, nullptr, opts);
    std::string error;
    check(engine.connect(error), "connected");
    Client c(engine);
    const auto& names = engine.hello().implemented_commands;
    check(std::find(names.begin(), names.end(), "plugin.block") != names.end() &&
              std::find(names.begin(), names.end(), "plugin.unblock") != names.end(),
          "plugin.block and plugin.unblock are announced");
    const auto entry = [&](const std::string& id) -> const api::PluginInfo* {
        if (c.in.catalogs.empty()) return nullptr;
        for (const api::PluginInfo& p : c.in.catalogs.back().plugins)
            if (p.id == id) return &p;
        return nullptr;
    };
    check(c.until([&] {
        const api::PluginInfo* m = entry("urn:mono");
        const api::PluginInfo* h = entry("urn:hang");
        return c.replica.synced() && m && m->usable && h && !h->usable;
    }),
          "the scan completes without waiting on the blocked, hung plugin");
    check(entry("urn:hang")->refusal == kBlockedByOperator, "refused in the operator's words");
    std::string probes;
    {
        std::ifstream f(log);
        for (std::string l; std::getline(f, l);)
            if (l.find("--probe urn:hang") != std::string::npos) probes += l;
    }
    check(probes.empty(), "and never probed");
    const model::Id track = c.replica.current().edit->tracks.at(0)->id;

    check(says(c.command("plugin.block", -1, "lv2 urn:mono"), DispatchResult::Ok, ""), "plugin.block is Ok");
    check(c.until([&] { return entry("urn:mono") && !entry("urn:mono")->usable; }), "a new catalog shows it blocked");
    check(says(c.apply(add(track, c.replica.current().next_id, "urn:mono")), DispatchResult::Failed,
               std::string("Fake urn:mono cannot be used: ") + kBlockedByOperator),
          "AddInsert of a blocked plugin is refused in those words");
    check(says(c.command("plugin.block", -1, "urn:mono"), DispatchResult::Failed, "name the plug-in as"),
          "an argument without a format is refused");
    check(says(c.command("plugin.block", -1, "lv2 urn:absent"), DispatchResult::Failed,
               "no plug-in urn:absent is in the catalog"),
          "a plugin the catalog does not have is refused");
    std::string text;
    check(c.until([&] {
        std::ifstream f(list);
        text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return text.find("urn:mono") != std::string::npos;
    }),
          "the list on disk has it");
    check(text.find("Fake urn:mono") != std::string::npos && text.find("urn:hang") != std::string::npos,
          "with its name, beside the one blocked before");

    check(says(c.command("plugin.unblock", -1, "lv2 urn:mono"), DispatchResult::Ok, ""), "plugin.unblock is Ok");
    check(c.until([&] { return entry("urn:mono") && entry("urn:mono")->usable; }), "usable again");
    const auto* ok = c.apply(add(track, c.replica.current().next_id, "urn:mono"));
    check(ok && ok->result == DispatchResult::Ok, "and inserted");
    engine.shutdown();
    ::unsetenv("SONORA_FAKE_HOST_LOG");
}

void test_no_rescan_while_recording() {
    std::printf("no_rescan_while_recording\n");
    World w("recording", {"urn:mono 1 1 ok"});
    SessionConfig cfg;
    cfg.dir = w.root + "/session";
    cfg.track_count = 2;
    cfg.ring_seconds = 1.0;
    Session session(cfg);
    session.create_dirs();
    FakeBackend::Config bc;
    FakeBackend::Device d = FakeBackend::umc1820();
    d.capture = 2;
    bc.devices = {d};
    bc.mode = FakeBackend::Mode::Thread;
    FakeBackend audio(bc);
    std::string error;
    check(audio.open({"sonora", 2, 0, 48000, ""}, session, error) && audio.activate(error) &&
              audio.connect_inputs_in_order() == 2,
          "fake backend up");
    session.disk().start();
    EngineControlServer::Options opts;
    opts.plugin_host = g_host;
    {
        InProcessEngine engine(session, &audio, opts);
        check(engine.connect(error), "connected");
        Client c(engine);
        check(says(c.command("track.toggle_arm", 0), DispatchResult::Ok, ""), "armed");
        check(says(c.command("transport.record"), DispatchResult::Ok, ""), "recording");
        check(says(c.command("plugin.rescan"), DispatchResult::Failed, "not during a take"),
              "plugin.rescan refused during a take (Q3)");
        check(says(c.command("transport.stop"), DispatchResult::Ok, ""), "stopped");
        check(says(c.command("plugin.rescan"), DispatchResult::Ok, ""), "and Ok after it");
        engine.shutdown();
    }
    audio.deactivate();
    session.stop();
}


// The follow-up to #95: send_to() split and copied the whole encoded catalog
// for each client (about 17 ms at 18 MB). The worker splits it once; each
// client is handed the same frames.
void test_shared_frames() {
    std::printf("shared_frames\n");
    World w("shared", {"urn:mono 1 1 ok", "urn:stereo 2 2 ok"});
    SessionConfig cfg;
    cfg.dir = w.root + "/session";
    cfg.track_count = 2;
    Session session(cfg);
    session.create_dirs();
    EngineControlServer::Options opts;
    opts.plugin_host = g_host;
    InProcessEngine engine(session, nullptr, opts);
    std::string error;
    check(engine.connect(error), "connected");

    struct Seen {
        std::mutex m;
        api::wire::Reassembler reassembler;
        std::vector<EngineControlServer::Frame> parts;
        // Each complete catalog message: its frames, and the plugins it has.
        std::vector<std::pair<std::vector<EngineControlServer::Frame>, std::size_t>> catalogs;
        std::set<const std::string*> all;
    };
    Seen a, b;
    const auto out = [](Seen& s) {
        return [&s](EngineControlServer::Frame f) {
            std::lock_guard<std::mutex> l(s.m);
            s.all.insert(f.get());
            s.parts.push_back(f);
            std::string message, e;
            const auto r = s.reassembler.add(*f, message, e);
            if (r == api::wire::Reassembler::Result::Partial) return;
            std::vector<EngineControlServer::Frame> parts;
            parts.swap(s.parts);
            api::wire::Type t{};
            api::PluginCatalog cat;
            if (r == api::wire::Reassembler::Result::Complete && api::wire::peek_type(message, t) &&
                t == api::wire::Type::PluginCatalog && api::wire::decode(message, cat, e))
                s.catalogs.push_back({std::move(parts), cat.plugins.size()});
        };
    };
    engine.server().attach(out(a), {});
    engine.server().attach(out(b), {});
    const auto has_full = [](Seen& s) {
        std::lock_guard<std::mutex> l(s.m);
        return !s.catalogs.empty() && s.catalogs.back().second == 2;
    };
    const uint64_t end = now_ms() + 15000;
    while (now_ms() < end && !(has_full(a) && has_full(b))) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(has_full(a) && has_full(b), "both clients have the scanned catalog");
    {
        std::lock_guard<std::mutex> la(a.m), lb(b.m);
        bool shared = !a.catalogs.empty() && !b.catalogs.empty();
        if (shared) {
            for (const auto& f : a.catalogs.back().first) shared = shared && b.all.count(f.get()) == 1;
            shared = shared && a.catalogs.back().first.size() == b.catalogs.back().first.size();
        }
        check(shared, "every frame of the catalog is the one object both clients were handed");
    }
    engine.shutdown();
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--host") g_host = fs::absolute(argv[i + 1]).string();
    if (g_host.empty()) {
        std::printf("usage: test_plugin_catalog --host FAKE_HOST\n");
        return 2;
    }
    test_resolution();
    test_block();
    test_no_rescan_while_recording();
    test_shared_frames();
    if (failures) {
        std::printf("test_plugin_catalog: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_plugin_catalog: ok\n");
    return 0;
}
