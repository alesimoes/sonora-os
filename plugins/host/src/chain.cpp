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
      guard_frames_(config.max_frames),
      stride_(config.max_frames + config.max_frames) {
    stages_.emplace_back(std::size_t(config_.channels) * stride_, 0.f);
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
    std::vector<float*> in, out;
    for (uint32_t c = 0; c < config_.channels; ++c) {
        in.push_back(buffer(in_stage, c));
        out.push_back(buffer(out_stage, c));
    }
    std::unique_ptr<Processor> p = source_.instantiate(id, config_, in, out, error);
    if (!p) {
        error.instance = index;
        stages_.pop_back();
        return false;
    }
    slots_.push_back(std::move(p));
    error = ChainError{};
    return true;
}

void Chain::fill_canaries(uint32_t frames) {
    float canary;
    std::memcpy(&canary, &kCanary, sizeof canary);
    for (auto& stage : stages_)
        for (uint32_t c = 0; c < config_.channels; ++c)
            std::fill(stage.begin() + std::ptrdiff_t(c) * stride_ + frames,
                      stage.begin() + std::ptrdiff_t(c + 1) * stride_, canary);
}

bool Chain::canaries_intact(uint32_t from_frame, uint32_t words) const noexcept {
    for (const auto& stage : stages_)
        for (uint32_t c = 0; c < config_.channels; ++c) {
            const float* p = stage.data() + std::size_t(c) * stride_ + from_frame;
            for (uint32_t i = 0; i < words; ++i)
                if (!is_canary(p + i)) return false;
        }
    return true;
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
                    plugin::ChannelHeader* report, ChainError& error) noexcept {
    frames = std::min(frames, config_.max_frames);
    for (uint32_t c = 0; c < config_.channels; ++c)
        std::memcpy(buffer(0, c), in[c], frames * sizeof(float));

    for (std::size_t k = 0; k < slots_.size(); ++k) {
        Processor& p = *slots_[k];
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

bool Chain::set_control(uint32_t instance, uint32_t id, float value) noexcept {
    if (instance >= slots_.size()) return false;
    return slots_[instance]->set_param(id, value);
}

bool Chain::set_control(uint32_t instance, const std::string& symbol, float value) {
    if (instance >= slots_.size()) return false;
    uint32_t id = 0;
    return slots_[instance]->param_id(symbol, id) && slots_[instance]->set_param(id, value);
}

uint32_t Chain::instance_latency(std::size_t i) const { return slots_[i]->latency(); }

uint32_t Chain::total_latency() const {
    uint32_t sum = 0;
    for (const auto& s : slots_) sum += s->latency();
    return sum;
}

uint32_t Chain::replicas(std::size_t i) const { return slots_[i]->replicas(); }

const PluginInfo& Chain::info(std::size_t i) const { return slots_[i]->info(); }

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
