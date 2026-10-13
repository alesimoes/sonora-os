// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace sonora::drivers {

// The physical capture ports that feed the backend's inputs, so the loss of
// one -- the interface unplugged -- can be told from a port nobody records
// from any more. The backend filters out non-physical sources (pw-play, a
// soft synth) before calling in. Not thread-safe: the backend serialises
// calls. Never on the audio thread: it allocates.
class SourceWatch {
public:
    // A source unlinked this recently still counts when its port goes. When
    // a device goes, pw-jack 1.4.2 delivers the link's disconnect before
    // the port's unregistration, in one batch, so forgetting on the
    // disconnect itself would miss every unplug.
    static constexpr uint64_t kUnlinkGraceNs = 1000000000ull;

    void linked(uint32_t id, const std::string& name, uint64_t now_ns) {
        drop_expired(now_ns);
        for (Source& s : sources_) {
            if (s.id != id) continue;
            // Same id, other name: the id was reused after a removal we
            // never saw, and the old entry describes nothing that exists.
            if (s.name != name) s = Source{id, name, 0, 0};
            ++s.links;
            return;
        }
        sources_.push_back(Source{id, name, 1, 0});
    }

    void unlinked(uint32_t id, const std::string& name, uint64_t now_ns) {
        for (Source& s : sources_) {
            if (s.id != id || s.name != name || s.links == 0) continue;
            if (--s.links == 0) s.unlinked_at_ns = now_ns;
            return;
        }
    }

    // Whether the port `id` going away ends the take; `gone` is then its
    // name. `name` may be null when the server no longer knows it, and the
    // id alone decides. Forgets the port either way.
    bool removed(uint32_t id, const char* name, uint64_t now_ns,
                 std::string& gone) {
        const auto it = std::find_if(sources_.begin(), sources_.end(),
                                     [id](const Source& s) { return s.id == id; });
        if (it == sources_.end()) return false;
        const bool same = name == nullptr || it->name == name;
        const bool feeding = it->links > 0 || now_ns - it->unlinked_at_ns < kUnlinkGraceNs;
        if (same && feeding) gone = it->name;
        sources_.erase(it);
        return same && feeding;
    }

    std::size_t size() const { return sources_.size(); }

private:
    struct Source {
        uint32_t id;
        std::string name;
        uint32_t links;
        uint64_t unlinked_at_ns;
    };

    void drop_expired(uint64_t now_ns) {
        sources_.erase(std::remove_if(sources_.begin(), sources_.end(),
                                      [now_ns](const Source& s) {
                                          return s.links == 0 &&
                                                 now_ns - s.unlinked_at_ns >=
                                                     kUnlinkGraceNs;
                                      }),
                       sources_.end());
    }

    std::vector<Source> sources_;
};

}  // namespace sonora::drivers
