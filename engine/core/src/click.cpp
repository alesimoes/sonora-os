// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/click.h"

#include <algorithm>
#include <cmath>

namespace sonora {
namespace {

constexpr uint64_t kLimit = uint64_t{1} << 62;

// ADR-0020 §2: a bar's downbeat higher and louder than its other beats,
// 30 ms each, -6 and -10 dBFS at their first sample.
constexpr double kAccentHz = 1500.0;
constexpr double kBeatHz = 1000.0;
constexpr float kAccentGain = 0.5f;
constexpr float kBeatGain = 0.3f;
constexpr double kSoundSeconds = 0.030;
constexpr double kDecaySeconds = 0.0045;

bool usable(const ClickSegment& c) noexcept SONORA_RT {
    return c.num != 0 && c.beat_ticks != 0 && c.beats_per_bar != 0 && c.beats != 0;
}

}  // namespace

int64_t click_scale(uint64_t x, uint64_t den, uint64_t num) noexcept SONORA_RT {
    if (num == 0) return kNoClickFrame;
    const uint64_t qd = den / num;
    const uint64_t rd = den % num;
    if (qd > kLimit) return kNoClickFrame;
    // x * den = q * num + r, 0 <= r < num, built from the top bit of x down;
    // r stays under num, so 2r and r + rd are compared, never formed.
    uint64_t q = 0;
    uint64_t r = 0;
    for (int b = 63; b >= 0; --b) {
        if (q > kLimit) return kNoClickFrame;
        q <<= 1;
        if (r >= num - r) {
            r -= num - r;
            ++q;
        } else {
            r += r;
        }
        if ((x >> b) & 1u) {
            q += qd;
            if (r >= num - rd) {
                r -= num - rd;
                ++q;
            } else {
                r += rd;
            }
        }
    }
    if (r >= num - r) ++q;
    return q > kLimit ? kNoClickFrame : static_cast<int64_t>(q);
}

int64_t click_beat_frame(const ClickSegment& s, uint64_t j) noexcept SONORA_RT {
    if (s.beat_ticks == 0 || j > (kLimit - s.first_tick) / s.beat_ticks) return kNoClickFrame;
    const int64_t off = click_scale(s.first_tick + j * s.beat_ticks, s.den, s.num);
    if (off == kNoClickFrame || s.at > static_cast<int64_t>(kLimit) - off) return kNoClickFrame;
    return s.at + off;
}

Click::Click(uint32_t sample_rate) {
    const double rate = sample_rate != 0 ? sample_rate : 48000.0;
    sound_len_ = std::max<uint32_t>(1, static_cast<uint32_t>(kSoundSeconds * rate));
    accent_.reset(new float[sound_len_]);
    beat_.reset(new float[sound_len_]);
    constexpr double kTwoPi = 6.283185307179586;
    for (uint32_t n = 0; n < sound_len_; ++n) {
        // An exponential decay, tapered to 0 at the end so the sound
        // stops without a step of its own.
        const double env = std::exp(-static_cast<double>(n) / (kDecaySeconds * rate)) *
                           (1.0 - static_cast<double>(n) / sound_len_);
        accent_[n] = static_cast<float>(kAccentGain * env * std::cos(kTwoPi * kAccentHz * n / rate));
        beat_[n] = static_cast<float>(kBeatGain * env * std::cos(kTwoPi * kBeatHz * n / rate));
    }
    posted_ = std::make_unique<RingBuffer<CountOffRecord>>(8);
}

void Click::set_count_off_bars(uint32_t bars) {
    bars_.store(std::min(std::max<uint32_t>(bars, 1), kMaxCountOffBars), std::memory_order_relaxed);
}

bool Click::post_count_off(uint32_t start) {
    const CountOffRecord r{start, count_off_bars()};
    return posted_->write(&r, 1) == 1;
}

bool Click::begin_block(const Transport::Word& word) noexcept SONORA_RT {
    CountOffRecord r{};
    while (posted_->read(&r, 1) == 1) {
        pending_ = true;
        pending_start_ = r.start;
        pending_bars_ = r.bars;
    }
    const bool recording = word.state == TransportState::Recording;
    if (counting_off_ && !(recording && word.start == start_)) counting_off_ = false;
    // Posted before the take's store, so a block may drain it while the
    // word is still Stopped: it waits for its take. Any other take means
    // its own was never seen, and it is dropped.
    if (!counting_off_ && pending_ && recording) {
        if (word.start == pending_start_) {
            counting_off_ = true;
            set_up_ = false;
            start_ = pending_start_;
            bars_now_ = pending_bars_;
        }
        pending_ = false;
    }
    counting_.store(counting_off_, std::memory_order_relaxed);
    return counting_off_;
}

void Click::render(const PlaybackPlan& plan, const backend::AudioBlock& block, bool rolling,
                   uint64_t playhead, uint32_t frames, uint64_t loop_from,
                   uint64_t loop_to) noexcept SONORA_RT {
    out_[0] = plan.click_out[0];
    out_[1] = plan.click_out[1];
    done_ = 0;
    const uint32_t nframes = block.nframes;
    if (counting_off_) {
        if (!set_up_) start_count_off(plan, playhead, nframes);
        count_off_beats(block, nframes);
    } else if (rolling && on() && plan.click_count != 0) {
        const bool looped = loop_to > loop_from;
        uint64_t t = playhead;
        for (uint32_t off = 0; off < frames;) {
            uint32_t span = frames - off;
            if (looped && t < loop_to && t + span > loop_to) span = static_cast<uint32_t>(loop_to - t);
            timeline(plan, block, t, off, span);
            off += span;
            t += span;
            if (looped && t == loop_to) t = loop_from;
        }
    } else {
        cursor_.at = ~uint64_t{0};
    }
    sound_to(block, nframes);
}

void Click::seek(const PlaybackPlan& plan, uint64_t t) noexcept SONORA_RT {
    cursor_.epoch = plan.epoch;
    cursor_.at = t;
    cursor_.frame = kNoClickFrame;
    const auto at = static_cast<int64_t>(t);
    // The last segment whose first beat is at or before t.
    uint32_t lo = 0;
    uint32_t hi = plan.click_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (plan.click[mid].first_frame <= at) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (uint32_t s = lo == 0 ? 0 : lo - 1; s < plan.click_count; ++s) {
        const ClickSegment& c = plan.click[s];
        if (!usable(c)) continue;
        // Its first beat at or after t: beats only move forward in frames.
        uint64_t a = 0;
        uint64_t b = c.beats;
        while (a < b) {
            const uint64_t m = a + (b - a) / 2;
            if (click_beat_frame(c, m) < at) {
                a = m + 1;
            } else {
                b = m;
            }
        }
        if (a == c.beats) continue;
        const int64_t f = click_beat_frame(c, a);
        if (f == kNoClickFrame) return;
        cursor_.seg = s;
        cursor_.beat = a;
        cursor_.frame = f;
        return;
    }
}

void Click::step(const PlaybackPlan& plan) noexcept SONORA_RT {
    ++cursor_.beat;
    while (cursor_.seg < plan.click_count) {
        const ClickSegment& c = plan.click[cursor_.seg];
        if (usable(c) && cursor_.beat < c.beats) {
            cursor_.frame = click_beat_frame(c, cursor_.beat);
            return;
        }
        ++cursor_.seg;
        cursor_.beat = 0;
    }
    cursor_.frame = kNoClickFrame;
}

void Click::timeline(const PlaybackPlan& plan, const backend::AudioBlock& block, uint64_t t,
                     uint32_t off, uint32_t n) noexcept SONORA_RT {
    if (cursor_.epoch != plan.epoch || cursor_.at != t) seek(plan, t);
    const auto end = static_cast<int64_t>(t + n);
    while (cursor_.frame < end) {
        const ClickSegment& c = plan.click[cursor_.seg];
        const bool accent = (c.first_beat + cursor_.beat % c.beats_per_bar) % c.beats_per_bar == 0;
        trigger(block, off + static_cast<uint32_t>(cursor_.frame - static_cast<int64_t>(t)), accent);
        step(plan);
    }
    cursor_.at = t + n;
}

void Click::start_count_off(const PlaybackPlan& plan, uint64_t playhead,
                            uint32_t nframes) noexcept SONORA_RT {
    set_up_ = true;
    elapsed_ = 0;
    next_ = 0;
    lead_ = 0;
    total_ = 0;
    seg_ = ClickSegment{};
    // The tempo and meter in force where the take starts: the last entry
    // at or before the playhead.
    const auto at = static_cast<int64_t>(playhead);
    uint32_t lo = 0;
    uint32_t hi = plan.click_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (plan.click[mid].at <= at) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0 || nframes == 0) return;
    const ClickSegment& c = plan.click[lo - 1];
    if (c.num == 0 || c.beat_ticks == 0 || c.beats_per_bar == 0) return;
    seg_ = c;
    seg_.at = 0;
    seg_.first_frame = 0;
    seg_.first_tick = 0;
    seg_.first_beat = 0;
    seg_.beats = static_cast<uint64_t>(bars_now_) * c.beats_per_bar;
    const int64_t length = click_beat_frame(seg_, seg_.beats);
    if (length == kNoClickFrame || length <= 0) {
        seg_.beats = 0;
        return;
    }
    const auto len = static_cast<uint64_t>(length);
    // Started this far into its first block, it ends on a block boundary
    // while the period holds, so the take's first frame follows its last
    // beat by exactly one beat.
    lead_ = (nframes - len % nframes) % nframes;
    total_ = lead_ + len;
}

