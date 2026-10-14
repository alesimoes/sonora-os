// SPDX-License-Identifier: GPL-3.0-or-later
#include "chain.h"

#include "ipc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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

// NaN and Inf have an all-ones exponent. Tested as bits so it holds under any
// floating-point flags a plugin leaves behind.
bool is_finite_bits(float f) noexcept {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    return (bits & 0x7F800000u) != 0x7F800000u;
}

bool is_canary(const float* p) {
    uint32_t bits;
    std::memcpy(&bits, p, sizeof bits);
    return bits == kCanary;
}

}  // namespace

Chain::Chain(PluginSource& source, const ChainConfig& config)
    : source_(source),
      config_(config),
      // One buffer per signal channel at least, so every replica of a mono
      // plugin reads a key buffer of its own.
      key_channels_(config.sidechain_channels > 0 ? std::max(config.sidechain_channels, config.channels)
                                                  : 0),
      guard_frames_(config.max_frames),
      stride_(config.max_frames + config.max_frames) {
    stages_.emplace_back(std::size_t(config_.channels) * stride_, 0.f);
    key_.assign(std::size_t(key_channels_) * stride_, 0.f);
}

// Processors go before the buffers they point into.
Chain::~Chain() { slots_.clear(); }

float* Chain::buffer(std::size_t stage, uint32_t channel) {
    return stages_[stage].data() + std::size_t(channel) * stride_;
}

bool Chain::add(const std::string& id, ChainError& error) {
    const int index = static_cast<int>(slots_.size());
    error.instance = index;
    if (slots_.size() >= plugin::kMaxInstances) {
        error.reason = Refusal::TooManyInstances;
        error.message = "a host process runs at most " + std::to_string(plugin::kMaxInstances) +
                        " plugins";
        return false;
    }
    stages_.emplace_back(std::size_t(config_.channels) * stride_, 0.f);
    const std::size_t in_stage = slots_.size(), out_stage = in_stage + 1;
    std::vector<float*> in, out, key;
    for (uint32_t c = 0; c < config_.channels; ++c) {
        in.push_back(buffer(in_stage, c));
        out.push_back(buffer(out_stage, c));
    }
    for (uint32_t c = 0; c < key_channels_; ++c) key.push_back(key_.data() + std::size_t(c) * stride_);
    std::unique_ptr<Processor> p = source_.instantiate(id, config_, in, out, key, error);
    if (!p) {
        error.instance = index;
        stages_.pop_back();
        return false;
    }
    slots_.push_back(std::move(p));
    error = ChainError{};
    return true;
}

// Key buffer k holds the region's key channel min(k, sidechain - 1), so a
// mono key feeds every buffer. Every plugin gets this fresh copy.
void Chain::copy_key(const float* const* in, uint32_t frames) noexcept {
    for (uint32_t k = 0; k < key_channels_; ++k)
        std::memcpy(key_.data() + std::size_t(k) * stride_,
                    in[config_.channels + std::min(k, config_.sidechain_channels - 1)], frames * sizeof(float));
}

void Chain::fill_canaries(uint32_t frames) {
    float canary;
    std::memcpy(&canary, &kCanary, sizeof canary);
    const auto fill = [&](std::vector<float>& planes, uint32_t channels) {
        for (uint32_t c = 0; c < channels; ++c)
            std::fill(planes.begin() + std::ptrdiff_t(c) * stride_ + frames,
                      planes.begin() + std::ptrdiff_t(c + 1) * stride_, canary);
    };
    for (auto& stage : stages_) fill(stage, config_.channels);
    fill(key_, key_channels_);
}

bool Chain::canaries_intact(uint32_t from_frame, uint32_t words) const noexcept {
    const auto intact = [&](const std::vector<float>& planes, uint32_t channels) {
        for (uint32_t c = 0; c < channels; ++c) {
            const float* p = planes.data() + std::size_t(c) * stride_ + from_frame;
            for (uint32_t i = 0; i < words; ++i)
                if (!is_canary(p + i)) return false;
        }
        return true;
    };
    for (const auto& stage : stages_)
        if (!intact(stage, config_.channels)) return false;
    return intact(key_, key_channels_);
}

uint64_t Chain::sanitise_stage(std::size_t stage, uint32_t frames) noexcept {
    uint64_t bad = 0;
    for (uint32_t c = 0; c < config_.channels; ++c) {
        float* b = buffer(stage, c);
        for (uint32_t i = 0; i < frames; ++i)
            if (!is_finite_bits(b[i])) {
                b[i] = 0.f;
                ++bad;
            }
    }
    return bad;
}

