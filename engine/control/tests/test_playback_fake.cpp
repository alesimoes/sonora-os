// SPDX-License-Identifier: GPL-3.0-or-later
//
// Record, then play back, end to end against drivers/fake (ADR-0007 §7,
// ADR-0009 §4.6), through the control server as an interface drives it
// (ADR-0009, update "playback on the M4/M7 control server"): track 1
// records the fake's channel-0 tone for a second; the take joins the
// server's document, and every client is sent the AddSource and RecordTake;
// return to zero and play go through the residency gate; and the fake's
// captured outputs must be the recorded tone at the -3 dB centre gain on
// both sides of the monitor bus, from the first frame rolled, with every
// other block written and no chunk missing. Then the record reply's three
// outcomes: Ok once the take has started, Failed with the reason when it is
// refused, Failed "cancelled" when a second press withdraws it. And record
// with the session's punch range set punches in over the take before.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "sonora/engine_control_in_process.h"
#include "sonora/fake_backend.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"
#include "sonora/session.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

namespace fs = std::filesystem;
using sonora::FakeBackend;
using sonora::DispatchResult;
using sonora::TransportState;

constexpr uint32_t kRate = 48000;
constexpr uint32_t kQuantum = 128;

std::size_t drain(FakeBackend& audio, uint32_t index, std::vector<float>* into = nullptr) {
    float buf[4096];
    std::size_t total = 0;
    for (;;) {
        const std::size_t n = audio.read_output(index, buf, 4096);
        if (into) into->insert(into->end(), buf, buf + n);
        total += n;
        if (n < 4096) return total;
    }
}