void Click::count_off_beats(const backend::AudioBlock& block, uint32_t nframes) noexcept SONORA_RT {
    const uint64_t end = elapsed_ + nframes;
    while (next_ < seg_.beats) {
        const uint64_t f = lead_ + static_cast<uint64_t>(click_beat_frame(seg_, next_));
        if (f >= end) break;
        trigger(block, static_cast<uint32_t>(f - elapsed_), next_ % seg_.beats_per_bar == 0);
        ++next_;
    }
    elapsed_ = end;
    // A period that changed mid count-off ends it at this block's end.
    if (elapsed_ >= total_) {
        counting_off_ = false;
        counting_.store(false, std::memory_order_relaxed);
    }
}

void Click::sound_to(const backend::AudioBlock& block, uint32_t until) noexcept SONORA_RT {
    if (until > block.nframes) until = block.nframes;
    while (voice_ != nullptr && done_ < until) {
        const uint32_t n = std::min(until - done_, sound_len_ - voice_pos_);
        for (int side = 0; side < 2; ++side) {
            const uint32_t o = out_[side];
            if (o >= block.output_count || (side == 1 && o == out_[0])) continue;
            float* const d = block.outputs[o] + done_;
            for (uint32_t i = 0; i < n; ++i) d[i] += voice_[voice_pos_ + i];
        }
        voice_pos_ += n;
        done_ += n;
        if (voice_pos_ >= sound_len_) voice_ = nullptr;
    }
    done_ = until;
}

void Click::trigger(const backend::AudioBlock& block, uint32_t at, bool accent) noexcept SONORA_RT {
    sound_to(block, at);
    voice_ = accent ? accent_.get() : beat_.get();
    voice_pos_ = 0;
}

}  // namespace sonora
