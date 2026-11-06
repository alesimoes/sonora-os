// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/bounce.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include "sonora/audio_export.h"
#include "sonora/dsp/loudness.h"
#include "sonora/master_tap.h"
#include "sonora/playback_controller.h"
#include "sonora/session.h"
#include "sonora/wav_reader.h"
#include "sonora/wav_writer.h"

namespace sonora {

namespace {

constexpr uint32_t kBlock = 512;
constexpr uint32_t kRewindBlocks = 2;      // lead-in after a seek, discarded
constexpr double kPeakCeilingDb = -1.0;    // a normalised file never peaks above
constexpr std::size_t kCopyFrames = 4096;

std::string file_safe(std::string name) {
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':') c = '_';
    }
    return name;
}

const model::Playlist* active_playlist(const model::TrackEdit& t) {
    for (const auto& p : t.playlists) {
        if (p->id == t.active_playlist) return p.get();
    }
    return nullptr;
}

bool any_active_insert(const model::Snapshot& s) {
    for (const auto& t : s.edit->tracks) {
        for (const model::Insert& i : t->inserts) {
            if (i.active) return true;
        }
    }
    return false;
}

// The file being written: stereo, measured as it goes.
class Sink {
public:
    // The mix is rendered to a float32 file next to the one that is kept
    // (`<name>.render`), then converted to the chosen file in finish(): the
    // loudness is known only at the end, and so is the gain it needs.
    bool open(const std::string& session_dir, const std::string& stem, uint32_t rate,
              const api::BounceOptions& opts, uint64_t start, const std::string& description,
              std::string& error) {
        spec_ = export_spec_of(opts);
        error = export_spec_problem(spec_);
        if (!error.empty()) return false;
        dir_ = session_dir + "/" + kBouncedDir;
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        if (!std::filesystem::is_directory(dir_, ec)) {
            error = "cannot create " + dir_ + ": " + std::strerror(errno);
            return false;
        }
        start_ = start;
        rate_ = rate;
        description_ = description;
        writer_.set_description(description);
        // The kept name is reserved exclusively, so a bounce never takes
        // another's, whatever its extension.
        for (int n = 1; n <= 9999; ++n) {
            char digits[16];
            std::snprintf(digits, sizeof digits, "%02d", n);
            const std::string name =
                file_safe(stem) + "_" + digits + export_extension(spec_.container);
            std::FILE* f = std::fopen((dir_ + "/" + name).c_str(), "wbx");
            if (f == nullptr) {
                if (errno == EEXIST) continue;
                error = "cannot create " + name + ": " + std::strerror(errno);
                return false;
            }
            std::fclose(f);
            file_ = name;
            std::remove(render_path().c_str());   // a stale one of a crashed bounce
            if (!writer_.open(render_path(), rate, 2, WavWriter::Format::Float32, start)) {
                error = "cannot create " + file_ + ".render: " + std::strerror(errno);
                std::remove(path().c_str());
                file_.clear();
                return false;
            }
            meter_.prepare(rate, 2);
            buf_.resize(kCopyFrames * 2);
            return true;
        }
        error = "no free bounce file name for " + stem;
        return false;
    }

    bool add(const float* l, const float* r, std::size_t n, std::string& error) {
        for (std::size_t done = 0; done < n;) {
            const std::size_t k = std::min(kCopyFrames, n - done);
            for (std::size_t i = 0; i < k; ++i) {
                buf_[2 * i] = l[done + i];
                buf_[2 * i + 1] = r[done + i];
                // max() would pass over a NaN and keep the old peak.
                if (!std::isfinite(l[done + i]) || !std::isfinite(r[done + i])) {
                    nonfinite_ = true;
                    continue;
                }
                peak_ = std::max({peak_, std::fabs(l[done + i]), std::fabs(r[done + i])});
            }
            const float* planes[2] = {l + done, r + done};
            meter_.process(planes, static_cast<uint32_t>(k));
            if (!writer_.write(buf_.data(), k)) {
                error = "cannot write " + file_ + ": " + writer_.failure();
                return false;
            }
            done += k;
        }
        frames_ += n;
        return true;
    }

