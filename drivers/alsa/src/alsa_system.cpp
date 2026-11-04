// SPDX-License-Identifier: GPL-3.0-or-later
//
// libasound behind alsa::System. The only file that includes an ALSA header.
// Written from the alsa-lib PCM documentation and not yet run against a
// physical card here; tools/test-alsa-loopback.sh (snd-aloop) and the UMC1820
// checks in ADR-0053 are what turn that into evidence.
#include <alsa/asoundlib.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "sonora/alsa_card.h"
#include "sonora/alsa_device.h"
#include "sonora/alsa_proc.h"

namespace sonora::alsa {
namespace {

constexpr uint32_t kMaxChannels = 64;

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

// Asks the PCM, if it can be opened without waiting: false when another
// program holds it.
bool probe_channels(int card, snd_pcm_stream_t dir, uint32_t& channels) {
    char name[24];
    std::snprintf(name, sizeof name, "hw:%d,0", card);
    snd_pcm_t* pcm = nullptr;
    if (snd_pcm_open(&pcm, name, dir, SND_PCM_NONBLOCK) < 0) return false;
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    bool ok = snd_pcm_hw_params_any(pcm, hw) >= 0;
    unsigned max = 0;
    ok = ok && snd_pcm_hw_params_get_channels_max(hw, &max) >= 0;
    snd_pcm_close(pcm);
    if (ok) channels = std::min<unsigned>(max, kMaxChannels);
    return ok;
}

Status map_error(long err) {
    switch (-err) {
        case EPIPE:
        case ESTRPIPE:
        case EBADFD:
            return Status::Xrun;
        case EAGAIN:
        case EINTR:
            return Status::Timeout;
        default:
            return Status::Gone;
    }
}

struct Stream {
    snd_pcm_t* pcm = nullptr;
    snd_pcm_format_t format = SND_PCM_FORMAT_UNKNOWN;
    unsigned channels = 0;
    snd_pcm_uframes_t offset = 0;
    ~Stream() {
        if (pcm) snd_pcm_close(pcm);
    }
};

SampleFormat sample_format(snd_pcm_format_t f) {
    switch (f) {
        case SND_PCM_FORMAT_S16_LE: return SampleFormat::S16;
        case SND_PCM_FORMAT_S24_3LE: return SampleFormat::S24_3;
        case SND_PCM_FORMAT_S24_LE: return SampleFormat::S24_4;
        default: return SampleFormat::S32;
    }
}

uint8_t* first_frame(const snd_pcm_channel_area_t* areas, snd_pcm_uframes_t offset) {
    return static_cast<uint8_t*>(areas[0].addr) + (areas[0].first + offset * areas[0].step) / 8;
}

class AlsaDuplex final : public Duplex {
public:
    bool open(const DuplexRequest& req, DuplexInfo& info, std::string& error) override {
        if (!configure(cap_, req.capture_pcm, SND_PCM_STREAM_CAPTURE, req.in_channels, req,
                       req.period_frames, error)) {
            return false;
        }
        // The playback side is asked for the period the capture side was
        // granted: two directions on one card must cycle together.
        if (!configure(play_, req.playback_pcm, SND_PCM_STREAM_PLAYBACK, req.out_channels, req,
                       period_, error)) {
            return false;
        }
        if (period_ != play_period_) {
            error = "capture was granted a period of " + std::to_string(period_) +
                    " frames and playback " + std::to_string(play_period_);
            return false;
        }
        linked_ = snd_pcm_link(cap_.pcm, play_.pcm) == 0;
        info.rate = rate_;
        info.in_channels = cap_.channels;
        info.out_channels = play_.channels;
        info.period_frames = static_cast<uint32_t>(period_);
        info.buffer_frames = static_cast<uint32_t>(std::min(buffer_, play_buffer_));
        info.in_format = sample_format(cap_.format);
        info.out_format = sample_format(play_.format);
        info.linked = linked_;
        buffer_ = std::min(buffer_, play_buffer_);
        return true;
    }

    bool start(std::string& error) override {
        const long err = restart();
        if (err < 0) {
            error = snd_strerror(static_cast<int>(err));
            return false;
        }
        return true;
    }

