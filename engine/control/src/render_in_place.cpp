// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/render_in_place.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

#include "sonora/api/render.h"
#include "sonora/model/operations.h"
#include "sonora/playback_controller.h"
#include "sonora/render_solo.h"
#include "sonora/session.h"
#include "sonora/session_files/session_folder.h"
#include "sonora/track_tap.h"
#include "sonora/wav_reader.h"
#include "sonora/wav_writer.h"

namespace sonora {

namespace {

// Frames the roll runs past the last one the file needs, for a chain whose
// latency is not known until it has played: 1.4 s at 48 kHz.
constexpr uint64_t kRollSlack = 65536;

const model::TrackEdit* track_of(const model::Snapshot& s, model::Id id, std::size_t* index) {
    for (std::size_t i = 0; i < s.edit->tracks.size(); ++i) {
        if (s.edit->tracks[i]->id == id) {
            if (index != nullptr) *index = i;
            return s.edit->tracks[i].get();
        }
    }
    return nullptr;
}

std::string file_safe(std::string name) {
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':') c = '_';
    }
    return name;
}

const char* tag_of(model::RenderKind k) {
    switch (k) {
        case model::RenderKind::Freeze: return "frz";
        case model::RenderKind::Commit: return "cmt";
        case model::RenderKind::Process: break;
    }
    return "prc";
}

std::string default_name(model::RenderKind k, const std::string& track) {
    switch (k) {
        case model::RenderKind::Freeze: return track + " Frozen";
        case model::RenderKind::Commit: return track + " Committed";
        case model::RenderKind::Process: break;
    }
    return track + " Processed";
}

std::string label_of(model::RenderKind k, const std::string& track) {
    switch (k) {
        case model::RenderKind::Freeze: return track + " frozen";
        case model::RenderKind::Commit: return track + " committed";
        case model::RenderKind::Process: break;
    }
    return track + ": selection processed";
}

const char* undo_label(model::RenderKind k) {
    switch (k) {
        case model::RenderKind::Freeze: return "Freeze Track";
        case model::RenderKind::Commit: return "Commit Track";
        case model::RenderKind::Process: break;
    }
    return "Process Selection";
}

// What the render must find unchanged: the playlist the track plays and the
// inserts it runs.
bool same_basis(const model::TrackEdit& a, const model::TrackEdit& b) {
    if (&a == &b) return true;
    if (a.active_playlist != b.active_playlist || model::active_inserts(a) != model::active_inserts(b)) {
        return false;
    }
    for (const auto& p : a.playlists) {
        if (p->id != a.active_playlist) continue;
        for (const auto& q : b.playlists) {
            if (q->id == p->id) return q == p || *q == *p;
        }
    }
    return false;
}

}  // namespace

std::vector<std::pair<model::Id, bool>> insert_bypass(const model::Snapshot& snapshot,
                                                      const model::TrackEdit& track) {
    std::vector<std::pair<model::Id, bool>> out;
    for (const model::Id id : model::active_inserts(track)) {
        const model::InsertMix* m = snapshot.mix.find_insert(id);
        out.emplace_back(id, m != nullptr && m->bypass);
    }
    return out;
}

bool plan_render(const model::Snapshot& snapshot, model::RenderKind kind, const RenderAsk& ask,
                 RenderRequest& out, std::string& error) {
    model::RenderRange range;
    if (!model::render_range_of(snapshot, ask.track, kind, static_cast<model::Frames>(ask.start),
                                static_cast<model::Frames>(ask.end), range, error)) {
        return false;
    }
    if (ask.only_insert != 0) {
        if (kind != model::RenderKind::Process) {
            error = "only Process Selection runs through one plug-in; freeze and commit print "
                    "the whole chain";
            return false;
        }
        if (!model::render_insert_valid(snapshot, ask.track, ask.only_insert, error)) return false;
    }
    const model::TrackEdit* t = track_of(snapshot, ask.track, nullptr);
    RenderRequest r;
    r.kind = kind;
    r.track = ask.track;
    r.only_insert = ask.only_insert;
    r.start = range.start;
    r.end = range.end;
    const int64_t tail_ms = ask.tail_ms >= 0
                                ? ask.tail_ms
                                : (kind == model::RenderKind::Process ? 0 : kRenderFreezeTailMs);
    r.tail = static_cast<model::Frames>(tail_ms) * ask.sample_rate / 1000;
    r.name = ask.name.empty() ? default_name(kind, t->name) : ask.name;
    out = std::move(r);
    return true;
}