    void abandon() {
        if (writer_.is_open()) writer_.close();
        if (!file_.empty()) {
            std::remove(path().c_str());
            std::remove(render_path().c_str());
        }
    }

    // Synced and closed; normalised when asked. Fills `out`.
    bool finish(const api::BounceOptions& opts, BounceResult& out, std::string& error) {
        if (!writer_.checkpoint() || !writer_.close()) {
            error = "cannot write " + file_ + ": " +
                    (writer_.failure().empty() ? "the disk refused it" : writer_.failure());
            abandon();
            return false;
        }
        if (opts.normalise && nonfinite_) {
            // A gain from a peak of infinity is zero: the file would be
            // silent. Better no file than that one.
            error = "the mix holds a sample that is infinite or not a number, so it cannot be "
                    "normalised; nothing was kept";
            abandon();
            return false;
        }
        const double lufs = meter_.integrated_lufs();
        double gain_db = 0.0;
        if (opts.normalise && lufs > -70.0 && peak_ > 0.0f) {
            gain_db = opts.target_lufs - lufs;
            // One gain for the whole file, never into the ceiling.
            const double room = kPeakCeilingDb - 20.0 * std::log10(static_cast<double>(peak_));
            gain_db = std::min(gain_db, room);
        }
        // Every file is converted from the float render, so a bounce with no
        // gain, rate or depth change takes the same road as the rest.
        ExportMeta meta;
        meta.description = description_;
        meta.time_reference = start_;
        meta.loudness_lufs = lufs > -70.0 ? lufs + gain_db : lufs;
        if (!export_audio(render_path(), path(), spec_, meta, gain_db, error)) {
            abandon();
            return false;
        }
        std::remove(render_path().c_str());
        out.relative_path = std::string(kBouncedDir) + "/" + file_;
        out.frames = spec_.sample_rate == 0 || spec_.sample_rate == rate_
                         ? frames_
                         : static_cast<uint64_t>(std::llround(static_cast<double>(frames_) *
                                                              spec_.sample_rate / rate_));
        out.integrated_lufs = lufs > -70.0 ? lufs + gain_db : lufs;
        out.peak_dbfs = peak_ > 0.0f ? 20.0 * std::log10(static_cast<double>(peak_)) + gain_db
                                     : -200.0;
        out.gain_db = gain_db;
        return true;
    }

    uint64_t frames() const { return frames_; }
    std::string path() const { return dir_ + "/" + file_; }

private:
    std::string render_path() const { return path() + ".render"; }

    static ExportSpec export_spec_of(const api::BounceOptions& o) {
        ExportSpec s;
        switch (o.file) {
            case api::BounceFile::Wav: s.container = ExportContainer::Wav; break;
            case api::BounceFile::Aiff: s.container = ExportContainer::Aiff; break;
            case api::BounceFile::Flac: s.container = ExportContainer::Flac; break;
            default: s.container = ExportContainer::Bwf; break;
        }
        s.bits = o.format == api::BounceFormat::Float32 ? 32
                 : o.format == api::BounceFormat::Int16 ? 16 : 24;
        s.sample_rate = o.sample_rate;
        // Auto dithers where the cut is audible: 16-bit.
        switch (o.dither) {
            case api::BounceDither::Tpdf: s.dither = ExportDither::Tpdf; break;
            case api::BounceDither::Shaped: s.dither = ExportDither::Shaped; break;
            case api::BounceDither::None: s.dither = ExportDither::None; break;
            default: s.dither = s.bits == 16 ? ExportDither::Tpdf : ExportDither::None; break;
        }
        return s;
    }

    WavWriter writer_;
    ExportSpec spec_;
    dsp::LoudnessMeter meter_;
    std::vector<float> buf_;
    std::string dir_, file_, description_;
    uint32_t rate_ = 48000;
    uint64_t start_ = 0;
    uint64_t frames_ = 0;
    float peak_ = 0.0f;
    bool nonfinite_ = false;
};