bool Chain::process(const float* const* in, float* const* out, uint32_t frames,
                    plugin::ChannelHeader* report, ChainError& error) noexcept SONORA_RT {
    frames = std::min(frames, config_.max_frames);
    for (uint32_t c = 0; c < config_.channels; ++c)
        std::memcpy(buffer(0, c), in[c], frames * sizeof(float));

    for (std::size_t k = 0; k < slots_.size(); ++k) {
        Processor& p = *slots_[k];
        // Copied, not pointed to: no plugin ever sees the engine's planes.
        // Again for every plugin, so a key one plugin wrote into is not the
        // next one's key (a bounded copy: key channels x frames).
        copy_key(in, frames);
        if (report) report->current_instance.store(static_cast<int32_t>(k), std::memory_order_relaxed);
        const uint64_t t0 = monotonic_ns();
        p.run(frames);
        const uint64_t took = monotonic_ns() - t0;
        if (report) {
            auto& r = report->instances[k];
            r.last_run_ns.store(took, std::memory_order_relaxed);
            if (took > r.worst_run_ns.load(std::memory_order_relaxed))
                r.worst_run_ns.store(took, std::memory_order_relaxed);
        }
        // Checking every stage, not only this slot's two, costs a few hundred
        // compares at most, and a plugin that writes into a neighbour's
        // buffer is attributed to the plugin that just ran.
        if (!p.intact() || !canaries_intact(config_.max_frames,
                                            std::min<uint32_t>(kRuntimeGuardWords, guard_frames_))) {
            error.reason = Refusal::WroteOutsideBuffers;
            error.instance = static_cast<int>(k);
            return false;
        }
        // Before the next plugin sees it: a NaN fed into a filter stays in
        // its state, so one bad block from k would silence k + 1 for good.
        if (const uint64_t bad = sanitise_stage(k + 1, frames); bad && report)
            report->instances[k].nonfinite_samples.fetch_add(bad, std::memory_order_relaxed);
        run_bypass(k, frames);
    }
    if (report) report->current_instance.store(-1, std::memory_order_relaxed);

    const std::size_t last = slots_.size();
    for (uint32_t c = 0; c < config_.channels; ++c)
        std::memcpy(out[c], buffer(last, c), frames * sizeof(float));
    return true;
}