// Waits for a whole reader pass that started after the call, so the window
// from the playhead now is linked. Two counted passes: the first may have
// begun, and read the playhead, before the wake.
bool reader_turn(sonora::Session& s) {
    const uint64_t before = s.reader().passes();
    s.reader().wake();
    for (int i = 0; i < 20000; ++i) {
        if (s.reader().passes() >= before + 2) return true;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return false;
}

// A client of the server, as an interface's tick is: everything that
// arrives is kept.
struct Client {
    sonora::InProcessEngine& engine;
    sonora::api::Inbox inbox;

    uint64_t send(const std::string& id, int32_t target = -1) {
        sonora::api::CommandRequest r;
        r.command = id;
        r.target = target;
        return engine.send(r);
    }
    const sonora::api::CommandReply* reply(uint64_t seq) {
        engine.poll(inbox);
        for (const auto& r : inbox.replies) {
            if (r.seq == seq) return &r;
        }
        return nullptr;
    }
    // Waits up to five seconds; the fake runs no blocks meanwhile.
    bool await(uint64_t seq, sonora::api::CommandReply& out) {
        for (int i = 0; i < 2500; ++i) {
            if (const auto* r = reply(seq)) {
                out = *r;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    bool run(const std::string& id, int32_t target = -1) {
        sonora::api::CommandReply r;
        if (!await(send(id, target), r)) return false;
        if (r.result != DispatchResult::Ok) {
            std::fprintf(stderr, "  %s: %s\n", id.c_str(), r.message.c_str());
        }
        return r.result == DispatchResult::Ok;
    }
};

std::vector<const sonora::api::ModelUpdate*> op_updates(const sonora::api::Inbox& inbox) {
    std::vector<const sonora::api::ModelUpdate*> ops;
    for (const auto& m : inbox.model) {
        if (m.kind == sonora::api::ModelUpdateKind::Op) ops.push_back(&m);
    }
    return ops;
}

// A closed take reaches every client as exactly one update: a Compound
// labelled "Record" holding the track's AddSource and RecordTake. Polls on
// after the first update, so a second one sent a moment later is seen too.
bool one_take_update(Client& c) {
    for (int i = 0; i < 500 && op_updates(c.inbox).empty(); ++i) {
        c.engine.poll(c.inbox);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    for (int i = 0; i < 50; ++i) {
        c.engine.poll(c.inbox);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto ops = op_updates(c.inbox);
    if (ops.size() != 1) {
        std::fprintf(stderr, "  %zu updates for one take\n", ops.size());
        return false;
    }
    sonora::model::Operation op;
    std::string error;
    if (!sonora::model::decode_operation(ops[0]->bytes, op, error)) {
        std::fprintf(stderr, "  the take's update does not decode: %s\n", error.c_str());
        return false;
    }
    const auto* take = std::get_if<sonora::model::Compound>(&op);
    return take != nullptr && take->label == "Record" && take->ops.size() == 2 &&
           std::holds_alternative<sonora::model::AddSource>(take->ops[0]) &&
           std::holds_alternative<sonora::model::RecordTake>(take->ops[1]);
}

sonora::SessionConfig config(const fs::path& dir) {
    sonora::SessionConfig cfg;
    cfg.dir = dir.string();
    cfg.track_count = 2;
    cfg.sample_rate = kRate;
    cfg.format = sonora::WavWriter::Format::Float32;
    cfg.ring_seconds = 2.0;
    cfg.playback.chunk_frames = 4096;
    cfg.playback.window_seconds = 0.1;
    cfg.playback.max_block_frames = 256;
    return cfg;
}

int test_record_then_play_back() {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_playback_fake";
    fs::remove_all(dir);
    sonora::Session s(config(dir));
    CHECK(s.create_dirs());

    FakeBackend::Config fc;
    fc.quantum = kQuantum;
    fc.output_capture_frames = 4 * kRate;
    FakeBackend audio(fc);
    std::string error;
    CHECK(audio.open({"sonora", 2, 2, kRate, ""}, s, error));
    CHECK(audio.activate(error));
    CHECK(audio.connect_inputs_in_order() == 2);
    s.disk().start();
    s.reader().start();

    {
        sonora::InProcessEngine engine(s, &audio);
        CHECK(engine.connect(error));
        Client c{engine, {}};

        // One second of channel 0's tone on track 1, from server frame 0:
        // the reply comes once the take has started, before any block.
        CHECK(c.run("track.toggle_arm", 0));
        CHECK(c.run("transport.record"));
        CHECK(s.transport().state() == TransportState::Recording);
        constexpr uint32_t kBlocks = kRate / kQuantum;
        CHECK(audio.run_blocks(kBlocks) == kBlocks);
        CHECK(c.run("transport.stop"));
        CHECK(s.tracks()[0]->writer().frames_written() == uint64_t{kBlocks} * kQuantum);
        // After the stop: the take's tracks are the ones armed when it closed.
        CHECK(c.run("track.toggle_arm", 0));
        // Disarmed, its input fades out over monitor_ramp_frames rather than
        // stopping dead; let that pass before any output is kept.
        CHECK(audio.run_blocks(2) == 2);

        // The take is in the document every client replicates: its source
        // and its region, as one compound and so one update (ADR-0009
        // update 2026-10-14).
        CHECK(one_take_update(c));

        CHECK(c.run("transport.return_to_zero"));
        CHECK(c.run("transport.play"));
        drain(audio, 0);
        drain(audio, 1);
        // The callback keeps running while the gate waits: the locate lands
        // at a block start, and the roll starts once the take's window is
        // resident. Every frame from here is kept, so the rolled ones are
        // the last `playhead` of them.
        std::vector<float> left, right;
        for (int i = 0; i < 2000 && s.transport().state() != TransportState::Rolling; ++i) {
            CHECK(audio.run_blocks(1) == 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(s.transport().state() == TransportState::Rolling);
        CHECK(s.transport().playhead() <= kQuantum);

        // The whole take and a little past its end. The blocks run faster
        // than real time, so the reader is given what real time gives it:
        // in 8 blocks (21 ms) its 5 ms tick passes about four times; here at
        // least one whole pass. A sleep instead outran the reader on a loaded
        // host, and a chunk it had not linked yet played as silence.
        constexpr uint32_t kPlayBlocks = kBlocks + 20;
        for (uint32_t b = 0; b < kPlayBlocks; ++b) {
            if (b % 8 == 0) CHECK(reader_turn(s));
            CHECK(audio.run_blocks(1) == 1);
        }
        drain(audio, 0, &left);
        drain(audio, 1, &right);
        CHECK(right.size() == left.size());
        const uint64_t rolled = s.transport().playhead();
        CHECK(rolled >= uint64_t{kPlayBlocks} * kQuantum);
        CHECK(left.size() >= rolled);
        const std::size_t first = left.size() - static_cast<std::size_t>(rolled);

        const float gain = std::pow(10.0f, -3.0f / 20.0f);
        const uint64_t take = uint64_t{kBlocks} * kQuantum;
        double worst = 0.0;
        int64_t worst_at = 0;   // in frames rolled
        for (std::size_t i = 0; i < left.size(); ++i) {
            const uint64_t t = i - first;
            const float want =
                i < first ? 0.0f : (t < take ? FakeBackend::tone(0, t, kRate) * gain : 0.0f);
            const double e = std::max(std::fabs(left[i] - want), std::fabs(right[i] - want));
            if (e > worst) {
                worst = e;
                worst_at = static_cast<int64_t>(i) - static_cast<int64_t>(first);
            }
        }
        // A chunk missed plays silence, and the residual is then the tone's
        // peak at the bus gain, 0.354: the misses say which it was.
        std::printf("playback through the server on drivers/fake: %llu frames rolled,"
                    " worst residual %.3g at frame %lld, %llu chunk misses\n",
                    static_cast<unsigned long long>(rolled), worst,
                    static_cast<long long>(worst_at),
                    static_cast<unsigned long long>(s.playback_misses()));
        CHECK(worst < 1e-6);
        CHECK(s.playback_misses() == 0);
        CHECK(audio.unwritten_output_periods() == 0);
        CHECK(audio.output_dropped() == 0);
        CHECK(audio.xruns() == 0);
        CHECK(c.run("transport.stop"));

        engine.shutdown();
        audio.deactivate();
        engine.server().after_deactivate();
    }
    s.reader().stop();
    fs::remove_all(dir);
    return 0;
}

// What a record reply says once the gate resolves. The fake runs no
// blocks, so a take asked for behind a locate waits: the locate is applied
// only at a block start.
int test_record_replies() {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_playback_fake_replies";
    fs::remove_all(dir);
    sonora::Session s(config(dir));
    CHECK(s.create_dirs());
    FakeBackend::Config fc;
    fc.quantum = kQuantum;
    FakeBackend audio(fc);
    std::string error;
    CHECK(audio.open({"sonora", 2, 2, kRate, ""}, s, error));
    CHECK(audio.activate(error));
    CHECK(audio.connect_inputs_in_order() == 2);
    s.disk().start();
    s.reader().start();
    {
        sonora::InProcessEngine engine(s, &audio);
        CHECK(engine.connect(error));
        Client c{engine, {}};
        sonora::api::CommandReply r;

        // Refused inside the gate: the reason, not an Ok.
        CHECK(c.await(c.send("transport.record"), r));
        CHECK(r.result == DispatchResult::Failed);
        CHECK(r.message.find("no track is armed") != std::string::npos);

        // Withdrawn: the first press waits behind the locate, and the
        // second takes it back. The first is told so; the second is Ok.
        CHECK(c.run("track.toggle_arm", 1));
        CHECK(c.run("transport.return_to_zero"));
        const uint64_t first = c.send("transport.toggle_record");
        for (int i = 0; i < 25; ++i) {
            CHECK(c.reply(first) == nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(s.transport().state() == TransportState::Stopped);
        CHECK(c.run("transport.toggle_record"));
        CHECK(c.await(first, r));
        CHECK(r.result == DispatchResult::Failed && r.message == "cancelled");
        CHECK(s.transport().state() == TransportState::Stopped);

        // Started: held until the locate lands and the take opens, then Ok.
        const uint64_t again = c.send("transport.record");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(c.reply(again) == nullptr);
        CHECK(audio.run_blocks(1) == 1);
        CHECK(c.await(again, r));
        CHECK(r.result == DispatchResult::Ok);
        CHECK(s.transport().state() == TransportState::Recording);
        CHECK(audio.run_blocks(4) == 4);
        CHECK(c.run("transport.stop"));

        // Still waiting when the server stops: answered, not left for a
        // server started again to hold.
        CHECK(c.run("transport.return_to_zero"));
        const uint64_t held = c.send("transport.record");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(c.reply(held) == nullptr);
        engine.shutdown();
        CHECK(c.await(held, r));
        CHECK(r.result == DispatchResult::Failed && r.message == "cancelled");
        CHECK(audio.run_blocks(4) == 4);
        CHECK(s.transport().state() == TransportState::Stopped);

        audio.deactivate();
        engine.server().after_deactivate();
    }
    s.reader().stop();
    fs::remove_all(dir);
    return 0;
}

sonora::model::ReplicationUpdate from_wire(const sonora::api::ModelUpdate& m) {
    using sonora::api::ModelUpdateKind;
    using Kind = sonora::model::ReplicationUpdate::Kind;
    sonora::model::ReplicationUpdate u;
    u.kind = m.kind == ModelUpdateKind::Op     ? Kind::Op
             : m.kind == ModelUpdateKind::Undo ? Kind::Undo
             : m.kind == ModelUpdateKind::Redo ? Kind::Redo
                                               : Kind::Snapshot;
    u.revision = m.revision;
    u.undo_floor = m.undo_floor;
    u.next_id = m.next_id;
    u.bytes = m.bytes;
    u.label = m.label;
    return u;
}

// What a replica shows and what the Track atomics hold are the same mix
// after every model request, an undo and a redo included (ADR-0009 §2).
// Mix is outside undo (§3): undoing the edit before a mute leaves the track
// muted, in the document and in the audio alike.
int test_mix_follows_the_document() {
    namespace model = sonora::model;
    const fs::path dir = fs::temp_directory_path() / "sonora_test_playback_fake_mix";
    fs::remove_all(dir);
    sonora::Session s(config(dir));
    CHECK(s.create_dirs());
    FakeBackend::Config fc;
    fc.quantum = kQuantum;
    FakeBackend audio(fc);
    std::string error;
    CHECK(audio.open({"sonora", 2, 2, kRate, ""}, s, error));
    CHECK(audio.activate(error));
    s.disk().start();
    s.reader().start();
    {
        sonora::InProcessEngine engine(s, &audio);
        CHECK(engine.connect(error));
        Client c{engine, {}};
        model::Replica replica;
        std::size_t fed = 0;
        const auto feed = [&]() {
            engine.poll(c.inbox);
            for (; fed < c.inbox.model.size(); ++fed) {
                std::string why;
                if (!replica.receive(from_wire(c.inbox.model[fed]), why)) return false;
            }
            return true;
        };
        const auto submit = [&](sonora::api::ModelAction action, const model::Operation* op) {
            sonora::api::ModelRequest m;
            m.action = action;
            std::string why;
            if (op != nullptr && !model::encode_operation(*op, m.op, why)) return false;
            sonora::api::CommandReply r;
            return c.await(engine.submit(m), r) && r.result == DispatchResult::Ok && feed();
        };
        for (int i = 0; i < 500 && !replica.synced(); ++i) {
            CHECK(feed());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(replica.synced());
        const model::Id t1 = replica.current().edit->tracks[0]->id;
        const auto same_mix = [&]() {
            const model::TrackMix* m = replica.current().mix.find(t1);
            const sonora::Track& t = *s.tracks()[0];
            return m != nullptr && t.muted() == m->mute && t.soloed() == m->solo &&
                   std::fabs(t.gain() - std::pow(10.0f, m->gain_db / 20.0f)) < 1e-6f;
        };

        // An undoable edit, so the undo has something to take back.
        model::AddMarker add;
        add.marker.id = replica.current().next_id;
        add.marker.name = "verse";
        model::Operation op = add;
        CHECK(submit(sonora::api::ModelAction::Apply, &op));

        model::SetMix mute;
        mute.mix = *replica.current().mix.find(t1);
        mute.mix.mute = true;
        mute.mix.gain_db = -6.0f;
        op = mute;
        CHECK(submit(sonora::api::ModelAction::Apply, &op));
        CHECK(s.tracks()[0]->muted());
        CHECK(same_mix());

        CHECK(submit(sonora::api::ModelAction::Undo, nullptr));
        CHECK(replica.current().edit->markers->markers.empty());
        CHECK(replica.current().mix.find(t1)->mute);
        CHECK(same_mix());
        CHECK(submit(sonora::api::ModelAction::Redo, nullptr));
        CHECK(replica.current().edit->markers->markers.size() == 1);
        CHECK(same_mix());

        mute.mix.mute = false;
        op = mute;
        CHECK(submit(sonora::api::ModelAction::Apply, &op));
        CHECK(!s.tracks()[0]->muted());
        CHECK(submit(sonora::api::ModelAction::Undo, nullptr));
        CHECK(same_mix());

        engine.shutdown();
        audio.deactivate();
        engine.server().after_deactivate();
    }
    s.reader().stop();
    fs::remove_all(dir);
    return 0;
}

// Space is transport.toggle_play in both shipped keymaps, and the only key
// there that stops a take: during a take it closes it, the take joins the
// document and the next take number is announced.
int test_toggle_play_stops_a_take() {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_playback_fake_toggle";
    fs::remove_all(dir);
    sonora::Session s(config(dir));
    CHECK(s.create_dirs());
    FakeBackend::Config fc;
    fc.quantum = kQuantum;
    FakeBackend audio(fc);
    std::string error;
    CHECK(audio.open({"sonora", 2, 2, kRate, ""}, s, error));
    CHECK(audio.activate(error));
    CHECK(audio.connect_inputs_in_order() == 2);
    s.disk().start();
    s.reader().start();
    {
        sonora::InProcessEngine engine(s, &audio);
        CHECK(engine.connect(error));
        Client c{engine, {}};
        const auto take_shown = [&](int32_t take) {
            for (int i = 0; i < 2500; ++i) {
                engine.poll(c.inbox);
                const auto t = engine.state().transport();
                if (!t.stale && t.take == take) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        };
        const int32_t take = s.next_free_take();
        CHECK(take_shown(take));

        CHECK(c.run("track.toggle_arm", 0));
        CHECK(c.run("transport.record"));
        CHECK(s.transport().state() == TransportState::Recording);
        CHECK(audio.run_blocks(16) == 16);
        CHECK(c.run("transport.toggle_play"));
        CHECK(s.transport().state() == TransportState::Stopped);
        CHECK(s.tracks()[0]->writer().frames_written() == uint64_t{16} * kQuantum);
        CHECK(take_shown(take + 1));

        CHECK(one_take_update(c));

        engine.shutdown();
        audio.deactivate();
        engine.server().after_deactivate();
    }
    s.reader().stop();
    fs::remove_all(dir);
    return 0;
}

// A take the model refuses is an error the operator sees, not a side line:
// TakeFailed naming why, and the take-failed health flag. Its audio still
// reaches every client's pool. The refusal is made real by recording past
// the end of the timeline, where no RecordTake fits.
int test_a_refused_take_is_shown_as_failed() {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_playback_fake_refused";
    fs::remove_all(dir);
    sonora::Session s(config(dir));
    CHECK(s.create_dirs());
    FakeBackend::Config fc;
    fc.quantum = kQuantum;
    FakeBackend audio(fc);
    std::string error;
    CHECK(audio.open({"sonora", 2, 2, kRate, ""}, s, error));
    CHECK(audio.activate(error));
    CHECK(audio.connect_inputs_in_order() == 2);
    s.disk().start();
    s.reader().start();
    {
        sonora::InProcessEngine engine(s, &audio);
        CHECK(engine.connect(error));
        Client c{engine, {}};

        const uint64_t near_the_end = uint64_t{sonora::model::kMaxInteger} - kQuantum;
        CHECK(s.post_locate(near_the_end));
        CHECK(audio.run_blocks(2) == 2);
        CHECK(s.transport().playhead() >= near_the_end);

        CHECK(c.run("track.toggle_arm", 0));
        CHECK(c.run("transport.record"));
        CHECK(audio.run_blocks(16) == 16);
        CHECK(c.run("transport.stop"));

        const auto failed = [&]() -> const sonora::api::EngineEvent* {
            for (const auto& e : c.inbox.events) {
                if (e.kind == sonora::api::EventKind::TakeFailed && !e.text.empty()) return &e;
            }
            return nullptr;
        };
        for (int i = 0; i < 1000 && failed() == nullptr; ++i) {
            engine.poll(c.inbox);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const sonora::api::EngineEvent* e = failed();
        CHECK(e != nullptr);
        CHECK(e->text.find("not on the timeline") != std::string::npos);
        CHECK(e->text.find("past the end of the timeline") != std::string::npos);
        bool flagged = false;
        for (int i = 0; i < 1000 && !flagged; ++i) {
            engine.poll(c.inbox);
            const auto t = engine.state().transport();
            flagged = !t.stale && (t.health_flags & sonora::api::kHealthTakeFailed) != 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(flagged);
        // Also a Notice, which stays for the session: the alert clears on
        // the next clean take, which while rolling can be the next tick,
        // and take N is still not on the timeline then.
        bool noticed = false;
        for (const auto& n : c.inbox.events) {
            noticed = noticed || (n.kind == sonora::api::EventKind::Notice &&
                                  n.text.find("not on the timeline") != std::string::npos);
        }
        CHECK(noticed);
        // The fallback's AddSource reached the client: the audio is in the
        // session's pool.
        const auto ops = op_updates(c.inbox);
        CHECK(ops.size() == 1);
        sonora::model::Operation op;
        CHECK(sonora::model::decode_operation(ops[0]->bytes, op, error));
        CHECK(std::holds_alternative<sonora::model::AddSource>(op));

        engine.shutdown();
        audio.deactivate();
        engine.server().after_deactivate();
    }
    s.reader().stop();
    fs::remove_all(dir);
    return 0;
}

// Record with a punch range set punches (timeline-model.md, "Punch in"):
// take 1 records track 1 whole for a second; the session's punch range is
// set to [A, B); from zero, transport.record captures [A - H, B + H) with
// H a second, cut at zero, and once stopped the take goes in over [A, B)
// only, take 1 split around it with a crossfade of the session's
// punch_crossfade at each end, sent to every client as one update.
int test_record_with_a_punch_range_punches() {
    namespace model = sonora::model;
    const fs::path dir = fs::temp_directory_path() / "sonora_test_playback_fake_punch";
    fs::remove_all(dir);
    sonora::Session s(config(dir));
    CHECK(s.create_dirs());
    FakeBackend::Config fc;
    fc.quantum = kQuantum;
    FakeBackend audio(fc);
    std::string error;
    CHECK(audio.open({"sonora", 2, 2, kRate, ""}, s, error));
    CHECK(audio.activate(error));
    CHECK(audio.connect_inputs_in_order() == 2);
    s.disk().start();
    s.reader().start();
    constexpr model::Frames kA = 18000, kB = 30000, kX = 240;
    constexpr model::Frames kH = kRate;
    {
        sonora::InProcessEngine engine(s, &audio);
        CHECK(engine.connect(error));
        Client c{engine, {}};
        model::Replica replica;
        std::size_t fed = 0;
        const auto feed = [&]() {
            engine.poll(c.inbox);
            for (; fed < c.inbox.model.size(); ++fed) {
                std::string why;
                if (!replica.receive(from_wire(c.inbox.model[fed]), why)) return false;
            }
            return true;
        };
        const auto playlist = [&]() -> const model::Playlist& {
            const model::TrackEdit& t = *replica.current().edit->tracks[0];
            for (const auto& p : t.playlists) {
                if (p->id == t.active_playlist) return *p;
            }
            return *t.playlists.front();
        };
        const auto await_regions = [&](std::size_t n) {
            for (int i = 0; i < 2500; ++i) {
                if (!feed()) return false;
                if (replica.synced() && playlist().regions.size() == n) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        };

        CHECK(c.run("track.toggle_arm", 0));
        CHECK(c.run("transport.record"));
        constexpr uint32_t kBlocks = kRate / kQuantum;
        CHECK(audio.run_blocks(kBlocks) == kBlocks);
        CHECK(c.run("transport.stop"));
        CHECK(await_regions(1));
        const model::Region old = playlist().regions[0];
        CHECK(old.position == 0 && old.length == model::Frames{kBlocks} * kQuantum);

        model::SetSettings set;
        set.settings = replica.current().settings;
        set.settings.transport.punch = {{kA, kB}};
        set.settings.transport.punch_crossfade = kX;
        sonora::api::ModelRequest m;
        m.action = sonora::api::ModelAction::Apply;
        CHECK(model::encode_operation(model::Operation{set}, m.op, error));
        sonora::api::CommandReply r;
        CHECK(c.await(engine.submit(m), r) && r.result == DispatchResult::Ok);

        CHECK(c.run("transport.return_to_zero"));
        const uint64_t rec = c.send("transport.record");
        bool answered = false;
        for (int i = 0; i < 2000 && !answered; ++i) {
            CHECK(audio.run_blocks(1) == 1);
            if (c.reply(rec) != nullptr) answered = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(c.await(rec, r));
        if (r.result != DispatchResult::Ok) std::fprintf(stderr, "  %s\n", r.message.c_str());
        CHECK(r.result == DispatchResult::Ok);
        CHECK(s.transport().state() == TransportState::Recording);
        uint32_t blocks = 0;
        while (s.transport().playhead() < static_cast<uint64_t>(kB + kH + kQuantum)) {
            if (blocks++ % 8 == 0) CHECK(reader_turn(s));
            CHECK(audio.run_blocks(1) == 1);
        }
        // Past B + H the take's files are closed: the transport drops out of
        // record and rolls on, as Pro Tools' does at the punch-out, rather
        // than showing Recording while nothing records.
        const auto await_rolling = [&]() {
            for (int i = 0; i < 2500; ++i) {
                engine.poll(c.inbox);
                if (s.transport().state() == TransportState::Rolling) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        };
        CHECK(await_rolling());
        CHECK(s.recording_take() == 0);
        // Placed before any stop: the take closed in band.
        CHECK(await_regions(3));
        CHECK(c.run("transport.stop"));
        CHECK(await_regions(3));

        const model::Playlist& p = playlist();
        CHECK(p.regions[0].id == old.id && p.regions[0].position == 0 &&
              p.regions[0].end() == kA);
        const model::Region& n = p.regions[1];
        CHECK(n.position == kA && n.end() == kB);
        // Captured from zero, so A is A frames into the take's file.
        CHECK(n.source_offset == kA);
        const model::Source* src = replica.current().sources->find(n.channels.at(0).source);
        CHECK(src != nullptr && src->length == kB + kH);
        CHECK(p.regions[2].position == kB && p.regions[2].end() == old.end());
        CHECK(p.crossfades.size() == 2);
        for (const model::Crossfade& x : p.crossfades) CHECK(x.length == kX);
        CHECK(s.tracks()[0]->overruns() == 0);

        // transport.punch with the range set does the same past B + H.
        CHECK(c.run("transport.return_to_zero"));
        const uint64_t punch = c.send("transport.punch");
        answered = false;
        for (int i = 0; i < 2000 && !answered; ++i) {
            CHECK(audio.run_blocks(1) == 1);
            if (c.reply(punch) != nullptr) answered = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(c.await(punch, r));
        if (r.result != DispatchResult::Ok) std::fprintf(stderr, "  %s\n", r.message.c_str());
        CHECK(r.result == DispatchResult::Ok);
        CHECK(s.transport().state() == TransportState::Recording);
        while (s.transport().playhead() < static_cast<uint64_t>(kB + kH + kQuantum)) {
            if (blocks++ % 8 == 0) CHECK(reader_turn(s));
            CHECK(audio.run_blocks(1) == 1);
        }
        CHECK(await_rolling());
        CHECK(c.run("transport.stop"));

        engine.shutdown();
        audio.deactivate();
        engine.server().after_deactivate();
    }
    s.reader().stop();
    fs::remove_all(dir);
    return 0;
}

}  // namespace

int main() {
    if (test_record_then_play_back()) return 1;
    if (test_record_with_a_punch_range_punches()) return 1;
    if (test_record_replies()) return 1;
    if (test_mix_follows_the_document()) return 1;
    if (test_toggle_play_stops_a_take()) return 1;
    if (test_a_refused_take_is_shown_as_failed()) return 1;
    std::printf("playback_fake: ok\n");
    return 0;
}