std::string stem_of(const model::Snapshot& s, const api::BounceOptions& o) {
    if (!o.name.empty()) return o.name;
    return (s.settings.name.empty() ? std::string("Untitled") : s.settings.name) + " Bounce";
}

bool range_of(const model::Snapshot& s, const api::BounceOptions& o, uint64_t& start,
              uint64_t& end, std::string& error) {
    start = o.start;
    end = o.end != 0 ? o.end : static_cast<uint64_t>(bounce_extent(s));
    if (end <= start) {
        error = "there is nothing to bounce: the range is empty";
        return false;
    }
    return true;
}

}  // namespace

model::Frames bounce_extent(const model::Snapshot& snapshot) {
    model::Frames end = 0;
    for (const auto& t : snapshot.edit->tracks) {
        if (t->kind != model::TrackKind::Audio) continue;
        const model::Playlist* pl = active_playlist(*t);
        if (pl == nullptr) continue;
        for (const model::Region& r : pl->regions) end = std::max(end, r.end());
    }
    return end;
}

bool bounce_check(const model::Snapshot& snapshot, const api::BounceOptions& opts,
                  std::string& error) {
    uint64_t start = 0, end = 0;
    if (!range_of(snapshot, opts, start, end, error)) return false;
    if (opts.format == api::BounceFormat::Float32 &&
        (opts.file == api::BounceFile::Flac || opts.file == api::BounceFile::Aiff)) {
        error = std::string(opts.file == api::BounceFile::Flac ? "FLAC" : "AIFF") +
                " holds 16 or 24 bits, not 32-bit float";
        return false;
    }
    if (opts.mode == api::BounceMode::Offline && any_active_insert(snapshot)) {
        error = "offline bounce does not run plug-in inserts; bounce in real time instead";
        return false;
    }
    return true;
}

