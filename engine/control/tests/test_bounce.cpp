// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bounce Mix, the engine's half (ui.mixer.bounce): the offline file is the
// mix as playback plays it (hand-computed here), real time records the
// master through the tap with nothing lost, loudness normalise is off unless
// asked and never goes past the peak ceiling, a cancel leaves no file, and
// the audio thread's tap neither allocates nor waits.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sonora/bounce.h"
#include "sonora/bounce_job.h"
#include "sonora/closed_take.h"
#include "sonora/model/operations.h"
#include "sonora/playback_controller.h"
#include "sonora/session.h"
#include "sonora/wav_reader.h"
#include "sonora/wav_writer.h"

// A bounce that fails says why.
#define CHECK_BOUNCE(call, err)                                          \
    do {                                                                 \
        if (!(call)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         (err).c_str());                                 \
            return 1;                                                    \
        }                                                                \
    } while (0)

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace sonora {
// The reader's hold hook is private to it, as in test_control.
struct DiskReaderTestAccess {
    static void set_hold_hook(DiskReader& r, DiskReader::HoldHook hook, void* context) {
        r.set_hold_hook(hook, context);
    }
};
}  // namespace sonora

namespace {

using namespace sonora;
std::atomic<bool> g_hold_reader{false};
namespace fs = std::filesystem;

constexpr uint32_t kRate = 48000;
constexpr int64_t kLength = 96000;   // 2 s: long enough for an integrated figure
constexpr float kAmp = 0.1f;
// Centre of the default -3 dB pan law.
const float kCentre = std::pow(10.0f, -3.0f / 20.0f);

SessionConfig config(const std::string& dir) {
    SessionConfig cfg;
    cfg.dir = dir;
    cfg.track_count = 1;
    cfg.ring_seconds = 0.1;
    cfg.sample_rate = kRate;
    return cfg;
}

float sine_at(int64_t i) {
    return kAmp * std::sin(2.0f * 3.14159265f * static_cast<float>(i % 48) / 48.0f);
}

struct Rig {
    std::string dir;
    std::unique_ptr<Session> session;
    std::unique_ptr<model::Document> doc;
};

// One mono track playing a 1 kHz sine (48 samples a period, so it repeats
// exactly) from frame 0 for kLength frames.
// The source and its one region are `file_frames` long. `infinite` puts one
// infinite sample in the file.
bool make_rig(Rig& r, const std::string& dir, int64_t file_frames = kLength,
              bool infinite = false) {
    fs::remove_all(dir);
    r.dir = dir;
    r.session = std::make_unique<Session>(config(dir));
    if (!r.session->create_dirs()) return false;
    r.doc = std::make_unique<model::Document>(engine_document(*r.session));
    WavWriter w;
    if (!w.open(dir + "/Audio Files/s.wav", kRate, 1, WavWriter::Format::Float32, 0)) return false;
    std::vector<float> data(static_cast<std::size_t>(file_frames));
    for (int64_t i = 0; i < file_frames; ++i) data[static_cast<std::size_t>(i)] = sine_at(i);
    if (infinite) data[1000] = std::numeric_limits<float>::infinity();
    if (!w.write(data.data(), data.size()) || !w.close()) return false;

    const model::TrackEdit& t = *r.doc->current().edit->tracks[0];
    std::string err;
    model::ReplicationUpdate u;
    model::AddSource add;
    add.source.id = r.doc->current().next_id;
    add.source.path = "Audio Files/s.wav";
    add.source.channels = 1;
    add.source.length = file_frames;
    add.source.format = model::AudioFormat::Float32;
    if (!r.doc->apply(add, u, err)) return false;
    model::PlaceRegion p;
    p.playlist = t.active_playlist;
    p.region.id = r.doc->current().next_id;
    p.region.name = "r";
    p.region.position = 0;
    p.region.length = file_frames;
    p.region.channels.push_back({add.source.id, 0});
    p.split_right_id = p.region.id + 100;
    return r.doc->apply(p, u, err);
}

bool read_stereo(const std::string& path, std::vector<float>& out, int64_t& frames) {
    WavReader rd;
    std::string e;
    if (!rd.open(path, e) || rd.channels() != 2) return false;
    frames = rd.frames();
    out.assign(static_cast<std::size_t>(frames) * 2, 0.0f);
    return rd.read(0, frames, out.data()) == frames;
}

// Every frame of the file is the source at the pan law's centre gain, on
// both sides, within `tol`.
bool matches_mix(const std::string& path, int64_t frames_wanted, float gain, float tol) {
    std::vector<float> d;
    int64_t frames = 0;
    if (!read_stereo(path, d, frames) || frames != frames_wanted) return false;
    for (int64_t i = 0; i < frames; ++i) {
        const float want = sine_at(i) * kCentre * gain;
        if (std::fabs(d[static_cast<std::size_t>(2 * i)] - want) > tol) return false;
        if (std::fabs(d[static_cast<std::size_t>(2 * i + 1)] - want) > tol) return false;
    }
    return true;
}

int offline_bounce_is_the_mix_as_played() {
    Rig r;
    CHECK(make_rig(r, "bounce_t1"));
    api::BounceOptions o;
    o.format = api::BounceFormat::Float32;
    o.name = "Mixdown";
    BounceResult res;
    std::string err;
    std::atomic<bool> cancel{false};
    double last = 0.0;
    BounceHooks hooks;
    hooks.cancel = &cancel;
    hooks.progress = [&](double f) { last = f; };
    CHECK_BOUNCE(bounce_offline(r.doc->current(), r.dir, o, hooks, res, err), err);
    CHECK(res.relative_path == "Bounced Files/Mixdown_01.wav");
    CHECK(res.frames == kLength);
    CHECK(last == 1.0);
    CHECK(res.gain_db == 0.0);   // normalise is off by default
    CHECK(matches_mix(r.dir + "/" + res.relative_path, kLength, 1.0f, 1e-4f));
    WavReader rd;
    CHECK(rd.open(r.dir + "/" + res.relative_path, err));
    CHECK(rd.time_reference() == 0);

    // A second bounce never overwrites the first, and a range starts where
    // asked: its BWF origin says so.
    o.start = 24000;
    o.end = 48000;
    CHECK_BOUNCE(bounce_offline(r.doc->current(), r.dir, o, hooks, res, err), err);
    CHECK(res.relative_path == "Bounced Files/Mixdown_02.wav");
    CHECK(res.frames == 24000);
    WavReader rd2;
    CHECK(rd2.open(r.dir + "/" + res.relative_path, err));
    CHECK(rd2.time_reference() == 24000);
    std::vector<float> d;
    int64_t frames = 0;
    CHECK(read_stereo(r.dir + "/" + res.relative_path, d, frames));
    for (int64_t i = 0; i < frames; i += 97) {
        CHECK(std::fabs(d[static_cast<std::size_t>(2 * i)] - sine_at(24000 + i) * kCentre) < 1e-4f);
    }
    return 0;
}

bool starts_with(const std::string& path, const char* magic) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    char b[4] = {};
    const bool ok = std::fread(b, 1, 4, f) == 4 && std::memcmp(b, magic, 4) == 0;
    std::fclose(f);
    return ok;
}

