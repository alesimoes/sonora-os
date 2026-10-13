// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control server on a socket (ADR-0007 §3, M7), driven by raw
// platform channels: what any client, however buggy, can do to the engine.
//
// - Several clients each get Hello with the state block, mapped read-only.
// - A reply goes to the asker; a standing event reaches a late client.
// - A client gone mid-take leaves the take recording, and the files valid.
// - Malformed input (a frame claiming a gigabyte, a frame out of place, a
//   message only the engine sends, a datagram over the frame size) gets
//   that client disconnected; the take and the other clients carry on.
#include <sys/mman.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sonora/api/state_view.h"
#include "sonora/api/wire.h"
#include "sonora/control_socket_server.h"
#include "sonora/engine_control_server.h"
#include "sonora/fake_backend.h"
#include "sonora/platform/ipc.h"
#include "sonora/session.h"

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {

namespace fs = std::filesystem;
using namespace sonora;
using Clock = std::chrono::steady_clock;
using platform::IoStatus;

constexpr uint32_t kTracks = 4;

fs::path fresh_dir(const char* what) {
    static int n = 0;
    fs::path p = fs::temp_directory_path() /
                 ("sonora_test_control_socket_" + std::string(what) + "_" + std::to_string(++n));
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

// The engine half, as engine/app wires it: a session on drivers/fake, the
// state block in a shared region, the server on a socket.
class Engine {
public:
    explicit Engine(const char* what) : dir_(fresh_dir(what)), session_(config(dir_)) {
        FakeBackend::Config bc;
        FakeBackend::Device d = FakeBackend::umc1820();
        d.capture = kTracks;
        bc.devices = {d};
        bc.mode = FakeBackend::Mode::Thread;
        audio_ = std::make_unique<FakeBackend>(bc);
        std::string error;
        ok_ = session_.create_dirs() &&
              audio_->open({"sonora", kTracks, 0, 48000, ""}, session_, error) &&
              audio_->activate(error) && audio_->connect_inputs_in_order() == kTracks;
        session_.disk().start();
        region_ = platform::SharedRegion::create("sonora-state", api::state_block_bytes(kTracks),
                                                 error);
        // A directory of its own, made 0700 by the listener.
        endpoint_ = dir_.string() + "-sock/engine.sock";
        auto listener = platform::ControlListener::listen(endpoint_, error);
        ok_ = ok_ && region_ && listener;
        if (!ok_) {
            std::fprintf(stderr, "  engine: %s\n", error.c_str());
            return;
        }
        EngineControlServer::Options opts;
        opts.state_memory = region_->writable_data();
        opts.state_memory_bytes = region_->bytes();
        server_ = std::make_unique<EngineControlServer>(session_, audio_.get(), opts);
        api::EngineEvent early;
        early.kind = api::EventKind::Notice;
        early.text = "a notice from before anyone connected";
        server_->post_event(early);
        server_->start();
        sockets_ = std::make_unique<ControlSocketServer>(*server_, *region_, std::move(listener));
        sockets_->start();
    }
    ~Engine() {
        if (sockets_) sockets_->stop();
        if (server_) server_->stop();
        if (audio_) audio_->deactivate();
        session_.stop();
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::remove_all(dir_.string() + "-sock", ec);
    }

    bool ok() const { return ok_ && server_ && server_->state_ready(); }
    const std::string& endpoint() const { return endpoint_; }
    Session& session() { return session_; }
    EngineControlServer& server() { return *server_; }
    ControlSocketServer& sockets() { return *sockets_; }
    const fs::path& dir() const { return dir_; }

private:
    static SessionConfig config(const fs::path& dir) {
        SessionConfig cfg;
        cfg.dir = dir.string();
        cfg.track_count = kTracks;
        cfg.ring_seconds = 1.0;
        return cfg;
    }

    fs::path dir_;
    Session session_;
    std::unique_ptr<FakeBackend> audio_;
    std::unique_ptr<platform::SharedRegion> region_;
    std::string endpoint_;
    std::unique_ptr<EngineControlServer> server_;
    std::unique_ptr<ControlSocketServer> sockets_;
    bool ok_ = false;
};

// A client with nothing between it and the socket but the codec.
class RawClient {
public:
    explicit RawClient(const std::string& endpoint) {
        std::string error;
        channel_ = platform::ControlChannel::connect(endpoint, error);
        if (!channel_) std::fprintf(stderr, "  client: %s\n", error.c_str());
    }

    bool connected() const { return channel_ != nullptr; }
    platform::ControlChannel& channel() { return *channel_; }

    // The next whole message within `ms`, or false.
    bool next(std::string& message, int ms = 2000) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        std::string frame, error;
        while (Clock::now() < end) {
            std::unique_ptr<platform::SharedRegion> region;
            const IoStatus s =
                channel_->receive(frame, api::wire::kMaxFrameBytes, &region, 50, error);
            if (s == IoStatus::Closed || s == IoStatus::Error) {
                closed_ = true;
                return false;
            }
            if (s != IoStatus::Ok) continue;
            if (region) region_ = std::move(region);
            if (reassembler_.add(frame, message, error) ==
                api::wire::Reassembler::Result::Complete) {
                return true;
            }
        }
        return false;
    }

    // Reads up to and including the message of `type`.
    bool until(api::wire::Type type, std::string& message, int ms = 2000) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end) {
            if (!next(message, 100)) {
                if (closed_) return false;
                continue;
            }
            api::wire::Type t{};
            if (api::wire::peek_type(message, t) && t == type) return true;
        }
        return false;
    }

    // Hello, its region, and a StateView on it.
    bool greet() {
        std::string m;
        if (!next(m) || !api::wire::decode(m, hello_, error_)) return false;
        return region_ && view_.attach(region_->data(), region_->bytes(), error_);
    }

    void send_message(const std::string& message) {
        std::vector<std::string> frames;
        std::string error;
        api::wire::split_frames(message, frames, error);
        for (const auto& f : frames) channel_->send(f, 1000, error);
    }

    uint64_t command(const std::string& id, int32_t target = -1) {
        api::CommandRequest r;
        r.seq = ++seq_;
        r.command = id;
        r.target = target;
        std::string bytes;
        api::wire::encode(r, bytes);
        send_message(bytes);
        return r.seq;
    }

    // The reply to `seq`, skipping anything else.
    bool reply(uint64_t seq, api::CommandReply& out) {
        std::string m;
        while (until(api::wire::Type::CommandReply, m)) {
            std::string error;
            if (api::wire::decode(m, out, error) && out.seq == seq) return true;
        }
        return false;
    }

    // Whether the engine closed this connection within `ms`.
    bool closed_within(int ms) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        std::string m;
        while (Clock::now() < end && !closed_) next(m, 50);
        return closed_;
    }

    const api::Hello& hello() const { return hello_; }
    api::StateView& view() { return view_; }
    const platform::SharedRegion* region() const { return region_.get(); }