    Status begin(int timeout_ms, Cycle& cy) noexcept override {
        bool waited = false;
        for (;;) {
            const snd_pcm_sframes_t ca = snd_pcm_avail_update(cap_.pcm);
            if (ca < 0) return map_error(ca);
            const snd_pcm_sframes_t pa = snd_pcm_avail_update(play_.pcm);
            if (pa < 0) return map_error(pa);
            if (static_cast<snd_pcm_uframes_t>(ca) >= period_ &&
                static_cast<snd_pcm_uframes_t>(pa) >= period_) {
                break;
            }
            if (waited) return Status::Timeout;
            const int w = snd_pcm_wait(static_cast<snd_pcm_uframes_t>(ca) < period_ ? cap_.pcm : play_.pcm,
                                       timeout_ms);
            if (w < 0) return map_error(w);
            if (w == 0) return Status::Timeout;
            waited = true;
        }

        cy.stamp_ns = 0;
        snd_pcm_uframes_t avail = 0;
        snd_htimestamp_t ts{};
        if (snd_pcm_htimestamp(cap_.pcm, &avail, &ts) == 0 && (ts.tv_sec != 0 || ts.tv_nsec != 0)) {
            const uint64_t at = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                                static_cast<uint64_t>(ts.tv_nsec);
            const uint64_t back = static_cast<uint64_t>(avail) * 1000000000ull / rate_;
            cy.stamp_ns = at > back ? at - back : 0;
        }

        const snd_pcm_channel_area_t* areas = nullptr;
        snd_pcm_uframes_t frames = period_;
        int r = snd_pcm_mmap_begin(cap_.pcm, &areas, &cap_.offset, &frames);
        if (r < 0) return map_error(r);
        if (frames < period_) {
            snd_pcm_mmap_commit(cap_.pcm, cap_.offset, 0);
            return Status::Xrun;
        }
        cy.capture = first_frame(areas, cap_.offset);

        frames = period_;
        r = snd_pcm_mmap_begin(play_.pcm, &areas, &play_.offset, &frames);
        if (r < 0) {
            snd_pcm_mmap_commit(cap_.pcm, cap_.offset, 0);
            return map_error(r);
        }
        if (frames < period_) {
            snd_pcm_mmap_commit(cap_.pcm, cap_.offset, 0);
            snd_pcm_mmap_commit(play_.pcm, play_.offset, 0);
            return Status::Xrun;
        }
        cy.playback = first_frame(areas, play_.offset);
        cy.frames = static_cast<uint32_t>(period_);
        return Status::Ready;
    }

    Status commit() noexcept override {
        const snd_pcm_sframes_t c = snd_pcm_mmap_commit(cap_.pcm, cap_.offset, period_);
        const snd_pcm_sframes_t p = snd_pcm_mmap_commit(play_.pcm, play_.offset, period_);
        if (c < 0) return map_error(c);
        if (p < 0) return map_error(p);
        if (static_cast<snd_pcm_uframes_t>(c) != period_ ||
            static_cast<snd_pcm_uframes_t>(p) != period_) {
            return Status::Xrun;
        }
        return Status::Ready;
    }

    bool recover() noexcept override {
        for (snd_pcm_t* pcm : {cap_.pcm, play_.pcm}) {
            if (snd_pcm_state(pcm) == SND_PCM_STATE_SUSPENDED) {
                for (int i = 0; i < 10 && snd_pcm_resume(pcm) == -EAGAIN; ++i) usleep(10000);
            }
        }
        return restart() >= 0;
    }

private:
    // Stop both, prepare, queue silence, start. Not noexcept-throwing:
    // returns the first negative ALSA error.
    long restart() noexcept {
        snd_pcm_drop(cap_.pcm);
        snd_pcm_drop(play_.pcm);
        int r = snd_pcm_prepare(cap_.pcm);
        if (r < 0) return r;
        if (!linked_ && (r = snd_pcm_prepare(play_.pcm)) < 0) return r;
        // One period of silence less than the ring: the next begin() then
        // finds exactly one period of room, and the queue never starts empty.
        snd_pcm_uframes_t left = buffer_ - period_;
        while (left > 0) {
            snd_pcm_uframes_t frames = left;
            const snd_pcm_channel_area_t* areas = nullptr;
            snd_pcm_uframes_t offset = 0;
            r = snd_pcm_mmap_begin(play_.pcm, &areas, &offset, &frames);
            if (r < 0) return r;
            snd_pcm_areas_silence(areas, offset, play_.channels, frames, play_.format);
            const snd_pcm_sframes_t done = snd_pcm_mmap_commit(play_.pcm, offset, frames);
            if (done < 0) return done;
            left -= std::min<snd_pcm_uframes_t>(left, static_cast<snd_pcm_uframes_t>(done));
        }
        if (!linked_ && (r = snd_pcm_start(play_.pcm)) < 0) return r;
        return snd_pcm_start(cap_.pcm);
    }