RenderAsk RenderAsk::from_options(const api::RenderOptions& o, uint32_t sample_rate) {
    RenderAsk a;
    a.track = o.track;
    a.start = o.start;
    a.end = o.end;
    a.tail_ms = o.tail_ms;
    a.only_insert = o.only_insert;
    a.name = o.name;
    a.sample_rate = sample_rate;
    return a;
}

// --- the roll ---------------------------------------------------------------

struct RenderInPlace::Run {
    WavWriter writer;
    std::string path;
    std::string file;
    uint16_t channels = 1;
    uint64_t length = 0;          // frames the file keeps
    uint64_t lead = 0;            // frames rolled before the range starts
    RenderResult result;
    std::string error;
    bool cancelled = false;
    uint64_t playback_misses_before = 0;
};

RenderInPlace::RenderInPlace(Session& session, PlaybackController& playback)
    : session_(session), playback_(playback) {}

RenderInPlace::~RenderInPlace() {
    if (worker_.joinable()) {
        cancel_.store(true);
        worker_.join();
        release_tap();
        if (run_) {
            run_->writer.close();
            std::remove(run_->path.c_str());
        }
    }
    lift_solo();
}

bool RenderInPlace::start(const model::Snapshot& snapshot, const std::string& session_dir,
                          const RenderRequest& request, ChainMisses chain_misses,
                          std::string& error) {
    if (running() || run_ != nullptr) {
        error = "a render is already running";
        return false;
    }
    if (session_.recording_take() != 0 || session_.takes_closing()) {
        error = "stop recording before rendering a track";
        return false;
    }
    if (session_.transport().state() != TransportState::Stopped) {
        error = "stop the transport before rendering a track";
        return false;
    }
    std::size_t index = 0;
    const model::TrackEdit* track = track_of(snapshot, request.track, &index);
    if (track == nullptr || track->kind != model::TrackKind::Audio) {
        error = "no such audio track; nothing was rendered";
        return false;
    }
    if (index >= session_.tracks().size()) {
        error = track->name + " has no strip in the engine; nothing was rendered";
        return false;
    }
    if (request.end <= request.start) {
        error = "there is nothing to render: the range is empty";
        return false;
    }
    if (request.only_insert != 0) {
        // Checked again here: what a caller hands start() need not have come
        // through plan_render, and a print through more than was asked is
        // the one outcome this must never have.
        if (request.kind != model::RenderKind::Process ||
            !model::render_insert_valid(snapshot, request.track, request.only_insert, error)) {
            if (error.empty()) error = "only Process Selection runs through one plug-in";
            return false;
        }
        if (solo_ == nullptr) {
            error = "this engine cannot silence the track's other inserts for a render; "
                    "nothing was rendered";
            return false;
        }
    }

    const uint32_t rate = session_.config().sample_rate;
    auto run = std::make_unique<Run>();
    run->channels = std::min<uint16_t>(2, std::max<uint16_t>(1, track->channels));
    run->length = static_cast<uint64_t>(request.end - request.start + request.tail);
    const uint64_t start = static_cast<uint64_t>(request.start);
    const uint64_t preroll = uint64_t{rate} * kRenderPrerollMs / 1000;
    const uint64_t from = start > preroll ? start - preroll : 0;
    run->lead = start - from;

    const std::string dir = session_dir + "/" + session_files::kAudioDir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir, ec)) {
        error = "cannot create " + dir + ": " + std::strerror(errno);
        return false;
    }
    run->writer.set_description(label_of(request.kind, track->name));
    // open() refuses an existing name with EEXIST and leaves it untouched.
    bool opened = false;
    for (int n = 1; n <= 9999 && !opened; ++n) {
        char digits[16];
        std::snprintf(digits, sizeof digits, "%02d", n);
        run->file = file_safe(track->name) + "_" + tag_of(request.kind) + "_" + digits + ".wav";
        opened = run->writer.open(dir + "/" + run->file, rate, run->channels,
                                  WavWriter::Format::Float32, start);
        if (!opened && errno != EEXIST) {
            error = "cannot create " + run->file + ": " + std::strerror(errno);
            return false;
        }
    }
    if (!opened) {
        error = "no free file name for " + track->name;
        return false;
    }
    run->path = dir + "/" + run->file;
    run->result.relative_path = std::string(session_files::kAudioDir) + "/" + run->file;
    run->result.start = request.start;
    run->result.label = label_of(request.kind, track->name);
    const auto discard = [&](const std::string& why) {
        run->writer.close();
        std::remove(run->path.c_str());
        error = why;
        return false;
    };

    if (!playback_.locate(from, error) || !playback_.settle(30.0)) {
        if (error.empty()) error = "the disk did not serve the start of the range in time";
        return discard(error);
    }
    // Before the roll, so the pre-roll warms the one insert alone too.
    if (request.only_insert != 0 && !engage_solo(request.track, request.only_insert, error)) {
        return discard(error);
    }
    run->playback_misses_before = session_.playback_misses();

    request_ = request;
    basis_.track = snapshot.edit->tracks[index];
    basis_.bypass = insert_bypass(snapshot, *track);
    chain_misses_ = std::move(chain_misses);
    run_ = std::move(run);
    tap_ = std::make_unique<TrackTap>(static_cast<uint32_t>(index),
                                      static_cast<std::size_t>(rate) * 2);
    cancel_.store(false);
    finished_.store(false);
    progress_.store(0.0);
    session_.set_track_tap(tap_.get());
    playback_.play();
    // After play(), which clears it.
    session_.set_roll_end(from + run_->lead + run_->length + kRollSlack);
    worker_ = std::thread([this] { work(); });
    return true;
}

