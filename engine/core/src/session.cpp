// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/session.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "denormals.h"
#include "sonora/frame_text.h"
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

std::string take_description(int take, uint64_t track_id, const std::string& session,
                             const PunchRange* punch, const LoopRange* loop,
                             const std::string& session_id, uint32_t latency) {
    constexpr std::size_t kMax = 255;
    std::string head = "sonora take=" + std::to_string(take);
    if (track_id != 0) head += " track=" + std::to_string(track_id);
    if (loop != nullptr) {
        head += " loop=" + std::to_string(loop->from) + "-" + std::to_string(loop->to);
    } else if (punch != nullptr) {
        head += " punch=" + std::to_string(punch->monitor_in) + "-" +
                std::to_string(punch->monitor_out);
    }
    if (latency != 0) head += " lat=" + std::to_string(latency);
    if (!session_id.empty()) head += " id=" + session_id;
    head += " session=";
    const std::size_t room = kMax - std::min(kMax, head.size());
    std::size_t n = std::min(room, session.size());
    // Back off continuation bytes (10xxxxxx) so the cut lands before a
    // character's lead byte, not inside the character.
    if (n < session.size()) {
        while (n > 0 && (static_cast<unsigned char>(session[n]) & 0xC0) == 0x80) --n;
    }
    return head + session.substr(0, n);
}

bool loop_of_description(const std::string& description, uint64_t& from, uint64_t& to) {
    const std::size_t name = description.find(" session=");
    const std::string head = description.substr(0, name);
    const std::string key = " loop=";
    const std::size_t at = head.find(key);
    if (head.compare(0, 7, "sonora ") != 0 || at == std::string::npos) return false;
    const std::size_t begin = at + key.size();
    const std::size_t dash = head.find('-', begin);
    if (dash == std::string::npos) return false;
    std::size_t end = head.find(' ', dash);
    if (end == std::string::npos) end = head.size();
    const auto number = [&head](std::size_t b, std::size_t e, uint64_t& out) {
        if (b == e || e - b > 19) return false;
        uint64_t v = 0;
        for (std::size_t i = b; i < e; ++i) {
            if (head[i] < '0' || head[i] > '9') return false;
            v = v * 10 + static_cast<uint64_t>(head[i] - '0');
        }
        out = v;
        return true;
    };
    uint64_t a = 0;
    uint64_t b = 0;
    if (!number(begin, dash, a) || !number(dash + 1, end, b) || b <= a) return false;
    from = a;
    to = b;
    return true;
}

bool parse_take_description(const std::string& description, TakeDescription& out) {
    const std::string key = " session=";
    const std::size_t name = description.find(key);
    if (name == std::string::npos) return false;
    const std::string head = description.substr(0, name);
    const auto number = [](const std::string& s, uint64_t& v) {
        if (s.empty() || s.size() > 19) return false;
        v = 0;
        for (const char c : s) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<uint64_t>(c - '0');
        }
        return true;
    };
    const auto range = [&number](const std::string& s, uint64_t& a, uint64_t& b) {
        const std::size_t dash = s.find('-');
        return dash != std::string::npos && number(s.substr(0, dash), a) &&
               number(s.substr(dash + 1), b) && b > a;
    };
    TakeDescription d;
    std::size_t at = 0;
    bool first = true;
    while (at <= head.size()) {
        std::size_t end = head.find(' ', at);
        if (end == std::string::npos) end = head.size();
        const std::string word = head.substr(at, end - at);
        at = end + 1;
        if (first) {
            if (word != "sonora") return false;
            first = false;
            continue;
        }
        const std::size_t eq = word.find('=');
        if (eq == std::string::npos) return false;
        const std::string k = word.substr(0, eq);
        const std::string v = word.substr(eq + 1);
        uint64_t n = 0;
        if (k == "take") {
            if (d.take != 0 || !number(v, n) || n == 0 || n > INT32_MAX) return false;
            d.take = static_cast<int>(n);
        } else if (k == "track") {
            if (d.track_id != 0 || !number(v, n) || n == 0) return false;
            d.track_id = n;
        } else if (k == "punch" || k == "loop") {
            if (d.punched || d.looped || !range(v, d.from, d.to)) return false;
            (k == "punch" ? d.punched : d.looped) = true;
        } else if (k == "lat") {
            if (d.latency != 0 || !number(v, n) || n == 0 || n > UINT32_MAX) return false;
            d.latency = static_cast<uint32_t>(n);
        } else if (k == "id") {
            if (!d.session_id.empty() || v.empty() || v.size() > 64) return false;
            for (const char c : v) {
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-')) return false;
            }
            d.session_id = v;
        } else {
            return false;
        }
    }
    if (d.take == 0) return false;
    d.session = description.substr(name + key.size());
    out = std::move(d);
    return true;
}

Session::Session(SessionConfig cfg)
    : cfg_(std::move(cfg)),
      folder_(std::make_shared<const std::string>(cfg_.dir)),
      session_name_(cfg_.name) {
    transport_ = std::make_unique<Transport>(cfg_.sample_rate);
    probe_ = std::make_unique<LatencyProbe>(cfg_.sample_rate);
    click_ = std::make_unique<Click>(cfg_.sample_rate);

    const auto ring_frames =
        static_cast<std::size_t>(cfg_.ring_seconds * cfg_.sample_rate);

    std::vector<Track*> raw;
    for (int i = 0; i < cfg_.track_count; ++i) {
        tracks_.push_back(std::make_unique<Track>(i, track_name(i + 1),
                                                  cfg_.channels_per_track,
                                                  ring_frames));
        raw.push_back(tracks_.back().get());
    }

    const auto tracks_n = static_cast<uint32_t>(cfg_.track_count);
    // timeline-model.md: at least four records per track; a take started
    // over another pushes two at once, and the disk thread wakes every 10 ms.
    // A loop take adds a Wrap per track per pass, and start_loop() holds a
    // pass to one block at least (RT audit of PR #117).
    const double stall_blocks =
        std::ceil(std::max(0.0, cfg_.disk_stall_seconds) * cfg_.sample_rate /
                  std::max<uint32_t>(1, cfg_.playback.max_block_frames));
    const uint32_t records = cfg_.boundary_records
                                 ? cfg_.boundary_records
                                 : std::max<uint32_t>(64, 8 * tracks_n) +
                                       tracks_n * static_cast<uint32_t>(stall_blocks);
    boundaries_ = std::make_unique<RingBuffer<CaptureBoundary>>(records);
    // One more than it holds: the ring keeps a slot empty.
    punches_ = std::make_unique<RingBuffer<PunchRecord>>(cfg_.playback.punch_capacity + 1);
    punch_slots_ = static_cast<uint32_t>(punches_->capacity());
    punch_table_.reset(new PunchRecord[punch_slots_]);
    for (uint32_t k = 0; k < punch_slots_; ++k) punch_table_[k] = kWholeTake;
    punch_stored_.reset(new uint64_t[punch_slots_]());
    monitor_pos_.reset(new float[tracks_n + 1]());
    monitor_stamp_.reset(new uint64_t[tracks_n + 1]());
    source_seen_.reset(new uint64_t[tracks_n + 1]());
    window_take_.reset(new uint32_t[tracks_n + 1]);
    for (uint32_t t = 0; t <= tracks_n; ++t) window_take_[t] = kNoWindow;
    window_start_.reset(new uint32_t[tracks_n + 1]());
    captured_.reset(new uint64_t[tracks_n + 1]());

    // 64 kframes per disk write keeps syscall count low without stalling.
    disk_ = std::make_unique<DiskThread>(std::move(raw), 65536, *boundaries_,
                                         cfg_.loop_pass_cap);
    disk_->set_checkpoint_interval(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(cfg_.sync_seconds)));

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
    // (ADR-0009 §4.3); two plans may need disjoint chunks, and during a
    // loop take each needs a second window at A.
    rc.pool_chunks = pb.pool_chunks
                         ? pb.pool_chunks
                         : 4 * tracks * cfg_.channels_per_track * (window_chunks + 1) + 16;
    plans_ = std::make_unique<PlanExchange>();
    reader_ = std::make_unique<DiskReader>(rc, *plans_, *transport_);
    commands_ = std::make_unique<RingBuffer<RtCommand>>(pb.command_capacity);
    param_commands_ = std::make_unique<RingBuffer<RtCommand>>(
        pb.plugin_param_capacity ? pb.plugin_param_capacity : 1);
    params_.reset(new RtCommand[param_commands_->capacity()]());
    node_params_.reset(new ChainParamEvent[param_commands_->capacity()]());

    // Every track with a chain, and every bus: the largest graph a plan can
    // have.
    max_buses_ = pb.max_buses;
    scratch_buffers_ = graph_nodes(tracks, tracks) + 2 * max_buses_;
    // The store-to-node index (#68 RT audit N1). A plan has at most one
    // InsertChain node per track.
    max_param_stores_ = pb.max_param_stores;
    max_order_ = tracks;
    store_order_.reset(new uint32_t[max_param_stores_ + 1]);
    for (uint32_t s = 0; s <= max_param_stores_; ++s) store_order_[s] = kNoNode;
    node_order_.reset(new uint32_t[scratch_buffers_ + 1]);
    for (uint32_t n = 0; n <= scratch_buffers_; ++n) node_order_[n] = kNoNode;
    order_store_.reset(new uint32_t[max_order_ + 1]());
    order_first_.reset(new uint32_t[max_order_ + 1]());
    order_count_.reset(new uint32_t[max_order_ + 1]());
    order_pos_.reset(new uint32_t[max_order_ + 1]());
    scratch_.reset(new float[static_cast<std::size_t>(scratch_buffers_) * 2 * max_block_]());
    track_buf_.reset(new float[2 * static_cast<std::size_t>(max_block_)]());
    delay_len_ = pb.max_delay_frames ? pb.max_delay_frames + 1 : 0;
    // One edge per track into the bus, and with buses one per return and
    // per send.
    const uint32_t delayable =
        tracks + (max_buses_ ? max_buses_ + (tracks + max_buses_) * kStripSendSlots : 0);
    delay_lines_ = delay_len_ ? std::min(pb.max_delayed_edges, delayable) : 0;
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
    track_open_.reset(new float[tracks + 1]());
    track_level_.reset(new float[tracks + 1]());
    track_pan_.reset(new float[tracks + 1]());
    send_states_ = (tracks + max_buses_) * kStripSendSlots;
    sends_.reset(new SendState[send_states_ + 1]);
    aux_.reset(new AuxState[max_buses_ + 1]);
    send_gain_.reset(new float[2 * static_cast<std::size_t>(send_states_) + 2]());
    send_target_.reset(new float[2 * static_cast<std::size_t>(send_states_) + 2]());
    send_stamp_.reset(new uint64_t[send_states_ + 1]());
    aux_gain_.reset(new float[2 * static_cast<std::size_t>(max_buses_) + 2]());
    aux_target_.reset(new float[2 * static_cast<std::size_t>(max_buses_) + 2]());
    aux_stamp_.reset(new uint64_t[max_buses_ + 1]());
    aux_open_.reset(new float[max_buses_ + 1]());
    aux_level_.reset(new float[max_buses_ + 1]());
    aux_pan_.reset(new float[max_buses_ + 1]());

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

