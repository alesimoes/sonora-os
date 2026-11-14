#pragma once
// One read per channel and side per tick, for every peak a meter can show.
// take_*peak() moves this reader's cursor, so a second read in the same tick
// would see nothing and draw a meter that lies; the cache lets any number of
// meters (and the live take) share a read. Header-only so the Qt build
// list does not change.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine_adapter.h"

namespace sonora::ui {

// Where on the strip a channel meter reads: before the fader (the input the
// recording hears) or after it (what the mix hears). A mixer-wide property.
enum class MeterPoint { Pre, Post };

class MeterFeed {
public:
    void resize(std::size_t tracks) {
        input_.assign(tracks, Slot{});
        post_.assign(tracks * 2, Slot{});
    }
    std::size_t tracks() const { return input_.size(); }

    void begin_tick() {
        for (Slot& s : input_) s.read = false;
        for (Slot& s : post_) s.read = false;
        for (Slot& s : bus_) s.read = false;
    }

    // False past the track count: the caller skips the meter rather than
    // drawing a zero that looks like silence.
    bool input(EngineAdapter& e, std::size_t ch, float& out) {
        if (ch >= input_.size()) return false;
        Slot& s = input_[ch];
        if (!s.read) fill(s, e.take_peak(static_cast<uint32_t>(ch)));
        out = s.value;
        return true;
    }

    bool post(EngineAdapter& e, std::size_t ch, std::size_t side, float& out) {
        if (side > 1 || ch >= input_.size()) return false;
        Slot& s = post_[ch * 2 + side];
        if (!s.read) {
            fill(s, e.take_post_peak(static_cast<uint32_t>(ch), static_cast<uint32_t>(side)));
        }
        out = s.value;
        return true;
    }

    // A mono meter shows the louder side.
    bool post_max(EngineAdapter& e, std::size_t ch, float& out) {
        float l = 0.0f;
        float r = 0.0f;
        if (!post(e, ch, 0, l) || !post(e, ch, 1, r)) return false;
        out = std::max(l, r);
        return true;
    }

    bool bus(EngineAdapter& e, std::size_t side, float& out) {
        if (side >= bus_.size()) return false;
        Slot& s = bus_[side];
        if (!s.read) fill(s, e.take_bus_peak(static_cast<uint32_t>(side)));
        out = s.value;
        return true;
    }

    // Input peaks as read this tick, for the live take; unread tracks hold 0.
    const std::vector<float>& input_values() {
        values_.resize(input_.size());
        for (std::size_t i = 0; i < input_.size(); ++i) values_[i] = input_[i].value;
        return values_;
    }
    bool input_read(std::size_t ch) const { return ch < input_.size() && input_[ch].read; }

private:
    struct Slot {
        float value = 0.0f;
        bool read = false;
    };
    static void fill(Slot& s, float v) {
        s.value = v;
        s.read = true;
    }
    std::vector<Slot> input_;
    std::vector<Slot> post_;
    std::vector<float> values_;
    std::array<Slot, 2> bus_{};
};

}  // namespace sonora::ui