    bool configure(Stream& s, const std::string& name, snd_pcm_stream_t dir, unsigned channels,
                   const DuplexRequest& req, snd_pcm_uframes_t want_period, std::string& error) {
        const char* what = dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback";
        int r = snd_pcm_open(&s.pcm, name.c_str(), dir, SND_PCM_NONBLOCK);
        if (r < 0) {
            error = std::string(what) + " " + name + ": " + snd_strerror(r);
            if (r == -EBUSY) {
                error += " (another program holds the card, usually PipeWire; this backend"
                         " needs the card to itself)";
            }
            return false;
        }
        snd_pcm_hw_params_t* hw;
        snd_pcm_hw_params_alloca(&hw);
        const auto fail = [&](const char* step, int code) {
            error = std::string(what) + " " + name + ": " + step + ": " + snd_strerror(code);
            return false;
        };
        if ((r = snd_pcm_hw_params_any(s.pcm, hw)) < 0) return fail("hw_params_any", r);
        snd_pcm_hw_params_set_rate_resample(s.pcm, hw, 0);
        if ((r = snd_pcm_hw_params_set_access(s.pcm, hw, SND_PCM_ACCESS_MMAP_INTERLEAVED)) < 0) {
            return fail("mmap interleaved access", r);
        }
        bool format_set = false;
        for (snd_pcm_format_t f : {SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_3LE,
                                   SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S16_LE}) {
            if (snd_pcm_hw_params_set_format(s.pcm, hw, f) == 0) {
                s.format = f;
                format_set = true;
                break;
            }
        }
        if (!format_set) return fail("sample format", -EINVAL);
        if ((r = snd_pcm_hw_params_set_channels(s.pcm, hw, channels)) < 0) {
            return fail(("channels " + std::to_string(channels)).c_str(), r);
        }
        s.channels = channels;
        unsigned rate = req.rate;
        if ((r = snd_pcm_hw_params_set_rate_near(s.pcm, hw, &rate, nullptr)) < 0) {
            return fail("rate", r);
        }
        rate_ = rate;
        snd_pcm_uframes_t period = want_period;
        if ((r = snd_pcm_hw_params_set_period_size_near(s.pcm, hw, &period, nullptr)) < 0) {
            return fail("period size", r);
        }
        unsigned periods = req.periods;
        if ((r = snd_pcm_hw_params_set_periods_near(s.pcm, hw, &periods, nullptr)) < 0) {
            return fail("period count", r);
        }
        if ((r = snd_pcm_hw_params(s.pcm, hw)) < 0) return fail("hw_params", r);
        snd_pcm_uframes_t buffer = 0;
        snd_pcm_hw_params_get_period_size(hw, &period, nullptr);
        snd_pcm_hw_params_get_buffer_size(hw, &buffer);

        snd_pcm_sw_params_t* sw;
        snd_pcm_sw_params_alloca(&sw);
        snd_pcm_sw_params_current(s.pcm, sw);
        snd_pcm_uframes_t boundary = 0;
        snd_pcm_sw_params_get_boundary(sw, &boundary);
        snd_pcm_sw_params_set_avail_min(s.pcm, sw, period);
        // Started explicitly after the prefill, never by a first write.
        snd_pcm_sw_params_set_start_threshold(s.pcm, sw, boundary);
        snd_pcm_sw_params_set_tstamp_mode(s.pcm, sw, SND_PCM_TSTAMP_ENABLE);
        snd_pcm_sw_params_set_tstamp_type(s.pcm, sw, SND_PCM_TSTAMP_TYPE_MONOTONIC);
        if ((r = snd_pcm_sw_params(s.pcm, sw)) < 0) return fail("sw_params", r);

        if (dir == SND_PCM_STREAM_CAPTURE) {
            period_ = period;
            buffer_ = buffer;
        } else {
            play_period_ = period;
            play_buffer_ = buffer;
        }
        return true;
    }