bool bounce_offline(const model::Snapshot& snapshot, const std::string& session_dir,
                    const api::BounceOptions& opts, const BounceHooks& hooks, BounceResult& out,
                    std::string& error) {
    out = BounceResult{};
    uint64_t start = 0, end = 0;
    if (!range_of(snapshot, opts, start, end, error)) return false;
    // `opts.mode` is ignored here, so the insert refusal is not bounce_check's.
    if (any_active_insert(snapshot)) {
        error = "offline bounce does not run plug-in inserts; bounce in real time instead";
        return false;
    }

    SessionConfig cfg;
    cfg.name = snapshot.settings.name;
    cfg.dir = session_dir;
    cfg.sample_rate = snapshot.settings.sample_rate;
    cfg.track_count = std::max<int>(1, static_cast<int>(snapshot.edit->tracks.size()));
    uint16_t channels = 1;
    for (const auto& t : snapshot.edit->tracks) channels = std::max(channels, t->channels);
    cfg.channels_per_track = channels;
    cfg.ring_seconds = 0.1;   // it never records
    Session session(cfg);
    // The reader thread serves the blocks this thread plays.
    session.reader().start();
    PlaybackController ctl(session);

    std::string why;
    if (!ctl.show(snapshot, why)) {
        error = "cannot play the session to bounce it: " + why;
        return false;
    }
    if (!ctl.settle(30.0)) {
        error = "the session's audio did not become ready to bounce";
        return false;
    }
    ctl.apply_mix(snapshot);

    Sink sink;
    if (!sink.open(session_dir, stem_of(snapshot, opts), cfg.sample_rate, opts, start,
                   "Bounce of " + snapshot.settings.name, error)) {
        return false;
    }
    const auto fail = [&](const std::string& because) {
        sink.abandon();
        ctl.stop();
        session.stop_playback();
        ctl.after_deactivate();
        error = because;
        return false;
    };

    // Rolls from a little before `target`: the first blocks after a locate
    // ramp, and are heard by nobody.
    const auto seek = [&](uint64_t target) {
        ctl.stop();
        const uint64_t from = target > uint64_t{kRewindBlocks} * kBlock
                                  ? target - uint64_t{kRewindBlocks} * kBlock
                                  : 0;
        std::string e;
        if (!ctl.locate(from, e)) return e;
        // The locate lands, and the roll starts, only at the blocks the loop
        // below plays; settle() would wait for them here.
        ctl.play();
        return std::string();
    };

    std::vector<float> left(kBlock), right(kBlock);
    float* outs[2] = {left.data(), right.data()};
    backend::AudioBlock block{};
    block.outputs = outs;
    block.output_count = 2;
    block.nframes = kBlock;

    // Fader and pan glide to the mix over a few blocks; let them arrive while
    // the transport is stopped, so the first rolled frame is the settled mix.
    for (int i = 0; i < 8; ++i) session.process(block);

    uint64_t pos = start;
    int retries = 0;
    int idle = 0;
    why = seek(pos);
    if (!why.empty()) return fail(why);
    while (pos < end) {
        if (hooks.cancel != nullptr && hooks.cancel->load(std::memory_order_relaxed)) {
            return fail("bounce was cancelled");
        }
        ctl.pump();
        const bool rolling = session.transport().state() != TransportState::Stopped;
        const uint64_t misses = session.playback_misses();
        block.frame_time += kBlock;
        session.process(block);
        if (!rolling) {
            if (++idle > 5000) return fail("the session never started to play");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        idle = 0;
        const uint64_t after = session.transport().playhead();
        const uint64_t at = after >= kBlock ? after - kBlock : 0;
        if (session.playback_misses() != misses || at > pos) {
            // The reader was behind: those frames are not the mix. Wait a
            // moment and play them again.
            if (++retries > 200) return fail("the disk is too slow to bounce this session");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            why = seek(pos);
            if (!why.empty()) return fail(why);
            continue;
        }
        if (at + kBlock <= pos) continue;   // lead-in
        retries = 0;
        const std::size_t off = static_cast<std::size_t>(pos - at);
        const std::size_t count =
            static_cast<std::size_t>(std::min<uint64_t>(kBlock - off, end - pos));
        if (!sink.add(left.data() + off, right.data() + off, count, why)) return fail(why);
        pos += count;
        if (hooks.progress) {
            hooks.progress(static_cast<double>(pos - start) / static_cast<double>(end - start));
        }
    }
    ctl.stop();
    session.stop_playback();
    ctl.after_deactivate();
    return sink.finish(opts, out, error);
}

// --- real time ---------------------------------------------------------------

struct RealTimeBounce::Run {
    Sink sink;
    api::BounceOptions opts;
    uint64_t start = 0;
    uint64_t total = 0;
    BounceResult result;
    std::string error;
    bool cancelled = false;
    // Playback misses before the roll: a later one is a block the disk did
    // not serve, which the live master then holds as silence.
    uint64_t misses_before = 0;
};

RealTimeBounce::RealTimeBounce(Session& session, PlaybackController& playback)
    : session_(session), playback_(playback) {}

RealTimeBounce::~RealTimeBounce() {
    if (worker_.joinable()) {
        cancel_.store(true);
        worker_.join();
        release_tap();
        if (run_) run_->sink.abandon();
    }
}

bool RealTimeBounce::start(const model::Snapshot& snapshot, const std::string& session_dir,
                           const api::BounceOptions& opts, std::string& error) {
    if (running() || run_ != nullptr) {
        error = "a bounce is already running";
        return false;
    }
    if (session_.recording_take() != 0 || session_.takes_closing()) {
        error = "stop recording before bouncing";
        return false;
    }
    if (session_.transport().state() != TransportState::Stopped) {
        error = "stop the transport before bouncing";
        return false;
    }
    uint64_t start = 0, end = 0;
    if (!range_of(snapshot, opts, start, end, error)) return false;

    auto run = std::make_unique<Run>();
    run->opts = opts;
    run->start = start;
    run->total = end - start;
    const uint32_t rate = session_.config().sample_rate;
    if (!run->sink.open(session_dir, stem_of(snapshot, opts), rate, opts, start,
                        "Bounce of " + snapshot.settings.name, error)) {
        return false;
    }
    if (!playback_.locate(start, error) || !playback_.settle(30.0)) {
        if (error.empty()) error = "the disk did not serve the start of the range in time";
        run->sink.abandon();
        return false;
    }
    run->misses_before = session_.playback_misses();
    run_ = std::move(run);
    tap_ = std::make_unique<MasterTap>(static_cast<std::size_t>(rate) * 2);
    cancel_.store(false);
    finished_.store(false);
    progress_.store(0.0);
    session_.set_master_tap(tap_.get());
    playback_.play();
    // After play(), which clears it.
    session_.set_roll_end(end);
    worker_ = std::thread([this] { work(); });
    return true;
}

void RealTimeBounce::work() {
    Run& r = *run_;
    constexpr std::size_t kChunk = 4096;
    std::vector<float> buf(kChunk * 2), l(kChunk), rr(kChunk);
    uint64_t done = 0;
    bool seen = false;
    auto last = std::chrono::steady_clock::now();
    const auto fail = [&](const std::string& why) {
        r.error = why;
        r.sink.abandon();
    };
    while (done < r.total) {
        if (cancel_.load(std::memory_order_relaxed)) {
            r.cancelled = true;
            fail("bounce was cancelled");
            finished_.store(true, std::memory_order_release);
            return;
        }
        const std::size_t got = tap_->read(buf.data(), std::min<uint64_t>(kChunk, r.total - done));
        if (got == 0) {
            const auto now = std::chrono::steady_clock::now();
            const auto idle = std::chrono::duration<double>(now - last).count();
            const bool stopped = session_.transport().state() == TransportState::Stopped;
            if ((seen && stopped && idle > 0.3) || (!seen && idle > 10.0)) {
                fail(seen ? "the transport stopped before the end of the range"
                          : "the transport never started to roll");
                finished_.store(true, std::memory_order_release);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        seen = true;
        last = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < got; ++i) {
            l[i] = buf[2 * i];
            rr[i] = buf[2 * i + 1];
        }
        std::string why;
        if (!r.sink.add(l.data(), rr.data(), got, why)) {
            fail(why);
            finished_.store(true, std::memory_order_release);
            return;
        }
        done += got;
        progress_.store(static_cast<double>(done) / static_cast<double>(r.total),
                        std::memory_order_relaxed);
    }
    const uint64_t missed = session_.playback_misses() - r.misses_before;
    if (tap_->lost_frames() != 0) {
        fail("the bounce lost " + std::to_string(tap_->lost_frames()) +
             " frames to a full ring; nothing was kept");
    } else if (missed != 0) {
        fail("the disk did not keep up for " + std::to_string(missed) +
             " block(s) of the roll, so the file has gaps; nothing was kept. Bounce offline "
             "instead");
    } else {
        std::string why;
        if (!r.sink.finish(r.opts, r.result, why)) r.error = why;
    }
    finished_.store(true, std::memory_order_release);
}

void RealTimeBounce::release_tap() {
    playback_.stop();
    session_.stop_playback();
    session_.set_roll_end(Session::kNoRollEnd);
    session_.set_master_tap(nullptr);
    // A callback already inside push() has finished once the count moves.
    const uint64_t b = session_.plans().blocks();
    for (int i = 0; i < 100 && session_.plans().blocks() == b; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    tap_.reset();
}

void RealTimeBounce::cancel() { cancel_.store(true); }

bool RealTimeBounce::poll(BounceResult& out, std::string& error, bool& cancelled) {
    if (!worker_.joinable() || !finished_.load(std::memory_order_acquire)) return false;
    worker_.join();
    release_tap();
    out = run_->result;
    error = run_->error;
    cancelled = run_->cancelled;
    run_.reset();
    return true;
}

}  // namespace sonora
