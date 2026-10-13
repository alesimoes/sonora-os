// SPDX-License-Identifier: GPL-3.0-or-later
#include "lv2_host.h"

#include "ipc.h"

#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/parameters/parameters.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace sonora::plugin_host {

using plugin::Refusal;

namespace {

// A signalling-NaN bit pattern: no plugin computes it from silence by
// accident, and it is compared as bits, never as a float.
constexpr uint32_t kCanary = 0x7FA5A5A5u;

// What is checked behind each buffer on every block: enough to catch a
// plugin that overruns by a little, cheap enough to do at quantum 128. The
// whole guard is checked during validation.
constexpr uint32_t kRuntimeGuardWords = 64;

// Atom port buffers. LSP and x42 plugins use them for UI notifications; we
// have no UI yet, so inputs are always an empty sequence.
constexpr std::size_t kAtomBytes = 8192;

bool is_canary(const float* p) {
    uint32_t bits;
    std::memcpy(&bits, p, sizeof bits);
    return bits == kCanary;
}

std::string node_string(const LilvNode* n) {
    return n ? std::string(lilv_node_as_string(n)) : std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// URID map

UridMap::UridMap() {
    map_.handle = this;
    map_.map = [](LV2_URID_Map_Handle h, const char* uri) {
        return static_cast<UridMap*>(h)->map(uri);
    };
    unmap_.handle = this;
    unmap_.unmap = [](LV2_URID_Unmap_Handle h, LV2_URID id) {
        return static_cast<UridMap*>(h)->unmap(id);
    };
}

LV2_URID UridMap::map(const char* uri) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = ids_.find(uri);
    if (it != ids_.end()) return it->second;
    uris_.emplace_back(uri);
    const LV2_URID id = static_cast<LV2_URID>(uris_.size());
    ids_.emplace(uris_.back(), id);
    return id;
}

const char* UridMap::unmap(LV2_URID id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id == 0 || id > uris_.size()) return nullptr;
    return uris_[id - 1].c_str();
}

// ---------------------------------------------------------------------------
// World

Lv2World::Lv2World() : world_(lilv_world_new()) {
    lilv_world_load_all(world_);
    n.audio = lilv_new_uri(world_, LV2_CORE__AudioPort);
    n.control = lilv_new_uri(world_, LV2_CORE__ControlPort);
    n.atom = lilv_new_uri(world_, LV2_ATOM__AtomPort);
    n.cv = lilv_new_uri(world_, LV2_CORE__CVPort);
    n.input = lilv_new_uri(world_, LV2_CORE__InputPort);
    n.output = lilv_new_uri(world_, LV2_CORE__OutputPort);
    n.optional = lilv_new_uri(world_, LV2_CORE__connectionOptional);
    n.reports_latency = lilv_new_uri(world_, LV2_CORE__reportsLatency);
    n.latency_designation = lilv_new_uri(world_, LV2_CORE__latency);
}

Lv2World::~Lv2World() {
    for (LilvNode* node : {n.audio, n.control, n.atom, n.cv, n.input, n.output, n.optional,
                           n.reports_latency, n.latency_designation})
        lilv_node_free(node);
    lilv_world_free(world_);
}

bool Lv2World::load_bundle(const std::string& dir, std::string& error) {
    // A bundle URI must be absolute and end in '/'; lilv_new_file_uri does
    // the percent-escaping a hand-built "file://" + path gets wrong.
    std::error_code ec;
    std::string path = std::filesystem::absolute(dir, ec).lexically_normal().string();
    if (ec || path.empty()) {
        error = "not a bundle path: " + dir;
        return false;
    }
    if (path.back() != '/') path += '/';
    LilvNode* node = lilv_new_file_uri(world_, nullptr, path.c_str());
    if (!node) {
        error = "not a bundle path: " + dir;
        return false;
    }
    lilv_world_load_bundle(world_, node);
    lilv_node_free(node);
    return true;
}

const LilvPlugin* Lv2World::find(const std::string& uri) const {
    LilvNode* node = lilv_new_uri(world_, uri.c_str());
    if (!node) return nullptr;
    const LilvPlugin* p = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world_), node);
    lilv_node_free(node);
    return p;
}

std::vector<const LilvPlugin*> Lv2World::all() const {
    std::vector<const LilvPlugin*> out;
    const LilvPlugins* plugins = lilv_world_get_all_plugins(world_);
    LILV_FOREACH (plugins, i, plugins) out.push_back(lilv_plugins_get(plugins, i));
    return out;
}

