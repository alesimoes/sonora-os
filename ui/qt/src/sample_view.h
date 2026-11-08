// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sample view and the Pencil on the waveform (ADR-0096). Peak files stop
// at 256 frames a bucket, so zoomed in past that the overview has nothing true
// to draw; the sample view draws the samples themselves, and the Pencil drawn
// on it ends in the engine's existing `sample.edit draw` (ADR-0052 §4). No
// protocol or file format changes: the interface reads the WAV, the engine
// still owns every write.
//
// Header-only on purpose: ui/qt's CMake files are held by other features
// (ADR-0096 §7), so this adds no compiled file.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "model_access.h"

namespace sonora::ui {

// At or below this many frames per pixel the sample view replaces the
// overview, and the Pencil draws. Above it a pixel is more frames than a
// sample view can show truthfully, and the Pencil refuses.
inline constexpr double kSampleViewMaxFpp = 4.0;
// At or below this the samples are dots: four pixels apart or more, room for
// a dot that does not touch its neighbour. At one frame a pixel they would
// touch and read as the line they already are.
inline constexpr double kSampleDotsMaxFpp = 0.25;
// The engine's limits on one draw (engine/control sample_edit.cpp,
// sample_edit.h). Mirrored, not included: nothing under ui/ depends on
// engine/ (ADR-0007). The contract test pins them.
inline constexpr std::size_t kPencilMaxPoints = 4096;
inline constexpr model::Frames kPencilMaxFrames = 16'000'000;

class SampleSource {
public:
    virtual ~SampleSource() = default;
    // `count` samples of one channel of a source, from source frame `first`,
    // into out[0, count). A frame outside the source, or one not loaded yet,
    // is 0. Runs on the GUI thread, so it answers from memory only: the
    // loading happens on the implementation's own worker thread, never here
    // and never on the audio thread.
    virtual void read(model::Id source, uint16_t channel, model::Frames first, int count,
                      float* out) const = 0;
    // False while the window around what read() was last asked for is still
    // loading: read() answers zeros, which are not the audio, so the view
    // says it is loading and the Pencil refuses rather than draw over them.
    virtual bool ready(model::Id source) const {
        (void)source;
        return true;
    }
};

// A deterministic source for the tests and the screenshot scenes: a sine of
// `period` frames at `level`, or a ramp from -level to +level over `period`
// frames. Channel n is shifted by n quarter periods so channels differ.
class FakeSampleSource final : public SampleSource {
public:
    enum class Shape { Sine, Ramp };
    FakeSampleSource(Shape shape, model::Frames period, float level = 0.5f,
                     model::Frames length = 48000 * 60)
        : shape_(shape), period_(std::max<model::Frames>(period, 2)), level_(level),
          length_(length) {}

    void read(model::Id source, uint16_t channel, model::Frames first, int count,
              float* out) const override {
        (void)source;
        for (int i = 0; i < count; ++i) out[i] = at(channel, first + i);
    }
    bool ready(model::Id source) const override { return source != not_ready_; }