int the_file_type_depth_rate_and_dither_are_honoured() {
    Rig r;
    CHECK(make_rig(r, "bounce_t_export"));
    BounceHooks hooks;
    BounceResult res;
    std::string err;
    api::BounceOptions o;
    o.name = "Out";

    // A 16-bit WAV at 44.1 kHz: tpdf by default, the length follows the rate.
    o.file = api::BounceFile::Wav;
    o.format = api::BounceFormat::Int16;
    o.sample_rate = 44100;
    CHECK_BOUNCE(bounce_offline(r.doc->current(), r.dir, o, hooks, res, err), err);
    CHECK(res.relative_path == "Bounced Files/Out_01.wav");
    CHECK(res.frames == 88200);
    WavReader rd;
    CHECK(rd.open(r.dir + "/" + res.relative_path, err));
    CHECK(rd.sample_rate() == 44100 && rd.frames() == 88200 && rd.encoding() == WavReader::Encoding::Pcm16);
    CHECK(rd.description().empty());   // a plain WAV: no bext
    // 1 kHz, 0.1 at the centre gain, whatever the rate: the peak is kept.
    std::vector<float> d(88200 * 2);
    CHECK(rd.read(0, 88200, d.data()) == 88200);
    float peak = 0.0f;
    for (int i = 20000; i < 60000; ++i) peak = std::max(peak, std::fabs(d[2 * static_cast<std::size_t>(i)]));
    CHECK(std::fabs(peak - kAmp * kCentre) < 0.002f);

    // The same name in another type is a name of its own; each type is what
    // its magic says, and no render file is left behind.
    const struct { api::BounceFile f; const char* ext; const char* magic; } types[] = {
        {api::BounceFile::Bwf, ".wav", "RIFF"}, {api::BounceFile::Aiff, ".aiff", "FORM"},
        {api::BounceFile::Flac, ".flac", "fLaC"}};
    for (const auto& t : types) {
        o.file = t.f;
        o.format = api::BounceFormat::Int24;
        o.sample_rate = 0;
        CHECK_BOUNCE(bounce_offline(r.doc->current(), r.dir, o, hooks, res, err), err);
        CHECK(res.relative_path.size() > 4 &&
              res.relative_path.substr(res.relative_path.size() - std::strlen(t.ext)) == t.ext);
        CHECK(starts_with(r.dir + "/" + res.relative_path, t.magic));
        CHECK(res.frames == kLength);
    }
    int files = 0;
    for (const auto& e : fs::directory_iterator(r.dir + "/Bounced Files")) {
        CHECK(e.path().extension() != ".render");
        ++files;
    }
    CHECK(files == 4);

    // A pair the type cannot hold is refused before anything is written.
    o.file = api::BounceFile::Flac;
    o.format = api::BounceFormat::Float32;
    CHECK(!bounce_check(r.doc->current(), o, err) && err.find("FLAC") != std::string::npos);
    CHECK(!bounce_offline(r.doc->current(), r.dir, o, hooks, res, err));
    int after = 0;
    for (const auto& e : fs::directory_iterator(r.dir + "/Bounced Files")) {
        (void)e;
        ++after;
    }
    CHECK(after == 4);
    return 0;
}

