// SPDX-License-Identifier: GPL-3.0-or-later
//
// The seam for compressed delivery files (ADR-0182). A bounce renders to
// float once (ADR-0054 §1); a lossy file is one more conversion of that
// render, run by the converter on the bounce's worker thread. Nothing here is
// reachable from Session::process(): an encoder allocates, may take seconds
// and its sink writes a file.
//
// Which codecs a build carries is decided by what registers itself in a
// LossyRegistry. A codec nobody registered is refused when the bounce starts,
// with an error naming it, never replaced by another format: an operator who
// asked for Opus and got a WAV would ship the wrong file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace sonora {

enum class LossyCodec : uint32_t { Opus = 0, Vorbis = 1, Mp3 = 2, Aac = 3 };

// What ADR-0182 records about a codec. `accepted` false: its licence keeps it
// out of the image, so no build may register it.
struct LossyCodecInfo {
    LossyCodec codec;
    const char* word;       // the mix.bounce `file=` value
    const char* name;       // for the operator: "Opus", "MP3"
    const char* extension;  // with its dot
    const char* library;    // what would encode it
    const char* licence;
    bool accepted;
};

const LossyCodecInfo& lossy_codec_info(LossyCodec codec);

// "opus" -> Opus; false for a word that is not a lossy codec (bwf, flac...).
bool parse_lossy_codec(const std::string& word, LossyCodec& out);

// Where an encoder puts its bytes: the export file, or a buffer in a test.
class LossyByteSink {
public:
    virtual ~LossyByteSink() = default;
    // false: the bytes did not all land (a full disk); `error` says why.
    virtual bool write(const uint8_t* data, std::size_t n, std::string& error) = 0;
};

// One file's encoder. Called in this order: open once, write any number of
// times, finish once. After a false return the converter stops and removes
// the file; the encoder is not called again.
class LossyEncoder {
public:
    virtual ~LossyEncoder() = default;
    // `bitrate` is bits per second for the whole stream. An encoder refuses a
    // rate, channel count or bitrate it cannot take, naming what it takes.
    virtual bool open(uint32_t sample_rate, int channels, uint32_t bitrate,
                      std::string& error) = 0;
    // The file's DESCRIPTION tag. Called after open, before the first write,
    // so an encoder writes its headers on the first write or at finish.
    virtual void set_description(const std::string& /*text*/) {}
    // `frames` frames of `channels` interleaved floats at full scale +-1.
    virtual bool write(const float* interleaved, std::size_t frames, std::string& error) = 0;
    // Flushes the encoder and closes the stream (Ogg end-of-stream page).
    virtual bool finish(std::string& error) = 0;
};

using LossyEncoderFactory = std::function<std::unique_ptr<LossyEncoder>(LossyByteSink&)>;

// The codecs this build can write. Filled once at startup, before any bounce,
// then only read; it is not synchronised for writes during a bounce.
class LossyRegistry {
public:
    // Refuses a codec ADR-0182 rejects, and a second factory for one codec.
    bool add(LossyCodec codec, LossyEncoderFactory factory, std::string& error);
    bool available(LossyCodec codec) const;

    // Checks the request, creates the encoder and opens it. Null with
    // `error` naming the codec when it is not in this build, when the request
    // is out of range, or when the encoder refuses it.
    std::unique_ptr<LossyEncoder> start(LossyCodec codec, LossyByteSink& sink,
                                        uint32_t sample_rate, int channels,
                                        uint32_t bitrate, std::string& error) const;

private:
    static constexpr std::size_t kCodecs = 4;
    LossyEncoderFactory factories_[kCodecs];
};

}  // namespace sonora
