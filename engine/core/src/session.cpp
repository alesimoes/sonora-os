// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/session.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "denormals.h"
#include "sonora/plan_graph.h"

namespace sonora {
namespace {

bool make_dir(const std::string& path) {
    if (::mkdir(path.c_str(), 0775) == 0) return true;
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string audio_dir(const std::string& session_dir) {
    return session_dir + "/Audio Files";
}

std::string take_file_name(const std::string& track, int take) {
    char digits[16];
    std::snprintf(digits, sizeof digits, "%02d", take);
    return track + "_" + digits + ".wav";
}

std::string track_name(int number) {
    char name[32];
    std::snprintf(name, sizeof name, "Audio %d", number);
    return name;
}

// The number in a run of decimal digits, or 0 if the run is empty, holds
// anything else, or is longer than `max_digits`. The bound is what keeps a
// run of any length from overflowing.
int parse_number(const std::string& s, std::size_t begin, std::size_t end,
                 std::size_t max_digits) {
    if (end <= begin || end - begin > max_digits) return 0;
    int n = 0;
    for (std::size_t k = begin; k < end; ++k) {
        if (s[k] < '0' || s[k] > '9') return 0;
        n = n * 10 + (s[k] - '0');
    }
    return n;
}

// The take number in `name` if it is exactly what start_recording() would
// write for some track of a session, or 0. Any "Audio <n>" counts, not only
// the tracks open now: the same folder may have been recorded with more.
// Anything looser lets a stray file set the counter: "Guitar_20261012.wav"
// would make the next take 20261013.
int take_of(const std::string& name) {
    const std::string ext = ".wav";
    if (name.size() <= ext.size() ||
        name.compare(name.size() - ext.size(), ext.size(), ext) != 0) {
        return 0;
    }
    const std::size_t end = name.size() - ext.size();
    const std::size_t under = name.rfind('_', end);
    if (under == std::string::npos) return 0;
    const int take = parse_number(name, under + 1, end, 4);  // 4 == digits in kMaxTake
    if (take < 1 || take > Session::kMaxTake) return 0;

    const std::string prefix = "Audio ";
    if (name.compare(0, prefix.size(), prefix) != 0) return 0;
    const int track = parse_number(name, prefix.size(), under, 9);
    if (track < 1) return 0;
    // Round-tripped rather than compared piecewise, so "Audio 1_1.wav",
    // "Audio 01_05.wav" and "Audio 1_0005.wav" -- not spellings we produce --
    // do not count.
    return take_file_name(track_name(track), take) == name ? take : 0;
}

}  // namespace

Session::Session(SessionConfig cfg) : cfg_(std::move(cfg)) {
    transport_ = std::make_unique<Transport>(cfg_.sample_rate);

    const auto ring_frames =
        static_cast<std::size_t>(cfg_.ring_seconds * cfg_.sample_rate);

    std::vector<Track*> raw;
    for (int i = 0; i < cfg_.track_count; ++i) {
        tracks_.push_back(std::make_unique<Track>(i, track_name(i + 1),
                                                  cfg_.channels_per_track,
                                                  ring_frames));
        raw.push_back(tracks_.back().get());
    }

    // 64 kframes per disk write keeps syscall count low without stalling.
    disk_ = std::make_unique<DiskThread>(std::move(raw), 65536);

    const PlaybackConfig& pb = cfg_.playback;
    const auto tracks = static_cast<uint32_t>(cfg_.track_count);
    max_block_ = pb.max_block_frames;
    // ADR-0009 §4.4: the window must outlast a read; until Unverified 1 is
    // measured the read latency term is the configured placeholder, plus a
    // block.
    window_frames_ = static_cast<int64_t>(pb.window_seconds * cfg_.sample_rate) + max_block_;
    const auto window_chunks =
        static_cast<uint32_t>((window_frames_ + pb.chunk_frames - 1) / pb.chunk_frames);
    ReaderConfig rc;
    rc.chunk_frames = pb.chunk_frames;
    rc.window_frames = window_frames_;
    rc.max_sources = pb.max_sources;
    // A track's demand is channels x (ceil(overlap / C) + 1) per window
    // (ADR-0009 §4.3); two plans may need disjoint chunks.
    rc.pool_chunks = pb.pool_chunks
                         ? pb.pool_chunks
                         : 2 * tracks * cfg_.channels_per_track * (window_chunks + 1) + 16;
    plans_ = std::make_unique<PlanExchange>();
    reader_ = std::make_unique<DiskReader>(rc, *plans_, *transport_);
    commands_ = std::make_unique<RingBuffer<RtCommand>>(pb.command_capacity);

    // Every track with a chain: the largest graph a plan can have.
    scratch_buffers_ = graph_nodes(tracks, tracks);
    scratch_.reset(new float[static_cast<std::size_t>(scratch_buffers_) * 2 * max_block_]());
    track_buf_.reset(new float[2 * static_cast<std::size_t>(max_block_)]());
    delay_len_ = pb.max_delay_frames ? pb.max_delay_frames + 1 : 0;
    delay_lines_ = delay_len_ ? std::min(pb.max_delayed_edges, tracks) : 0;
    // Value-initialised, so every page is touched here, not on the audio
    // thread.
    delay_.reset(new float[static_cast<std::size_t>(delay_lines_) * 2 * delay_len_ + 1]());
    delay_pos_.reset(new uint32_t[delay_lines_ + 1]());
    line_owner_.reset(new uint64_t[delay_lines_ + 1]());
    line_filled_.reset(new uint32_t[delay_lines_ + 1]());
    line_stamp_.reset(new uint64_t[delay_lines_ + 1]());
    cursor_.reset(new uint32_t[tracks + 1]());
    cursor_frame_.reset(new uint64_t[tracks + 1]);
    for (uint32_t t = 0; t <= tracks; ++t) cursor_frame_[t] = ~uint64_t{0};
    track_gain_.reset(new float[2 * static_cast<std::size_t>(tracks) + 2]());
    track_target_.reset(new float[2 * static_cast<std::size_t>(tracks) + 2]());

    // Until engine/control publishes one: every track into the bus, no
    // region. Published before any backend can call process().
    PlanSizes sizes;
    sizes.tracks = tracks;
    sizes.nodes = graph_nodes(tracks, 0);
    sizes.edges = graph_edges(tracks, 0);
    PlaybackPlan* empty = create_plan(sizes);
    empty->epoch = plans_->next_epoch();
    empty->chunk_frames = pb.chunk_frames;
    // 0.5 ^ e = 10 ^ (law / 20)
    empty->pan_exponent = static_cast<float>(pb.pan_law_db / (20.0 * std::log10(0.5)));
    for (uint32_t t = 0; t < tracks; ++t) {
        empty->tracks[t].engine_track = t;
        empty->tracks[t].channels = cfg_.channels_per_track;
    }
    layout_monitor_graph(*empty);
    plans_->publish(empty);
    reader_->serve(empty, nullptr);
}

Session::~Session() {
    stop();
    reader_->stop();
}

void Session::play() {
    // A take keeps its own state; this only starts a roll.
    if (transport_->state() == TransportState::Stopped) {
        transport_->set_state(TransportState::Rolling);
    }
}

void Session::stop_playback() {
    if (transport_->state() == TransportState::Rolling) {
        transport_->set_state(TransportState::Stopped);
    }
}

bool Session::post_locate(uint64_t frame) {
    RtCommand c{};
    c.kind = RtCommand::Kind::Locate;
    c.frame = frame;
    return commands_->write(&c, 1) == 1;
}

bool Session::create_dirs() {
    if (!make_dir(cfg_.dir)) return false;
    return make_dir(audio_dir(cfg_.dir));
}

int Session::next_free_take() const {
    // Every track shares the take number, so any file of ours counts, on any
    // track, armed now, open now, or neither.
    int highest = 0;
    DIR* d = ::opendir(audio_dir(cfg_.dir).c_str());
    if (!d) return 1;
    while (const dirent* e = ::readdir(d)) {
        const int n = take_of(e->d_name);
        if (n > highest) highest = n;
    }
    ::closedir(d);
    return highest + 1;
}

bool Session::start_recording(int take_number, std::string* error) {
    // Provisional: stop() writes the frame the audio thread recorded first.
    const uint64_t origin = transport_->playhead();
    std::vector<Track*> opened;

    if (take_number < 1 || take_number > kMaxTake) {
        const std::string why =
            "take " + std::to_string(take_number) + " is past the last take a"
            " session holds (" + std::to_string(kMaxTake) +
            "); start a new session";
        std::fprintf(stderr, "sonora: not recording, %s\n", why.c_str());
        if (error) *error = why;
        return false;
    }

    for (auto& t : tracks_) {
        if (!t->rec_armed()) continue;
        const std::string file =
            audio_dir(cfg_.dir) + "/" + take_file_name(t->name(), take_number);
        // The disk thread reads this ring only once the track is published
        // below, a release store that comes after this reset. The audio
        // thread is not fenced off the same way. process() loads the
        // transport state once per block and writes the ring later, so a
        // callback that loaded Recording for the previous take and was then
        // descheduled can still write while this runs: a data race on the
        // indices, and old samples at the head of the new take. Reaching
        // that needs the RT thread stalled across all of stop() (setting
        // Stopped, joining the disk thread, closing every file) and the
        // operator's next record command, ~100 ms at the least, dozens of
        // missed periods. It is not ruled out, only made implausible; a
        // handshake with the callback (it acknowledges a take epoch) is
        // what would rule it out.
        t->capture_ring().reset();
        if (!t->writer().open(file, cfg_.sample_rate, t->channels(),
                              cfg_.format, origin)) {
            const int err = errno;
            // A take missing a track is not the take that was armed, and the
            // operator would find out at playback. Undo and say so instead.
            // Removing is safe: the writer creates exclusively, so every file
            // in `opened` was made by this call and holds nothing yet.
            for (Track* o : opened) {
                const std::string path = o->writer().path();
                o->writer().close();
                std::remove(path.c_str());
            }
            const std::string why =
                file + ": " +
                (err == EEXIST ? "a take with this name is already on disk"
                               : std::strerror(err));
            std::fprintf(stderr, "sonora: not recording, %s\n", why.c_str());
            if (error) *error = why;
            return false;
        }
        opened.push_back(t.get());
    }

    if (opened.empty()) {
        if (error) *error = "no track is armed";
        return false;
    }
    // Only now may the disk thread see these writers; until here the
    // rollback above could close them under it. See Track::set_draining().
    for (Track* t : opened) t->set_draining(true);
    // Ordered before the audio thread can see Recording by the release
    // store below, so the take's first block finds no origin and sets it.
    take_origin_.store(kNoOrigin, std::memory_order_relaxed);
    transport_->set_state(TransportState::Recording);
    return true;
}

namespace {

// Past the 4 GB limit every armed track fails together; one line with a
// count says that better than eighteen.
std::string describe_failures(const std::string& first, int more) {
    if (more == 0) return first;
    return first + " (and " + std::to_string(more) + " more track" +
           (more == 1 ? "" : "s") + ")";
}

}  // namespace

std::string Session::take_failure() const {
    std::string first;
    int more = 0;
    for (const auto& t : tracks_) {
        // failed() is an acquire load, so failure() is settled once it is
        // true, while the disk thread goes on writing the other tracks.
        if (!t->writer().is_open() || !t->writer().failed()) continue;
        if (first.empty()) {
            first = t->writer().path() + ": " + t->writer().failure();
        } else {
            ++more;
        }
    }
    return first.empty() ? first : describe_failures(first, more);
}

bool Session::stop(std::string* error) {
    transport_->set_state(TransportState::Stopped);
    // Let the disk thread flush what is still buffered before we close files.
    // stop() joins it, so after this line nothing else uses the writers.
    if (disk_) disk_->stop();
    // A take stopped before any block recorded keeps the provisional
    // origin; it holds no audio either.
    const uint64_t origin = take_origin_.load(std::memory_order_acquire);
    std::string first_failure;
    int more_failures = 0;
    for (auto& t : tracks_) {
        t->set_draining(false);
        if (origin != kNoOrigin && t->writer().is_open()) {
            t->writer().set_time_reference(origin);
        }
        // Only a take this call ends is reported; a writer keeps its last
        // failure after closing, and ~Session() stops again.
        const bool was_open = t->writer().is_open();
        t->writer().close();
        if (!was_open || !t->writer().failed()) continue;
        if (first_failure.empty()) {
            first_failure = t->writer().path() + ": " + t->writer().failure();
        } else {
            ++more_failures;
        }
    }
    if (first_failure.empty()) return true;
    const std::string why = describe_failures(first_failure, more_failures);
    std::fprintf(stderr, "sonora: take failed, %s\n", why.c_str());
    if (error) *error = why;
    return false;
}

namespace {

// Mono: ((1 -+ pan) / 2) ^ exponent, which is the plan's pan law at centre
// and 1 / 0 at the extremes. Stereo: balance, unity at centre, because each
// side already carries its own channel.
void pan_gains(float pan, bool stereo, float exponent, float& l, float& r) noexcept SONORA_RT {
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;
    if (stereo) {
        l = pan > 0.0f ? 1.0f - pan : 1.0f;
        r = pan < 0.0f ? 1.0f + pan : 1.0f;
        return;
    }
    l = std::pow((1.0f - pan) * 0.5f, exponent);
    r = std::pow((1.0f + pan) * 0.5f, exponent);
}

// Region gain times its fades at timeline frame t. Frame i of an L-frame
// fade in is at i / L, of a fade out at 1 - i / L, so the two sides of a
// crossfade sum to one (linear, s-curve) or in power (equal power).
float segment_gain(const PlanSegment& s, int64_t t) noexcept SONORA_RT {
    float g = s.gain;
    if (t < s.fade_in_end) {
        const float x = static_cast<float>(t - s.start) / static_cast<float>(s.fade_in_end - s.start);
        g *= fade_in_gain(s.fade_in_shape, x);
    }
    if (t >= s.fade_out_start) {
        const float x = static_cast<float>(t - s.fade_out_start) /
                        static_cast<float>(s.end - s.fade_out_start);
        g *= fade_in_gain(s.fade_out_shape, 1.0f - x);
    }
    return g;
}

}  // namespace

void Session::process(const backend::AudioBlock& block) noexcept SONORA_RT {
    // Per thread, and a backend may call from a new thread after a
    // reactivation, so it is checked every block.
    const FlushDenormals flush_denormals;
    // ADR-0009 §4.1: the block count, then the plan, before anything reads
    // either. A new plan is told by its epoch, never its address.
    const PlaybackPlan* plan = plans_->acquire();
    if (plan->epoch != seen_epoch_) {
        seen_epoch_ = plan->epoch;
        reseek_ = true;
        plan_valid_ = plan_in_bounds(*plan);
    }
    ++callbacks_;

    const uint32_t nframes = block.nframes;
    const TransportState state = transport_->state();
    const bool recording = state == TransportState::Recording;
    const bool rolling = state != TransportState::Stopped;
    // A take is one contiguous span of the timeline. engine/control sends no
    // locate during one, but one it posted just before the take began can
    // still be queued. Once the take has its first frame such a locate is
    // acknowledged, so nothing waits on it, and dropped.
    const bool take_started =
        recording && take_origin_.load(std::memory_order_relaxed) != kNoOrigin;

    RtCommand cmd{};
    while (commands_->read(&cmd, 1) == 1) {
        if (cmd.kind != RtCommand::Kind::Locate) continue;
        if (take_started) {
            locates_dropped_.fetch_add(1, std::memory_order_relaxed);
        } else {
            transport_->set_playhead(cmd.frame);
            reseek_ = true;
        }
        transport_->note_locate();
    }

    // After the take's state is loaded and before its capture: a runner
    // that stalls here is the callback descheduled across stop() and the
    // next take (test_take_boundaries). insert_chain_runner.h: once per
    // callback, whole callback, or not at all.
    runner_ = nullptr;
    if (plan_valid_ && plan->last_insert_node != kNoNode) {
        InsertChainRunner* const r = chains_.load(std::memory_order_acquire);
        if (r != nullptr && nframes <= max_block_) {
            runner_ = r;
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            runner_->begin(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
        } else if (r != nullptr) {
            chain_bypassed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        Track* t = tracks_[i].get();
        const float* in = i < block.input_count ? block.inputs[i] : nullptr;
        if (!in) continue;

        // Meter every input, armed or not, so the UI shows signal before
        // anyone hits record.
        float peak = 0.0f;
        for (uint32_t f = 0; f < nframes; ++f) {
            const float a = in[f] < 0.0f ? -in[f] : in[f];
            if (a > peak) peak = a;
        }
        t->report_peak(peak);

        if (!recording || !t->rec_armed()) continue;

        // Capture is pre-fader by design: the fader is a monitoring decision
        // and must not be baked into the recorded file.
        const std::size_t wrote = t->capture_ring().write(in, nframes);
        if (wrote < nframes) t->note_overrun();
    }

    // Mix state, read once per block and ramped across it from what the
    // last block ended on (ADR-0009 §4.5). Solo, mute and gain fold into one
    // coefficient per side, so each ramps to and from zero like a fader.
    bool any_solo = false;
    for (const auto& t : tracks_) any_solo = any_solo || t->soloed();
    for (std::size_t e = 0; e < tracks_.size(); ++e) {
        const Track& t = *tracks_[e];
        float g = t.gain();
        if (t.muted() || (any_solo && !t.soloed())) g = 0.0f;
        float l = 0.0f;
        float r = 0.0f;
        pan_gains(t.pan(), t.channels() >= 2, plan->pan_exponent, l, r);
        track_target_[2 * e] = g * l;
        track_target_[2 * e + 1] = g * r;
    }
    bus_target_ = bus_gain_.load(std::memory_order_relaxed);

    const uint64_t playhead = transport_->playhead();
    if (recording && !take_started) take_origin_.store(playhead, std::memory_order_release);
    if (playhead != expected_playhead_) reseek_ = true;
    block_frames_ = nframes;
    if (!plan_valid_) {
        for (uint32_t o = 0; o < block.output_count; ++o) {
            std::memset(block.outputs[o], 0, nframes * sizeof(float));
        }
        invalid_plan_blocks_.fetch_add(1, std::memory_order_relaxed);
    }
    for (uint32_t off = 0; plan_valid_ && off < nframes;) {
        const uint32_t n = nframes - off < max_block_ ? nframes - off : max_block_;
        block_done_ = off;
        mix(*plan, block, off, n, rolling, recording, playhead + off);
        reseek_ = false;
        off += n;
    }
    for (std::size_t i = 0; i < 2 * tracks_.size(); ++i) track_gain_[i] = track_target_[i];
    bus_applied_ = bus_target_;

    if (rolling) transport_->advance(nframes);
    expected_playhead_ = playhead + (rolling ? nframes : 0);
}

void Session::mix(const PlaybackPlan& plan, const backend::AudioBlock& block, uint32_t offset,
                  uint32_t n, bool rolling, bool recording, uint64_t from) noexcept SONORA_RT {
    // A backend buffer holds whatever its last cycle left; HardwareOut
    // overwrites its two below.
    for (uint32_t o = 0; o < block.output_count; ++o) {
        std::memset(block.outputs[o] + offset, 0, n * sizeof(float));
    }
    const float inv_block = 1.0f / static_cast<float>(block_frames_);
    float* const tb[2] = {track_buf_.get(), track_buf_.get() + max_block_};

    for (uint32_t ni = 0; ni < plan.node_count; ++ni) {
        const PlanNode& node = plan.nodes[ni];
        float* const out[2] = {scratch(node.scratch_buffer, 0), scratch(node.scratch_buffer, 1)};
        float* const L = out[0];
        float* const R = out[1];

        switch (node.kind) {
            case NodeKind::TrackOut: {
                const bool stereo =
                    render_source(plan, node, block, offset, n, rolling, recording, from, tb);
                const float* const in[2] = {tb[0], tb[1]};
                apply_fader(plan.tracks[node.track].engine_track, stereo, in, out, n);
                continue;
            }
            case NodeKind::TrackSource: {
                const bool stereo =
                    render_source(plan, node, block, offset, n, rolling, recording, from, out);
                // A mono source feeds a stereo-planed chain; the copy keeps
                // side 1 defined for a runner that reads it anyway.
                if (!stereo) std::memcpy(R, L, n * sizeof(float));
                continue;
            }
            case NodeKind::InsertChain: {
                // One input edge, never delayed: compensation sits on the
                // edges into the bus.
                const uint32_t src = plan.nodes[plan.edges[node.first_input_edge].from_node].scratch_buffer;
                const float* const in[2] = {scratch(src, 0), scratch(src, 1)};
                if (runner_ != nullptr && node.channel_slot != kNoChannelSlot) {
                    runner_->run(node, in, out, n);
                } else {
                    std::memcpy(L, in[0], n * sizeof(float));
                    std::memcpy(R, in[1], n * sizeof(float));
                }
                if (runner_ != nullptr && ni == plan.last_insert_node) runner_->end();
                continue;
            }
            case NodeKind::TrackFader: {
                const uint32_t src = plan.nodes[plan.edges[node.first_input_edge].from_node].scratch_buffer;
                const float* const in[2] = {scratch(src, 0), scratch(src, 1)};
                const PlanTrack& pt = plan.tracks[node.track];
                apply_fader(pt.engine_track, pt.channels >= 2, in, out, n);
                continue;
            }
            case NodeKind::BusSum:
            case NodeKind::HardwareOut:
                break;
        }

        // BusSum and HardwareOut: the sum of their input edges, each
        // through its delay line when it has a delay.
        std::memset(L, 0, n * sizeof(float));
        std::memset(R, 0, n * sizeof(float));
        for (uint32_t k = 0; k < node.input_edge_count; ++k) {
            const PlanEdge& edge = plan.edges[node.first_input_edge + k];
            const uint32_t src = plan.nodes[edge.from_node].scratch_buffer;
            const float* const in[2] = {scratch(src, 0), scratch(src, 1)};
            sum_edge(plan, edge, in, out, n);
        }
        if (node.kind != NodeKind::HardwareOut) continue;

        const float b0 = bus_applied_;
        const float db = bus_target_ - b0;
        float peak_l = 0.0f;
        float peak_r = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            const float g = b0 + db * static_cast<float>(block_done_ + i + 1) * inv_block;
            L[i] *= g;
            R[i] *= g;
            const float al = L[i] < 0.0f ? -L[i] : L[i];
            const float ar = R[i] < 0.0f ? -R[i] : R[i];
            if (al > peak_l) peak_l = al;
            if (ar > peak_r) peak_r = ar;
        }
        if (plan.monitor_out[0] < block.output_count) {
            std::memcpy(block.outputs[plan.monitor_out[0]] + offset, L, n * sizeof(float));
        }
        if (plan.monitor_out[1] < block.output_count) {
            std::memcpy(block.outputs[plan.monitor_out[1]] + offset, R, n * sizeof(float));
        }
        for (int side = 0; side < 2; ++side) {
            const float p = side ? peak_r : peak_l;
            float prev = bus_peak_[side].load(std::memory_order_relaxed);
            while (p > prev &&
                   !bus_peak_[side].compare_exchange_weak(prev, p, std::memory_order_relaxed)) {
            }
        }
    }
}

bool Session::render_source(const PlaybackPlan& plan, const PlanNode& node,
                            const backend::AudioBlock& block, uint32_t offset, uint32_t n,
                            bool rolling, bool recording, uint64_t from,
                            float* const out[2]) noexcept SONORA_RT {
    const PlanTrack& pt = plan.tracks[node.track];
    const uint32_t e = pt.engine_track;
    const Track& t = *tracks_[e];
    const Track::Monitor mon = t.monitor();
    const bool input = mon == Track::Monitor::Input ||
                       (mon == Track::Monitor::Auto && t.rec_armed() && (!rolling || recording));
    if (input) {
        const float* in = e < block.input_count ? block.inputs[e] : nullptr;
        if (in) {
            std::memcpy(out[0], in + offset, n * sizeof(float));
        } else {
            std::memset(out[0], 0, n * sizeof(float));
        }
        return false;
    }
    if (rolling && mon != Track::Monitor::Off) {
        render_track(plan, node.track, from, n, out);
        return pt.channels >= 2;
    }
    std::memset(out[0], 0, n * sizeof(float));
    return false;
}

void Session::apply_fader(uint32_t e, bool stereo, const float* const in[2], float* const out[2],
                          uint32_t n) noexcept SONORA_RT {
    const float inv_block = 1.0f / static_cast<float>(block_frames_);
    const float l0 = track_gain_[2 * e];
    const float r0 = track_gain_[2 * e + 1];
    const float dl = track_target_[2 * e] - l0;
    const float dr = track_target_[2 * e + 1] - r0;
    const float* right_in = stereo ? in[1] : in[0];
    for (uint32_t i = 0; i < n; ++i) {
        const float a = static_cast<float>(block_done_ + i + 1) * inv_block;
        out[0][i] = in[0][i] * (l0 + dl * a);
        out[1][i] = right_in[i] * (r0 + dr * a);
    }
}

bool Session::plan_in_bounds(const PlaybackPlan& plan) const noexcept SONORA_RT {
    if (plan.last_insert_node != kNoNode &&
        (plan.last_insert_node >= plan.node_count ||
         plan.nodes[plan.last_insert_node].kind != NodeKind::InsertChain)) {
        return false;
    }
    for (uint32_t t = 0; t < plan.track_count; ++t) {
        const PlanTrack& pt = plan.tracks[t];
        if (pt.engine_track >= tracks_.size()) return false;
        if (pt.first_segment > plan.segment_count ||
            pt.segment_count > plan.segment_count - pt.first_segment) {
            return false;
        }
    }
    for (uint32_t ni = 0; ni < plan.node_count; ++ni) {
        const PlanNode& node = plan.nodes[ni];
        if (node.scratch_buffer >= scratch_buffers_) return false;
        if (node.first_input_edge > plan.edge_count ||
            node.input_edge_count > plan.edge_count - node.first_input_edge) {
            return false;
        }
        switch (node.kind) {
            case NodeKind::TrackOut:
            case NodeKind::TrackSource:
                if (node.track >= plan.track_count) return false;
                break;
            case NodeKind::TrackFader:
                if (node.track >= plan.track_count) return false;
                if (node.input_edge_count != 1) return false;
                break;
            case NodeKind::InsertChain:
                if (node.input_edge_count != 1) return false;
                break;
            case NodeKind::BusSum:
            case NodeKind::HardwareOut:
                break;
            default:
                return false;
        }
        for (uint32_t k = 0; k < node.input_edge_count; ++k) {
            const PlanEdge& edge = plan.edges[node.first_input_edge + k];
            if (edge.from_node >= plan.node_count || edge.to_node >= plan.node_count) return false;
        }
    }
    return true;
}

void Session::sum_edge(const PlaybackPlan& plan, const PlanEdge& edge, const float* const in[2],
                       float* const out[2], uint32_t n) noexcept SONORA_RT {
    // kNoDelayLine is past the pool too: the builder left the edge
    // undelayed, and reported it.
    if (edge.delay_frames == 0 || edge.delay_line >= delay_lines_) {
        for (uint32_t i = 0; i < n; ++i) {
            out[0][i] += in[0][i];
            out[1][i] += in[1][i];
        }
        return;
    }
    const uint32_t line = edge.delay_line;
    const uint64_t pair =
        edge_pair(plan.nodes[edge.from_node].stable_id, plan.nodes[edge.to_node].stable_id);
    // Another edge's past is in it, or this edge's with a hole where it went
    // unwritten: forget it in O(1) rather than clear 2 x max_delay samples
    // here. Pieces of one callback share its stamp.
    if (line_owner_[line] != pair || line_stamp_[line] + 1 < callbacks_) {
        line_owner_[line] = pair;
        line_filled_[line] = 0;
    }
    line_stamp_[line] = callbacks_;
    float* const ring = delay_.get() + static_cast<std::size_t>(line) * 2 * delay_len_;
    const uint32_t d = edge.delay_frames < delay_len_ ? edge.delay_frames : delay_len_ - 1;
    uint32_t pos = delay_pos_[line];
    uint32_t filled = line_filled_[line];
    for (uint32_t i = 0; i < n; ++i) {
        ring[pos] = in[0][i];
        ring[delay_len_ + pos] = in[1][i];
        if (filled < delay_len_) ++filled;
        // A frame d back that this edge never wrote is silence.
        if (filled > d) {
            const uint32_t rp = pos >= d ? pos - d : pos + delay_len_ - d;
            out[0][i] += ring[rp];
            out[1][i] += ring[delay_len_ + rp];
        }
        pos = pos + 1 == delay_len_ ? 0 : pos + 1;
    }
    delay_pos_[line] = pos;
    line_filled_[line] = filled;
}

// The segments under [from, from + n), found from a cursor that only moves
// forward while rolling; reset by binary search after a locate, a new plan
// or a jump in the playhead.
void Session::render_track(const PlaybackPlan& plan, uint32_t plan_track, uint64_t from,
                           uint32_t n, float* const out[2]) noexcept SONORA_RT {
    std::memset(out[0], 0, n * sizeof(float));
    std::memset(out[1], 0, n * sizeof(float));
    const PlanTrack& pt = plan.tracks[plan_track];
    const PlanSegment* segs = plan.segments + pt.first_segment;
    const uint32_t count = pt.segment_count;
    const auto t0 = static_cast<int64_t>(from);
    const int64_t t1 = t0 + n;
    uint32_t& c = cursor_[plan_track];
    if (reseek_ || cursor_frame_[plan_track] != from) {
        // Segment ends never decrease along a track, crossfades included.
        uint32_t lo = 0;
        uint32_t hi = count;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            if (segs[mid].end <= t0) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        c = lo;
    }
    if (c > count) c = count;
    for (uint32_t i = c; i < count && segs[i].start < t1; ++i) {
        render_segment(plan, segs[i], from, n, out);
    }
    while (c < count && segs[c].end <= t1) ++c;
    cursor_frame_[plan_track] = static_cast<uint64_t>(t1);
}

void Session::render_segment(const PlaybackPlan& plan, const PlanSegment& seg, uint64_t from,
                             uint32_t n, float* const out[2]) noexcept SONORA_RT {
    const auto t0 = static_cast<int64_t>(from);
    const int64_t a = seg.start > t0 ? seg.start : t0;
    const int64_t b = seg.end < t0 + n ? seg.end : t0 + n;
    if (a >= b) return;
    const int64_t chunk = plan.chunk_frames;
    const uint32_t channels = seg.channels < 2 ? seg.channels : 2;
    for (uint32_t ch = 0; ch < channels; ++ch) {
        float* const dst = out[ch];
        int64_t t = a;
        while (t < b) {
            const int64_t src = seg.source_offset + (t - seg.start);
            const int64_t k = src / chunk;
            const int64_t within = src - k * chunk;
            const int64_t run = b - t < chunk - within ? b - t : chunk - within;
            const uint32_t slot = chunk_slot_of(seg, ch, static_cast<uint64_t>(k));
            const uint32_t buf = slot == kNoChunk ? kNoChunk : load_chunk_slot(plan, slot);
            if (buf == kNoChunk) {
                // Silence, already there, and counted: never a wait.
                misses_.fetch_add(1, std::memory_order_relaxed);
            } else {
                const float* data = reader_->chunk_data(buf) + within;
                const bool faded = t < seg.fade_in_end || t + run > seg.fade_out_start;
                float* d = dst + (t - t0);
                if (!faded && seg.gain == 1.0f) {
                    for (int64_t i = 0; i < run; ++i) d[i] += data[i];
                } else if (!faded) {
                    for (int64_t i = 0; i < run; ++i) d[i] += data[i] * seg.gain;
                } else {
                    for (int64_t i = 0; i < run; ++i) d[i] += data[i] * segment_gain(seg, t + i);
                }
            }
            t += run;
        }
    }
}

}  // namespace sonora