private:
    std::unique_ptr<platform::ControlChannel> channel_;
    std::unique_ptr<platform::SharedRegion> region_;
    api::wire::Reassembler reassembler_;
    api::Hello hello_;
    api::StateView view_;
    std::string error_;
    uint64_t seq_ = 0;
    bool closed_ = false;
};

template <typename Pred>
bool eventually(Pred pred, int ms = 2000) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// A take on disk that another program can open: RIFF and data sizes agree
// with the file, and it holds whole frames of audio.
bool wav_valid(const fs::path& p, uint64_t& data_bytes) {
    std::ifstream f(p, std::ios::binary);
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), {});
    auto u32 = [&b](std::size_t at) {
        return static_cast<uint32_t>(b[at]) | static_cast<uint32_t>(b[at + 1]) << 8 |
               static_cast<uint32_t>(b[at + 2]) << 16 | static_cast<uint32_t>(b[at + 3]) << 24;
    };
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0) return false;
    if (u32(4) != b.size() - 8) return false;
    std::size_t at = 12;
    while (at + 8 <= b.size()) {
        const uint32_t size = u32(at + 4);
        if (std::memcmp(&b[at], "data", 4) == 0) {
            data_bytes = size;
            return at + 8 + size == b.size() && size > 0 && size % 3 == 0;
        }
        at += 8 + size + (size & 1);
    }
    return false;
}