int normalise_reaches_the_target_and_stops_at_the_ceiling() {
    Rig r;
    CHECK(make_rig(r, "bounce_t2"));
    api::BounceOptions o;
    o.format = api::BounceFormat::Float32;
    o.normalise = true;
    o.target_lufs = -20.0;
    BounceResult res;
    std::string err;
    BounceHooks hooks;
    CHECK_BOUNCE(bounce_offline(r.doc->current(), r.dir, o, hooks, res, err), err);
    CHECK(std::fabs(res.integrated_lufs - (-20.0)) < 0.3);
    CHECK(res.gain_db > 0.0);
    CHECK(matches_mix(r.dir + "/" + res.relative_path, kLength,
                      std::pow(10.0f, static_cast<float>(res.gain_db) / 20.0f), 2e-4f));

    // A target the peak ceiling forbids is reached only as far as the ceiling.
    o.target_lufs = 0.0;
    CHECK_BOUNCE(bounce_offline(r.doc->current(), r.dir, o, hooks, res, err), err);
    CHECK(res.peak_dbfs <= -0.99 && res.peak_dbfs > -1.1);
    CHECK(res.integrated_lufs < -0.2);   // 0 LUFS would have needed more gain
    return 0;
}

int cancel_and_an_empty_range_leave_no_file() {
    Rig r;
    CHECK(make_rig(r, "bounce_t3"));
    api::BounceOptions o;
    BounceResult res;
    std::string err;
    std::atomic<bool> cancel{false};
    BounceHooks hooks;
    hooks.cancel = &cancel;
    hooks.progress = [&](double f) { if (f > 0.3) cancel.store(true); };
    CHECK(!bounce_offline(r.doc->current(), r.dir, o, hooks, res, err));
    CHECK(err.find("cancelled") != std::string::npos);
    std::size_t files = 0;
    for (const auto& e : fs::directory_iterator(r.dir + "/Bounced Files")) {
        (void)e;
        ++files;
    }
    CHECK(files == 0);
    // An empty range is refused with its reason.
    o.start = 10;
    o.end = 10;
    hooks.progress = nullptr;
    CHECK(!bounce_offline(r.doc->current(), r.dir, o, hooks, res, err));
    CHECK(err.find("nothing to bounce") != std::string::npos);
    return 0;
}

// The audio thread: blocks of 256 frames, a little faster than real time.
// A roll runs at the audio clock, which a sanitizer build slows several times
// over: the wait is bounded by wall time, generously, not by a count of polls.
std::chrono::steady_clock::time_point deadline() {
    return std::chrono::steady_clock::now() + std::chrono::seconds(120);
}

