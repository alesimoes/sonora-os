// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/lossy_encoder.h"

#include <string>
#include <utility>

namespace sonora {

namespace {

// ADR-0182 §2. The order follows LossyCodec's values.
const LossyCodecInfo kCodecs[] = {
    {LossyCodec::Opus, "opus", "Opus", ".opus", "libopus + libogg", "BSD-3-Clause", true},
    {LossyCodec::Vorbis, "ogg", "Ogg Vorbis", ".ogg", "libvorbis + libogg", "BSD-3-Clause",
     true},
    {LossyCodec::Mp3, "mp3", "MP3", ".mp3", "LAME", "LGPL-2.0-or-later", false},
    {LossyCodec::Aac, "aac", "AAC", ".m4a", "FDK AAC", "FDK AAC licence, patent pool", false},
};

constexpr int kMaxChannels = 8;
// Below 6 kb/s no encoder we accept makes a usable stereo file; above
// 512 kb/s a lossless file is the better choice and Opus refuses it anyway.
constexpr uint32_t kMinBitrate = 6000;
constexpr uint32_t kMaxBitrate = 512000;

std::size_t index_of(LossyCodec codec) { return static_cast<std::size_t>(codec); }

bool known(LossyCodec codec) { return index_of(codec) < sizeof kCodecs / sizeof kCodecs[0]; }

}  // namespace

const LossyCodecInfo& lossy_codec_info(LossyCodec codec) {
    return known(codec) ? kCodecs[index_of(codec)] : kCodecs[0];
}

bool parse_lossy_codec(const std::string& word, LossyCodec& out) {
    for (const LossyCodecInfo& info : kCodecs) {
        if (word == info.word) {
            out = info.codec;
            return true;
        }
    }
    return false;
}

bool LossyRegistry::add(LossyCodec codec, LossyEncoderFactory factory, std::string& error) {
    if (!known(codec)) {
        error = "unknown lossy codec";
        return false;
    }
    const LossyCodecInfo& info = kCodecs[index_of(codec)];
    if (!info.accepted) {
        error = std::string(info.name) + " is not shipped: " + info.library + " (" +
                info.licence + ") is rejected for the image (ADR-0182)";
        return false;
    }
    if (!factory) {
        error = std::string(info.name) + " encoder factory is empty";
        return false;
    }
    if (factories_[index_of(codec)]) {
        error = std::string(info.name) + " encoder is already registered";
        return false;
    }
    factories_[index_of(codec)] = std::move(factory);
    return true;
}

bool LossyRegistry::available(LossyCodec codec) const {
    return known(codec) && static_cast<bool>(factories_[index_of(codec)]);
}

std::unique_ptr<LossyEncoder> LossyRegistry::start(LossyCodec codec, LossyByteSink& sink,
                                                   uint32_t sample_rate, int channels,
                                                   uint32_t bitrate, std::string& error) const {
    if (!known(codec)) {
        error = "unknown lossy codec";
        return nullptr;
    }
    const LossyCodecInfo& info = kCodecs[index_of(codec)];
    if (!info.accepted) {
        error = std::string(info.name) + " export is not available: " + info.library + " (" +
                info.licence + ") is rejected for the image";
        return nullptr;
    }
    if (!available(codec)) {
        error = std::string(info.name) + " export is not available in this build (" +
                info.library + " not installed)";
        return nullptr;
    }
    if (sample_rate == 0 || channels < 1 || channels > kMaxChannels) {
        error = std::string(info.name) + " export needs a rate and 1 to 8 channels";
        return nullptr;
    }
    if (bitrate < kMinBitrate || bitrate > kMaxBitrate) {
        error = std::string(info.name) + " bitrate is 6000 to 512000 b/s, not " +
                std::to_string(bitrate);
        return nullptr;
    }
    std::unique_ptr<LossyEncoder> encoder = factories_[index_of(codec)](sink);
    if (!encoder) {
        error = std::string(info.name) + " encoder could not be created";
        return nullptr;
    }
    std::string why;
    if (!encoder->open(sample_rate, channels, bitrate, why)) {
        error = std::string(info.name) + ": " + why;
        return nullptr;
    }
    return encoder;
}

}  // namespace sonora
