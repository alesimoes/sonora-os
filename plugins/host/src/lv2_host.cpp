// SPDX-License-Identifier: GPL-3.0-or-later
#include "lv2_host.h"

#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/state/state.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace sonora::plugin_host {

using plugin::Refusal;

namespace {

// Atom port buffers. LSP and x42 plugins use them for UI notifications; we
// have no UI yet, so inputs are always an empty sequence.
constexpr std::size_t kAtomBytes = 8192;

std::string node_string(const LilvNode* n) {
    return n ? std::string(lilv_node_as_string(n)) : std::string();
}

}  // namespace

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
    n.enabled_designation = lilv_new_uri(world_, LV2_CORE__enabled);
    // lv2:isSideChain is defined in lv2core.ttl, but lv2/core/lv2.h (1.18.10
    // on trixie) has no macro for it, so the URI is spelled out.
    n.sidechain = lilv_new_uri(world_, LV2_CORE_PREFIX "isSideChain");
}

Lv2World::~Lv2World() {
    for (LilvNode* node : {n.audio, n.control, n.atom, n.cv, n.input, n.output, n.optional,
                           n.reports_latency, n.latency_designation, n.enabled_designation, n.sidechain})
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
    info.format = "lv2";
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
    if (const LilvPort* lp = lilv_plugin_get_port_by_designation(p, n.input, n.enabled_designation))
        info.enabled_port = static_cast<int>(lilv_port_get_index(p, lp));

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
        pi.sidechain = pi.kind == PortKind::Audio && pi.input && lilv_port_has_property(p, port, n.sidechain);
        if (pi.sidechain) ++info.sidechain_in;
        else if (pi.kind == PortKind::Audio) (pi.input ? info.audio_in : info.audio_out)++;
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
// Processor

namespace {

class Lv2Processor final : public Processor {
public:
    Lv2Processor(Lv2World& world, const ChainConfig& config)
        : max_block_(static_cast<int32_t>(config.max_frames)),
          nominal_block_(static_cast<int32_t>(config.block_frames)),
          rate_f_(static_cast<float>(config.sample_rate)) {
        UridMap& u = world.urids();
        const LV2_URID atom_int = u.map(LV2_ATOM__Int);
        const LV2_URID atom_float = u.map(LV2_ATOM__Float);
        options_ = {
            {LV2_OPTIONS_INSTANCE, 0, u.map(LV2_BUF_SIZE__minBlockLength), sizeof(int32_t),
             atom_int, &min_block_},
            {LV2_OPTIONS_INSTANCE, 0, u.map(LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t),
             atom_int, &max_block_},
            {LV2_OPTIONS_INSTANCE, 0, u.map(LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t),
             atom_int, &nominal_block_},
            {LV2_OPTIONS_INSTANCE, 0, u.map(LV2_PARAMETERS__sampleRate), sizeof(float),
             atom_float, &rate_f_},
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
    }

    ~Lv2Processor() override {
        for (LilvInstance* inst : instances_) {
            if (active_) lilv_instance_deactivate(inst);
            lilv_instance_free(inst);
        }
    }

    bool init(Lv2World& world, const LilvPlugin* p, PluginInfo info, uint32_t sample_rate, uint32_t channels,
              const std::vector<float*>& in, const std::vector<float*>& out,
              const std::vector<float*>& key, ChainError& error) {
        world_ = &world;
        plugin_ = p;
        info_ = std::move(info);
        const std::string who = info_.name + " <" + info_.uri + ">";
        if (!info_.missing_features.empty()) {
            error.reason = Refusal::UnsupportedFeature;
            error.message = who + " requires " + info_.missing_features.front();
            return false;
        }
        for (const PortInfo& port : info_.ports) {
            if ((port.kind == PortKind::CV || port.kind == PortKind::Other) && !port.optional) {
                error.reason = Refusal::UnsupportedPort;
                error.message = who + ": port '" + port.symbol + "' has a type this host cannot connect";
                return false;
            }
        }
        uint32_t replicas = 0;
        if (info_.audio_in == channels && info_.audio_out == channels)
            replicas = 1;
        else if (info_.audio_in == 1 && info_.audio_out == 1)
            replicas = channels;
        if (replicas == 0) {
            error.reason = Refusal::ChannelMismatch;
            error.message = who + " declares " + std::to_string(info_.audio_in) + " in / " +
                            std::to_string(info_.audio_out) + " out; the chain carries " +
                            std::to_string(channels) + " channels";
            return false;
        }

        controls_.resize(info_.ports.size(), 0.f);
        for (const PortInfo& port : info_.ports) {
            if (port.kind != PortKind::Control) continue;
            controls_[port.index] = port.def;
            if (port.input) {
                if (static_cast<int>(port.index) == info_.enabled_port)
                    bypass_dense_ = static_cast<int>(dense_.size());
                dense_.push_back(port.index);
            }
        }

        for (uint32_t r = 0; r < replicas; ++r) {
            LilvInstance* inst = lilv_plugin_instantiate(p, sample_rate, features_);
            if (!inst) {
                error.reason = Refusal::InstantiateFailed;
                error.message = who + " failed to instantiate";
                return false;
            }
            instances_.push_back(inst);
            uint32_t audio_in = 0, audio_out = 0, key_in = 0;
            for (const PortInfo& port : info_.ports) {
                void* where = nullptr;
                switch (port.kind) {
                case PortKind::Audio: {
                    // Replicated: replica r is channel r. Otherwise port order
                    // is channel order.
                    if (port.sidechain) {
                        // With no key in the chain, the plugin's own input:
                        // an LV2 port must be connected, and silence would
                        // be a silent key, which no key is not (ADR-0011
                        // Amendment 2 item 3). In and out are different
                        // stages, so this input is never being written.
                        const uint32_t k = replicas > 1 ? r : key_in++;
                        const std::vector<float*>& from = key.empty() ? in : key;
                        where = from[std::min<std::size_t>(k, from.size() - 1)];
                        break;
                    }
                    const uint32_t ch = replicas > 1 ? r : (port.input ? audio_in++ : audio_out++);
                    where = port.input ? in[ch] : out[ch];
                    break;
                }
                case PortKind::Control:
                    where = &controls_[port.index];
                    break;
                case PortKind::Atom:
                    atoms_.push_back({port.input, std::vector<uint64_t>(kAtomBytes / 8, 0)});
                    where = atoms_.back().words.data();
                    break;
                default:
                    break;  // optional CV or unknown: left unconnected (null)
                }
                lilv_instance_connect_port(inst, port.index, where);
            }
        }
        reset_atom_ports();
        for (LilvInstance* inst : instances_) lilv_instance_activate(inst);
        active_ = true;
        return true;
    }

    uint32_t replicas() const noexcept override { return static_cast<uint32_t>(instances_.size()); }

    void run(uint32_t frames) noexcept override {
        reset_atom_ports();
        for (LilvInstance* inst : instances_) lilv_instance_run(inst, frames);
        if (info_.latency_port >= 0) {
            const float v = controls_[static_cast<std::size_t>(info_.latency_port)];
            latency_ = (std::isfinite(v) && v > 0.f) ? static_cast<uint32_t>(std::lround(v)) : 0;
        }
    }

    uint32_t latency() const noexcept override { return latency_; }

    // The latency port is an output written in run(), so the new latency is
    // read from the next block, which the caller runs (host --reactivate
    // validates the chain again).
    // LV2's activate() cannot fail.
    bool reactivate() override {
        for (LilvInstance* inst : instances_) lilv_instance_deactivate(inst);
        for (LilvInstance* inst : instances_) lilv_instance_activate(inst);
        return true;
    }

    uint32_t param_count() const noexcept override { return static_cast<uint32_t>(dense_.size()); }

    // LV2 control ports are read once per run(), so a change lands at the
    // block start whatever its offset (ADR-0012 §5.1). The engine clamps to
    // the described range; so does this, against a misbehaving engine.
    bool set_param(uint32_t dense, float value, uint32_t) noexcept override {
        if (dense >= dense_.size() || !std::isfinite(value)) return false;
        const PortInfo& p = info_.ports[dense_[dense]];
        if (p.min < p.max) value = std::min(std::max(value, p.min), p.max);
        controls_[p.index] = value;
        return true;
    }

    bool param_index(const std::string& symbol, uint32_t& dense) const override {
        for (std::size_t d = 0; d < dense_.size(); ++d)
            if (info_.ports[dense_[d]].symbol == symbol) {
                dense = static_cast<uint32_t>(d);
                return true;
            }
        return false;
    }

    std::string param_key(uint32_t dense) const override { return info_.ports[dense_[dense]].symbol; }
    std::string param_symbol(uint32_t dense) const override { return info_.ports[dense_[dense]].symbol; }
    float param_default(uint32_t dense) const override { return info_.ports[dense_[dense]].def; }
    int bypass_param() const noexcept override { return bypass_dense_; }
    // lv2:enabled is 1 while processing.
    float bypass_value(bool bypassed) const noexcept override { return bypassed ? 0.f : 1.f; }

    void param_range(uint32_t dense, float& min, float& max) const override {
        const PortInfo& p = info_.ports[dense_[dense]];
        min = std::min(p.min, p.max);
        max = std::max(p.min, p.max);
    }

    // One section: the plugin's state:interface save(), as Turtle. Port
    // values are left out (no get_value): they are the engine's, through the
    // table, and win over a state anyway (host_control.h, ordering). The LV2
    // state spec lets save() run beside run(). Replicas are copies of
    // replica 0. A plugin without state:interface keeps no state of its own.
    SaveResult save_state(std::vector<std::string>& sections, std::string& error) override {
        if (!has_state_interface()) return SaveResult::Empty;
        UridMap& u = world_->urids();
        LilvState* state = lilv_state_new_from_instance(plugin_, instances_[0], u.map_feature(), nullptr,
                                                         nullptr, nullptr, nullptr, nullptr, nullptr,
                                                         LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, features_);
        if (!state) {
            error = info_.name + " <" + info_.uri + ">: its state could not be saved";
            return SaveResult::Failed;
        }
        char* text = lilv_state_to_string(world_->world(), u.map_feature(), u.unmap_feature(), state,
                                          kStateUri, nullptr);
        lilv_state_free(state);
        if (!text) {
            error = info_.name + " <" + info_.uri + ">: its state could not be written as Turtle";
            return SaveResult::Failed;
        }
        sections = {std::string(text)};
        lilv_free(text);
        return SaveResult::Saved;
    }

    // Before the processing thread starts: restore() is in LV2's
    // instantiation threading class and must never run beside run().
    bool restore_state(const std::vector<std::string>& sections, std::string& error) override {
        if (sections.empty()) return true;
        const std::string who = info_.name + " <" + info_.uri + ">";
        if (sections.size() != 1) {
            error = who + ": an LV2 state has one section, not " + std::to_string(sections.size());
            return false;
        }
        if (!has_state_interface()) {
            error = who + " keeps no state of its own";
            return false;
        }
        UridMap& u = world_->urids();
        LilvState* state = lilv_state_new_from_string(world_->world(), u.map_feature(), sections[0].c_str());
        if (!state) {
            error = who + ": the state is not one lilv can read";
            return false;
        }
        const LilvNode* of = lilv_state_get_plugin_uri(state);
        if (!of || info_.uri != lilv_node_as_uri(of)) {
            error = who + ": the state is another plugin's";
            lilv_state_free(state);
            return false;
        }
        for (LilvInstance* inst : instances_) lilv_state_restore(state, inst, nullptr, nullptr, 0, features_);
        lilv_state_free(state);
        return true;
    }

private:
    static constexpr const char* kStateUri = "urn:sonora:plugin-state";

    bool has_state_interface() const {
        return !instances_.empty() &&
               lilv_instance_get_extension_data(instances_[0], LV2_STATE__interface) != nullptr;
    }

    void reset_atom_ports() noexcept {
        for (auto& a : atoms_) {
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

    Lv2World* world_ = nullptr;
    const LilvPlugin* plugin_ = nullptr;
    std::vector<LilvInstance*> instances_;  // one, or one per channel
    std::vector<float> controls_;           // indexed by port; shared by replicas
    std::vector<uint32_t> dense_;           // dense index -> input control port
    int bypass_dense_ = -1;
    struct AtomBuffer {
        bool input;
        std::vector<uint64_t> words;  // 8-byte aligned, as atoms require
    };
    std::vector<AtomBuffer> atoms_;  // replica-major, one per atom port
    uint32_t latency_ = 0;
    // An instance that failed half way through init was never activated,
    // and LV2 forbids deactivating it.
    bool active_ = false;

    // Features handed to every instance. Built once, address-stable: the
    // processor lives on the heap and is never moved.
    int32_t min_block_ = 0, max_block_, nominal_block_;
    float rate_f_;
    std::vector<LV2_Options_Option> options_;
    LV2_Feature map_f_, unmap_f_, options_f_, bounded_f_;
    const LV2_Feature* features_[5];
    LV2_URID atom_sequence_, atom_chunk_;
};

}  // namespace

std::unique_ptr<Processor> Lv2World::instantiate(const std::string& uri, const ChainConfig& config,
                                                 const std::vector<float*>& in,
                                                 const std::vector<float*>& out,
                                                 const std::vector<float*>& key,
                                                 ChainError& error) {
    const LilvPlugin* p = find(uri);
    if (!p) {
        error.reason = Refusal::NotFound;
        error.message = "no LV2 plugin with URI " + uri + " is installed";
        return nullptr;
    }
    auto proc = std::make_unique<Lv2Processor>(*this, config);
    if (!proc->init(*this, p, describe(p), config.sample_rate, config.channels, in, out, key, error)) return nullptr;
    return proc;
}

}  // namespace sonora::plugin_host
