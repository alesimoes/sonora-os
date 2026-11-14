// SPDX-License-Identifier: GPL-3.0-or-later
// Ogg Opus (RFC 7845) over libopus and libogg, behind the lossy seam
// (ADR-0182). Worker thread only.
#ifdef SONORA_HAVE_OPUS

#include <opus/opus.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "lossy_codecs.h"
#include "ogg_stream.h"

namespace sonora {

namespace {

constexpr uint32_t kOpusRate = 48000;   // the rate the converter resamples to
constexpr int kFrame = 960;             // 20 ms, the best quality per bit
constexpr int kMaxPacket = 4000;

void put16(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
    put16(b, v & 0xFFFF);
    put16(b, v >> 16);
}
void put_text(std::vector<uint8_t>& b, const std::string& s) {
    put32(b, static_cast<uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

class OpusFileEncoder : public LossyEncoder {
public:
    explicit OpusFileEncoder(LossyByteSink& sink) : ogg_(sink) {}
    ~OpusFileEncoder() override {
        if (enc_) opus_encoder_destroy(enc_);
    }

    bool open(uint32_t sample_rate, int channels, uint32_t bitrate, std::string& error) override {
        if (sample_rate != kOpusRate) {
            error = "takes 48000 Hz only, not " + std::to_string(sample_rate);
            return false;
        }
        if (channels < 1 || channels > 2) {
            error = "takes mono or stereo, not " + std::to_string(channels) + " channels";
            return false;
        }
        int err = 0;
        enc_ = opus_encoder_create(static_cast<opus_int32>(kOpusRate), channels,
                                   OPUS_APPLICATION_AUDIO, &err);
        if (err != OPUS_OK || !enc_) {
            enc_ = nullptr;
            error = std::string("libopus: ") + opus_strerror(err);
            return false;
        }
        opus_encoder_ctl(enc_, OPUS_SET_BITRATE(static_cast<opus_int32>(bitrate)));
        opus_int32 look = 0;
        opus_encoder_ctl(enc_, OPUS_GET_LOOKAHEAD(&look));
        preskip_ = static_cast<uint32_t>(look);
        channels_ = channels;
        pending_.assign(static_cast<std::size_t>(kFrame) * channels, 0.0f);
        ogg_.init();
        return true;
    }

    void set_description(const std::string& text) override { description_ = text; }

    bool write(const float* in, std::size_t frames, std::string& error) override {
        if (!headers_done_ && !headers(error)) return false;
        while (frames > 0) {
            const std::size_t room = static_cast<std::size_t>(kFrame) - filled_;
            const std::size_t k = frames < room ? frames : room;
            std::memcpy(pending_.data() + filled_ * channels_, in, k * channels_ * sizeof(float));
            filled_ += k;
            in += k * channels_;
            frames -= k;
            if (filled_ == static_cast<std::size_t>(kFrame) && !packet(false, error)) return false;
        }
        return true;
    }

    bool finish(std::string& error) override {
        if (!headers_done_ && !headers(error)) return false;
        // The last frame is zero padded; the granule position says how much
        // of it is audio, so the decoder trims the rest.
        std::fill(pending_.begin() + static_cast<std::ptrdiff_t>(filled_ * channels_),
                  pending_.end(), 0.0f);
        return packet(true, error);
    }

private:
    bool headers(std::string& error) {
        std::vector<uint8_t> head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1,
                                     static_cast<uint8_t>(channels_)};
        put16(head, preskip_);
        put32(head, kOpusRate);
        put16(head, 0);
        head.push_back(0);
        std::vector<uint8_t> tags = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
        put_text(tags, "Sonora");
        const bool has_desc = !description_.empty();
        put32(tags, has_desc ? 1u : 0u);
        if (has_desc) put_text(tags, "DESCRIPTION=" + description_);
        if (!submit(head, 0, false, true, error)) return false;
        if (!submit(tags, 0, false, true, error)) return false;
        headers_done_ = true;
        return true;
    }

    bool submit(std::vector<uint8_t>& data, int64_t granule, bool eos, bool force,
                std::string& error) {
        ogg_packet op{};
        op.packet = data.data();
        op.bytes = static_cast<long>(data.size());
        op.b_o_s = packets_ == 0 ? 1 : 0;
        op.e_o_s = eos ? 1 : 0;
        op.granulepos = granule;
        op.packetno = packets_++;
        ogg_stream_packetin(ogg_.state(), &op);
        return ogg_.pages(force || eos, error);
    }

    bool packet(bool last, std::string& error) {
        std::vector<uint8_t> out(kMaxPacket);
        const int n = opus_encode_float(enc_, pending_.data(), kFrame, out.data(), kMaxPacket);
        if (n < 0) {
            error = std::string("libopus: ") + opus_strerror(n);
            return false;
        }
        out.resize(static_cast<std::size_t>(n));
        samples_ += static_cast<int64_t>(last ? filled_ : static_cast<std::size_t>(kFrame));
        filled_ = 0;
        return submit(out, samples_ + preskip_, last, false, error);
    }

    OggOut ogg_;
    OpusEncoder* enc_ = nullptr;
    int channels_ = 0;
    uint32_t preskip_ = 0;
    std::vector<float> pending_;
    std::size_t filled_ = 0;
    int64_t samples_ = 0;
    int64_t packets_ = 0;
    bool headers_done_ = false;
    std::string description_;
};

}  // namespace

std::unique_ptr<LossyEncoder> make_opus_encoder(LossyByteSink& sink) {
    return std::unique_ptr<LossyEncoder>(new OpusFileEncoder(sink));
}

}  // namespace sonora

#endif  // SONORA_HAVE_OPUS