    Stream cap_, play_;
    unsigned rate_ = 0;
    snd_pcm_uframes_t period_ = 0, buffer_ = 0, play_period_ = 0, play_buffer_ = 0;
    bool linked_ = false;
};

class AlsaSystem final : public System {
public:
    std::vector<CardInfo> cards() override {
        std::vector<CardInfo> out;
        int index = -1;
        while (snd_card_next(&index) == 0 && index >= 0) {
            CardInfo c;
            c.index = index;
            char hw[16];
            std::snprintf(hw, sizeof hw, "hw:%d", index);
            snd_ctl_t* ctl = nullptr;
            if (snd_ctl_open(&ctl, hw, 0) < 0) continue;
            snd_ctl_card_info_t* ci;
            snd_ctl_card_info_alloca(&ci);
            bool has_capture = false, has_playback = false;
            if (snd_ctl_card_info(ctl, ci) == 0) {
                c.id = snd_ctl_card_info_get_id(ci);
                c.name = snd_ctl_card_info_get_name(ci);
                snd_pcm_info_t* pi;
                snd_pcm_info_alloca(&pi);
                snd_pcm_info_set_device(pi, 0);
                snd_pcm_info_set_subdevice(pi, 0);
                snd_pcm_info_set_stream(pi, SND_PCM_STREAM_CAPTURE);
                has_capture = snd_ctl_pcm_info(ctl, pi) == 0;
                snd_pcm_info_set_stream(pi, SND_PCM_STREAM_PLAYBACK);
                has_playback = snd_ctl_pcm_info(ctl, pi) == 0;
            }
            snd_ctl_close(ctl);
            if (c.id.empty()) continue;

            c.usb = drivers::alsa_card_is_usb(std::to_string(index));
            // /proc first for USB cards: it answers while PipeWire holds them.
            if (c.usb) {
                const StreamSummary s =
                    parse_stream0(read_file("/proc/asound/card" + std::to_string(index) + "/stream0"));
                c.capture_channels = s.capture_channels;
                c.playback_channels = s.playback_channels;
                c.rates = s.rates;
            }
            if (has_capture && c.capture_channels == 0) {
                probe_channels(index, SND_PCM_STREAM_CAPTURE, c.capture_channels);
            }
            if (has_playback && c.playback_channels == 0) {
                probe_channels(index, SND_PCM_STREAM_PLAYBACK, c.playback_channels);
            }
            if (!has_capture) c.capture_channels = 0;
            if (!has_playback) c.playback_channels = 0;
            c.capture_channels = std::min(c.capture_channels, kMaxChannels);
            c.playback_channels = std::min(c.playback_channels, kMaxChannels);
            c.capture_pcm = c.playback_pcm = "hw:CARD=" + c.id + ",DEV=0";
            // snd-aloop's two ends are different devices and a loopback test
            // cannot use one card both ways; every other card keeps the default.
            if (const char* e = std::getenv("SONORA_ALSA_CAPTURE_PCM")) c.capture_pcm = e;
            if (const char* e = std::getenv("SONORA_ALSA_PLAYBACK_PCM")) c.playback_pcm = e;
            out.push_back(std::move(c));
        }
        return out;
    }

    std::unique_ptr<Duplex> make_duplex() override { return std::make_unique<AlsaDuplex>(); }
};

}  // namespace

std::unique_ptr<System> make_system() { return std::make_unique<AlsaSystem>(); }

}  // namespace sonora::alsa
