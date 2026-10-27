// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/api/fake_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <limits>
#include <utility>
#include <variant>

#include "sonora/api/wire.h"
#include "sonora/frame_text.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/replication.h"
#include "sonora/model/sequence_file.h"
#include "sonora/model/session_file.h"

namespace sonora::api::testing {

namespace {

static_assert(kInsertSlots == model::kInsertSlots,
              "the state block has a slot for every insert slot the model has");
static_assert(static_cast<uint32_t>(PluginFormat::Lv2) ==
                      static_cast<uint32_t>(model::PluginFormat::Lv2) &&
                  static_cast<uint32_t>(PluginFormat::Vst3) ==
                      static_cast<uint32_t>(model::PluginFormat::Vst3),
              "the catalog and the model spell the formats alike");

std::string number(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
    return buf;
}

const PluginInfo* find_plugin(const std::vector<PluginInfo>& catalog, model::PluginFormat format,
                              const std::string& id) {
    for (const PluginInfo& p : catalog) {
        if (static_cast<uint32_t>(p.format) == static_cast<uint32_t>(format) && p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

const model::TrackEdit* track_by_id(const model::Snapshot& s, model::Id id) {
    for (const auto& t : s.edit->tracks) {
        if (t->id == id) return t.get();
    }
    return nullptr;
}

const model::Insert* find_insert(const model::Snapshot& s, model::Id id,
                                 const model::TrackEdit** on = nullptr) {
    for (const auto& t : s.edit->tracks) {
        for (const model::Insert& in : t->inserts) {
            if (in.id != id) continue;
            if (on != nullptr) *on = t.get();
            return &in;
        }
    }
    return nullptr;
}

// What validation refuses before a host is started (ADR-0012 §7.1): a
// plugin not installed, one the scan refused, one whose channels do not
// map to the track's.
std::string hostable(const std::vector<PluginInfo>& catalog, const model::PluginRef& ref,
                     uint16_t channels) {
    const PluginInfo* p = find_plugin(catalog, ref.format, ref.id);
    if (p == nullptr) return "no plugin " + ref.id + " is installed";
    if (!p->usable) return p->name + " cannot be used: " + p->refusal;
    if (p->inputs != channels || p->outputs != channels) {
        return p->name + " has " + std::to_string(p->inputs) + " in and " +
               std::to_string(p->outputs) + " out; the track has " + std::to_string(channels) +
               (channels == 1 ? " channel" : " channels");
    }
    return {};
}

// Empty when the operation is not an insert operation or can be hosted;
// apply() names whatever else is wrong with it.
std::string insert_refusal(const model::Snapshot& s, const std::vector<PluginInfo>& catalog,
                           model::MemberRef op) {
    if (const auto* a = op.get_if<model::AddInsert>()) {
        const model::TrackEdit* t = track_by_id(s, a->track);
        return t == nullptr ? std::string() : hostable(catalog, a->plugin, t->channels);
    }
    if (const auto* r = op.get_if<model::ReplaceInsert>()) {
        const model::TrackEdit* t = nullptr;
        if (find_insert(s, r->insert, &t) == nullptr) return {};
        return hostable(catalog, r->plugin, t->channels);
    }
    if (const auto* m = op.get_if<model::MoveInsert>()) {
        const model::TrackEdit* from = nullptr;
        const model::Insert* in = find_insert(s, m->insert, &from);
        const model::TrackEdit* to = track_by_id(s, m->to_track);
        if (in == nullptr || to == nullptr || to == from) return {};
        // A missing plugin moves as it is; it is out of the chain anyway.
        if (find_plugin(catalog, in->plugin.format, in->plugin.id) == nullptr) return {};
        return hostable(catalog, in->plugin, to->channels);
    }
    return {};
}

// engine/control's edit_transport_field() and take_punch_refusal():
// one field edited on the authority's document, and a punch range change
// refused while a take runs.
bool edit_transport_field(const std::string& id, model::Transport& tr, uint32_t rate,
                          const std::string& argument, std::string& why) {
    if (id == "transport.punch_selection") {
        if (argument.empty()) {
            why = "no selection to set the punch range from; select a range first";
            return false;
        }
        if (tr.loop_record) {
            why = "loop record is on, and a take cannot both loop and punch; turn Loop Record "
                  "off first";
            return false;
        }
        uint64_t from = 0;
        uint64_t to = 0;
        if (!parse_play_range(argument, from, to, why)) return false;
        tr.punch = {{static_cast<model::Frames>(from), static_cast<model::Frames>(to)}};
        return true;
    }
    if (id == "transport.clear_punch") {
        if (tr.punch[1] <= tr.punch[0]) {
            why = "no punch range to clear";
            return false;
        }
        tr.punch = {{0, 0}};
        return true;
    }
    const bool pre = id == "transport.set_pre_roll";
    if (argument.empty()) {
        why = pre ? "no pre-roll given" : "no post-roll given";
        return false;
    }
    uint64_t frames = 0;
    if (!parse_locate_frame(argument, frames, why)) return false;
    const uint64_t most = static_cast<uint64_t>(model::kMaxRollSeconds) * rate;
    if (frames > most) {
        why = std::string(pre ? "a pre-roll" : "a post-roll") + " goes up to " +
              std::to_string(model::kMaxRollSeconds) + " seconds (" + std::to_string(most) +
              " frames at " + std::to_string(rate) + " Hz); " + argument + " is past that";
        return false;
    }
    (pre ? tr.pre_roll : tr.post_roll) = static_cast<model::Frames>(frames);
    return true;
}

std::string take_punch_refusal(const model::Transport& was, const model::Transport& now) {
    if (now.punch == was.punch) return {};
    return "a take is recording; the punch range changes after Stop";
}

PluginParam param(uint32_t index, const char* key, const char* name, float min, float max,
                  float def, uint32_t flags, const char* unit = "", uint32_t steps = 0,
                  std::vector<std::string> labels = {}) {
    PluginParam p;
    p.index = index;
    p.key = key;
    p.name = name;
    p.min = min;
    p.max = max;
    p.default_value = def;
    p.flags = flags;
    p.unit = unit;
    p.steps = steps;
    p.labels = std::move(labels);
    return p;
}

std::string slot_name(uint8_t slot) { return std::string(1, static_cast<char>('A' + slot)); }

uint64_t steady_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

constexpr double kPi = 3.14159265358979323846;

ModelUpdate to_wire(const model::ReplicationUpdate& u) {
    ModelUpdate m;
    switch (u.kind) {
        case model::ReplicationUpdate::Kind::Op: m.kind = ModelUpdateKind::Op; break;
        case model::ReplicationUpdate::Kind::Undo: m.kind = ModelUpdateKind::Undo; break;
        case model::ReplicationUpdate::Kind::Redo: m.kind = ModelUpdateKind::Redo; break;
        case model::ReplicationUpdate::Kind::Snapshot: m.kind = ModelUpdateKind::Snapshot; break;
        case model::ReplicationUpdate::Kind::SnapshotHeader:
            m.kind = ModelUpdateKind::SnapshotHeader;
            break;
        case model::ReplicationUpdate::Kind::SnapshotPart:
            m.kind = ModelUpdateKind::SnapshotPart;
            break;
    }
    m.part_count = u.part_count;
    m.part_bytes = u.part_bytes;
    m.revision = u.revision;
    m.undo_floor = u.undo_floor;
    m.next_id = u.next_id;
    m.bytes = u.bytes;
    m.label = u.label;
    return m;
}

}  // namespace

std::vector<PluginInfo> fake_catalog() {
    std::vector<PluginInfo> c(4);
    c[0].format = PluginFormat::Lv2;
    c[0].id = "urn:sonora:fake:gain";
    c[0].name = "Fake Gain";
    c[0].vendor = "Sonora";
    c[0].version = "1.0.0";
    c[0].categories = {"Utility"};
    c[0].inputs = c[0].outputs = 1;
    c[0].params = {param(0, "gain", "Gain", -24.0f, 24.0f, 0.0f, kParamAutomatable, "dB"),
                   param(1, "invert", "Invert", 0.0f, 1.0f, 0.0f,
                         kParamAutomatable | kParamToggle, "", 2)};

    c[1].format = PluginFormat::Lv2;
    c[1].id = "urn:sonora:fake:limiter-stereo";
    c[1].name = "Fake Limiter Stereo";
    c[1].vendor = "Sonora";
    c[1].version = "1.0.0";
    c[1].categories = {"Dynamics", "Limiter"};
    c[1].inputs = c[1].outputs = 2;
    c[1].params = {param(0, "ceiling", "Ceiling", -12.0f, 0.0f, -1.0f, kParamAutomatable, "dB"),
                   param(1, "release", "Release", 0.001f, 1.0f, 0.05f,
                         kParamAutomatable | kParamLogarithmic, "s")};

    c[2].format = PluginFormat::Vst3;
    c[2].id = "534F4E4F524146414B45434F4D503031";
    c[2].name = "Fake Compressor";
    c[2].vendor = "Sonora";
    c[2].version = "2.1.0";
    c[2].categories = {"Fx", "Dynamics"};
    c[2].inputs = c[2].outputs = 1;
    c[2].has_editor = true;
    // A Windows build through yabridge (ADR-0017 Y2), so an interface's
    // "Windows (Wine)" tag has something to show against the fake.
    c[2].bridge = PluginBridge::Yabridge;
    c[2].params = {param(0, "0", "Threshold", 0.0f, 1.0f, 0.5f, kParamAutomatable, "dB"),
                   param(1, "1", "Ratio", 0.0f, 1.0f, 0.25f, kParamAutomatable),
                   param(2, "7", "Mode", 0.0f, 1.0f, 0.0f, kParamAutomatable | kParamEnum, "", 3,
                         {"Peak", "RMS", "Feedback"})};

    c[3].format = PluginFormat::Lv2;
    c[3].id = "urn:sonora:fake:refused";
    c[3].name = "Fake Refused";
    c[3].vendor = "Sonora";
    c[3].version = "0.1.0";
    c[3].categories = {"Utility"};
    c[3].inputs = c[3].outputs = 1;
    c[3].usable = false;
    c[3].refusal = "it needs the instance-access feature, which no out-of-process host offers";
    return c;
}

const std::vector<std::string>& FakeEngine::engine_commands() {
    // engine/control's install_session_commands(), less app.quit, which is
    // the interface's own (ADR-0007 §5), and the recovery offer's two,
    // which its server installs. engine/control's test_control fails if
    // the two lists differ.
    static const std::vector<std::string> ids = {
        "track.arm_all",        "track.disarm_all",         "track.toggle_arm",
        "transport.locate",     "transport.play",           "transport.punch",
        "transport.record",     "transport.return_to_zero", "transport.stop",
        "transport.toggle_play", "transport.toggle_record", "transport.toggle_click",
        "transport.toggle_count_off", "session.place_recovered", "session.leave_recovered",
        "transport.toggle_loop", "latency.measure", "latency.cancel", "latency.set",
        "transport.toggle_loop_record", "transport.toggle_cycle",
        "transport.cycle_selection", "transport.punch_selection", "transport.clear_punch",
        "transport.set_pre_roll", "transport.set_post_roll",
    };
    return ids;
}

float FakeEngine::level_at(uint32_t ch, double t) {
    const double base_db = -42.0 + 2.0 * static_cast<double>(ch % 18);
    double db;
    if (ch < 4) {
        const double period = 0.45 + 0.1 * ch;
        const double phase = std::fmod(t + 0.07 * ch, period) / period;
        db = base_db + 24.0 * std::exp(-phase * 9.0) - 6.0;
    } else {
        db = base_db + 6.0 * std::sin(2.0 * kPi * (0.15 + 0.03 * ch) * t + ch);
    }
    if (ch == 9 && std::fmod(t, 7.0) < 0.05) db = 0.4;
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

FakeEngine::FakeEngine() : FakeEngine(Config{}) {}

FakeEngine::FakeEngine(Config config) : cfg_(std::move(config)), block_(cfg_.tracks) {
    if (!cfg_.clock) cfg_.clock = steady_ns;
    hello_.protocol_version = cfg_.protocol_version;
    hello_.state_layout_version = cfg_.state_layout_version;
    hello_.sample_rate = cfg_.sample_rate;
    hello_.buffer_frames = cfg_.buffer_frames;
    hello_.track_count = cfg_.tracks;
    for (uint32_t i = 0; i < cfg_.tracks; ++i) {
        hello_.track_names.push_back("Audio " + std::to_string(i + 1));
    }
    hello_.implemented_commands = engine_commands();
    hello_.capabilities = cfg_.capabilities;
    for (const std::string& id : cfg_.planned_commands) {
        const auto& have = hello_.implemented_commands;
        if (!is_engine_command(id) || std::find(have.begin(), have.end(), id) != have.end()) {
            continue;
        }
        planned_.push_back(id);
        hello_.implemented_commands.push_back(id);
    }
    folder_ = cfg_.folder;
    folder_untitled_ = cfg_.folder_untitled;
    armed_.assign(cfg_.tracks, false);
    gain_.assign(cfg_.tracks, 1.0f);
    muted_.assign(cfg_.tracks, false);
    input_monitor_.assign(cfg_.tracks, false);
    silent_.assign(cfg_.tracks, false);
    scripted_.assign(cfg_.tracks, -1.0f);
    clips_.assign(cfg_.tracks, 0);

    model::SessionSettings settings;
    settings.name = "Untitled";
    settings.session_id = model::new_session_id();
    settings.sample_rate = cfg_.sample_rate;
    document_ = std::make_unique<model::Document>(
        model::new_session_with_tracks(settings, hello_.track_names),
        model::History::kDefaultBudget, cfg_.midi_max_entries);
}

FakeEngine::~FakeEngine() = default;

uint64_t FakeEngine::now() const { return cfg_.clock(); }

uint64_t FakeEngine::playhead() const {
    if (mode_ == TransportState::Stopped) return position_;
    const uint64_t t = now();
    return t <= moved_ns_ ? position_
                          : position_ + (t - moved_ns_) * cfg_.sample_rate / 1'000'000'000ull;
}

bool FakeEngine::alive() const { return !dead_ && now() < death_ns_; }

bool FakeEngine::connect(std::string& error) {
    // The engine's half: the Hello goes out through the codec.
    std::string bytes;
    wire::encode(hello_, bytes);

    // The client's half: versions first, so a mismatch is named even if
    // the rest of a newer Hello no longer parses.
    uint32_t protocol = 0, layout = 0;
    if (!wire::decode_hello_versions(bytes, protocol, layout, error)) return false;
    error = wire::version_mismatch(protocol, layout);
    if (!error.empty()) return false;
    Hello decoded;
    if (!wire::decode(bytes, decoded, error)) return false;
    hello_ = std::move(decoded);

    if (!writer_.init(block_.data(), block_.bytes(), cfg_.tracks, cfg_.tracks,
                      cfg_.publish_period_ns)) {
        error = "the fake engine could not lay out its state block";
        return false;
    }
    start_ns_ = now();
    last_publish_ns_ = start_ns_;
    publish();
    if (!view_.attach(block_.data(), block_.bytes(), error)) return false;
    connected_ = true;

    // A replica starts from the whole document, as after M7.
    model::ReplicationUpdate snap;
    std::string why;
    if (document_->snapshot(snap, why, wire::kMaxModelBytes)) broadcast(snap);
    apply_mix();
    send_catalog();
    offer_recovered();
    if (!folder_.empty()) announce_folder();
    announce_latency();
    return true;
}

const model::Snapshot& FakeEngine::document() const { return document_->current(); }

std::string FakeEngine::take_folder(int32_t take) const {
    const auto it = take_folders_.find(take);
    return it == take_folders_.end() ? std::string() : it->second;
}

void FakeEngine::announce_folder() {
    EngineEvent e;
    e.kind = EventKind::SessionFolder;
    e.text = folder_;
    e.flags = folder_untitled_ ? kFolderUntitled : 0u;
    post_event(e);
}

bool FakeEngine::name_folder(const std::string& file, std::string& why) {
    namespace fs = std::filesystem;
    if (!folder_untitled_ || folder_.empty()) return true;
    const fs::path target = fs::path(file).parent_path().lexically_normal();
    std::error_code ec;
    if (fs::exists(target, ec)) return true;
    // The engine's ActiveFolder::names_on_save(): a folder inside the
    // active one, itself included, is written to, not renamed to.
    const fs::path active = fs::path(folder_).lexically_normal();
    const auto mismatch = std::mismatch(active.begin(), active.end(), target.begin(), target.end());
    if (mismatch.first == active.end() ||
        (mismatch.first->empty() && std::next(mismatch.first) == active.end())) {
        return true;
    }
    for (fs::directory_iterator it(folder_, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".sonora") return true;
    }
    // The engine's words: ActiveFolder::rename().
    if (mode_ == TransportState::Recording) {
        why = "stop recording to name the session; the take is being written to " +
              fs::path(folder_).filename().string() + " (take " + std::to_string(take_) + " is open)";
        return false;
    }
    if (fs::exists(folder_, ec)) {
        fs::rename(folder_, target, ec);
        if (ec) {
            why = "cannot rename " + folder_ + " to " + target.string() + ": " + ec.message();
            return false;
        }
    }
    for (auto& [take, folder] : take_folders_) {
        if (folder == folder_) folder = target.string();
    }
    folder_ = target.string();
    folder_untitled_ = false;
    announce_folder();
    return true;
}

bool FakeEngine::load(const std::string& op, std::string& why) {
    namespace fs = std::filesystem;
    std::string file;
    std::string_view bytes;
    if (!wire::decode_load(op, file, bytes, why)) return false;
    if (!load_refused_.empty()) {
        why = load_refused_;
        return false;
    }
    // The engine's words: Session::folder_change_refusal() after the prefix.
    if (mode_ == TransportState::Recording) {
        why = "stop recording before opening another session: take " + std::to_string(take_) +
              " is open";
        return false;
    }
    std::string folder;
    bool untitled = false;
    if (!file.empty()) {
        const fs::path p(file);
        std::error_code ec;
        if (!p.is_absolute()) {
            why = "a session is opened by its file's absolute path, not \"" + file + "\"";
            return false;
        }
        if (fs::is_directory(p, ec)) {
            // An unsaved recording, as engine/control opens one.
            for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->path().extension() == ".sonora") {
                    why = file + " holds a session file; open " + it->path().string() + " instead";
                    return false;
                }
            }
            // The engine's words: only a session folder, which its first
            // Save would rename.
            const auto spelled = [](const fs::path& d) {
                std::error_code e;
                fs::path c = fs::weakly_canonical(d, e);
                if (e) return fs::path();
                return c.filename().empty() ? c.parent_path() : c;
            };
            const bool of_root = !cfg_.sessions_root.empty() &&
                                 !spelled(p).empty() &&
                                 spelled(p).parent_path() == spelled(cfg_.sessions_root);
            if (!fs::is_directory(p / "Audio Files", ec) && !of_root) {
                why = file + " is not a session folder: it has no \"Audio Files\" folder";
                return false;
            }
            folder = p.lexically_normal().string();
            untitled = true;
        } else if (!fs::is_regular_file(p, ec)) {
            why = "there is no session file at " + file;
            return false;
        } else {
            // common/session_files' session_folder_of(), which this module
            // may not link: a file in "Session File Backups/" is of the
            // folder above.
            fs::path dir = p.parent_path();
            if (dir.filename() == "Session File Backups") dir = dir.parent_path();
            folder = dir.lexically_normal().string();
        }
    }
    // As engine/control: refused, the old session kept, when the new one's
    // Snapshot could not be sent.
    if (!document_->load(bytes, cfg_.sample_rate, why, load_parts_.get(), wire::kMaxModelBytes)) {
        return false;
    }
    if (!folder.empty()) {
        folder_ = folder;
        folder_untitled_ = untitled;
        // A take is numbered from what the folder holds; the fake's hold none.
        take_ = 1;
        announce_folder();
    }
    return true;
}

void FakeEngine::apply_mix() {
    const model::Snapshot& doc = document_->current();
    const auto& tracks = doc.edit->tracks;
    // A track past the document's end keeps its fader and mute, as the real
    // engine's does: its old plan plays it until the swap.
    for (std::size_t i = 0; i < gain_.size() && i < tracks.size(); ++i) {
        const model::TrackMix* m = doc.mix.find(tracks[i]->id);
        if (m == nullptr) continue;
        float db = m->gain_db;
        if (!(db >= model::kMinGainDb)) db = model::kMinGainDb;
        db = std::min(db, model::kMaxGainDb);
        gain_[i] = std::pow(10.0f, db / 20.0f);
        muted_[i] = m->mute;
        input_monitor_[i] =
            m->monitor == model::MonitorMode::Input && tracks[i]->kind == model::TrackKind::Audio;
    }
    uint32_t silent = 0;
    for (std::size_t i = 0; i < silent_.size(); ++i) {
        silent_[i] = i < tracks.size() && tracks[i]->kind != model::TrackKind::Audio;
        if (!silent_[i]) continue;
        ++silent;
        // As the engine: an arm left on it goes, but not during a take.
        if (mode_ != TransportState::Recording) armed_[i] = false;
    }
    // The real engine's notice, said when the count changes
    // (EngineControlServer::show_document).
    if (silent != silent_said_) {
        silent_said_ = silent;
        if (silent > 0) {
            EngineEvent e;
            e.kind = EventKind::Notice;
            e.text = "playback: " + std::to_string(silent) +
                     (silent == 1 ? " MIDI or instrument track plays"
                                  : " MIDI or instrument tracks play") +
                     " nothing yet; this version edits and saves MIDI but does not play it";
            post_event(e);
        }
    }
}

void FakeEngine::send_catalog() {
    PluginCatalog c;
    c.generation = catalog_generation_;
    c.plugins = cfg_.plugins;
    std::string bytes, error;
    if (wire::encode(c, bytes, error)) queue(outbound_, bytes);
}

void FakeEngine::script_catalog(std::vector<PluginInfo> plugins) {
    cfg_.plugins = std::move(plugins);
    ++catalog_generation_;
    if (!connected_) return;
    send_catalog();
    refresh_faults(false);
}

void FakeEngine::script_plugin_latency(const std::string& plugin_id, uint32_t frames) {
    plugin_latency_[plugin_id] = frames;
}

void FakeEngine::script_insert_fault(uint64_t insert, const std::string& why) {
    if (why.empty()) {
        faults_.erase(insert);
    } else {
        faults_[insert] = why;
        ++misses_[insert];
    }
    if (connected_) refresh_faults(false);
}

void FakeEngine::script_editor_change(uint64_t insert, const std::string& key, float value) {
    editor_moved_[insert][key] = value;
    plugin_values_[insert][key] = value;
}

void FakeEngine::script_editor_gesture_end(uint64_t insert) { record_editor_values(insert); }

void FakeEngine::script_editor_closed(uint64_t insert) { record_editor_values(insert); }

void FakeEngine::script_plugin_state(uint64_t insert, const std::string& sha256, uint64_t bytes) {
    plugin_state_[insert] = ScriptedState{sha256, bytes};
}

float FakeEngine::plugin_value(uint64_t insert, const std::string& key) const {
    const auto it = plugin_values_.find(insert);
    if (it == plugin_values_.end()) return std::numeric_limits<float>::quiet_NaN();
    const auto v = it->second.find(key);
    return v == it->second.end() ? std::numeric_limits<float>::quiet_NaN() : v->second;
}

bool FakeEngine::apply_own(const std::string& op, std::string& why) {
    model::ReplicationUpdate u;
    if (!document_->apply(op, u, why)) return false;
    broadcast(to_wire(u));
    return true;
}

void FakeEngine::record_editor_values(uint64_t insert) {
    const auto moved = editor_moved_.find(insert);
    if (moved == editor_moved_.end()) return;
    const model::InsertMix* m = document_->current().mix.find_insert(insert);
    model::SetInsertParams op;
    op.insert = insert;
    if (m != nullptr) op.params = m->params;
    for (const auto& [key, value] : moved->second) {
        bool found = false;
        for (model::ParamValue& p : op.params) {
            if (p.key != key) continue;
            p.value = value;
            found = true;
        }
        if (!found) op.params.push_back(model::ParamValue{key, value});
    }
    editor_moved_.erase(moved);
    std::string bytes, why;
    if (model::encode_operation(op, bytes, why)) apply_own(bytes, why);
}

namespace {

// Through a temporary file and a rename, as the engine writes, unsynced.
bool write_replacing(const std::filesystem::path& target, const std::string& bytes) {
    const std::filesystem::path tmp = target.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) std::filesystem::remove(tmp, ec);
    return !ec;
}

}  // namespace

bool FakeEngine::save(const std::string& op, std::string& why) {
    namespace fs = std::filesystem;
    std::string file, bytes;
    if (!wire::decode_save(op, file, bytes, why)) return false;
    if (!save_failure_.empty()) {
        why = save_failure_;
        return false;
    }
    const fs::path path(file);
    if (!path.is_absolute() || path.extension() != ".sonora") {
        why = "a session file is an absolute path ending in \".sonora\", not \"" + file + "\"";
        return false;
    }
    if (!name_folder(file, why)) return false;
    std::set<std::string> refs;
    std::string parse_why;
    if (!model::sequence_references(bytes, refs, parse_why)) {
        why = "what was to be saved is not a session file: " + parse_why;
        return false;
    }
    model::SequenceDigests digests;
    std::vector<model::NamedSequence> named = model::named_sequences(document_->current(), digests);
    for (const std::string& sha : refs) {
        const bool held = std::any_of(named.begin(), named.end(), [&](const model::NamedSequence& n) {
            return n.ref.sha256 == sha;
        });
        if (!held) {
            why = "the session names MIDI data the engine does not hold (" + sha.substr(0, 12) +
                  "...), perhaps from an edit it refused; save again";
            return false;
        }
    }
    std::error_code ec;
    const fs::path dir = path.parent_path() / "MIDI Sequences";
    fs::create_directories(path.parent_path(), ec);
    for (model::NamedSequence& n : named) {
        if (n.data->missing || refs.count(n.ref.sha256) == 0) continue;
        const fs::path target = dir / (n.ref.sha256 + ".msq");
        if (fs::exists(target, ec)) continue;
        fs::create_directories(dir, ec);
        if (n.encoded.empty()) n.encoded = model::encode_sequence(*n.data);
        if (!write_replacing(target, n.encoded)) {
            why = "writing " + target.string() + " failed";
            return false;
        }
    }
    if (!write_replacing(path, bytes)) {
        why = "saving " + file + " failed";
        return false;
    }
    ++saves_;
    return true;
}

// ADR-0012 §8: values moved in an editor first, then the state, each as
// the operation a replica applies; the caller replies after them.
bool FakeEngine::flush_plugin_state(const std::string& which, std::string& why) {
    std::vector<model::Id> targets;
    const model::Snapshot& s = document_->current();
    if (which.empty()) {
        for (const auto& t : s.edit->tracks) {
            for (const model::Insert& in : t->inserts) targets.push_back(in.id);
        }
    } else {
        model::Id id = 0;
        for (const char ch : which) {
            if (ch < '0' || ch > '9' || id > (UINT64_MAX - 9) / 10) {
                why = "\"" + which + "\" is not an insert id";
                return false;
            }
            id = id * 10 + static_cast<model::Id>(ch - '0');
        }
        if (find_insert(s, id) == nullptr) {
            why = "insert " + which + " is not on any track";
            return false;
        }
        targets.push_back(id);
    }
    std::string down;
    for (const model::Id id : targets) {
        record_editor_values(id);
        const auto st = plugin_state_.find(id);
        if (st == plugin_state_.end()) continue;
        const model::Snapshot& now = document_->current();
        const model::TrackEdit* track = nullptr;
        const model::Insert* in = find_insert(now, id, &track);
        if (in == nullptr) continue;
        if (faults_.count(id) != 0) {
            down += (down.empty() ? "" : "; ") + in->plugin.name + " (" + track->name +
                    ", slot " + slot_name(in->slot) + "): its host is down";
            continue;
        }
        const model::InsertMix* m = now.mix.find_insert(id);
        const model::StateRef ref{model::plugin_state_file(st->second.sha256), st->second.sha256,
                                  st->second.bytes};
        if (m != nullptr && m->state == ref) continue;
        std::string bytes;
        if (!model::encode_operation(model::SetInsertState{id, ref}, bytes, why) ||
            !apply_own(bytes, why)) {
            return false;
        }
    }
    if (!down.empty()) {
        why = "no state from " + down;
        return false;
    }
    return true;
}

// ADR-0012 §6.1 and §8: an active insert whose plugin is not here, or whose
// host is down, has one fault standing; nothing else does.
void FakeEngine::refresh_faults(bool reset) {
    const auto say = [&](uint64_t insert, const std::string& text) {
        EngineEvent e;
        e.kind = EventKind::PluginFault;
        e.insert = insert;
        e.text = text;
        post_event(e);
    };
    if (reset) {
        for (const auto& [insert, text] : said_faults_) say(insert, "");
        said_faults_.clear();
    }
    std::map<uint64_t, std::string> now;
    const model::Snapshot& s = document_->current();
    for (const auto& t : s.edit->tracks) {
        for (const model::Insert& in : t->inserts) {
            if (!in.active) continue;
            const std::string where =
                in.plugin.name + " (" + t->name + ", slot " + slot_name(in.slot) + ")";
            if (find_plugin(cfg_.plugins, in.plugin.format, in.plugin.id) == nullptr) {
                now[in.id] = where + " is not installed here (" + in.plugin.id +
                             "); the insert is kept, out of the chain";
            } else if (const auto f = faults_.find(in.id); f != faults_.end()) {
                now[in.id] = where + ": " + f->second;
            }
        }
    }
    // A host is gone with its insert: a scripted fault does not outlive it.
    for (auto it = faults_.begin(); it != faults_.end();) {
        it = find_insert(s, it->first) == nullptr ? faults_.erase(it) : std::next(it);
    }
    for (auto it = said_faults_.begin(); it != said_faults_.end();) {
        if (now.count(it->first) != 0) {
            ++it;
            continue;
        }
        say(it->first, "");
        it = said_faults_.erase(it);
    }
    for (const auto& [insert, text] : now) {
        const auto was = said_faults_.find(insert);
        if (was != said_faults_.end() && was->second == text) continue;
        said_faults_[insert] = text;
        say(insert, text);
    }
}

bool FakeEngine::plugin_change(const PluginParameterChange& c, std::string& why) {
    const model::Insert* in = find_insert(document_->current(), c.insert);
    if (in == nullptr) {
        why = "insert " + std::to_string(c.insert) + " is not on any track";
        return false;
    }
    const PluginInfo* p = find_plugin(cfg_.plugins, in->plugin.format, in->plugin.id);
    if (p == nullptr) {
        why = in->plugin.name + " is not installed here";
        return false;
    }
    const PluginParam* q = nullptr;
    for (const PluginParam& each : p->params) {
        if (each.key == c.key) q = &each;
    }
    if (q == nullptr) {
        why = p->name + " has no parameter \"" + c.key + "\"";
        return false;
    }
    // Written so that NaN fails it.
    if (!(c.value >= q->min && c.value <= q->max)) {
        why = q->name + " must be from " + number(q->min) + " to " + number(q->max);
        return false;
    }
    plugin_values_[c.insert][c.key] = c.value;
    return true;
}

// One host per chain (ADR-0012 §2): a faulted insert takes its whole chain
// down, and a chain that runs adds one IPC block to its inserts' latency.
void FakeEngine::insert_fields(TrackFields* tracks, uint32_t count, TransportFields& tf) const {
    const model::Snapshot& s = document_->current();
    uint32_t worst = 0;
    for (uint32_t t = 0; t < count && t < s.edit->tracks.size(); ++t) {
        const model::TrackEdit& tr = *s.edit->tracks[t];
        bool down = false;
        for (const model::Insert& in : tr.inserts) {
            down = down || (in.active && faults_.count(in.id) != 0);
        }
        uint32_t chain = 0;
        bool running = false;
        for (const model::Insert& in : tr.inserts) {
            InsertFields& f = tracks[t].inserts[in.slot];
            const model::InsertMix* m = s.mix.find_insert(in.id);
            f.flags = kInsertPresent | (in.active ? kInsertActive : 0u) |
                      (m != nullptr && m->bypass ? kInsertBypassed : 0u);
            const auto miss = misses_.find(in.id);
            f.misses = miss != misses_.end() ? miss->second : 0;
            if (find_plugin(cfg_.plugins, in.plugin.format, in.plugin.id) == nullptr) {
                f.flags |= kInsertMissing;
            } else if (faults_.count(in.id) != 0) {
                f.flags |= kInsertFaulted;
            } else if (in.active && !down) {
                f.flags |= kInsertRunning;
                const auto lat = plugin_latency_.find(in.plugin.id);
                f.latency_frames = lat != plugin_latency_.end() ? lat->second : 0;
                chain += f.latency_frames;
                running = true;
            }
        }
        tracks[t].latency_frames = running ? chain + cfg_.buffer_frames : 0;
        worst = std::max(worst, tracks[t].latency_frames);
    }
    tf.compensation_frames = worst;
}

void FakeEngine::queue(std::deque<std::string>& q, const std::string& message) {
    std::vector<std::string> frames;
    std::string error;
    if (!wire::split_frames(message, frames, error)) return;
    for (std::string& f : frames) q.push_back(std::move(f));
}

void FakeEngine::script_inputs(bool connected, const std::string& why) {
    if (connected == inputs_connected_) return;
    inputs_connected_ = connected;
    health_ = connected ? (health_ & ~kHealthNoInputs) : (health_ | kHealthNoInputs);
    EngineEvent e;
    e.kind = EventKind::InputsMissing;
    e.text = connected ? std::string() : (why.empty() ? "no capture input is connected" : why);
    post_event(e);
}

std::string FakeEngine::take_file(int32_t take) {
    return "Audio 1_" + std::string(take < 10 ? "0" : "") + std::to_string(take) + ".wav";
}

// The engine control thread's check, in the same words.
void FakeEngine::check_take() {
    if (mode_ != TransportState::Recording) return;
    if (fail_takes_ && failed_take_ != take_) {
        failed_take_ = take_;
        take_failed_shown_ = true;
        health_ |= kHealthTakeFailed;
        EngineEvent e;
        e.kind = EventKind::TakeFailed;
        e.frame = static_cast<uint64_t>(take_);
        e.text = "take " + std::to_string(take_) + " failed: " + take_file(take_) +
                 ": no space left on device. Nothing more is being written; stop the take";
        post_event(e);
    } else if (!fail_takes_ && take_ != failed_take_ && take_failed_shown_) {
        take_failed_shown_ = false;
        health_ &= ~kHealthTakeFailed;
        EngineEvent e;
        e.kind = EventKind::TakeFailed;
        e.frame = static_cast<uint64_t>(take_);
        post_event(e);
    }
}

uint64_t FakeEngine::send(const CommandRequest& request) {
    CommandRequest r = request;
    r.seq = ++seq_;
    std::string bytes;
    wire::encode(r, bytes);
    queue(inbound_, bytes);
    return r.seq;
}

uint64_t FakeEngine::send(const ParameterChange& change) {
    ParameterChange c = change;
    c.seq = ++seq_;
    std::string bytes;
    wire::encode(c, bytes);
    queue(inbound_, bytes);
    return c.seq;
}

uint64_t FakeEngine::send(const PluginParameterChange& change) {
    PluginParameterChange c = change;
    c.seq = ++seq_;
    std::string bytes;
    wire::encode(c, bytes);
    queue(inbound_, bytes);
    return c.seq;
}

uint64_t FakeEngine::submit(const ModelRequest& request) {
    ModelRequest m = request;
    m.seq = ++seq_;
    std::string bytes;
    wire::encode(m, bytes);
    queue(inbound_, bytes);
    return m.seq;
}

namespace {

// The track index of `id` in display order; -1 if gone or not audio.
int audio_track_of(const model::Snapshot& doc, uint64_t id) {
    const auto& tracks = doc.edit->tracks;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i]->id == id && tracks[i]->kind == model::TrackKind::Audio) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string fake_seconds(uint64_t frames, uint32_t rate) {
    char text[32];
    std::snprintf(text, sizeof text, "%.3f s", rate ? static_cast<double>(frames) / rate : 0.0);
    return text;
}

}  // namespace

void FakeEngine::offer_recovered() {
    const model::Snapshot& doc = document_->current();
    std::set<std::string> named;
    for (const model::Source& s : doc.sources->sources) named.insert(s.path);
    named.insert(doc.settings.left_takes.begin(), doc.settings.left_takes.end());
    offered_.clear();
    std::string text;
    for (const RecoveredTake& r : recovered_) {
        if (r.folder != folder_ || named.count(r.path) != 0 || r.frames == 0) continue;
        offered_.push_back(r);
        const std::string name = r.path.substr(r.path.rfind('/') + 1);
        if (!text.empty()) text += "\n";
        text += name + ": take " + std::to_string(r.take);
        const int t = audio_track_of(doc, r.track_id);
        if (t < 0) {
            text += ", " + fake_seconds(r.frames, cfg_.sample_rate) +
                    ", into the audio pool only: its track is not in this session";
        } else {
            text += " on \"" + doc.edit->tracks[static_cast<std::size_t>(t)]->name + "\" at " +
                    fake_seconds(r.position, cfg_.sample_rate) + ", " +
                    fake_seconds(r.frames, cfg_.sample_rate) + " long";
        }
    }
    if (offered_.empty()) return withdraw_recovered();
    EngineEvent e;
    e.kind = EventKind::RecoveryOffer;
    e.frame = offered_.size();
    e.text = text;
    offer_standing_ = true;
    post_event(e);
}

void FakeEngine::withdraw_recovered() {
    offered_.clear();
    if (!offer_standing_) return;
    offer_standing_ = false;
    EngineEvent e;
    e.kind = EventKind::RecoveryOffer;
    post_event(e);
}

bool FakeEngine::place_recovered(std::string& why) {
    if (offered_.empty()) {
        why = "no recovered takes are waiting to be placed";
        return false;
    }
    if (mode_ == TransportState::Recording) {
        why = "recovered takes are placed while stopped; stop the take first";
        return false;
    }
    // One step per take, as the engine's add_closed_take() makes them.
    std::map<int32_t, model::Compound> steps;
    std::vector<model::Member> pool;
    model::Id next = document_->current().next_id;
    for (const RecoveredTake& r : offered_) {
        const model::Snapshot& doc = document_->current();
        model::AddSource a;
        a.source.id = next++;
        a.source.path = r.path;
        a.source.channels = r.channels;
        a.source.length = static_cast<model::Frames>(r.frames);
        a.source.time_reference = static_cast<int64_t>(r.position);
        a.source.kind = model::SourceKind::Recorded;
        a.source.take = r.take;
        const int t = audio_track_of(doc, r.track_id);
        if (t < 0 || r.channels < doc.edit->tracks[static_cast<std::size_t>(t)]->channels) {
            pool.push_back(a);
            continue;
        }
        const model::TrackEdit& track = *doc.edit->tracks[static_cast<std::size_t>(t)];
        a.source.recorded_on_track = track.id;
        model::RecordTake rec;
        rec.track = track.id;
        rec.playlist = track.active_playlist;
        rec.region.id = next++;
        const std::string name = r.path.substr(r.path.rfind('/') + 1);
        rec.region.name = name.substr(0, name.rfind('.'));
        rec.region.position = static_cast<model::Frames>(r.position);
        rec.region.length = static_cast<model::Frames>(r.frames);
        for (uint16_t c = 0; c < track.channels; ++c) {
            rec.region.channels.push_back({a.source.id, c});
        }
        rec.split_right_id = next++;
        model::Compound& step = steps[r.take];
        step.label = "Place Recovered Takes";
        step.ops.push_back(a);
        step.ops.push_back(rec);
    }
    if (!pool.empty()) {
        model::Compound c;
        c.label = "Place Recovered Takes";
        c.ops = std::move(pool);
        steps[INT32_MAX] = std::move(c);
    }
    for (auto& [take, step] : steps) {
        model::ReplicationUpdate u;
        std::string refused;
        if (document_->apply(model::Operation{std::move(step)}, u, refused)) {
            broadcast(u);
        } else {
            EngineEvent e;
            e.kind = EventKind::Notice;
            e.text = "recovered take " + std::to_string(take) + " could not be placed: " + refused;
            post_event(e);
        }
    }
    withdraw_recovered();
    return true;
}

void FakeEngine::post_event(const EngineEvent& event) {
    std::string bytes;
    wire::encode(event, bytes);
    queue(outbound_, bytes);
}

void FakeEngine::script_peak(uint32_t track, float peak) {
    if (track < scripted_.size()) scripted_[track] = peak;
}

void FakeEngine::reply(uint64_t seq, DispatchResult result, std::string message) {
    CommandReply r;
    r.seq = seq;
    r.result = result;
    r.message = std::move(message);
    std::string bytes;
    wire::encode(r, bytes);
    queue(outbound_, bytes);
}

void FakeEngine::broadcast(const ModelUpdate& u) {
    std::string bytes;
    wire::encode(u, bytes);
    queue(outbound_, bytes);
}

void FakeEngine::broadcast(const model::ReplicationUpdate& u) {
    if (u.kind != model::ReplicationUpdate::Kind::Snapshot) {
        broadcast(to_wire(u));
        return;
    }
    // As engine/control sends it: header, parts, the window last.
    ModelUpdate header = to_wire(u);
    header.kind = ModelUpdateKind::SnapshotHeader;
    header.bytes.clear();
    header.part_count = u.parts.size();
    header.part_bytes = 0;
    for (const std::string& p : u.parts) header.part_bytes += p.size();
    if (!u.parts.empty()) broadcast(header);
    for (const std::string& p : u.parts) {
        ModelUpdate part;
        part.kind = ModelUpdateKind::SnapshotPart;
        part.revision = u.revision;
        part.undo_floor = u.undo_floor;
        part.bytes = p;
        broadcast(part);
    }
    broadcast(to_wire(u));
}

void FakeEngine::poll(Inbox& inbox) {
    if (!connected_ || !alive()) {
        // A dead engine's requests go nowhere, and nothing comes back.
        inbound_.clear();
        return;
    }
    std::string bytes, error;
    while (!inbound_.empty()) {
        const std::string frame = std::move(inbound_.front());
        inbound_.pop_front();
        if (engine_side_.add(frame, bytes, error) == wire::Reassembler::Result::Complete) {
            handle(bytes);
        }
    }
    check_take();
    check_latency();
    publish();

    // The client's half: everything back through the codec.
    while (!outbound_.empty()) {
        const std::string frame = std::move(outbound_.front());
        outbound_.pop_front();
        if (client_side_.add(frame, bytes, error) != wire::Reassembler::Result::Complete) {
            continue;
        }
        wire::decode_into(bytes, inbox, error);
    }
}

void FakeEngine::handle(const std::string& bytes) {
    wire::Type type{};
    std::string error;
    if (!wire::peek_type(bytes, type)) return;
    if (type == wire::Type::CommandRequest) {
        CommandRequest r;
        if (!wire::decode(bytes, r, error)) return;
        if (find_command(r.command) == nullptr) {
            reply(r.seq, DispatchResult::UnknownCommand, "");
            return;
        }
        if (!is_engine_command(r.command)) {
            reply(r.seq, DispatchResult::NotImplemented,
                  r.command + " is the interface's own command; the engine does not run it");
            return;
        }
        if (std::find(planned_.begin(), planned_.end(), r.command) != planned_.end()) {
            planned_log_.push_back({r.command, r.target});
            reply(r.seq, DispatchResult::Ok, "");
            return;
        }
        const auto& ids = engine_commands();
        if (std::find(ids.begin(), ids.end(), r.command) == ids.end()) {
            reply(r.seq, DispatchResult::NotImplemented, "");
            return;
        }
        std::string why;
        const bool ok = command(r, why);
        reply(r.seq, ok ? DispatchResult::Ok : DispatchResult::Failed, ok ? "" : why);
    } else if (type == wire::Type::ParameterChange) {
        ParameterChange c;
        if (!wire::decode(bytes, c, error)) return;
        if (c.track >= cfg_.tracks) {
            reply(c.seq, DispatchResult::Failed,
                  "there is no track " + std::to_string(c.track + 1));
        } else if (!(c.value >= 0.0f && c.value <= 4.0f)) {
            reply(c.seq, DispatchResult::Failed, "gain must be from 0 to 4 (linear)");
        } else {
            gain_[c.track] = c.value;
            reply(c.seq, DispatchResult::Ok, "");
        }
    } else if (type == wire::Type::PluginParameterChange) {
        PluginParameterChange c;
        if (!wire::decode(bytes, c, error)) return;
        std::string why;
        const bool ok = plugin_change(c, why);
        reply(c.seq, ok ? DispatchResult::Ok : DispatchResult::Failed, ok ? "" : why);
    } else if (type == wire::Type::ModelRequest) {
        ModelRequest m;
        if (!wire::decode(bytes, m, error)) return;
        model::ReplicationUpdate u;
        std::string why, snapshot_failed;
        bool ok = false;
        // The real engine's rule: faders are written from the document only
        // when an update changed what they hold, so an unrelated edit leaves
        // a ride where it is.
        bool mix_changed = m.action == ModelAction::Resync || m.action == ModelAction::Load;
        std::vector<model::Id> order_before;
        for (const auto& t : document_->current().edit->tracks) order_before.push_back(t->id);
        const model::MixState mix_before = document_->current().mix;
        switch (m.action) {
            case ModelAction::Apply: {
                model::Operation op;
                if (!model::decode_operation(m.op, op, why)) break;
                // Per member, on the state the members before it made, as
                // engine/control asks (ADR-0009 update 2026-10-14).
                const model::MemberCheck check = [&](const model::Snapshot& at,
                                                     model::MemberRef member) {
                    if (member.holds<model::SetMix>() || member.holds<model::SetMonitorBus>()) {
                        mix_changed = true;
                    }
                    const auto* set = member.get_if<model::SetSettings>();
                    if (set != nullptr && mode_ == TransportState::Recording) {
                        std::string refusal = take_punch_refusal(
                            at.settings.transport, set->settings.transport);
                        if (!refusal.empty()) return refusal;
                    }
                    return insert_refusal(at, cfg_.plugins, member);
                };
                ok = document_->apply(op, u, why, check);
                break;
            }
            case ModelAction::Undo: ok = document_->undo(u, why); break;
            case ModelAction::Redo: ok = document_->redo(u, why); break;
            case ModelAction::Resync:
                ok = document_->snapshot(u, why, wire::kMaxModelBytes);
                break;
            case ModelAction::Load:
                // Accepted, its Snapshot always encodes, and a failure there
                // is only the backstop's Notice.
                ok = load(m.op, why);
                load_parts_.reset();
                load_refused_.clear();
                if (ok && !document_->snapshot(u, snapshot_failed, wire::kMaxModelBytes)) {
                    snapshot_failed = "the engine could not send its document: " + snapshot_failed;
                }
                break;
            case ModelAction::LoadPart: {
                if (!load_parts_) {
                    load_parts_ = std::make_unique<model::LoadParts>(cfg_.midi_max_entries);
                }
                ok = load_refused_.empty() && load_parts_->add(m.op, why);
                if (!ok && load_refused_.empty()) {
                    load_refused_ = "a part of the session was refused: " + why;
                }
                if (!load_refused_.empty()) why = load_refused_;
                reply(m.seq, ok ? DispatchResult::Ok : DispatchResult::Failed, ok ? "" : why);
                return;
            }
            case ModelAction::FlushPluginState:
                // Its updates are sent as it makes them, before this reply.
                ok = flush_plugin_state(m.op, why);
                reply(m.seq, ok ? DispatchResult::Ok : DispatchResult::Failed, ok ? "" : why);
                return;
            case ModelAction::Save:
                ok = save(m.op, why);
                reply(m.seq, ok ? DispatchResult::Ok : DispatchResult::Failed, ok ? "" : why);
                return;
        }
        reply(m.seq, ok ? DispatchResult::Ok : DispatchResult::Failed, ok ? "" : why);
        if (!ok) return;
        if (snapshot_failed.empty()) {
            broadcast(u);
        } else {
            EngineEvent e;
            e.kind = EventKind::Notice;
            e.text = snapshot_failed;
            post_event(e);
        }
        std::vector<model::Id> order_after;
        for (const auto& t : document_->current().edit->tracks) order_after.push_back(t->id);
        const model::MixState& mix_after = document_->current().mix;
        if (mix_changed || !(mix_after.bus == mix_before.bus) ||
            !(mix_after.tracks == mix_before.tracks) || order_after != order_before) {
            apply_mix();
        }
        if (m.action == ModelAction::Load) {
            // Another session, other hosts: nothing scripted for the old
            // inserts belongs to it, even where an id is the same.
            editor_moved_.clear();
            plugin_state_.clear();
            plugin_values_.clear();
            faults_.clear();
            misses_.clear();
            // The last session's offer is not this one's.
            withdraw_recovered();
            offer_recovered();
        }
        if (m.action != ModelAction::Resync) refresh_faults(m.action == ModelAction::Load);
    }
}

// The same handlers, refusals and words as engine/control's
// install_session_commands(); the conformance suite holds both to them.
bool FakeEngine::command(const CommandRequest& r, std::string& why) {
    const bool recording = mode_ == TransportState::Recording;
    // The transport moves from where the playhead stands now.
    const auto move = [&](TransportState to) {
        position_ = playhead();
        moved_ns_ = now();
        mode_ = to;
    };
    const auto start = [&]() {
        if (measuring_) {
            why = "a latency measurement is running; cancel it or let it finish before recording";
            return false;
        }
        if (std::find(armed_.begin(), armed_.end(), true) == armed_.end()) {
            why = "no track is armed";
            return false;
        }
        // A punch range set keeps the take inside it, as the real engine's
        // start_punch() does: one that ends at or before the playhead would
        // record only handle, or outside the range PUNCH shows lit.
        const auto& range = document_->current().settings.transport.punch;
        const uint64_t at = playhead();
        if (range[0] >= 0 && range[1] > range[0] && static_cast<uint64_t>(range[1]) <= at) {
            why = "the punch range ends at " + frame_text(static_cast<uint64_t>(range[1])) +
                  ", before the playhead at " + frame_text(at);
            return false;
        }
        move(TransportState::Recording);
        take_folders_[take_] = folder_;
        return true;
    };
    // Failed reports the take, not the stop, as the real one does.
    const auto finish = [&]() {
        move(TransportState::Stopped);
        const bool failed = failed_take_ == take_ && take_failed_shown_;
        if (failed) why = take_file(take_) + ": no space left on device";
        ++take_;
        return !failed;
    };
    // A roll is not a take: stopping it closes no file and uses no number.
    const auto stop = [&]() {
        if (recording) return finish();
        if (mode_ == TransportState::Rolling) move(TransportState::Stopped);
        return true;
    };
    const auto play = [&]() {
        if (mode_ == TransportState::Stopped) move(TransportState::Rolling);
        return true;
    };
    const std::string& id = r.command;
    if (id.compare(0, 8, "latency.") == 0) return latency_command(r, why);
    if (id == "session.place_recovered") return place_recovered(why);
    if (id == "session.leave_recovered") {
        if (offered_.empty()) {
            why = "no recovered takes are waiting to be placed";
            return false;
        }
        model::SetSettings op{document_->current().settings};
        std::set<std::string> left(op.settings.left_takes.begin(), op.settings.left_takes.end());
        for (const RecoveredTake& t : offered_) left.insert(t.path);
        if (left.size() != op.settings.left_takes.size()) {
            op.settings.left_takes.assign(left.begin(), left.end());
            model::ReplicationUpdate u;
            if (!document_->apply(model::Operation{std::move(op)}, u, why)) {
                why = "the takes could not be remembered as left: " + why;
                return false;
            }
            broadcast(u);
        }
        withdraw_recovered();
        return true;
    }
    if (id == "transport.toggle_record") return recording ? finish() : start();
    if (id == "transport.record") return recording ? true : start();
    if (id == "transport.punch") {
        if (!recording) return start();
        move(TransportState::Rolling);
        ++take_;
        return true;
    }
    if (id == "transport.stop") return stop();
    if (id == "transport.toggle_click") {
        transport_flags_ ^= kTransportClick;
        return true;
    }
    if (id == "transport.toggle_count_off") {
        transport_flags_ ^= kTransportCountOff;
        return true;
    }
    // As the engine's: the selection becomes the range when loop playback
    // or loop record turns on, and turning either off keeps the range;
    // Cycle sets both from loop playback's and takes the selection only
    // when no range is set, refusing with neither; cycle_selection always
    // takes it and turns both on.
    if (id == "transport.toggle_loop" || id == "transport.toggle_loop_record" ||
        id == "transport.toggle_cycle" || id == "transport.cycle_selection") {
        model::SetSettings op{document_->current().settings};
        model::Transport& tr = op.settings.transport;
        const bool cycle = id == "transport.toggle_cycle";
        bool take = !r.argument.empty();
        if (id == "transport.cycle_selection") {
            if (!take) {
                why = "no selection to set the cycle area from";
                return false;
            }
            tr.loop_playback = tr.loop_record = true;
        } else {
            bool& mode = id == "transport.toggle_loop_record" ? tr.loop_record : tr.loop_playback;
            mode = !mode;
            if (cycle) tr.loop_record = tr.loop_playback;
            take = take && mode && (!cycle || tr.loop[1] <= tr.loop[0]);
            if (cycle && mode && tr.loop[1] <= tr.loop[0] && !take) {
                why = "no cycle area: select a range first";
                return false;
            }
        }
        if (take) {
            uint64_t from = 0;
            uint64_t to = 0;
            if (!parse_play_range(r.argument, from, to, why)) return false;
            tr.loop = {{static_cast<model::Frames>(from), static_cast<model::Frames>(to)}};
        }
        model::ReplicationUpdate u;
        if (!document_->apply(model::Operation{std::move(op)}, u, why)) return false;
        broadcast(u);
        return true;
    }
    if (id == "transport.punch_selection" || id == "transport.clear_punch" ||
        id == "transport.set_pre_roll" || id == "transport.set_post_roll") {
        const model::SessionSettings& was = document_->current().settings;
        model::SetSettings op{was};
        if (!edit_transport_field(id, op.settings.transport, op.settings.sample_rate, r.argument,
                                  why)) {
            return false;
        }
        if (recording) {
            why = take_punch_refusal(was.transport, op.settings.transport);
            if (!why.empty()) return false;
        }
        model::ReplicationUpdate u;
        if (!document_->apply(model::Operation{std::move(op)}, u, why)) return false;
        broadcast(u);
        return true;
    }
    const auto locate = [&](uint64_t frame) {
        if (recording) {
            why = "locate during a take is not supported yet; stop first";
            return false;
        }
        position_ = frame;
        moved_ns_ = now();
        return true;
    };
    // A range is checked as the engine checks it and rolls from its start;
    // the fake keeps no pre-roll or post-roll, so it neither starts earlier
    // nor stops by itself.
    const auto roll = [&]() {
        if (r.argument.empty()) return play();
        uint64_t from = 0;
        uint64_t to = 0;
        if (!parse_play_range(r.argument, from, to, why)) return false;
        return locate(from) && play();
    };
    if (id == "transport.play") return mode_ == TransportState::Stopped ? roll() : true;
    if (id == "transport.toggle_play") return mode_ == TransportState::Stopped ? roll() : stop();
    if (id == "transport.return_to_zero") return locate(0);
    if (id == "transport.locate") {
        uint64_t frame = 0;
        if (!parse_locate_frame(r.argument, frame, why)) return false;
        return locate(frame);
    }

    const auto take_running = [&]() {
        if (recording) why = "arming cannot change during a take; stop first";
        return recording;
    };
    if (id == "track.toggle_arm") {
        if (r.target < 0) {
            why = "no track given, and there is no selection yet";
            return false;
        }
        const auto idx = static_cast<std::size_t>(r.target);
        if (idx >= armed_.size()) {
            why = "there is no track " + std::to_string(idx + 1);
            return false;
        }
        if (take_running()) return false;
        // As the engine (may_arm_track): a MIDI or instrument track records
        // no audio in this version.
        if (!armed_[idx] && silent_[idx]) {
            const model::TrackEdit& t = *document_->current().edit->tracks[idx];
            why = "\"" + t.name + "\" is a " +
                  (t.kind == model::TrackKind::Midi ? "MIDI" : "instrument") +
                  " track; this version records audio only";
            return false;
        }
        armed_[idx] = !armed_[idx];
        return true;
    }
    if (id == "track.arm_all" || id == "track.disarm_all") {
        if (take_running()) return false;
        for (std::size_t i = 0; i < armed_.size(); ++i) {
            armed_[i] = id == "track.arm_all" && !silent_[i];
        }
        return true;
    }
    why = "not handled";
    return false;
}

void FakeEngine::announce_latency() {
    EngineEvent e;
    e.kind = EventKind::Latency;
    e.frame = latency_;
    e.insert = cfg_.latency_outputs;
    e.flags = latency_flags(latency_source_, latency_status_);
    e.text = latency_text_;
    post_event(e);
}

// As engine/control's handlers, in the same words; the probe itself is
// LatencyProbe's, held to engine/core's tests.
bool FakeEngine::latency_command(const CommandRequest& r, std::string& why) {
    const auto fixed = [&]() {
        if (mode_ != TransportState::Recording) return false;
        why = "the round trip cannot change during a take; stop the take first";
        return true;
    };
    if (r.command == "latency.measure") {
        if (fixed()) return false;
        if (measuring_) {
            why = "a latency measurement is already running; cancel it first";
            return false;
        }
        uint32_t output = 0;
        uint32_t input = 0;
        if (!parse_latency_ports(r.argument, output, input, why)) return false;
        measuring_ = true;
        cancel_asked_ = false;
        measure_output_ = output;
        measure_input_ = input;
        measure_start_ns_ = now();
        pings_said_ = 0;
        latency_status_ = LatencyStatus::Measuring;
        latency_text_ = "measuring out_" + std::to_string(output) + " to in_" +
                        std::to_string(input) + ": 0 of 3 pings";
        announce_latency();
        return true;
    }
    if (r.command == "latency.cancel") {
        if (!measuring_) {
            why = "no latency measurement is running";
            return false;
        }
        cancel_asked_ = true;
        return true;
    }
    if (r.command == "latency.set") {
        if (fixed()) return false;
        if (measuring_) {
            why = "a latency measurement is running; cancel it to set the round trip by hand";
            return false;
        }
        uint64_t frames = 0;
        if (!parse_locate_frame(r.argument, frames, why)) return false;
        if (frames > cfg_.sample_rate) {
            why = "a round trip of " + std::to_string(frames) +
                  " frames is over one second, longer than any interface's";
            return false;
        }
        latency_ = static_cast<uint32_t>(frames);
        latency_source_ = LatencySource::Manual;
        announce_latency();
        return true;
    }
    why = "not handled";
    return false;
}

void FakeEngine::check_latency() {
    if (!measuring_) return;
    const auto finish = [&](LatencyStatus status, std::string text) {
        measuring_ = false;
        latency_status_ = status;
        latency_text_ = std::move(text);
        announce_latency();
    };
    if (cancel_asked_) {
        finish(LatencyStatus::Cancelled, "cancelled");
        return;
    }
    if (measure_output_ > cfg_.latency_outputs || measure_input_ > cfg_.tracks) {
        finish(LatencyStatus::NoPort, "the probe's output or input is not one the backend has");
        return;
    }
    const uint64_t elapsed = now() - measure_start_ns_;
    if (elapsed < cfg_.measure_ns) {
        const uint32_t heard =
            static_cast<uint32_t>(3 * elapsed / std::max<uint64_t>(cfg_.measure_ns, 1));
        if (heard != pings_said_) {
            pings_said_ = heard;
            latency_text_ = "measuring out_" + std::to_string(measure_output_) + " to in_" +
                            std::to_string(measure_input_) + ": " + std::to_string(heard) +
                            " of 3 pings";
            announce_latency();
        }
        return;
    }
    const Config::LatencyCable& cable = cfg_.latency_cable;
    if (cable.output == 0 || cable.output != measure_output_ || cable.input != measure_input_) {
        finish(LatencyStatus::NoSignal,
               "no arrival heard (arrival silence, floor silence): is the output cabled to "
               "the input, and the input's gain up?");
        return;
    }
    char ms[32];
    std::snprintf(ms, sizeof ms, "%.2f ms",
                  cfg_.sample_rate ? 1000.0 * cable.frames / cfg_.sample_rate : 0.0);
    std::string text = "round trip " + std::to_string(cable.frames) + " frames (" + ms +
                       "), 3 pings within 0 frame(s), arrival -20.0 dBFS, floor silence";
    if (mode_ == TransportState::Recording) {
        text += "; not applied: the round-trip latency cannot change during a take";
    } else {
        latency_ = cable.frames;
        latency_source_ = LatencySource::Measured;
    }
    finish(LatencyStatus::Measured, std::move(text));
}

void FakeEngine::publish() {
    if (!alive()) return;
    const uint64_t t1 = now();
    const uint64_t t0 = std::max(last_publish_ns_, t1 >= 1'000'000'000 ? t1 - 1'000'000'000 : 0);
    last_publish_ns_ = t1;

    TransportFields tf;
    tf.state = mode_;
    tf.take = take_;
    tf.playhead = playhead();
    tf.xruns = xruns_;
    tf.clock_steps = clock_steps_;
    tf.health_flags = health_;
    tf.inputs_connected = inputs_connected_ ? cfg_.tracks : 0;
    tf.buffer_frames = cfg_.buffer_frames;
    tf.transport_flags = transport_flags_;

    std::vector<TrackFields> tracks(cfg_.tracks);
    std::vector<float> peaks(cfg_.tracks, 0.0f);
    const double a = static_cast<double>(t0 - std::min(t0, start_ns_)) * 1e-9;
    const double b = static_cast<double>(t1 - std::min(t1, start_ns_)) * 1e-9;
    for (uint32_t i = 0; i < cfg_.tracks; ++i) {
        float peak = 0.0f;
        if (scripted_[i] >= 0.0f) {
            peak = scripted_[i];
        } else if (cfg_.signal && !silent_[i]) {
            // The engine's peak is the maximum of every sample since the
            // last publish; the envelope sampled each millisecond is close
            // enough, and a transient between two publishes still shows.
            for (double t = a; t <= b; t += 0.001) peak = std::max(peak, level_at(i, t));
            peak = std::max(peak, level_at(i, b));
        }
        if (peak >= 1.0f) ++clips_[i];
        peaks[i] = peak;
        tracks[i].flags =
            (armed_[i] ? kTrackArmed : 0u) | (muted_[i] ? kTrackMuted : 0u) |
            (input_monitor_[i] ? kTrackInputMonitor : 0u) |
            (inputs_connected_ ? kTrackInputConnected : 0u);
        tracks[i].gain = gain_[i];
        tracks[i].clip_count = clips_[i];
    }
    insert_fields(tracks.data(), cfg_.tracks, tf);
    writer_.publish(tf, tracks.data(), peaks.data());
}

}  // namespace sonora::api::testing