void RenderInPlace::work() {
    Run& r = *run_;
    constexpr std::size_t kChunk = 4096;
    std::vector<float> buf(kChunk * 2), out(kChunk * 2);
    uint64_t to_drop = 0;      // frames of lead-in and latency still to discard
    uint64_t kept = 0;
    bool seen = false;
    uint32_t latency = 0;
    uint64_t chain_base = 0;
    auto last = std::chrono::steady_clock::now();
    const auto unplayed_text = [&]() {
        return "the plug-in chain did not play " + std::to_string(tap_->unplayed_frames()) +
               " frames of the range (it was starting, restarting or silent); nothing was kept";
    };
    const auto lost_text = [](uint64_t frames) {
        return "the render lost " + std::to_string(frames) + " frames to a full ring; nothing was kept";
    };
    const auto fail = [&](const std::string& why) {
        r.error = why;
        r.writer.close();
        std::remove(r.path.c_str());
        finished_.store(true, std::memory_order_release);
    };
    while (kept < r.length) {
        if (cancel_.load(std::memory_order_relaxed)) {
            r.cancelled = true;
            return fail("the render was cancelled; nothing was changed");
        }
        const std::size_t got = tap_->read(buf.data(), kChunk);
        if (got == 0) {
            // A chain that plays nothing pushes nothing: say so now, not
            // after the ten seconds of waiting for a roll that is under way.
            if (tap_->unplayed_frames() != 0 && kept == 0) return fail(unplayed_text());
            const auto idle = std::chrono::duration<double>(std::chrono::steady_clock::now() - last).count();
            const bool stopped = session_.transport().state() == TransportState::Stopped;
            if ((seen && stopped && idle > 0.3) || (!seen && idle > 10.0)) {
                // Frames the full ring dropped never arrive, so a roll that
                // overran it ends here short of the length, not below.
                if (tap_->lost_frames() != 0) return fail(lost_text(tap_->lost_frames()));
                return fail(seen ? "the transport stopped before the end of the range"
                                 : "the transport never started to roll");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (!seen) {
            seen = true;
            // The latency is the chain's own, set with its first block; the
            // misses are read from here, on a chain that has been steady.
            latency = tap_->latency();
            to_drop = r.lead + latency;
            chain_base = chain_misses_ ? chain_misses_() : 0;
        }
        last = std::chrono::steady_clock::now();
        std::size_t at = 0;
        if (to_drop != 0) {
            const std::size_t d = static_cast<std::size_t>(std::min<uint64_t>(to_drop, got));
            to_drop -= d;
            at = d;
        }
        const std::size_t n = static_cast<std::size_t>(std::min<uint64_t>(got - at, r.length - kept));
        if (n == 0) continue;
        for (std::size_t i = 0; i < n; ++i) {
            for (uint16_t c = 0; c < r.channels; ++c) {
                out[i * r.channels + c] = buf[2 * (at + i) + c];
            }
        }
        if (!r.writer.write(out.data(), n)) {
            return fail("cannot write " + r.file + ": " + r.writer.failure());
        }
        kept += n;
        progress_.store(static_cast<double>(kept) / static_cast<double>(r.length),
                        std::memory_order_relaxed);
    }
    // Any of these is a stretch of the file that is not the chain's output.
    if (tap_->unplayed_frames() != 0) return fail(unplayed_text());
    if (tap_->lost_frames() != 0) {
        return fail(lost_text(tap_->lost_frames()));
    }
    if (tap_->latency() != latency) {
        return fail("the chain's latency changed while it rendered; nothing was kept");
    }
    if (chain_misses_ && chain_misses_() != chain_base) {
        return fail("a plug-in missed its deadline during the render, so the file would have "
                    "a hole; nothing was kept");
    }
    const uint64_t missed = session_.playback_misses() - r.playback_misses_before;
    if (missed != 0) {
        return fail("the disk did not keep up for " + std::to_string(missed) +
                    " block(s) of the roll, so the file has gaps; nothing was kept");
    }
    if (!r.writer.checkpoint() || !r.writer.close()) {
        return fail("cannot write " + r.file + ": " +
                    (r.writer.failure().empty() ? "the disk refused it" : r.writer.failure()));
    }
    r.result.frames = kept;
    finished_.store(true, std::memory_order_release);
}

void RenderInPlace::release_tap() {
    playback_.stop();
    session_.stop_playback();
    session_.set_roll_end(Session::kNoRollEnd);
    session_.set_track_tap(nullptr);
    // A callback already inside push() has finished once the count moves.
    const uint64_t b = session_.plans().blocks();
    for (int i = 0; i < 100 && session_.plans().blocks() == b; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    tap_.reset();
}

void RenderInPlace::cancel() { cancel_.store(true); }

bool RenderInPlace::poll(RenderResult& out, std::string& error, bool& cancelled) {
    if (!worker_.joinable() || !finished_.load(std::memory_order_acquire)) return false;
    worker_.join();
    release_tap();
    lift_solo();
    out = run_->result;
    error = run_->error;
    cancelled = run_->cancelled;
    run_.reset();
    return true;
}

// --- the document -----------------------------------------------------------

bool place_render(model::Document& document, const std::string& session_dir,
                  const RenderRequest& request, const RenderBasis& basis,
                  const RenderResult& rendered, std::vector<model::ReplicationUpdate>& updates,
                  std::string& error) {
    const std::string full = session_dir + "/" + rendered.relative_path;
    const auto refuse = [&](const std::string& why) {
        std::remove(full.c_str());
        error = why;
        return false;
    };
    const model::Snapshot now = document.current();
    const model::TrackEdit* track = track_of(now, request.track, nullptr);
    if (track == nullptr) return refuse("the track is gone; nothing was changed");
    if (!basis.track || !same_basis(*basis.track, *track) ||
        basis.bypass != insert_bypass(now, *track)) {
        return refuse(track->name +
                      " was edited while it rendered, so the print no longer matches it; "
                      "nothing was changed");
    }

    // The numbers come from the file, as for every source.
    WavReader file;
    std::string why;
    if (!file.open(full, why)) return refuse(rendered.relative_path + ": " + why);
    model::AddSource add;
    add.source.id = now.next_id;
    add.source.path = rendered.relative_path;
    add.source.channels = file.channels();
    add.source.length = file.frames();
    add.source.format = model::AudioFormat::Float32;
    add.source.time_reference = static_cast<int64_t>(file.time_reference());
    add.source.kind = model::SourceKind::Imported;
    file.close();

    model::Snapshot with;
    if (!model::apply(now, add, with, why)) return refuse(why);
    model::Id id = with.next_id;
    model::RenderPlacement p;
    p.track = request.track;
    p.kind = request.kind;
    p.start = rendered.start;
    p.source = add.source.id;
    p.only_insert = request.only_insert;
    p.name = request.name;
    std::vector<model::Member> members;
    if (!model::render_members(with, p, [&id] { return id++; }, members, why)) {
        return refuse("the session refused the print: " + why);
    }
    model::Compound c;
    c.label = undo_label(request.kind);
    c.ops.push_back(add);
    for (model::Member& m : members) c.ops.push_back(std::move(m));
    model::ReplicationUpdate u;
    if (!document.apply(c, u, why)) return refuse("the session refused the print: " + why);
    updates.push_back(std::move(u));
    return true;
}

bool unfreeze_track(model::Document& document, model::Id track,
                    std::vector<model::ReplicationUpdate>& updates, std::string& error) {
    std::vector<model::Member> members;
    if (!model::unfreeze_members(document.current(), track, members, error)) return false;
    model::Compound c;
    c.label = "Unfreeze Track";
    c.ops = std::move(members);
    model::ReplicationUpdate u;
    if (!document.apply(c, u, error)) return false;
    updates.push_back(std::move(u));
    return true;
}

}  // namespace sonora