bool Lv2World::feature_supported(const std::string& uri) {
    static const char* const kSupported[] = {
        LV2_URID__map,
        LV2_URID__unmap,
        LV2_OPTIONS__options,
        LV2_BUF_SIZE__boundedBlockLength,
        // Properties some plugins list as features; they ask nothing of us.
        LV2_CORE__hardRTCapable,
        LV2_CORE__isLive,
        // Inputs and outputs are always separate buffers here.
        LV2_CORE__inPlaceBroken,
    };
    for (const char* s : kSupported)
        if (uri == s) return true;
    return false;
}

PluginInfo Lv2World::describe(const LilvPlugin* p) const {
    PluginInfo info;
    info.uri = node_string(lilv_plugin_get_uri(p));
    if (LilvNode* name = lilv_plugin_get_name(p)) {
        info.name = node_string(name);
        lilv_node_free(name);
    }
    info.class_label = node_string(lilv_plugin_class_get_label(lilv_plugin_get_class(p)));

    const uint32_t count = lilv_plugin_get_num_ports(p);
    std::vector<float> mins(count), maxs(count), defs(count);
    lilv_plugin_get_port_ranges_float(p, mins.data(), maxs.data(), defs.data());

    if (const LilvPort* lp = lilv_plugin_get_port_by_designation(p, n.output, n.latency_designation))
        info.latency_port = static_cast<int>(lilv_port_get_index(p, lp));
    else if (lilv_plugin_has_latency(p))
        info.latency_port = static_cast<int>(lilv_plugin_get_latency_port_index(p));

    for (uint32_t i = 0; i < count; ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(p, i);
        PortInfo pi;
        pi.index = i;
        pi.symbol = node_string(lilv_port_get_symbol(p, port));
        if (LilvNode* nm = lilv_port_get_name(p, port)) {
            pi.name = node_string(nm);
            lilv_node_free(nm);
        }
        pi.input = lilv_port_is_a(p, port, n.input);
        pi.optional = lilv_port_has_property(p, port, n.optional);
        if (lilv_port_is_a(p, port, n.audio))
            pi.kind = PortKind::Audio;
        else if (lilv_port_is_a(p, port, n.control))
            pi.kind = PortKind::Control;
        else if (lilv_port_is_a(p, port, n.atom))
            pi.kind = PortKind::Atom;
        else if (lilv_port_is_a(p, port, n.cv))
            pi.kind = PortKind::CV;
        pi.min = std::isnan(mins[i]) ? 0.f : mins[i];
        pi.max = std::isnan(maxs[i]) ? 0.f : maxs[i];
        pi.def = std::isnan(defs[i]) ? pi.min : defs[i];
        pi.reports_latency = static_cast<int>(i) == info.latency_port;
        if (pi.kind == PortKind::Audio) (pi.input ? info.audio_in : info.audio_out)++;
        info.ports.push_back(std::move(pi));
    }

    if (LilvNodes* req = lilv_plugin_get_required_features(p)) {
        LILV_FOREACH (nodes, i, req) {
            const std::string f = node_string(lilv_nodes_get(req, i));
            info.required_features.push_back(f);
            if (!feature_supported(f)) info.missing_features.push_back(f);
        }
        lilv_nodes_free(req);
    }
    return info;
}

// ---------------------------------------------------------------------------
// Chain

struct Lv2Chain::Slot {
    PluginInfo info;
    std::vector<LilvInstance*> instances;  // one, or one per channel
    std::vector<float> controls;           // indexed by port; shared by replicas
    struct AtomBuffer {
        bool input;
        std::vector<uint64_t> words;  // 8-byte aligned, as atoms require
    };
    std::vector<AtomBuffer> atoms;  // replica-major, one per atom port
    uint32_t latency = 0;
};