void Session::set_send(uint32_t strip, uint32_t slot, float level, float pan, bool follow_pan,
                       bool mute, bool pre) {
    if (slot >= kStripSendSlots) return;
    const uint64_t k = static_cast<uint64_t>(strip) * kStripSendSlots + slot;
    if (k >= send_states_) return;
    SendState& s = sends_[k];
    s.level.store(level, std::memory_order_relaxed);
    s.pan.store(pan, std::memory_order_relaxed);
    s.follow_pan.store(follow_pan, std::memory_order_relaxed);
    s.mute.store(mute, std::memory_order_relaxed);
    s.pre.store(pre, std::memory_order_relaxed);
}

void Session::set_aux(uint32_t bus, float gain, float pan, bool mute) {
    if (bus >= max_buses_) return;
    AuxState& a = aux_[bus];
    a.gain.store(gain, std::memory_order_relaxed);
    a.pan.store(pan, std::memory_order_relaxed);
    a.mute.store(mute, std::memory_order_relaxed);
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

bool Session::set_input_latency(uint32_t frames, std::string* error) {
    if (recording_take_ != 0) {
        if (error) *error = "the round-trip latency cannot change during a take";
        return false;
    }
    if (frames > cfg_.sample_rate) {
        if (error) {
            *error = "a round trip of " + std::to_string(frames) +
                     " frames is over one second, longer than any interface's";
        }
        return false;
    }
    input_latency_.store(frames, std::memory_order_relaxed);
    return true;
}

bool Session::post_locate(uint64_t frame) {
    RtCommand c{};
    c.kind = RtCommand::Kind::Locate;
    c.frame = frame;
    return commands_->write(&c, 1) == 1;
}

bool Session::set_loop(uint64_t from, uint64_t to, std::string* error) {
    LoopHold want;
    if (to > from) {
        const std::string range = "the loop from " + frame_text(from) + " to " + frame_text(to);
        const auto refuse = [&](const std::string& why) {
            if (error) *error = why;
            return false;
        };
        if (to - from < max_block_) {
            return refuse(range + " is shorter than one block (" + std::to_string(max_block_) +
                          " frames)");
        }
        if (to > static_cast<uint64_t>(INT64_MAX)) return refuse(range + " is past the timeline");
        want = {static_cast<int64_t>(from), static_cast<int64_t>(to)};
    }
    if (want == loop_wanted_) return true;
    loop_wanted_ = want;
    hold_playback_loop();
    return true;
}

// A plain or punched take records through B, and a held B would stop
// the reader there; stop() and punch_out() hold the range again.
void Session::hold_playback_loop() {
    if (recording_take_ != 0) return;
    const bool change = loop_pending();
    loop_gen_ = reader_->hold_loop(loop_wanted_, change ? loop_sent_ : LoopHold{},
                                   change ? loops_sent_ + 1 : 0);
}

void Session::pump_loop() {
    if (!loop_pending() || recording_take_ != 0 || reader_->resident() < loop_gen_) return;
    RtCommand c{};
    c.kind = RtCommand::Kind::Loop;
    c.frame = static_cast<uint64_t>(loop_wanted_.from);
    c.to = static_cast<uint64_t>(loop_wanted_.to);
    if (commands_->write(&c, 1) != 1) return;
    loop_sent_ = loop_wanted_;
    ++loops_sent_;
}

bool Session::post_plugin_param(uint64_t epoch, uint32_t store, uint32_t instance, uint32_t param,
                                float value) {
    // A NaN would reach the store, every later submit and the host's
    // table; the plan never names kNoParamStore.
    if (epoch == 0 || store >= max_param_stores_ || !std::isfinite(value)) return false;
    RtCommand c{};
    c.kind = RtCommand::Kind::PluginParam;
    c.epoch = epoch;
    c.store = store;
    c.instance = instance;
    c.param = param;
    c.value = value;
    return param_commands_->write(&c, 1) == 1;
}

bool Session::create_dirs() {
    if (!make_dir(*folder_)) return false;
    return make_dir(audio_dir(*folder_));
}

std::string Session::folder_change_refusal() const {
    if (recording_take_ != 0) return "take " + std::to_string(recording_take_) + " is open";
    if (!closing_.empty()) return "take " + std::to_string(closing_.begin()->first) + " is closing";
    // A slot is released only once the disk thread has closed its file.
    for (const auto& t : tracks_) {
        for (int s = 0; s < Track::kWriterSlots; ++s) {
            if (const int take = t->slot_take(s)) {
                return "take " + std::to_string(take) + " is closing";
            }
        }
    }
    return {};
}

int Session::closing_take() const {
    for (const auto& [take, c] : closing_) {
        if (take != recording_take_) return take;
    }
    // Collected, but a slot is released only once its file is closed.
    for (const auto& t : tracks_) {
        for (int s = 0; s < Track::kWriterSlots; ++s) {
            const int take = t->slot_take(s);
            if (take != 0 && take != recording_take_) return take;
        }
    }
    return 0;
}

bool Session::set_folder(std::string path, std::string& why) {
    why = folder_change_refusal();
    if (!why.empty()) return false;
    folder_ = std::make_shared<const std::string>(std::move(path));
    return true;
}

int Session::next_free_take() const {
    // Every track shares the take number, so any file of ours counts, on any
    // track, armed now, open now, or neither.
    int highest = 0;
    DIR* d = ::opendir(audio_dir(*folder_).c_str());
    if (!d) return 1;
    while (const dirent* e = ::readdir(d)) {
        const int n = take_of(e->d_name);
        if (n > highest) highest = n;
    }
    ::closedir(d);
    return highest + 1;
}

bool Session::start_recording(int take_number, std::string* error) {
    return begin_take(take_number, nullptr, nullptr, error);
}

bool Session::start_punch(int take_number, const PunchRange& range, std::string* error) {
    const auto refuse = [error](const std::string& why) {
        std::fprintf(stderr, "sonora: not recording, %s\n", why.c_str());
        if (error) *error = why;
        return false;
    };
    const auto frame = [](uint64_t f) {
        return f == PunchRange::kOpenEnd ? std::string("the end") : frame_text(f);
    };
    if (range.capture_from >= range.capture_to) {
        return refuse("the punch captures nothing: from " + frame(range.capture_from) + " to " +
                      frame(range.capture_to));
    }
    // Auto monitoring hears the input only where it is recorded: what the
    // performer hears while punching is what plays back afterwards.
    if (range.monitor_in >= range.monitor_out || range.monitor_in < range.capture_from ||
        range.monitor_out > range.capture_to) {
        return refuse("the punch's monitored range [" + frame(range.monitor_in) + ", " +
                      frame(range.monitor_out) + ") is not inside what it captures [" +
                      frame(range.capture_from) + ", " + frame(range.capture_to) + ")");
    }
    // The playhead only moves forward while rolling, so a range already
    // behind it stays behind it. Past the punch-out but inside its handle,
    // the take would hold handle only and be placed nowhere: a take shown
    // as recording, then lost.
    const uint64_t playhead = transport_->playhead();
    if (range.monitor_out <= playhead) {
        return refuse("the punch range ends at " + frame(range.monitor_out) +
                      ", before the playhead at " + frame(playhead));
    }
    return begin_take(take_number, &range, nullptr, error);
}

bool Session::start_loop(int take_number, const LoopRange& loop, std::string* error) {
    const auto refuse = [error](const std::string& why) {
        std::fprintf(stderr, "sonora: not recording, %s\n", why.c_str());
        if (error) *error = why;
        return false;
    };
    // A loop take is a span of its own, from its pre-roll: started over a
    // take, the locate below would be dropped and its passes misplaced.
    if (transport_->state() == TransportState::Recording) {
        return refuse("a loop take cannot start during another take; stop first");
    }
    const std::string range = "the loop from " + frame_text(loop.from) + " to " + frame_text(loop.to);
    if (loop.to <= loop.from) return refuse(range + " is empty");
    if (loop.start > loop.from) {
        return refuse(range + " begins before its pre-roll, which starts at " +
                      frame_text(loop.start));
    }
    // ADR-0016 C4: a pass shorter than a block would put two wraps in one
    // callback, and one shorter than its handle has none to crossfade on.
    const uint64_t length = loop.to - loop.from;
    const uint64_t block = cfg_.playback.max_block_frames;
    if (length < block) {
        return refuse(range + " is " + std::to_string(length) + " frames, shorter than one block (" +
                      std::to_string(block) + " frames)");
    }
    if (length < loop.handle) {
        return refuse(range + " is " + std::to_string(length) + " frames, shorter than its handle (" +
                      std::to_string(loop.handle) + " frames)");
    }
    return begin_take(take_number, nullptr, &loop, error);
}

bool Session::begin_take(int take_number, const PunchRange* range, const LoopRange* loop,
                         std::string* error) {
    // Provisional: the disk thread writes the frame the audio thread
    // recorded first, from the take's open boundary record.
    const uint64_t origin = transport_->playhead();
    // Pro Tools counts off only a take started from stop.
    const bool count_off = click_->count_off() && transport_->state() == TransportState::Stopped;

    if (take_number < 1 || take_number > kMaxTake) {
        const std::string why =
            "take " + std::to_string(take_number) + " is past the last take a"
            " session holds (" + std::to_string(kMaxTake) +
            "); start a new session";
        std::fprintf(stderr, "sonora: not recording, %s\n", why.c_str());
        if (error) *error = why;
        return false;
    }

    // A free slot per armed track. A running take holds one; the other is
    // free unless the take before that is still being closed, which the
    // disk thread finishes within a pass or two.
    const auto free_slot = [](const Track& t) {
        if (t.slot_take(t.current_slot()) == 0) return t.current_slot();
        for (int s = 0; s < Track::kWriterSlots; ++s) {
            if (t.slot_take(s) == 0) return s;
        }
        return -1;
    };
    std::vector<std::pair<Track*, int>> opened;
    // Removing is safe: the writer creates exclusively, so every file in
    // `opened` was made by this call and holds nothing yet, and no slot of
    // theirs has been published to the disk thread.
    const auto undo_open = [](const std::vector<std::pair<Track*, int>>& files) {
        for (const auto& o : files) {
            WavWriter& w = o.first->writer(o.second);
            const std::string path = w.path();
            w.close();
            std::remove(path.c_str());
            WrapSidecar& side = o.first->sidecar(o.second);
            if (side.is_open()) {
                const std::string wraps = side.path();
                side.close();
                std::remove(wraps.c_str());
            }
        }
    };
    for (auto& t : tracks_) {
        if (!t->rec_armed()) continue;
        int slot = free_slot(*t);
        for (int i = 0; slot < 0 && i < 1000; ++i) {
            if (!disk_->running()) disk_->drain_once();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            slot = free_slot(*t);
        }
        const std::string file =
            audio_dir(*folder_) + "/" + take_file_name(t->name(), take_number);
        std::string failed = file;
        int err = 0;
        if (slot < 0) {
            err = EBUSY;
        } else {
            WavWriter& w = t->writer(slot);
            w.set_description(
                take_description(take_number, t->document_id(), session_name_, range, loop,
                                 session_id_,
                                 loop != nullptr ? loop->latency : input_latency()));
            if (!w.open(file, cfg_.sample_rate, t->channels(), cfg_.format, origin)) {
                err = errno;
            } else if (loop != nullptr &&
                       !t->sidecar(slot).open(WrapSidecar::path_for(file), loop->from, loop->to,
                                              loop->latency)) {
                // Recovery would have to place this take's passes by loop
                // length alone; a take that cannot keep its wraps is refused
                // like one that cannot keep its audio.
                err = errno;
                failed = WrapSidecar::path_for(file);
                w.close();
                std::remove(file.c_str());
            }
        }
        if (err != 0) {
            // A take missing a track is not the take that was armed, and the
            // operator would find out at playback. Undo and say so instead.
            undo_open(opened);
            const std::string why =
                failed + ": " +
                (err == EEXIST  ? "a take with this name is already on disk"
                 : err == EBUSY ? "the take before it is still being closed"
                                : std::strerror(err));
            std::fprintf(stderr, "sonora: not recording, %s\n", why.c_str());
            if (error) *error = why;
            return false;
        }
        opened.emplace_back(t.get(), slot);
    }

    if (opened.empty()) {
        if (error) *error = "no track is armed";
        return false;
    }
    const auto refuse_after_open = [&](const std::string& why) {
        undo_open(opened);
        std::fprintf(stderr, "sonora: not recording, %s\n", why.c_str());
        if (error) *error = why;
        return false;
    };
    // Ahead of the take, so the audio thread has the range by the block it
    // first sees the take in: the ring's release is ordered before the
    // transport's store below. Never waits; a full ring is refused.
    if (range != nullptr || loop != nullptr) {
        PunchRecord r = kWholeTake;
        r.take = static_cast<uint32_t>(take_number);
        if (range != nullptr) {
            r.crossfade = range->crossfade;
            r.shape = range->shape;
            r.capture_from = range->capture_from;
            r.capture_to = range->capture_to;
            r.monitor_in = range->monitor_in;
            r.monitor_out = range->monitor_out;
        } else {
            r.loop_from = loop->from;
            r.loop_to = loop->to;
            r.latency = loop->latency;
        }
        if (punches_->write(&r, 1) != 1) {
            return refuse_after_open("the audio thread has not taken the last punches yet");
        }
    }
    if (loop != nullptr) {
        // Stopped first, so no block rolls on between the locate and the
        // take: the take's first block applies the locate before it takes
        // its origin, and pass 1 is whole however short the pre-roll. A
        // callback that loaded Rolling before the store would still drain
        // the locate and roll a block on from it, so it is waited out
        // before the locate is posted (PR #117 review, N2).
        if (transport_->state() == TransportState::Rolling) {
            transport_->set_state(TransportState::Stopped);
            if (!wait_for_callback()) {
                return refuse_after_open("the audio callback did not return within 1 s");
            }
        }
        if (!post_locate(loop->start)) {
            return refuse_after_open("the audio thread has not taken the last locates yet");
        }
    }
    // The tempo is the record point's, not the held playhead's.
    const uint64_t record_point = range != nullptr  ? range->monitor_in
                                  : loop != nullptr ? loop->from
                                                    : Click::kAtPlayhead;
    if (count_off && !click_->post_count_off(transport_->next_start(), record_point)) {
        return refuse_after_open("the audio thread has not taken the last count-off yet");
    }
    // Only now may the disk thread see these writers; until here the
    // rollback above could close them under it. See Track::slot_take().
    // next_start() is the start the set_recording() at the end of this
    // function gives the take; it holds only because nothing between here
    // and there calls set_recording(). One that did would label these slots
    // with a start the take never gets.
    for (const auto& o : opened) {
        o.first->set_current_slot(o.second);
        o.first->set_slot_take(o.second, take_number, transport_->next_start());
    }
    // A take started over another ends it here.
    keep_origin(transport_->playhead());
    Closing& c = closing_[take_number];
    c.expected = opened.size();
    c.take.take = take_number;
    c.take.files.clear();
    c.take.punched = range != nullptr;
    c.take.punch = range != nullptr ? *range : PunchRange{};
    c.take.looped = loop != nullptr;
    c.take.loop = loop != nullptr ? *loop : LoopRange{};
    c.take.loop_wraps = 0;
    c.take.origin = kNoOrigin;
    c.take.end = kNoOrigin;
    c.take.latency = loop != nullptr ? loop->latency : input_latency();
    recording_take_ = take_number;
    loop_running_ = loop != nullptr;
    if (loop != nullptr) {
        reader_->hold_loop({static_cast<int64_t>(loop->from), static_cast<int64_t>(loop->to)});
    } else {
        reader_->release_loop();
    }
    newest_take_ = take_number;
    // Ordered before the audio thread can see the take by the store below,
    // so the take's first block finds no origin and sets it.
    take_origin_.store(kNoOrigin, std::memory_order_relaxed);
    punch_passed_.store(0, std::memory_order_relaxed);
    loop_wraps_.store(0, std::memory_order_relaxed);
    // A roll's end, left from a range played before this take, would stop
    // the transport when the take punches out.
    roll_end_.store(range != nullptr ? range->roll_end : kNoRollEnd, std::memory_order_relaxed);
    transport_->set_recording(static_cast<uint32_t>(take_number),
                              range != nullptr || loop != nullptr);
    return true;
}

bool Session::punch_out() {
    if (transport_->state() != TransportState::Recording) return false;
    const uint64_t end = transport_->playhead();
    transport_->set_state(TransportState::Rolling);
    keep_origin(end);
    recording_take_ = 0;
    // Rolling on, the playback loop wraps again.
    hold_playback_loop();
    return true;
}

bool Session::follow_punch_out() {
    if (recording_take_ == 0 || transport_->state() != TransportState::Recording) return false;
    if (punch_passed_.load(std::memory_order_acquire) != static_cast<uint32_t>(recording_take_)) {
        return false;
    }
    return punch_out();
}

void Session::keep_origin(uint64_t end) {
    if (recording_take_ == 0) return;
    const auto it = closing_.find(recording_take_);
    if (it == closing_.end()) return;
    it->second.take.origin = take_origin();
    it->second.take.end = end;
    if (it->second.take.looped) {
        it->second.take.loop_wraps = loop_wraps_.load(std::memory_order_acquire);
    }
}

void Session::collect_closed() {
    if (!disk_->running()) disk_->drain_once();
    for (DiskThread::ClosedFile& f : disk_->take_closed()) {
        if (!f.failure.empty()) failed_files_.push_back({f.take, f.path + ": " + f.failure});
        const auto it = closing_.find(f.take);
        if (it != closing_.end()) it->second.take.files.push_back(std::move(f));
    }
}

std::vector<Session::ClosedTake> Session::closed_takes() {
    collect_closed();
    std::vector<ClosedTake> out;
    for (auto it = closing_.begin(); it != closing_.end();) {
        if (it->first != recording_take_ && it->second.take.files.size() >= it->second.expected) {
            out.push_back(std::move(it->second.take));
            it = closing_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

bool Session::wait_for_callback() const {
    // The other half of process()'s entry store: either that callback loads
    // Stopped, or this load sees it in flight.
    const uint64_t seq = callback_seq_.load(std::memory_order_seq_cst);
    if ((seq & 1) == 0) return true;
    // A callback lasts a period; a second is a backend that has hung. What
    // it writes after this is dropped and counted: stop() closes its take's
    // files below, and the next start_recording() opens the next take's,
    // whose open record only a later callback pushes, so no slot claims
    // those samples (test_callback_stalled_past_the_bound).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (callback_seq_.load(std::memory_order_acquire) == seq) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
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
    const auto add = [&first, &more](const std::string& said) {
        if (first.empty()) {
            first = said;
        } else {
            ++more;
        }
    };
    if (newest_take_ == 0) return first;
    for (const FailedFile& f : failed_files_) {
        if (f.take == newest_take_) add(f.what);
    }
    for (const auto& t : tracks_) {
        for (int slot = 0; slot < Track::kWriterSlots; ++slot) {
            // The disk thread owns the writer while the slot holds a take.
            // failed() is an acquire load, so failure() is settled once it
            // is true; path() was set by this thread when it opened the file.
            if (t->slot_take(slot) != newest_take_) continue;
            WavWriter& w = t->writer(slot);
            if (!w.failed()) continue;
            const std::string said = w.path() + ": " + w.failure();
            // Closed and collected, but its slot not yet given back.
            const bool collected =
                std::any_of(failed_files_.begin(), failed_files_.end(),
                            [&said](const FailedFile& f) { return f.what == said; });
            if (!collected) add(said);
        }
    }
    return first.empty() ? first : describe_failures(first, more);
}

bool Session::stop(std::string* error) {
    const uint64_t end = transport_->playhead();
    transport_->set_state(TransportState::Stopped);
    if (!wait_for_callback()) {
        std::fprintf(stderr,
                     "sonora: the audio callback did not return within 1 s of the"
                     " stop; what it still records is dropped\n");
    }
    // Joins the disk thread and drains what is left, close records
    // included. After this line nothing else uses the writers.
    disk_->stop();
    // Files no close record reached: no callback ran after the store above.
    disk_->close_all();
    collect_closed();
    keep_origin(end);
    recording_take_ = 0;
    hold_playback_loop();

    // Every file is closed and collected, so only failed_files_ speaks.
    const std::string why = take_failure();
    failed_files_.clear();
    newest_take_ = 0;
    if (why.empty()) return true;
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

// Where a ramp of `len` frames centred on `at` stands at frame f: 0 before
// it, 1 after. Frame i of the ramp is at i / len, as a fade's is.
float monitor_ramp(uint64_t f, uint64_t at, uint32_t len) noexcept SONORA_RT {
    if (at == PunchRange::kOpenEnd) return 0.0f;
    const int64_t start = static_cast<int64_t>(at) - static_cast<int64_t>(len / 2);
    const auto x = static_cast<int64_t>(f);
    if (x < start) return 0.0f;
    if (x >= start + static_cast<int64_t>(len)) return 1.0f;
    return static_cast<float>(x - start) / static_cast<float>(len);
}

// Auto monitoring under a punch at frame f: 0 playback, 1 input, ramping
// up around A and back down around B.
float punch_position(uint64_t f, uint64_t a, uint64_t b, uint32_t len) noexcept SONORA_RT {
    const float rise = monitor_ramp(f, a, len);
    const float fall = 1.0f - monitor_ramp(f, b, len);
    return rise < fall ? rise : fall;
}

// Whether that ramp changes anywhere in [from, from + n).
bool monitor_ramp_touches(uint64_t at, uint32_t len, uint64_t from,
                          uint32_t n) noexcept SONORA_RT {
    if (at == PunchRange::kOpenEnd) return false;
    const int64_t start = static_cast<int64_t>(at) - static_cast<int64_t>(len / 2);
    const auto first = static_cast<int64_t>(from);
    const int64_t last = first + static_cast<int64_t>(n) - 1;
    return start <= last && start + static_cast<int64_t>(len) >= first;
}

}  // namespace

void Session::process(const backend::AudioBlock& block) noexcept SONORA_RT {
    // Per thread, and a backend may call from a new thread after a
    // reactivation, so it is checked every block.
    const FlushDenormals flush_denormals;
    // Odd from here. Before the transport is read, and seq_cst: with stop()'s
    // seq_cst store of Stopped and its seq_cst load of this count, either
    // this callback loads Stopped or stop() sees it in flight
    // (Transport::load_word). Weakened, both could miss.
    callback_seq_.fetch_add(1, std::memory_order_seq_cst);
    // ADR-0009 §4.1: the block count, then the plan, before anything reads
    // either. A new plan is told by its epoch, never its address.
    const PlaybackPlan* plan = plans_->acquire();
    if (plan->epoch != seen_epoch_) {
        seen_epoch_ = plan->epoch;
        reseek_ = true;
        plan_valid_ = plan_in_bounds(*plan);
        adopt_param_order(*plan);
        snap_mix_ = plan->mix_serial != 0;
        // The monitor ramps too: what an engine track was sending belonged
        // to the track that was on it.
        if (snap_mix_) {
            for (std::size_t e = 0; e < tracks_.size(); ++e) monitor_stamp_[e] = 0;
        }
    }
    ++callbacks_;

    const uint32_t nframes = block.nframes;
    const Transport::Word word = transport_->load_word();
    if (word_hook_ != nullptr) word_hook_(word_hook_context_);
    // ADR-0020 §3: a count-off holds its take, rolling and recording nothing.
    const bool counting = click_->begin_block(word, plan_valid_);
    const bool recording = !counting && word.state == TransportState::Recording;
    const bool rolling = !counting && word.state != TransportState::Stopped;
    // A take is one contiguous span of the timeline. engine/control sends no
    // locate during one, but one it posted just before the take began can
    // still be queued. Once the take has its first frame such a locate is
    // acknowledged, so nothing waits on it, and dropped.
    if (!recording) origin_take_ = kNoWindow;
    // A take numbered as the last one this callback saw, with no Stopped
    // block between them (stop(), a folder switch, the next take: ADR-0018
    // §1), would pass for that take still running and open nothing. stop()
    // closed that take's files, so its windows are dropped here without a
    // Close.
    if (recording && word.start != recording_start_) {
        recording_start_ = word.start;
        if (origin_take_ == word.take) origin_take_ = kNoWindow;
        for (std::size_t i = 0; i < tracks_.size(); ++i) {
            if (window_take_[i] == word.take) window_take_[i] = kNoWindow;
        }
    }
    const bool take_started = recording && origin_take_ == word.take;

    RtCommand cmd{};
    while (commands_->read(&cmd, 1) == 1) {
        // In order with the locates: one posted after a range lands under it.
        if (cmd.kind == RtCommand::Kind::Loop) {
            play_loop_from_ = cmd.frame;
            play_loop_to_ = cmd.to;
            transport_->note_loop();
            continue;
        }
        if (cmd.kind != RtCommand::Kind::Locate) continue;
        if (take_started) {
            locates_dropped_.fetch_add(1, std::memory_order_relaxed);
        } else {
            transport_->set_playhead(cmd.frame);
            reseek_ = true;
        }
        transport_->note_locate();
    }
    // The running take's range is never overwritten, and a drain takes no
    // more than the ring holds, so this callback's records each find a slot
    // other than it, however many takes the control thread started since
    // the word was loaded (PR #81 review, N1, N2).
    const bool punched = recording && word.punched;
    const uint32_t keep = punched ? newest_punch(word.take) : punch_slots_;
    PunchRecord punch{};
    for (uint32_t k = 0; k < punch_slots_ && punches_->read(&punch, 1) == 1; ++k) {
        if (punch_next_ == keep) punch_next_ = punch_next_ + 1 == punch_slots_ ? 0 : punch_next_ + 1;
        punch_table_[punch_next_] = punch;
        punch_stored_[punch_next_] = ++punch_count_;
        punch_next_ = punch_next_ + 1 == punch_slots_ ? 0 : punch_next_ + 1;
    }
    punch_ = &kWholeTake;
    if (punched) {
        const uint32_t slot = newest_punch(word.take);
        if (slot < punch_slots_) {
            punch_ = &punch_table_[slot];
        } else {
            punch_ = &kNoCapture;
            if (!take_started) punches_lost_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // After the take's state is loaded and the locates applied, and before
    // the capture: a runner that stalls here is the callback descheduled
    // across stop() and the next take (test_take_boundaries).
    // insert_chain_runner.h: once per callback, whole callback, or not at
    // all. A zero-frame callback mixes nothing, so it would reach begin()
    // and never end(); it is not a chain block, bypassed or otherwise.
    // The period's start as the backend's server stamped it: the callback
    // reads no clock (contracts/backend, callback_ns). A period it could not
    // stamp carries the last stamp forward by the last period.
    uint64_t callback_ns = block.callback_ns;
    if (callback_ns == 0 && last_callback_ns_ != 0 && cfg_.sample_rate != 0) {
        callback_ns = last_callback_ns_ +
                      static_cast<uint64_t>(last_nframes_) * 1'000'000'000ull / cfg_.sample_rate;
    }
    last_callback_ns_ = callback_ns;
    last_nframes_ = nframes;
    runner_ = nullptr;
    skip_runner_ = nullptr;
    if (plan_valid_ && plan->last_insert_node != kNoNode && nframes > 0) {
        InsertChainRunner* const r = chains_.load(std::memory_order_acquire);
        skip_runner_ = r;
        // A runner with a fixed block runs only callbacks of that size
        // (contracts/plugin: every block is block_frames); in any other
        // period its bound chains play silence, counted, until
        // engine/control has rebuilt the client for the new period.
        const uint32_t fixed = r != nullptr ? r->block_frames() : 0;
        if (r != nullptr && nframes <= max_block_ && (fixed == 0 || nframes == fixed)) {
            runner_ = r;
            runner_->begin(callback_ns);
        } else if (r != nullptr) {
            chain_bypassed_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Every parameter change posted before this callback, at most the
    // ring's capacity: what a producer adds meanwhile waits for the next.
    // Drained only in a callback whose chains run (RT re-audit of #60's
    // merge): a zero-frame callback, one of another size or a plan with no
    // chain leaves them queued for the next that runs, where nothing would
    // re-seed a change dropped here. Behind the changes held for a plan not
    // yet adopted, which were posted first. Sorted by node; what was posted
    // for an earlier plan, or that no node of this plan runs, is dropped,
    // counted, never handed to a node of another plan.
    params_count_ = params_held_;
    if (runner_ != nullptr) {
        params_count_ += static_cast<uint32_t>(param_commands_->read(
            params_.get() + params_held_, param_commands_->capacity() - params_held_));
    }
    gather_params();

    // Meter every input, armed or not, so the UI shows signal before anyone
    // hits record.
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        const uint32_t c = mix_view(*plan, i).input;
        const float* in = c < block.input_count ? block.inputs[c] : nullptr;
        if (!in) continue;
        float peak = 0.0f;
        for (uint32_t f = 0; f < nframes; ++f) {
            const float a = in[f] < 0.0f ? -in[f] : in[f];
            if (a > peak) peak = a;
        }
        tracks_[i]->report_peak(peak);
    }

    // Settled for the block once the locates are applied.
    const uint64_t playhead = transport_->playhead();
    const bool first = recording && !take_started;
    if (first) {
        // Fixed for the take: set_input_latency() refuses while one runs.
        take_latency_ = punch_->loop_to > punch_->loop_from
                            ? 0
                            : input_latency_.load(std::memory_order_relaxed);
    }
    // The input that lines up with timeline frame t arrives at playhead
    // t + L, so the take's timeline range is captured that much later.
    capture_from_ = punch_->capture_from > PunchRange::kOpenEnd - take_latency_
                        ? PunchRange::kOpenEnd
                        : punch_->capture_from + take_latency_;
    capture_to_ = punch_->capture_to > PunchRange::kOpenEnd - take_latency_
                      ? PunchRange::kOpenEnd
                      : punch_->capture_to + take_latency_;
    if (first) {
        origin_take_ = word.take;
        // Its first sample, in timeline frames: the range's start, or this
        // block's if that has passed (capture()). kNoCapture's open end is
        // kNoOrigin, and stays it.
        const uint64_t origin = capture_from_ == PunchRange::kOpenEnd ? kNoOrigin
                                : capture_from_ > playhead ? capture_from_ - take_latency_
                                                           : playhead - take_latency_;
        take_origin_.store(origin, std::memory_order_release);
        loop_wrap_count_ = 0;
    }
    // A loop take's playhead wraps from B to A inside the block (ADR-0016
    // §2), and so does a roll's under the playback loop, marking no wrap.
    // start_loop() and set_loop() hold B - A to at least max_block_ frames,
    // so a callback within that bound wraps at most once; a longer one is
    // followed all the same. Any other take is one contiguous span.
    loop_from_ = 0;
    loop_to_ = 0;
    block_wraps_ = 0;
    if (!recording && rolling && play_loop_to_ > play_loop_from_ && playhead < play_loop_to_) {
        loop_from_ = play_loop_from_;
        loop_to_ = play_loop_to_;
    }
    if (recording && punch_->loop_to > punch_->loop_from && playhead < punch_->loop_to) {
        loop_from_ = punch_->loop_from;
        loop_to_ = punch_->loop_to;
        const uint64_t to_end = loop_to_ - playhead;
        if (to_end <= nframes) {
            block_wrap_offset_ = static_cast<uint32_t>(to_end);
            block_wraps_ = 1 + static_cast<uint32_t>((nframes - to_end) / (loop_to_ - loop_from_));
        }
    }
    // Post-roll (set_roll_end()): decided before the mix, so this block
    // renders as the roll it loaded, cut at the end. Under a playback loop
    // it applies only while the end comes no later than the wrap point: a
    // loop set further on must not stop a roll from ever ending, and an end
    // past B is never reached once the roll wraps (ADR-0016).
    play_until_ = nframes;
    bool roll_stops = false;
    if (word.state == TransportState::Rolling) {
        uint64_t end = roll_end_.load(std::memory_order_acquire);
        if (end != kNoRollEnd && end <= playhead + nframes && (loop_to_ == 0 || end <= loop_to_) &&
            transport_->stop_roll()) {
            play_until_ = end > playhead ? static_cast<uint32_t>(end - playhead) : 0;
            roll_stops = true;
            roll_end_.compare_exchange_strong(end, kNoRollEnd, std::memory_order_acq_rel);
        }
    }
    capture(block, recording, word.take, word.start, playhead, first);
    if (block_wraps_ != 0) {
        loop_wrap_count_ += block_wraps_;
        loop_wraps_.store(loop_wrap_count_, std::memory_order_release);
    }
    // Said every block from here until the control thread ends the take;
    // kWholeTake and kNoCapture end at kOpenEnd, which no block reaches.
    if (punched && playhead + nframes >= capture_to_) {
        punch_passed_.store(word.take, std::memory_order_release);
    }

    // Mix state, read once per block and ramped across it from what the
    // last block ended on (ADR-0009 §4.5). Solo, mute and gain fold into one
    // coefficient per side, so each ramps to and from zero like a fader.
    bool any_solo = false;
    for (std::size_t e = 0; e < tracks_.size(); ++e) any_solo = any_solo || mix_view(*plan, e).solo;
    for (std::size_t e = 0; e < tracks_.size(); ++e) {
        const TrackMixView v = mix_view(*plan, e);
        const float level = v.gain;
        const bool open = !(v.mute || (any_solo && !v.solo && !v.solo_safe));
        const float g = open ? level : 0.0f;
        const float pan = v.pan;
        float l = 0.0f;
        float r = 0.0f;
        pan_gains(pan, tracks_[e]->channels() >= 2, plan->pan_exponent, l, r);
        track_target_[2 * e] = g * l;
        track_target_[2 * e + 1] = g * r;
        track_open_[e] = open ? 1.0f : 0.0f;
        track_level_[e] = level;
        track_pan_[e] = pan;
    }
    if (snap_mix_) {
        snap_mix_ = false;
        for (std::size_t i = 0; i < 2 * tracks_.size(); ++i) track_gain_[i] = track_target_[i];
    }
    bus_target_ = bus_gain_.load(std::memory_order_relaxed);
    // Indices from a plan that is not in bounds are never followed.
    if (plan_valid_) send_targets(*plan);

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
        mix(*plan, block, off, n, rolling, recording, loop_frame(playhead, off));
        reseek_ = false;
        off += n;
    }
    for (std::size_t i = 0; i < 2 * tracks_.size(); ++i) track_gain_[i] = track_target_[i];
    bus_applied_ = bus_target_;
    if (plan_valid_) {
        for (uint32_t ei = 0; ei < plan->edge_count; ++ei) {
            const uint32_t k = plan->edges[ei].send;
            if (k == kNoSend) continue;
            send_gain_[2 * k] = send_target_[2 * k];
            send_gain_[2 * k + 1] = send_target_[2 * k + 1];
        }
    }
    for (uint32_t i = 0; i < 2 * max_buses_; ++i) aux_gain_[i] = aux_target_[i];

    if (plan_valid_) {
        click_->render(*plan, block, rolling, playhead, play_until_, loop_from_, loop_to_);
    }

    params_count_ = 0;
    // After the mix, which it overwrites on its own output while it runs.
    probe_->process(block);

    // A roll that stops on B stays on B rather than wrapping to A.
    const uint64_t next =
        roll_stops ? playhead + play_until_ : rolling ? loop_frame(playhead, play_until_) : playhead;
    if (rolling && loop_to_ != 0 && !roll_stops) {
        transport_->set_playhead(next);
    } else if (rolling) {
        transport_->advance(play_until_);
    }
    expected_playhead_ = next;
    // Release is enough here, not at entry: a stop() whose load reads this
    // value reads it before the next callback's seq_cst entry in the single
    // total order, so that callback's transport load sees Stopped.
    callback_seq_.fetch_add(1, std::memory_order_release);
}

void Session::capture(const backend::AudioBlock& block, bool recording, uint32_t take,
                      uint32_t start, uint64_t playhead, bool first) noexcept SONORA_RT {
    const uint32_t nframes = block.nframes;
    // The frames of this block the take records: its range, cut to the
    // block. Empty before the range and after it.
    const uint64_t end = playhead + nframes;
    const uint64_t from = capture_from_ > playhead ? capture_from_ : playhead;
    const uint64_t to = capture_to_ < end ? capture_to_ : end;
    const bool in_range = recording && from < to;
    // Started too late to record anything (the files open after
    // start_punch() checked the playhead, and the transport rolls on).
    // Without a window no close record would ever reach these files, which
    // stop() alone would then close.
    const bool passed = first && playhead >= capture_to_;
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        Track& t = *tracks_[i];
        uint32_t& window = window_take_[i];
        const bool wanted = in_range && t.rec_armed();
        CaptureBoundary b{};
        b.track = static_cast<uint32_t>(i);
        // Timeline frames are the playhead's less the take's L (ADR-0019 §2).
        b.timeline = playhead > take_latency_ ? playhead - take_latency_ : 0;
        // A take started over another closes it and opens itself on the
        // same sample: nothing lost or repeated between the two files.
        if (window != kNoWindow && (!wanted || window != take)) {
            b.kind = CaptureBoundary::Kind::Close;
            b.take = window;
            b.start = window_start_[i];
            b.at = captured_[i];
            // Lost, the file ends where the next window opens, or at stop().
            if (boundaries_->write(&b, 1) != 1) {
                boundary_overruns_.fetch_add(1, std::memory_order_relaxed);
            }
            window = kNoWindow;
        }
        if (passed && t.rec_armed()) {
            b.kind = CaptureBoundary::Kind::Open;
            b.take = take;
            b.start = start;
            b.at = captured_[i];
            if (boundaries_->write(&b, 1) == 1) {
                b.kind = CaptureBoundary::Kind::Close;
                if (boundaries_->write(&b, 1) != 1) {
                    boundary_overruns_.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                boundary_overruns_.fetch_add(1, std::memory_order_relaxed);
                t.note_overrun();
            }
            continue;
        }
        if (wanted && window == kNoWindow) {
            b.kind = CaptureBoundary::Kind::Open;
            b.take = take;
            b.start = start;
            b.at = captured_[i];
            b.timeline = from - take_latency_;
            if (boundaries_->write(&b, 1) == 1) {
                window = take;
                window_start_[i] = start;
            } else {
                // Tried again next block: the take starts late on this track.
                // If it ends first (punch_out(), a take over it, its range),
                // there is no retry: the slot opened for it stays held, its
                // file empty, and closed_takes() holds the take back, until
                // this track's next open or stop() closes it.
                boundary_overruns_.fetch_add(1, std::memory_order_relaxed);
                t.note_overrun();
            }
        }
        if (window == kNoWindow) continue;

        // Capture is pre-fader by design: the fader is a monitoring decision
        // and must not be baked into the recorded file.
        const uint32_t c = t.input();
        const float* in = c < block.input_count ? block.inputs[c] : nullptr;
        const uint64_t block_start = captured_[i];
        if (in) {
            const auto count = static_cast<std::size_t>(to - from);
            const std::size_t wrote = t.capture_ring().write(in + (from - playhead), count);
            captured_[i] += wrote;
            if (wrote < count) t.note_overrun();
        }
        // After the samples before it, as an Open or a Close is. A loop
        // take captures its whole block, so the sample at the wrap's offset
        // is block_start plus that offset; the one that lines up with A
        // arrives `latency` samples later (ADR-0016 C7).
        for (uint32_t k = 0; k < block_wraps_ && window == take; ++k) {
            b.kind = CaptureBoundary::Kind::Wrap;
            b.take = window;
            b.start = window_start_[i];
            b.ordinal = loop_wrap_count_ + 1 + k;
            b.at = block_start + block_wrap_offset_ +
                   static_cast<uint64_t>(k) * (loop_to_ - loop_from_) + punch_->latency;
            b.timeline = loop_from_;
            // Lost, its place in the take's wrap list stays empty, and
            // engine/control places that pass by loop length (ADR-0016 §5).
            if (boundaries_->write(&b, 1) != 1) {
                boundary_overruns_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // The out point: the file closes at its frame, in band, and the
        // transport rolls on.
        if (to == capture_to_) {
            b.kind = CaptureBoundary::Kind::Close;
            b.take = window;
            b.start = window_start_[i];
            b.at = captured_[i];
            b.timeline = to - take_latency_;
            if (boundaries_->write(&b, 1) != 1) {
                boundary_overruns_.fetch_add(1, std::memory_order_relaxed);
            }
            window = kNoWindow;
        }
    }
}

void Session::adopt_param_order(const PlaybackPlan& plan) noexcept SONORA_RT {
    // The previous plan's entries only: O(chains), never the whole table.
    for (uint32_t o = 0; o < order_chains_; ++o) store_order_[order_store_[o]] = kNoNode;
    order_chains_ = 0;
    if (!plan_valid_) return;
    for (uint32_t ni = 0; ni < plan.node_count; ++ni) {
        const PlanNode& node = plan.nodes[ni];
        node_order_[ni] = kNoNode;
        // Only a node the runner can run takes changes: unbound and silent
        // chains drop theirs, as does a second node naming the same store
        // (a plan the builder never makes).
        if (node.kind != NodeKind::InsertChain || node.channel_slot == kNoChannelSlot ||
            node.channel_slot == kSilentChannelSlot || node.param_store >= max_param_stores_ ||
            store_order_[node.param_store] != kNoNode || order_chains_ >= max_order_) {
            continue;
        }
        store_order_[node.param_store] = order_chains_;
        order_store_[order_chains_] = node.param_store;
        node_order_[ni] = order_chains_++;
    }
}

void Session::gather_params() noexcept SONORA_RT {
    uint32_t dropped = 0;
    for (uint32_t o = 0; o < order_chains_; ++o) order_count_[o] = 0;
    if (runner_ == nullptr) {
        // Nothing was drained (process()): the changes wait in the ring.
        dropped = 0;
    } else {
        for (uint32_t i = 0; i < params_count_; ++i) {
            const RtCommand& c = params_[i];
            // Posted for an earlier plan: its store may name another chain
            // now (PR #77 RT audit M1). For a later one: held, below.
            if (c.epoch != seen_epoch_) {
                if (c.epoch < seen_epoch_) ++dropped;
                continue;
            }
            const uint32_t o = c.store < max_param_stores_ ? store_order_[c.store] : kNoNode;
            if (o == kNoNode) {
                ++dropped;
            } else {
                ++order_count_[o];
            }
        }
        uint32_t at = 0;
        for (uint32_t o = 0; o < order_chains_; ++o) {
            order_first_[o] = at;
            order_pos_[o] = at;
            at += order_count_[o];
        }
        // Stable: each node's changes keep the order they were posted in.
        // Those for a later plan move to the front, in order, never past
        // the one being read.
        uint32_t held = 0;
        for (uint32_t i = 0; i < params_count_; ++i) {
            const RtCommand c = params_[i];
            if (c.epoch != seen_epoch_) {
                if (c.epoch > seen_epoch_) params_[held++] = c;
                continue;
            }
            const uint32_t o = c.store < max_param_stores_ ? store_order_[c.store] : kNoNode;
            if (o != kNoNode) node_params_[order_pos_[o]++] = ChainParamEvent{c.instance, c.param, 0, c.value};
        }
        params_held_ = held;
    }
    if (dropped != 0) {
        params_dropped_.store(params_dropped_.load(std::memory_order_relaxed) + dropped,
                              std::memory_order_relaxed);
    }
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
                const bool silent = node.channel_slot == kSilentChannelSlot;
                if (runner_ != nullptr && node.channel_slot != kNoChannelSlot && !silent) {
                    // runner_ is set only for a callback mixed whole, so
                    // this runs once per node per callback, with the
                    // changes gather_params() sorted to it.
                    const uint32_t o = node_order_[ni];
                    const uint32_t k = o < order_chains_ ? order_count_[o] : 0;
                    runner_->run(node, in, out, n, k != 0 ? node_params_.get() + order_first_[o] : nullptr, k);
                } else if (node.channel_slot == kNoChannelSlot) {
                    // Unbound: laid out with latency 0, so dry is aligned.
                    std::memcpy(L, in[0], n * sizeof(float));
                    std::memcpy(R, in[1], n * sizeof(float));
                } else {
                    // Laid out silent, or bound but not run this callback
                    // (no runner, a callback the runner does not take): the
                    // plan delays every other path by this chain's latency,
                    // so dry would reach the bus that much early (PR #59
                    // re-review). Silence, as for a miss (ADR-0012 §3),
                    // faded by the runner from the node's last sample when
                    // there is one (#68 review item 4).
                    if (skip_runner_ != nullptr) {
                        skip_runner_->skip(node, out, n);
                    } else {
                        std::memset(L, 0, n * sizeof(float));
                        std::memset(R, 0, n * sizeof(float));
                    }
                    std::atomic<uint64_t>& c = silent ? silent_chain_blocks_ : unrun_chain_blocks_;
                    c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
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
            case NodeKind::AuxSum: {
                // Every input is a send (ADR-0022 §5), in the builder's
                // order, which is the summing order.
                std::memset(L, 0, n * sizeof(float));
                std::memset(R, 0, n * sizeof(float));
                for (uint32_t k = 0; k < node.input_edge_count; ++k) {
                    const PlanEdge& edge = plan.edges[node.first_input_edge + k];
                    const PlanNode& from = plan.nodes[edge.from_node];
                    const float* const in[2] = {scratch(from.scratch_buffer, 0),
                                                scratch(from.scratch_buffer, 1)};
                    if (edge.send == kNoSend) {
                        sum_edge(plan, edge, in, out, n);
                        continue;
                    }
                    const bool mono = from.kind != NodeKind::AuxSum &&
                                      plan.tracks[from.track].channels < 2;
                    sum_send(plan, edge, mono, in, out, n);
                }
                continue;
            }
            case NodeKind::AuxFader: {
                const uint32_t src = plan.nodes[plan.edges[node.first_input_edge].from_node].scratch_buffer;
                const float* const in0 = scratch(src, 0);
                const float* const in1 = scratch(src, 1);
                const uint32_t b = node.track;
                const float l0 = aux_gain_[2 * b];
                const float r0 = aux_gain_[2 * b + 1];
                const float dl = aux_target_[2 * b] - l0;
                const float dr = aux_target_[2 * b + 1] - r0;
                for (uint32_t i = 0; i < n; ++i) {
                    const float a = static_cast<float>(block_done_ + i + 1) * inv_block;
                    L[i] = in0[i] * (l0 + dl * a);
                    R[i] = in1[i] * (r0 + dr * a);
                }
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
    const TrackMixView v = mix_view(plan, e);
    const Track::Monitor mon = v.monitor;
    const bool armed = v.armed;
    // Auto: the input while armed and stopped or recording, playback
    // otherwise; in a take, the punch points say where, frame by frame.
    const bool by_punch = mon == Track::Monitor::Auto && armed && rolling && recording;
    const float fixed =
        mon == Track::Monitor::Input || (mon == Track::Monitor::Auto && armed && !rolling)
            ? 1.0f
            : 0.0f;
    const PunchRecord& p = *punch_;
    // A punch with no crossfade still never cuts: it gets the monitor ramp,
    // centred on A and B like any other punch crossfade.
    const uint32_t xf = p.crossfade != 0 ? p.crossfade : cfg_.playback.monitor_ramp_frames;
    const float first =
        by_punch ? punch_position(from, p.monitor_in, p.monitor_out, xf) : fixed;

    float& pos = monitor_pos_[e];
    if (monitor_stamp_[e] == 0 || monitor_stamp_[e] + 1 < callbacks_) pos = first;
    monitor_stamp_[e] = callbacks_;

    const bool steady = !by_punch || !(monitor_ramp_touches(p.monitor_in, xf, from, n) ||
                                       monitor_ramp_touches(p.monitor_out, xf, from, n));
    const float* in = v.input < block.input_count ? block.inputs[v.input] : nullptr;
    if (steady && pos == first && pos == 1.0f) {
        if (in) {
            std::memcpy(out[0], in + offset, n * sizeof(float));
        } else {
            std::memset(out[0], 0, n * sizeof(float));
        }
        return false;
    }
    const bool play = rolling && mon != Track::Monitor::Off;
    if (steady && pos == first && pos == 0.0f) {
        if (play) {
            render_track(plan, node.track, from, n, out);
            cut_at_roll_end(out, offset, n);
            return pt.channels >= 2;
        }
        std::memset(out[0], 0, n * sizeof(float));
        return false;
    }

    // A switch under way: both sources, crossfaded. The position moves at
    // most a ramp's step a frame, so a step in the target (a mode switched,
    // a take started on the fly) becomes a ramp, and a punch's crossfade,
    // never steeper than the step, is followed exactly.
    uint32_t len = cfg_.playback.monitor_ramp_frames;
    if (by_punch && xf != 0 && (len == 0 || xf < len)) len = xf;
    const float step = len == 0 ? 1.0f : 1.0f / static_cast<float>(len);
    const FadeCurve shape = by_punch ? p.shape : FadeCurve::Linear;
    bool stereo = false;
    if (play) {
        render_track(plan, node.track, from, n, out);
        cut_at_roll_end(out, offset, n);
        stereo = pt.channels >= 2;
    } else {
        std::memset(out[0], 0, n * sizeof(float));
        std::memset(out[1], 0, n * sizeof(float));
    }
    for (uint32_t i = 0; i < n; ++i) {
        const float goal =
            by_punch ? punch_position(from + i, p.monitor_in, p.monitor_out, xf) : fixed;
        const float d = goal - pos;
        pos += d > step ? step : (d < -step ? -step : d);
        // Settled at 0 or 1, fade_in_gain returns before any sin or cos:
        // the curve costs only on the ramp's own frames (PR #81 audit, N4).
        const float g_in = fade_in_gain(shape, pos);
        const float g_play = fade_in_gain(shape, 1.0f - pos);
        const float x = in ? in[offset + i] : 0.0f;
        out[0][i] = g_in * x + g_play * out[0][i];
        if (stereo) out[1][i] = g_in * x + g_play * out[1][i];
    }
    return stereo;
}

void Session::cut_at_roll_end(float* const out[2], uint32_t offset,
                              uint32_t n) const noexcept SONORA_RT {
    if (play_until_ >= offset + n) return;
    const uint32_t keep = play_until_ > offset ? play_until_ - offset : 0;
    // Side 1 too, whether or not the track is stereo: a mono track's side 1
    // is never read, so clearing it costs a memset and saves a branch.
    std::memset(out[0] + keep, 0, (n - keep) * sizeof(float));
    std::memset(out[1] + keep, 0, (n - keep) * sizeof(float));
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

uint32_t Session::newest_punch(uint32_t take) const noexcept SONORA_RT {
    uint32_t found = punch_slots_;
    for (uint32_t k = 0; k < punch_slots_; ++k) {
        if (punch_table_[k].take != take || punch_stored_[k] == 0) continue;
        if (found == punch_slots_ || punch_stored_[k] > punch_stored_[found]) found = k;
    }
    return found;
}

Session::TrackMixView Session::mix_view(const PlaybackPlan& plan,
                                        std::size_t e) const noexcept SONORA_RT {
    const Track& t = *tracks_[e];
    TrackMixView v;
    if (plan.mix_serial != 0 && e < plan.track_mix_count && t.mix_serial() != plan.mix_serial) {
        const PlanTrackMix& m = plan.track_mix[e];
        v.gain = m.gain;
        v.pan = m.pan;
        v.mute = m.mute;
        v.solo = m.solo;
        v.solo_safe = m.solo_safe;
        v.armed = m.armed;
        // Read before plan_in_bounds() has judged the plan, so a value no
        // Monitor names is taken as Off rather than trusted.
        v.monitor = m.monitor <= static_cast<uint8_t>(Track::Monitor::Off)
                        ? static_cast<Track::Monitor>(m.monitor)
                        : Track::Monitor::Off;
        v.input = m.input;
        return v;
    }
    v.gain = t.gain();
    v.pan = t.pan();
    v.mute = t.muted();
    v.solo = t.soloed();
    v.solo_safe = t.solo_safe();
    v.armed = t.rec_armed();
    v.monitor = t.monitor();
    v.input = t.input();
    return v;
}

bool Session::plan_in_bounds(const PlaybackPlan& plan) noexcept SONORA_RT {
    ++bounds_pass_;
    // node_order_ has an entry per node. The builder gives every node its
    // own scratch buffer, which implies this; a plan whose nodes share
    // buffers does not, and would index past it (PR #77 RT audit L4).
    if (plan.node_count > scratch_buffers_) return false;
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
            case NodeKind::TrackSource: {
                if (node.track >= plan.track_count) return false;
                // In bounds: every plan track's engine track was checked above.
                uint64_t& seen = source_seen_[plan.tracks[node.track].engine_track];
                if (seen == bounds_pass_) return false;
                seen = bounds_pass_;
                break;
            }
            case NodeKind::TrackFader:
                if (node.track >= plan.track_count) return false;
                if (node.input_edge_count != 1) return false;
                break;
            case NodeKind::InsertChain:
                if (node.input_edge_count != 1) return false;
                if (node.track >= plan.track_count) return false;
                break;
            case NodeKind::AuxSum:
                if (node.track >= max_buses_) return false;
                break;
            case NodeKind::AuxFader:
                if (node.track >= max_buses_) return false;
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
    // Every edge, not only those some node lists: send_targets() walks them
    // all. A send leaves a pre-fader node, whose track says mono or stereo.
    for (uint32_t ei = 0; ei < plan.edge_count; ++ei) {
        const PlanEdge& edge = plan.edges[ei];
        if (edge.send == kNoSend) continue;
        if (edge.send >= send_states_ || edge.from_node >= plan.node_count) return false;
        const NodeKind from = plan.nodes[edge.from_node].kind;
        if (from != NodeKind::TrackSource && from != NodeKind::InsertChain &&
            from != NodeKind::AuxSum) {
            return false;
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

void Session::sum_send(const PlaybackPlan& plan, const PlanEdge& edge, bool mono,
                       const float* const in[2], float* const out[2], uint32_t n) noexcept SONORA_RT {
    const uint32_t k = edge.send;
    const float inv_block = 1.0f / static_cast<float>(block_frames_);
    const float l0 = send_gain_[2 * k];
    const float r0 = send_gain_[2 * k + 1];
    const float dl = send_target_[2 * k] - l0;
    const float dr = send_target_[2 * k + 1] - r0;
    const float* right_in = mono ? in[0] : in[1];
    // track_buf_ is free here: only a TrackOut uses it, within its own node.
    float* const tb[2] = {track_buf_.get(), track_buf_.get() + max_block_};
    for (uint32_t i = 0; i < n; ++i) {
        const float a = static_cast<float>(block_done_ + i + 1) * inv_block;
        tb[0][i] = in[0][i] * (l0 + dl * a);
        tb[1][i] = right_in[i] * (r0 + dr * a);
    }
    const float* const scaled[2] = {tb[0], tb[1]};
    sum_edge(plan, edge, scaled, out, n);
}

void Session::send_targets(const PlaybackPlan& plan) noexcept SONORA_RT {
    const auto tracks_n = static_cast<uint32_t>(tracks_.size());
    for (uint32_t b = 0; b < max_buses_; ++b) {
        const AuxState& a = aux_[b];
        const float gain = a.gain.load(std::memory_order_relaxed);
        const float pan = a.pan.load(std::memory_order_relaxed);
        const bool open = !a.mute.load(std::memory_order_relaxed);
        float l = 0.0f;
        float r = 0.0f;
        pan_gains(pan, true, plan.pan_exponent, l, r);
        aux_open_[b] = open ? 1.0f : 0.0f;
        aux_level_[b] = gain;
        aux_pan_[b] = pan;
        aux_target_[2 * b] = open ? gain * l : 0.0f;
        aux_target_[2 * b + 1] = open ? gain * r : 0.0f;
    }
    for (uint32_t ni = 0; ni < plan.node_count; ++ni) {
        const PlanNode& node = plan.nodes[ni];
        if (node.kind != NodeKind::AuxFader) continue;
        const uint32_t b = node.track;
        if (aux_stamp_[b] + 1 < callbacks_) {
            aux_gain_[2 * b] = 0.0f;
            aux_gain_[2 * b + 1] = 0.0f;
        }
        aux_stamp_[b] = callbacks_;
    }
    for (uint32_t ei = 0; ei < plan.edge_count; ++ei) {
        const uint32_t k = plan.edges[ei].send;
        if (k == kNoSend) continue;
        const uint32_t strip = k / kStripSendSlots;
        const SendState& s = sends_[k];
        const float level = s.level.load(std::memory_order_relaxed);
        const float own_pan = s.pan.load(std::memory_order_relaxed);
        const bool follow = s.follow_pan.load(std::memory_order_relaxed);
        const bool mute = s.mute.load(std::memory_order_relaxed);
        const bool pre = s.pre.load(std::memory_order_relaxed);
        float open = 0.0f;
        float fader = 0.0f;
        float strip_pan = 0.0f;
        bool stereo = true;
        if (strip < tracks_n) {
            open = track_open_[strip];
            fader = track_level_[strip];
            strip_pan = track_pan_[strip];
            stereo = tracks_[strip]->channels() >= 2;
        } else {
            const uint32_t b = strip - tracks_n;
            open = aux_open_[b];
            fader = aux_level_[b];
            strip_pan = aux_pan_[b];
        }
        // Pre-fader is post-insert and pre-pan, and still silenced by its
        // strip's mute, as Pro Tools' default (ADR-0022 §6).
        const float g = mute ? 0.0f : level * open * (pre ? 1.0f : fader);
        float l = 0.0f;
        float r = 0.0f;
        pan_gains(follow ? strip_pan : own_pan, stereo, plan.pan_exponent, l, r);
        send_target_[2 * k] = g * l;
        send_target_[2 * k + 1] = g * r;
        if (send_stamp_[k] + 1 < callbacks_) {
            send_gain_[2 * k] = 0.0f;
            send_gain_[2 * k + 1] = 0.0f;
        }
        send_stamp_[k] = callbacks_;
    }
}

// The segments under [from, from + n), found from a cursor that only moves
// forward while rolling; reset by binary search after a locate, a new plan
// or a jump in the playhead.
uint64_t Session::loop_frame(uint64_t playhead, uint64_t off) const noexcept SONORA_RT {
    const uint64_t f = playhead + off;
    if (loop_to_ <= loop_from_ || f < loop_to_) return f;
    return loop_from_ + (f - loop_to_) % (loop_to_ - loop_from_);
}

void Session::render_track(const PlaybackPlan& plan, uint32_t plan_track, uint64_t from,
                           uint32_t n, float* const out[2]) noexcept SONORA_RT {
    // Split where the playhead wraps: the cursor reseeks at A by
    // itself, since the span after the wrap does not start where it ended.
    uint64_t at = from;
    for (uint32_t done = 0; done < n;) {
        uint32_t span = n - done;
        if (loop_to_ > loop_from_ && at < loop_to_ && at + span > loop_to_) {
            span = static_cast<uint32_t>(loop_to_ - at);
        }
        float* const part[2] = {out[0] + done, out[1] + done};
        render_span(plan, plan_track, at, span, part);
        done += span;
        at += span;
        if (loop_to_ > loop_from_ && at == loop_to_) at = loop_from_;
    }
}

void Session::render_span(const PlaybackPlan& plan, uint32_t plan_track, uint64_t from,
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
