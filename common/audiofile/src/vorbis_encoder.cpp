// SPDX-License-Identifier: GPL-3.0-or-later
// Ogg Vorbis over libvorbis, libvorbisenc and libogg, behind the lossy seam
// (ADR-0182). Worker thread only. Any rate libvorbisenc takes is written as
// it is.
#ifdef SONORA_HAVE_VORBIS

#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>

#include "lossy_codecs.h"
#include "ogg_stream.h"

namespace sonora {

namespace {

class VorbisFileEncoder : public LossyEncoder {
public:
    explicit VorbisFileEncoder(LossyByteSink& sink) : ogg_(sink) {}
    ~VorbisFileEncoder() override {
        if (dsp_ready_) {
            vorbis_block_clear(&vb_);
            vorbis_dsp_clear(&vd_);
        }
        if (info_ready_) {
            vorbis_comment_clear(&vc_);
            vorbis_info_clear(&vi_);
        }
    }

    bool open(uint32_t sample_rate, int channels, uint32_t bitrate, std::string& error) override {
        if (channels < 1 || channels > 2) {
            error = "takes mono or stereo, not " + std::to_string(channels) + " channels";
            return false;
        }
        vorbis_info_init(&vi_);
        vorbis_comment_init(&vc_);
        info_ready_ = true;
        if (vorbis_encode_init(&vi_, channels, static_cast<long>(sample_rate), -1,
                               static_cast<long>(bitrate), -1) != 0) {
            error = "libvorbis cannot encode " + std::to_string(channels) + " channels at " +
                    std::to_string(sample_rate) + " Hz and " + std::to_string(bitrate) + " b/s";
            return false;
        }
        vorbis_analysis_init(&vd_, &vi_);
        vorbis_block_init(&vd_, &vb_);
        dsp_ready_ = true;
        channels_ = channels;
        ogg_.init();
        return true;
    }

    void set_description(const std::string& text) override { description_ = text; }

    bool write(const float* in, std::size_t frames, std::string& error) override {
        if (!headers_done_ && !headers(error)) return false;
        while (frames > 0) {
            const std::size_t k = frames < kChunk ? frames : kChunk;
            float** buf = vorbis_analysis_buffer(&vd_, static_cast<int>(k));
            for (std::size_t i = 0; i < k; ++i) {
                for (int c = 0; c < channels_; ++c) buf[c][i] = in[i * channels_ + c];
            }
            vorbis_analysis_wrote(&vd_, static_cast<int>(k));
            if (!drain(error)) return false;
            in += k * channels_;
            frames -= k;
        }
        return true;
    }

    bool finish(std::string& error) override {
        if (!headers_done_ && !headers(error)) return false;
        vorbis_analysis_wrote(&vd_, 0);
        return drain(error);
    }

private:
    static constexpr std::size_t kChunk = 4096;

    bool headers(std::string& error) {
        vorbis_comment_add_tag(&vc_, "ENCODER", "Sonora");
        if (!description_.empty()) vorbis_comment_add_tag(&vc_, "DESCRIPTION", description_.c_str());
        ogg_packet id{}, comment{}, code{};
        vorbis_analysis_headerout(&vd_, &vc_, &id, &comment, &code);
        ogg_stream_packetin(ogg_.state(), &id);
        if (!ogg_.pages(true, error)) return false;
        ogg_stream_packetin(ogg_.state(), &comment);
        ogg_stream_packetin(ogg_.state(), &code);
        headers_done_ = true;
        return ogg_.pages(true, error);
    }

    bool drain(std::string& error) {
        while (vorbis_analysis_blockout(&vd_, &vb_) == 1) {
            vorbis_analysis(&vb_, nullptr);
            vorbis_bitrate_addblock(&vb_);
            ogg_packet op;
            while (vorbis_bitrate_flushpacket(&vd_, &op)) {
                ogg_stream_packetin(ogg_.state(), &op);
                if (!ogg_.pages(false, error)) return false;
            }
        }
        return true;
    }

    OggOut ogg_;
    vorbis_info vi_{};
    vorbis_comment vc_{};
    vorbis_dsp_state vd_{};
    vorbis_block vb_{};
    bool info_ready_ = false;
    bool dsp_ready_ = false;
    bool headers_done_ = false;
    int channels_ = 0;
    std::string description_;
};

}  // namespace

std::unique_ptr<LossyEncoder> make_vorbis_encoder(LossyByteSink& sink) {
    return std::unique_ptr<LossyEncoder>(new VorbisFileEncoder(sink));
}

}  // namespace sonora

#endif  // SONORA_HAVE_VORBIS