Lv2Chain::Lv2Chain(Lv2World& world, const ChainConfig& config)
    : world_(world),
      config_(config),
      guard_frames_(config.max_frames),
      stride_(config.max_frames + config.max_frames),
      max_block_(static_cast<int32_t>(config.max_frames)),
      nominal_block_(static_cast<int32_t>(config.block_frames)),
      rate_f_(static_cast<float>(config.sample_rate)) {
    UridMap& u = world_.urids();
    const LV2_URID atom_int = u.map(LV2_ATOM__Int);
    const LV2_URID atom_float = u.map(LV2_ATOM__Float);
    options_ = {
        {LV2_OPTIONS_INSTANCE, 0, u.map(LV2_BUF_SIZE__minBlockLength), sizeof(int32_t), atom_int,
         &min_block_},
        {LV2_OPTIONS_INSTANCE, 0, u.map(LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t), atom_int,
         &max_block_},
        {LV2_OPTIONS_INSTANCE, 0, u.map(LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t),
         atom_int, &nominal_block_},
        {LV2_OPTIONS_INSTANCE, 0, u.map(LV2_PARAMETERS__sampleRate), sizeof(float), atom_float,
         &rate_f_},
        {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
    };
    map_f_ = {LV2_URID__map, u.map_feature()};
    unmap_f_ = {LV2_URID__unmap, u.unmap_feature()};
    options_f_ = {LV2_OPTIONS__options, options_.data()};
    bounded_f_ = {LV2_BUF_SIZE__boundedBlockLength, nullptr};
    features_[0] = &map_f_;
    features_[1] = &unmap_f_;
    features_[2] = &options_f_;
    features_[3] = &bounded_f_;
    features_[4] = nullptr;
    atom_sequence_ = u.map(LV2_ATOM__Sequence);
    atom_chunk_ = u.map(LV2_ATOM__Chunk);
    stages_.emplace_back(std::size_t(config_.channels) * stride_, 0.f);
}

Lv2Chain::~Lv2Chain() {
    for (auto& s : slots_)
        for (LilvInstance* inst : s->instances) {
            lilv_instance_deactivate(inst);
            lilv_instance_free(inst);
        }
}

float* Lv2Chain::buffer(std::size_t stage, uint32_t channel) {
    return stages_[stage].data() + std::size_t(channel) * stride_;
}

bool Lv2Chain::add(const std::string& uri, ChainError& error) {
    const int index = static_cast<int>(slots_.size());
    error.instance = index;
    if (slots_.size() >= plugin::kMaxInstances) {
        error.reason = Refusal::TooManyInstances;
        error.message = "a host process runs at most " + std::to_string(plugin::kMaxInstances) +
                        " plugins";
        return false;
    }
    const LilvPlugin* p = world_.find(uri);
    if (!p) {
        error.reason = Refusal::NotFound;
        error.message = "no LV2 plugin with URI " + uri + " is installed";
        return false;
    }
    auto slot = std::make_unique<Slot>();
    slot->info = world_.describe(p);
    const PluginInfo& info = slot->info;
    const std::string who = info.name + " <" + info.uri + ">";

    if (!info.missing_features.empty()) {
        error.reason = Refusal::UnsupportedFeature;
        error.message = who + " requires " + info.missing_features.front();
        return false;
    }
    for (const PortInfo& port : info.ports) {
        if ((port.kind == PortKind::CV || port.kind == PortKind::Other) && !port.optional) {
            error.reason = Refusal::UnsupportedPort;
            error.message = who + ": port '" + port.symbol + "' has a type this host cannot connect";
            return false;
        }
    }

    uint32_t replicas = 0;
    if (info.audio_in == config_.channels && info.audio_out == config_.channels)
        replicas = 1;
    else if (info.audio_in == 1 && info.audio_out == 1)
        replicas = config_.channels;
    if (replicas == 0) {
        error.reason = Refusal::ChannelMismatch;
        error.message = who + " declares " + std::to_string(info.audio_in) + " in / " +
                        std::to_string(info.audio_out) + " out; the chain carries " +
                        std::to_string(config_.channels) + " channels";
        return false;
    }

    slot->controls.resize(info.ports.size(), 0.f);
    for (const PortInfo& port : info.ports)
        if (port.kind == PortKind::Control) slot->controls[port.index] = port.def;

    stages_.emplace_back(std::size_t(config_.channels) * stride_, 0.f);
    const std::size_t in_stage = slots_.size(), out_stage = in_stage + 1;

    for (uint32_t r = 0; r < replicas; ++r) {
        LilvInstance* inst = lilv_plugin_instantiate(p, config_.sample_rate, features_);
        if (!inst) {
            error.reason = Refusal::InstantiateFailed;
            error.message = who + " failed to instantiate";
            for (LilvInstance* done : slot->instances) lilv_instance_free(done);
            stages_.pop_back();
            return false;
        }
        slot->instances.push_back(inst);
        uint32_t audio_in = 0, audio_out = 0;
        for (const PortInfo& port : info.ports) {
            void* where = nullptr;
            switch (port.kind) {
            case PortKind::Audio: {
                // Replicated: replica r is channel r. Otherwise port order is
                // channel order.
                const uint32_t ch = replicas > 1 ? r : (port.input ? audio_in++ : audio_out++);
                where = buffer(port.input ? in_stage : out_stage, ch);
                break;
            }
            case PortKind::Control:
                where = &slot->controls[port.index];
                break;
            case PortKind::Atom: {
                slot->atoms.push_back({port.input, std::vector<uint64_t>(kAtomBytes / 8, 0)});
                where = slot->atoms.back().words.data();
                break;
            }
            default:
                break;  // optional CV or unknown: left unconnected (null)
            }
            lilv_instance_connect_port(inst, port.index, where);
        }
    }
    reset_atom_ports(*slot);
    for (LilvInstance* inst : slot->instances) lilv_instance_activate(inst);
    slots_.push_back(std::move(slot));
    error = ChainError{};
    return true;
}

void Lv2Chain::reset_atom_ports(Slot& s) noexcept {
    for (auto& a : s.atoms) {
        auto* atom = reinterpret_cast<LV2_Atom*>(a.words.data());
        if (a.input) {
            atom->size = sizeof(LV2_Atom_Sequence_Body);
            atom->type = atom_sequence_;
            reinterpret_cast<LV2_Atom_Sequence*>(atom)->body.unit = 0;
            reinterpret_cast<LV2_Atom_Sequence*>(atom)->body.pad = 0;
        } else {
            // An output atom port is told its capacity this way (LV2 atom spec).
            atom->size = static_cast<uint32_t>(kAtomBytes - sizeof(LV2_Atom));
            atom->type = atom_chunk_;
        }
    }
}

void Lv2Chain::fill_canaries(uint32_t frames) {
    float canary;
    std::memcpy(&canary, &kCanary, sizeof canary);
    for (auto& stage : stages_)
        for (uint32_t c = 0; c < config_.channels; ++c)
            std::fill(stage.begin() + std::ptrdiff_t(c) * stride_ + frames,
                      stage.begin() + std::ptrdiff_t(c + 1) * stride_, canary);
}

bool Lv2Chain::canaries_intact(uint32_t from_frame, uint32_t words) const noexcept {
    for (const auto& stage : stages_)
        for (uint32_t c = 0; c < config_.channels; ++c) {
            const float* p = stage.data() + std::size_t(c) * stride_ + from_frame;
            for (uint32_t i = 0; i < words; ++i)
                if (!is_canary(p + i)) return false;
        }
    return true;
}

bool Lv2Chain::process(const float* const* in, float* const* out, uint32_t frames,
                       plugin::ChannelHeader* report, ChainError& error) noexcept {
    frames = std::min(frames, config_.max_frames);
    for (uint32_t c = 0; c < config_.channels; ++c)
        std::memcpy(buffer(0, c), in[c], frames * sizeof(float));

    for (std::size_t k = 0; k < slots_.size(); ++k) {
        Slot& s = *slots_[k];
        if (report) report->current_instance.store(static_cast<int32_t>(k), std::memory_order_relaxed);
        reset_atom_ports(s);
        const uint64_t t0 = monotonic_ns();
        for (LilvInstance* inst : s.instances) lilv_instance_run(inst, frames);
        const uint64_t took = monotonic_ns() - t0;

        if (s.info.latency_port >= 0) {
            const float v = s.controls[static_cast<std::size_t>(s.info.latency_port)];
            s.latency = (std::isfinite(v) && v > 0.f) ? static_cast<uint32_t>(std::lround(v)) : 0;
        }
        if (report) {
            auto& r = report->instances[k];
            r.last_run_ns.store(took, std::memory_order_relaxed);
            if (took > r.worst_run_ns.load(std::memory_order_relaxed))
                r.worst_run_ns.store(took, std::memory_order_relaxed);
        }
        // Checking every stage, not only this slot's two, costs a few hundred
        // compares at most, and a plugin that writes into a neighbour's
        // buffer is attributed to the plugin that just ran.
        if (!canaries_intact(config_.max_frames,
                             std::min<uint32_t>(kRuntimeGuardWords, guard_frames_))) {
            error.reason = Refusal::WroteOutsideBuffers;
            error.instance = static_cast<int>(k);
            return false;
        }
    }
    if (report) report->current_instance.store(-1, std::memory_order_relaxed);

    const std::size_t last = slots_.size();
    for (uint32_t c = 0; c < config_.channels; ++c)
        std::memcpy(out[c], buffer(last, c), frames * sizeof(float));
    return true;
}

bool Lv2Chain::validate(ChainError& error) {
    const uint32_t frames = config_.block_frames;
    std::vector<float> silence(config_.max_frames, 0.f), sink(config_.max_frames, 0.f);
    std::vector<const float*> in(config_.channels, silence.data());
    std::vector<float*> out(config_.channels, sink.data());

    // Slot by slot, so a violation is attributed to the plugin that made it.
    // A chain of one is run, checked, then the next is added to the check.
    for (int pass = 0; pass < 4; ++pass) {
        fill_canaries(frames);
        for (uint32_t c = 0; c < config_.channels; ++c)
            std::fill(buffer(0, c), buffer(0, c) + frames, 0.f);
        for (std::size_t k = 0; k < slots_.size(); ++k) {
            Slot& s = *slots_[k];
            const std::string who = s.info.name + " <" + s.info.uri + ">";
            reset_atom_ports(s);
            for (LilvInstance* inst : s.instances) lilv_instance_run(inst, frames);
            if (s.info.latency_port >= 0) {
                const float v = s.controls[static_cast<std::size_t>(s.info.latency_port)];
                s.latency = (std::isfinite(v) && v > 0.f) ? static_cast<uint32_t>(std::lround(v)) : 0;
            }
            if (!canaries_intact(frames, stride_ - frames)) {
                error.reason = Refusal::WroteOutsideBuffers;
                error.instance = static_cast<int>(k);
                error.message = who + " declares " + std::to_string(s.info.audio_out) +
                                " audio output(s) of " + std::to_string(frames) +
                                " frames and wrote outside them";
                return false;
            }
            for (uint32_t c = 0; c < config_.channels; ++c) {
                const float* b = buffer(k + 1, c);
                for (uint32_t i = 0; i < frames; ++i)
                    if (!std::isfinite(b[i])) {
                        error.reason = Refusal::NonFiniteOutput;
                        error.instance = static_cast<int>(k);
                        error.message = who + " produced NaN or Inf from silence";
                        return false;
                    }
            }
        }
    }
    // Leave the guards whole for the runtime check.
    fill_canaries(config_.max_frames);
    error = ChainError{};
    return true;
}

bool Lv2Chain::set_control(uint32_t instance, uint32_t port, float value) noexcept {
    if (instance >= slots_.size()) return false;
    Slot& s = *slots_[instance];
    if (port >= s.info.ports.size()) return false;
    const PortInfo& p = s.info.ports[port];
    if (p.kind != PortKind::Control || !p.input) return false;
    s.controls[port] = value;
    return true;
}

bool Lv2Chain::set_control(uint32_t instance, const std::string& symbol, float value) {
    if (instance >= slots_.size()) return false;
    for (const PortInfo& p : slots_[instance]->info.ports)
        if (p.symbol == symbol) return set_control(instance, p.index, value);
    return false;
}

uint32_t Lv2Chain::instance_latency(std::size_t i) const { return slots_[i]->latency; }

uint32_t Lv2Chain::total_latency() const {
    uint32_t sum = 0;
    for (const auto& s : slots_) sum += s->latency;
    return sum;
}

uint32_t Lv2Chain::replicas(std::size_t i) const {
    return static_cast<uint32_t>(slots_[i]->instances.size());
}

const PluginInfo& Lv2Chain::info(std::size_t i) const { return slots_[i]->info; }

// ---------------------------------------------------------------------------

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char ch : s) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default:
            if (ch < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", ch);
                out += buf;
            } else {
                out += static_cast<char>(ch);
            }
        }
    }
    return out;
}

std::string port_kind_name(PortKind k) {
    switch (k) {
    case PortKind::Audio: return "audio";
    case PortKind::Control: return "control";
    case PortKind::Atom: return "atom";
    case PortKind::CV: return "cv";
    default: return "other";
    }
}

}  // namespace sonora::plugin_host