void several_clients_each_get_hello_and_the_block() {
    Engine e("hello");
    CHECK(e.ok());
    if (!e.ok()) return;
    RawClient a(e.endpoint()), b(e.endpoint());
    CHECK(a.greet());
    CHECK(b.greet());
    CHECK(a.hello().protocol_version == api::kProtocolVersion);
    CHECK(a.hello().track_count == kTracks && b.hello().track_count == kTracks);
    CHECK(eventually([&]() { return e.server().clients() == 2; }));
    // Both read the one block, and it is live.
    const uint64_t hb = a.view().transport().heartbeat;
    CHECK(eventually([&]() { return b.view().transport().heartbeat > hb + 2; }));
    // The kernel will not let a client write it.
    if (a.region() != nullptr) {
        const int rc =
            ::mprotect(const_cast<void*>(a.region()->data()), 4096, PROT_READ | PROT_WRITE);
        CHECK(rc != 0 && errno == EACCES);
    }
    // After the snapshot, the notice posted before anyone connected.
    std::string m;
    CHECK(a.until(api::wire::Type::ModelUpdate, m));
    CHECK(a.until(api::wire::Type::EngineEvent, m));
    api::EngineEvent ev;
    std::string error;
    CHECK(api::wire::decode(m, ev, error) && ev.kind == api::EventKind::Notice);
}

void a_reply_goes_to_the_asker_alone() {
    Engine e("reply");
    if (!e.ok()) {
        CHECK(e.ok());
        return;
    }
    RawClient a(e.endpoint()), b(e.endpoint());
    CHECK(a.greet() && b.greet());
    const uint64_t seq = a.command("track.toggle_arm", 0);
    api::CommandReply r;
    CHECK(a.reply(seq, r) && r.result == DispatchResult::Ok);
    std::string m;
    CHECK(!b.until(api::wire::Type::CommandReply, m, 300));
    // Both see the arm in the block.
    CHECK(eventually([&]() { return b.view().track(0).armed(); }));
}

void a_client_gone_mid_take_leaves_the_take_recording() {
    Engine e("gone");
    if (!e.ok()) {
        CHECK(e.ok());
        return;
    }
    auto a = std::make_unique<RawClient>(e.endpoint());
    CHECK(a->greet());
    api::CommandReply r;
    CHECK(a->reply(a->command("track.arm_all"), r) && r.result == DispatchResult::Ok);
    CHECK(a->reply(a->command("transport.record"), r) && r.result == DispatchResult::Ok);
    CHECK(eventually([&]() {
        return a->view().transport().state == api::TransportState::Recording;
    }));
    // As a crashed interface: the socket closes with no goodbye.
    a.reset();
    CHECK(eventually([&]() { return e.server().clients() == 0; }));
    const uint64_t at = e.session().transport().playhead();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(e.session().transport().state() == TransportState::Recording);
    CHECK(e.session().transport().playhead() > at + 48000 / 10);

    // A new interface finds the take running and stops it.
    RawClient b(e.endpoint());
    CHECK(b.greet());
    CHECK(b.view().transport().state == api::TransportState::Recording);
    CHECK(b.reply(b.command("transport.stop"), r) && r.result == DispatchResult::Ok);
    CHECK(e.session().disk().total_overruns() == 0);
    int files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(e.dir())) {
        if (entry.path().extension() != ".wav") continue;
        uint64_t bytes = 0;
        CHECK(wav_valid(entry.path(), bytes));
        CHECK(bytes >= 3ull * 48000 * 3 / 10);
        ++files;
    }
    CHECK(files == static_cast<int>(kTracks));
}

void malformed_input_is_refused_and_nothing_else_changes() {
    Engine e("malformed");
    if (!e.ok()) {
        CHECK(e.ok());
        return;
    }
    RawClient good(e.endpoint());
    CHECK(good.greet());
    api::CommandReply r;
    CHECK(good.reply(good.command("track.arm_all"), r) && r.result == DispatchResult::Ok);
    CHECK(good.reply(good.command("transport.record"), r) && r.result == DispatchResult::Ok);

    std::string error;
    // A frame header claiming a whole message of 1 GiB.
    {
        RawClient bad(e.endpoint());
        CHECK(bad.greet());
        std::string frame(12, '\0');
        const uint32_t huge = 1u << 30;
        std::memcpy(&frame[0], &huge, 4);
        bad.channel().send(frame, 1000, error);
        CHECK(bad.closed_within(2000));
    }
    // A frame that continues a message nobody began.
    {
        RawClient bad(e.endpoint());
        CHECK(bad.greet());
        std::string frame(12, '\0');
        const uint32_t len = 100, off = 50;
        std::memcpy(&frame[0], &len, 4);
        std::memcpy(&frame[4], &off, 4);
        bad.channel().send(frame, 1000, error);
        CHECK(bad.closed_within(2000));
    }
    // Well framed, but a message only the engine sends.
    {
        RawClient bad(e.endpoint());
        CHECK(bad.greet());
        std::string bytes;
        api::wire::encode(api::Hello{}, bytes);
        bad.send_message(bytes);
        CHECK(bad.closed_within(2000));
    }
    // A command whose fields do not decode.
    {
        RawClient bad(e.endpoint());
        CHECK(bad.greet());
        std::string bytes;
        api::CommandRequest req;
        req.command = "transport.stop";
        api::wire::encode(req, bytes);
        bytes.resize(bytes.size() - 3);
        bad.send_message(bytes);
        CHECK(bad.closed_within(2000));
    }
    // A datagram larger than any frame may be.
    {
        RawClient bad(e.endpoint());
        CHECK(bad.greet());
        bad.channel().send(std::string(api::wire::kMaxFrameBytes + 100, 'z'), 1000, error);
        CHECK(bad.closed_within(2000));
    }
    CHECK(e.server().refused() >= 4);
    // The take never noticed, and the good client is still answered.
    CHECK(e.session().transport().state() == TransportState::Recording);
    CHECK(good.reply(good.command("transport.stop"), r) && r.result == DispatchResult::Ok);
    CHECK(e.session().disk().total_overruns() == 0);
    CHECK(eventually([&]() { return e.server().clients() == 1; }));
}