    // One sample, as read() gives it. Public for the tests.
    float at(uint16_t channel, model::Frames frame) const {
        if (frame < 0 || frame >= length_) return 0.0f;
        const model::Frames shifted = frame + static_cast<model::Frames>(channel) * (period_ / 4);
        const double phase = static_cast<double>(shifted % period_) / static_cast<double>(period_);
        if (shape_ == Shape::Ramp) return static_cast<float>(level_ * (2.0 * phase - 1.0));
        return static_cast<float>(level_ * std::sin(2.0 * 3.14159265358979323846 * phase));
    }
    // A source that reports itself still loading, for the refusal paths.
    void set_not_ready(model::Id source) { not_ready_ = source; }

private:
    Shape shape_;
    model::Frames period_;
    float level_;
    model::Frames length_;
    model::Id not_ready_ = kNoId;
};

// One pointer position of a Pencil stroke: x in view pixels (as
// ViewState::frame_at takes it), the level its y stands for.
struct PencilPosition {
    double x = 0.0;
    float level = 0.0f;
};

// What the view was when the stroke was drawn, and the one clip it is on, in
// timeline frames: [clip_first, clip_end).
struct PencilFrame {
    double scroll = 0.0;
    double fpp = 1.0;
    model::Frames clip_first = 0;
    model::Frames clip_end = 0;
};

// A finished stroke: the stretch [from, to) and the points of
// `sample.edit draw REGION FROM TO CHANNEL POINTS`.
struct PencilStroke {
    model::Frames from = 0;
    model::Frames to = 0;
    std::string points;  // "FRAME:LEVEL,FRAME:LEVEL,..."
    std::size_t count = 0;
};

namespace detail {

// "%g" would follow the C locale Qt sets from the environment and could give
// "0,5", which the engine's strtod in the "C" locale reads as 0. Six decimals
// are finer than any pointer can place a level.
inline void append_level(std::string& s, float level) {
    long long micro = std::llround(static_cast<double>(level) * 1e6);
    if (micro < 0) {
        s += '-';
        micro = -micro;
    }
    s += std::to_string(micro / 1000000);
    long long frac = micro % 1000000;
    if (frac == 0) return;
    std::string digits = std::to_string(frac);
    digits.insert(0, 6 - digits.size(), '0');
    while (!digits.empty() && digits.back() == '0') digits.pop_back();
    s += '.';
    s += digits;
}

}  // namespace detail

// Turns the pointer positions of one stroke into the Draw points: one point
// per pixel column crossed, the columns between two positions interpolated so
// a fast stroke skips none, levels clamped to -1..1. Columns outside the clip
// draw nothing (a stroke stays on one clip); where two columns land on one
// frame (zoomed past a frame a pixel) the later one wins, as the pointer
// left it. Points come out in increasing frame order, as the engine wants.
// False, with `why` for the status line, when there is nothing to send: no
// position on the clip, a zoom coarser than kSampleViewMaxFpp, more points or
// frames than one draw takes.
inline bool pencil_points(const std::vector<PencilPosition>& stroke, const PencilFrame& view,
                          PencilStroke& out, std::string& why) {
    out = PencilStroke{};
    if (!(view.fpp > 0.0) || view.fpp > kSampleViewMaxFpp) {
        why = "zoom in to the samples to draw: the Pencil draws at 4 samples a pixel or closer";
        return false;
    }
    std::map<model::Frames, float> drawn;
    const auto put = [&](long long column, float level) {
        const auto f = static_cast<model::Frames>(
            std::llround(view.scroll + static_cast<double>(column) * view.fpp));
        if (f < view.clip_first || f >= view.clip_end) return;
        drawn[f] = std::clamp(level, -1.0f, 1.0f);
    };
    for (std::size_t i = 0; i < stroke.size(); ++i) {
        const long long c1 = std::llround(stroke[i].x);
        const float l1 = stroke[i].level;
        if (i == 0) {
            put(c1, l1);
            continue;
        }
        const long long c0 = std::llround(stroke[i - 1].x);
        const float l0 = stroke[i - 1].level;
        const long long span = c1 - c0;
        if (span == 0) {
            put(c1, l1);
            continue;
        }
        const long long step = span > 0 ? 1 : -1;
        if ((span > 0 ? span : -span) > static_cast<long long>(kPencilMaxPoints) * 64) {
            why = "the stroke is longer than one drawing takes";
            return false;
        }
        for (long long c = c0 + step;; c += step) {
            const double t = static_cast<double>(c - c0) / static_cast<double>(span);
            put(c, static_cast<float>(l0 + (l1 - l0) * t));
            if (c == c1) break;
        }
    }
    if (drawn.empty()) {
        why = "nothing drawn: draw on the clip's waveform";
        return false;
    }
    if (drawn.size() > kPencilMaxPoints) {
        why = "a drawing holds at most " + std::to_string(kPencilMaxPoints) +
              " points; draw a shorter stroke";
        return false;
    }
    out.from = drawn.begin()->first;
    out.to = drawn.rbegin()->first + 1;
    if (out.to - out.from > kPencilMaxFrames) {
        why = "a drawing covers at most 16 million frames";
        return false;
    }
    for (const auto& [frame, level] : drawn) {
        if (!out.points.empty()) out.points += ',';
        out.points += std::to_string(frame);
        out.points += ':';
        detail::append_level(out.points, level);
    }
    out.count = drawn.size();
    return true;
}

}  // namespace sonora::ui