bool Chain::validate(ChainError& error) {
    const uint32_t frames = config_.block_frames;
    // Slot by slot, so a violation is attributed to the plugin that made it.
    for (int pass = 0; pass < 4; ++pass) {
        fill_canaries(frames);
        for (uint32_t c = 0; c < config_.channels; ++c)
            std::fill(buffer(0, c), buffer(0, c) + frames, 0.f);
        for (std::size_t k = 0; k < slots_.size(); ++k) {
            Processor& p = *slots_[k];
            const PluginInfo& info = p.info();
            const std::string who = info.name + " <" + info.uri + ">";
            // Silence again for every plugin, as process() copies the key
            // again: the canaries behind it stay as fill_canaries left them.
            for (uint32_t c = 0; c < key_channels_; ++c)
                std::fill(key_.begin() + std::ptrdiff_t(c) * stride_,
                          key_.begin() + std::ptrdiff_t(c) * stride_ + frames, 0.f);
            p.run(frames);
            if (!p.intact() || !canaries_intact(frames, stride_ - frames)) {
                error.reason = Refusal::WroteOutsideBuffers;
                error.instance = static_cast<int>(k);
                error.message = who + " declares " + std::to_string(info.audio_out) +
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
    for (auto& p : slots_) p->validated();
    error = ChainError{};
    return true;
}

bool Chain::set_param(uint32_t instance, uint32_t dense, float value, uint32_t offset) noexcept SONORA_RT {
    if (instance >= slots_.size()) return false;
    return slots_[instance]->set_param(dense, value, offset);
}

bool Chain::replace_param(uint32_t instance, uint32_t dense, float value) noexcept SONORA_RT {
    if (instance >= slots_.size()) return false;
    return slots_[instance]->replace_param(dense, value);
}

bool Chain::set_control(uint32_t instance, const std::string& symbol, float value) {
    if (instance >= slots_.size()) return false;
    uint32_t dense = 0;
    return slots_[instance]->param_index(symbol, dense) && slots_[instance]->set_param(dense, value, 0);
}

uint32_t Chain::param_count(std::size_t i) const { return slots_[i]->param_count(); }
std::string Chain::param_key(std::size_t i, uint32_t dense) const { return slots_[i]->param_key(dense); }
std::string Chain::param_symbol(std::size_t i, uint32_t dense) const { return slots_[i]->param_symbol(dense); }
float Chain::param_default(std::size_t i, uint32_t dense) const { return slots_[i]->param_default(dense); }

bool Chain::designated_bypass(std::size_t i) const { return slots_[i]->bypass_param() >= 0; }

bool Chain::set_bypass(uint32_t instance, bool bypassed, uint32_t offset) noexcept SONORA_RT {
    if (instance >= slots_.size()) return false;
    Processor& p = *slots_[instance];
    if (const int d = p.bypass_param(); d >= 0)
        return p.set_param(static_cast<uint32_t>(d), p.bypass_value(bypassed), offset);
    if (instance >= bypass_.size()) return false;  // before prepare_bypass
    bypass_[instance].on = bypassed;
    return true;
}

void Chain::prepare_bypass() {
    bypass_.assign(slots_.size(), BypassLine{});
    for (std::size_t k = 0; k < slots_.size(); ++k) {
        if (designated_bypass(k)) continue;
        // Bounded before it sizes anything (auditor L6 on PR #45): a
        // plugin reporting ~UINT32_MAX would otherwise end the host in
        // bad_alloc and truncate the mask. Past kMaxLatencyFrames the
        // engine treats the figure as a fault anyway.
        const uint32_t latency = std::min(slots_[k]->latency(), plugin::kMaxLatencyFrames);
        const std::size_t need = std::size_t(latency) * 2 + kBypassHeadroomFrames + config_.max_frames;
        std::size_t size = 1;
        while (size < need) size <<= 1;
        BypassLine& b = bypass_[k];
        b.ring.assign(config_.channels, std::vector<float>(size, 0.f));
        b.mask = static_cast<uint32_t>(size - 1);
    }
}

// Every block, bypassed or not, so the history is there when bypass turns
// on. The ring is written before it is read, so a latency of 0 reads the
// sample just written.
void Chain::run_bypass(std::size_t k, uint32_t frames) noexcept SONORA_RT {
    if (k >= bypass_.size() || bypass_[k].ring.empty()) return;
    BypassLine& b = bypass_[k];
    const uint32_t size = b.mask + 1;
    const uint32_t delay = std::min(slots_[k]->latency(), size - config_.max_frames);
    for (uint32_t c = 0; c < config_.channels; ++c) {
        const float* in = buffer(k, c);
        float* ring = b.ring[c].data();
        for (uint32_t s = 0; s < frames; ++s) ring[(b.write + s) & b.mask] = in[s];
        if (!b.on && !b.was) continue;
        float* out = buffer(k + 1, c);
        for (uint32_t s = 0; s < frames; ++s) {
            const float dry = ring[(b.write + s - delay) & b.mask];
            if (b.on && b.was) {
                out[s] = dry;
                continue;
            }
            // A change: one block's linear crossfade, wet to dry or back.
            const float g = float(s + 1) / float(frames);
            out[s] = b.on ? out[s] * (1.f - g) + dry * g : dry * (1.f - g) + out[s] * g;
        }
    }
    b.write += frames;
    b.was = b.on;
}

uint32_t Chain::instance_latency(std::size_t i) const noexcept SONORA_RT { return slots_[i]->latency(); }

uint32_t Chain::total_latency() const noexcept SONORA_RT {
    uint32_t sum = 0;
    for (const auto& s : slots_) sum += s->latency();
    return sum;
}

uint32_t Chain::replicas(std::size_t i) const { return slots_[i]->replicas(); }

const PluginInfo& Chain::info(std::size_t i) const { return slots_[i]->info(); }

void Chain::control() {
    for (auto& s : slots_) s->control();
}

bool Chain::reactivate(ChainError& error) {
    for (std::size_t k = 0; k < slots_.size(); ++k) {
        if (slots_[k]->reactivate()) continue;
        const PluginInfo& info = slots_[k]->info();
        error.reason = Refusal::InstantiateFailed;
        error.instance = static_cast<int>(k);
        error.message = info.name + " <" + info.uri + "> failed to re-activate";
        return false;
    }
    error = ChainError{};
    return true;
}

// ---------------------------------------------------------------------------

std::atomic<uint32_t>& control_word() noexcept {
    static std::atomic<uint32_t> word{0};
    return word;
}

void notify_control() noexcept { signal(control_word()); }

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