// Clients that flood: frames of large messages that never end, as fast as
// their sockets take them. The engine stops reading each once its backlog is
// full, so what it holds stays bounded; no more large messages than the cap
// are reassembled at once; and another client's commands are answered as
// promptly as ever.
void flooding_clients_are_held_back_and_others_carry_on() {
    Engine e("flood");
    if (!e.ok()) {
        CHECK(e.ok());
        return;
    }
    const EngineControlServer::Options defaults;
    RawClient good(e.endpoint());
    CHECK(good.greet());

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> sent{0};
    std::vector<std::thread> flooders;
    for (int k = 0; k < 3; ++k) {
        flooders.emplace_back([&]() {
            RawClient f(e.endpoint());
            if (!f.connected()) return;
            const uint32_t total = 8u << 20;
            const std::size_t payload = api::wire::kMaxFrameBytes - api::wire::kFrameHeaderBytes;
            std::string frame(api::wire::kMaxFrameBytes, 'x');
            const auto put = [&frame](std::size_t at, uint32_t v) {
                for (int i = 0; i < 4; ++i) frame[at + i] = static_cast<char>(v >> (8 * i));
            };
            put(0, total);
            uint32_t offset = 0;
            std::string error;
            // Never the last frame: the message stays open, its slot held.
            while (!stop.load() && offset + 2 * payload < total) {
                put(4, offset);
                const IoStatus s = f.channel().send(frame, 50, error);
                if (s == IoStatus::Closed || s == IoStatus::Error) return;
                if (s == IoStatus::Ok) {
                    offset += static_cast<uint32_t>(payload);
                    sent.fetch_add(frame.size());
                }
            }
            while (!stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        });
    }
    CHECK(eventually([&]() { return e.server().clients() == 4; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Each command of the good client answered promptly, flood or not.
    for (int i = 0; i < 20; ++i) {
        const auto t0 = Clock::now();
        const uint64_t seq = good.command("track.toggle_arm", 0);
        api::CommandReply r;
        CHECK(good.reply(seq, r) && r.result == DispatchResult::Ok);
        CHECK(Clock::now() - t0 < std::chrono::milliseconds(500));
    }
    CHECK(sent.load() > defaults.max_client_backlog_bytes);
    CHECK(e.server().peak_backlog_bytes() <=
          defaults.max_client_backlog_bytes + api::wire::kMaxFrameBytes);
    CHECK(e.server().peak_large_in_flight() <= defaults.max_large_in_flight);
    CHECK(e.server().peak_large_in_flight() >= 1);

    stop.store(true);
    for (auto& t : flooders) t.join();
    CHECK(eventually([&]() { return e.server().clients() == 1; }));
    const uint64_t seq = good.command("track.toggle_arm", 0);
    api::CommandReply r;
    CHECK(good.reply(seq, r) && r.result == DispatchResult::Ok);
}

}  // namespace

int main() {
    several_clients_each_get_hello_and_the_block();
    a_reply_goes_to_the_asker_alone();
    a_client_gone_mid_take_leaves_the_take_recording();
    malformed_input_is_refused_and_nothing_else_changes();
    flooding_clients_are_held_back_and_others_carry_on();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("control socket: ok\n");
    return 0;
}