struct Driver {
    explicit Driver(Session& s) : s_(s) {
        thread_ = std::thread([this] {
            std::vector<float> l(256), rr(256);
            float* outs[2] = {l.data(), rr.data()};
            uint64_t n = 0;
            while (run_.load()) {
                backend::AudioBlock b{};
                b.outputs = outs;
                b.output_count = 2;
                b.nframes = 256;
                b.frame_time = n++ * 256;
                s_.process(b);
                std::this_thread::sleep_for(std::chrono::microseconds(1500));
            }
        });
    }
    ~Driver() {
        run_.store(false);
        thread_.join();
    }
    Session& s_;
    std::atomic<bool> run_{true};
    std::thread thread_;
};

int real_time_bounce_records_the_master() {
    Rig r;
    CHECK(make_rig(r, "bounce_t4"));
    r.session->reader().start();
    PlaybackController ctl(*r.session);
    std::string err;
    CHECK(ctl.show(r.doc->current(), err));
    CHECK(ctl.settle(5.0));
    ctl.apply_mix(r.doc->current());
    RealTimeBounce rt(*r.session, ctl);
    api::BounceOptions o;
    o.mode = api::BounceMode::RealTime;
    o.format = api::BounceFormat::Float32;
    o.start = 4800;
    o.end = 4800 + 48000;
    o.name = "Live";
    BounceResult res;
    bool cancelled = false;
    {
        Driver audio(*r.session);
        CHECK(rt.start(r.doc->current(), r.dir, o, err));
        CHECK(!rt.start(r.doc->current(), r.dir, o, err));   // one at a time
        bool ended = false;
        for (const auto until = deadline(); !ended && std::chrono::steady_clock::now() < until;) {
            ctl.pump();
            ended = rt.poll(res, err, cancelled);
            if (!ended) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK_BOUNCE(ended, err);
    }
    CHECK_BOUNCE(err.empty(), err);
    CHECK(!cancelled);
    CHECK(res.frames == 48000);
    // The file is the master from its first frame: the source from 4800.
    std::vector<float> d;
    int64_t frames = 0;
    CHECK(read_stereo(r.dir + "/" + res.relative_path, d, frames));
    CHECK(frames == 48000);
    for (int64_t i = 0; i < frames; i += 53) {
        CHECK(std::fabs(d[static_cast<std::size_t>(2 * i)] - sine_at(4800 + i) * kCentre) < 2e-3f);
    }
    WavReader rd;
    CHECK(rd.open(r.dir + "/" + res.relative_path, err));
    CHECK(rd.time_reference() == 4800);
    CHECK(r.session->transport().state() == TransportState::Stopped);

    // A cancel stops it and removes the file.
    {
        Driver audio(*r.session);
        o.start = 0;
        o.end = kLength;
        CHECK(rt.start(r.doc->current(), r.dir, o, err));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        rt.cancel();
        bool ended = false;
        for (const auto until = deadline(); !ended && std::chrono::steady_clock::now() < until;) {
            ctl.pump();
            ended = rt.poll(res, err, cancelled);
            if (!ended) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(ended && cancelled);
    }
    std::size_t files = 0;
    for (const auto& e : fs::directory_iterator(r.dir + "/Bounced Files")) {
        (void)e;
        ++files;
    }
    CHECK(files == 1);
    return 0;
}

// A full ring is counted, never waited on; a chunk that does not fit whole
// is dropped so the ring stays frame-aligned.
int tap_counts_an_overrun_and_stays_aligned() {
    MasterTap tap(300);
    std::vector<float> l(256, 1.0f), r(256, -1.0f);
    tap.push(l.data(), r.data(), 256);
    tap.push(l.data(), r.data(), 256);   // does not fit
    CHECK(tap.lost_frames() == 256);
    CHECK(tap.frames_available() == 256);
    std::vector<float> out(512);
    CHECK(tap.read(out.data(), 256) == 256);
    CHECK(out[0] == 1.0f && out[1] == -1.0f);
    return 0;
}

// A peak of infinity would make the gain zero and the file silent: the
// bounce is refused with its reason, and still succeeds without normalise.
int normalise_refuses_a_mix_with_an_infinite_sample() {
    Rig r;
    CHECK(make_rig(r, "bounce_t5", kLength, true));
    api::BounceOptions o;
    o.format = api::BounceFormat::Float32;
    o.normalise = true;
    BounceResult res;
    std::string err;
    BounceHooks hooks;
    CHECK(!bounce_offline(r.doc->current(), r.dir, o, hooks, res, err));
    CHECK(err.find("infinite") != std::string::npos);
    std::size_t files = 0;
    for (const auto& e : fs::directory_iterator(r.dir + "/Bounced Files")) {
        (void)e;
        ++files;
    }
    CHECK(files == 0);
    o.normalise = false;
    CHECK_BOUNCE(bounce_offline(r.doc->current(), r.dir, o, hooks, res, err), err);
    CHECK(res.gain_db == 0.0);
    return 0;
}

// The disk reader is held once the roll starts: blocks past the resident
// chunks are silence in the live master. The bounce says so rather than keep
// a file with a hole.
int real_time_bounce_refuses_a_roll_the_disk_missed() {
    Rig r;
    CHECK(make_rig(r, "bounce_t6", 480000));   // 10 s: far past what stays resident
    g_hold_reader = false;
    DiskReaderTestAccess::set_hold_hook(
        r.session->reader(), [](void*) { return g_hold_reader.load(); }, nullptr);
    r.session->reader().start();
    PlaybackController ctl(*r.session);
    std::string err;
    CHECK(ctl.show(r.doc->current(), err));
    CHECK(ctl.settle(5.0));
    ctl.apply_mix(r.doc->current());
    RealTimeBounce rt(*r.session, ctl);
    api::BounceOptions o;
    o.mode = api::BounceMode::RealTime;
    o.format = api::BounceFormat::Float32;
    o.end = 480000;
    BounceResult res;
    bool cancelled = false;
    bool ended = false;
    {
        Driver audio(*r.session);
        CHECK(rt.start(r.doc->current(), r.dir, o, err));
        // play() asks for a new generation and the roll waits until the
        // reader has made it resident; holding before that never lets the
        // transport roll (a slow machine hit this).
        const auto rolling = deadline();
        while (r.session->transport().state() == TransportState::Stopped &&
               std::chrono::steady_clock::now() < rolling) {
            ctl.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(r.session->transport().state() != TransportState::Stopped);
        // Nothing new becomes resident from here: the roll outruns the
        // chunks the start left, and each block past them is a miss.
        g_hold_reader = true;
        for (const auto until = deadline(); !ended && std::chrono::steady_clock::now() < until;) {
            ctl.pump();
            ended = rt.poll(res, err, cancelled);
            if (!ended) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    g_hold_reader = false;
    CHECK(ended && !cancelled);
    CHECK_BOUNCE(err.find("did not keep up") != std::string::npos, err);
    std::size_t files = 0;
    for (const auto& e : fs::directory_iterator(r.dir + "/Bounced Files")) {
        (void)e;
        ++files;
    }
    CHECK(files == 0);
    return 0;
}

// The control server's way in: a job, offline, reports progress and a
// result; a second job cancelled part way reports it and leaves no file.
int job_runs_offline_and_cancels() {
    Rig r;
    CHECK(make_rig(r, "bounce_t7"));
    PlaybackController ctl(*r.session);
    api::BounceOptions o;
    o.format = api::BounceFormat::Float32;
    BounceJob job;
    std::string err;
    CHECK_BOUNCE(job.start(*r.session, ctl, r.doc->current(), r.dir, o, err), err);
    CHECK(job.active());
    for (const auto until = deadline(); !job.finished() && std::chrono::steady_clock::now() < until;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(job.finished());
    BounceJob::Result done = job.collect();
    CHECK(done.ok && !done.cancelled && done.bounced.frames == kLength);
    CHECK(!job.active());
    CHECK(fs::exists(r.dir + "/" + done.bounced.relative_path));

    CHECK_BOUNCE(job.start(*r.session, ctl, r.doc->current(), r.dir, o, err), err);
    CHECK(job.abandon());
    CHECK(!job.active());
    std::size_t files = 0;
    for (const auto& e : fs::directory_iterator(r.dir + "/Bounced Files")) {
        (void)e;
        ++files;
    }
    CHECK(files == 1);   // the first bounce's
    return 0;
}

}  // namespace

int main() {
    int rc = 0;
    rc |= tap_counts_an_overrun_and_stays_aligned();
    rc |= offline_bounce_is_the_mix_as_played();
    rc |= the_file_type_depth_rate_and_dither_are_honoured();
    rc |= normalise_reaches_the_target_and_stops_at_the_ceiling();
    rc |= normalise_refuses_a_mix_with_an_infinite_sample();
    rc |= cancel_and_an_empty_range_leave_no_file();
    rc |= real_time_bounce_records_the_master();
    rc |= real_time_bounce_refuses_a_roll_the_disk_missed();
    rc |= job_runs_offline_and_cancels();
    if (rc == 0) std::printf("bounce: ok\n");
    return rc;
}
