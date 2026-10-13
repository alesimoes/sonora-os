// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sonora/measure/measure.h"

namespace sonora::measure {

Chans render(native::Processor& p, const Chans& in, const std::vector<Event>& events,
             const RenderOptions& opt) {
    const std::size_t n = in.empty() ? 0 : in[0].size();
    const auto ch = static_cast<std::uint32_t>(in.size());
    Chans out(ch, std::vector<float>(n, 0.0f));
    std::vector<const float*> ip(ch), sp;
    std::vector<float*> op(ch);
    std::vector<native::ParamEvent> ev;
    ev.reserve(64);
    std::size_t next = 0;
    if (opt.block_us != nullptr) opt.block_us->clear();
    for (std::size_t pos = 0; pos < n; pos += opt.block) {
        const auto len = static_cast<std::uint32_t>(std::min<std::size_t>(opt.block, n - pos));
        ev.clear();
        while (next < events.size() && events[next].frame < pos + len) {
            const Event& e = events[next++];
            const std::uint64_t at = e.frame < pos ? pos : e.frame;
            ev.push_back({static_cast<std::uint32_t>(at - pos), e.index, e.value});
        }
        for (std::uint32_t c = 0; c < ch; ++c) {
            ip[c] = &in[c][pos];
            op[c] = &out[c][pos];
        }
        native::AudioBuffers io{ip.data(), op.data(), nullptr, ch, 0, len};
        if (opt.sidechain != nullptr) {
            sp.resize(opt.sidechain->size());
            for (std::size_t c = 0; c < sp.size(); ++c) sp[c] = &(*opt.sidechain)[c][pos];
            io.sidechain = sp.data();
            io.sidechain_channels = static_cast<std::uint32_t>(sp.size());
        }
        if (opt.block_us != nullptr) {
            const auto t0 = std::chrono::steady_clock::now();
            p.process(io, ev.data(), static_cast<std::uint32_t>(ev.size()), opt.transport);
            const auto t1 = std::chrono::steady_clock::now();
            opt.block_us->push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        } else {
            p.process(io, ev.data(), static_cast<std::uint32_t>(ev.size()), opt.transport);
        }
    }
    return out;
}

std::uint32_t param_index(const native::Processor& p, const char* id) {
    const native::Descriptor& d = p.descriptor();
    for (std::uint32_t i = 0; i < d.param_count; ++i) {
        if (std::strcmp(d.params[i].id, id) == 0) return i;
    }
    std::fprintf(stderr, "measure: %s has no parameter '%s'\n", d.id, id);
    std::abort();
}

void set(native::Processor& p, const char* id, float value) {
    p.set_param(param_index(p, id), value);
}

void apply(native::Processor& p, const Config& c) {
    for (const auto& kv : c) set(p, kv.first, kv.second);
}

}  // namespace sonora::measure
