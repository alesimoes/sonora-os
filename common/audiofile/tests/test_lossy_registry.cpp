// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0182: a codec the build does not carry is refused at the start of a
// bounce with its name in the error, never swapped for another format; a
// codec whose licence is rejected cannot be registered at all; a registered
// one is created, opened with what was asked and fed through the seam.
#include "sonora/lossy_encoder.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            return 1;                                                              \
        }                                                                          \
    } while (0)

namespace {

using namespace sonora;

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

class BufferSink : public LossyByteSink {
public:
    bool write(const uint8_t* data, std::size_t n, std::string&) override {
        bytes.insert(bytes.end(), data, data + n);
        return true;
    }
    std::vector<uint8_t> bytes;
};

// Writes a header at open, the frame count of each block, a trailer at finish.
struct FakeLog {
    uint32_t rate = 0;
    int channels = 0;
    uint32_t bitrate = 0;
    std::size_t frames = 0;
    bool finished = false;
};

class FakeEncoder : public LossyEncoder {
public:
    FakeEncoder(LossyByteSink& sink, FakeLog& log, uint32_t only_rate)
        : sink_(sink), log_(log), only_rate_(only_rate) {}

    bool open(uint32_t sample_rate, int channels, uint32_t bitrate, std::string& error) override {
        if (only_rate_ != 0 && sample_rate != only_rate_) {
            error = "takes 48000 Hz only";
            return false;
        }
        log_.rate = sample_rate;
        log_.channels = channels;
        log_.bitrate = bitrate;
        const uint8_t head[4] = {'F', 'A', 'K', 'E'};
        return sink_.write(head, sizeof head, error);
    }
    bool write(const float*, std::size_t frames, std::string& error) override {
        log_.frames += frames;
        const uint8_t n = static_cast<uint8_t>(frames);
        return sink_.write(&n, 1, error);
    }
    bool finish(std::string& error) override {
        log_.finished = true;
        const uint8_t tail[3] = {'E', 'O', 'S'};
        return sink_.write(tail, sizeof tail, error);
    }

private:
    LossyByteSink& sink_;
    FakeLog& log_;
    uint32_t only_rate_;
};

LossyEncoderFactory fake_factory(FakeLog& log, uint32_t only_rate = 0) {
    return [&log, only_rate](LossyByteSink& sink) {
        return std::unique_ptr<LossyEncoder>(new FakeEncoder(sink, log, only_rate));
    };
}

int test_licence_record() {
    const LossyCodecInfo& opus = lossy_codec_info(LossyCodec::Opus);
    CHECK(opus.accepted && contains(opus.licence, "BSD-3") && contains(opus.library, "libopus"));
    const LossyCodecInfo& vorbis = lossy_codec_info(LossyCodec::Vorbis);
    CHECK(vorbis.accepted && contains(vorbis.licence, "BSD-3") &&
          contains(vorbis.library, "libvorbis"));
    CHECK(!lossy_codec_info(LossyCodec::Mp3).accepted);
    CHECK(!lossy_codec_info(LossyCodec::Aac).accepted);

    LossyCodec c = LossyCodec::Aac;
    CHECK(parse_lossy_codec("opus", c) && c == LossyCodec::Opus);
    CHECK(parse_lossy_codec("ogg", c) && c == LossyCodec::Vorbis);
    CHECK(!parse_lossy_codec("flac", c));
    CHECK(!parse_lossy_codec("", c));
    return 0;
}

int test_unavailable_codec_refused_at_start() {
    LossyRegistry registry;
    BufferSink sink;
    std::string error;
    CHECK(!registry.available(LossyCodec::Opus));
    CHECK(registry.start(LossyCodec::Opus, sink, 48000, 2, 128000, error) == nullptr);
    CHECK(contains(error, "Opus") && contains(error, "not available"));
    CHECK(sink.bytes.empty());

    // Opus registered does not make Vorbis appear, nor stand in for it.
    FakeLog log;
    CHECK(registry.add(LossyCodec::Opus, fake_factory(log), error));
    error.clear();
    CHECK(registry.start(LossyCodec::Vorbis, sink, 48000, 2, 128000, error) == nullptr);
    CHECK(contains(error, "Ogg Vorbis") && contains(error, "not available"));
    CHECK(sink.bytes.empty() && log.rate == 0);
    return 0;
}

int test_rejected_licence_cannot_register() {
    LossyRegistry registry;
    FakeLog log;
    std::string error;
    CHECK(!registry.add(LossyCodec::Mp3, fake_factory(log), error));
    CHECK(contains(error, "MP3") && contains(error, "LGPL"));
    error.clear();
    CHECK(!registry.add(LossyCodec::Aac, fake_factory(log), error));
    CHECK(contains(error, "AAC"));
    CHECK(!registry.available(LossyCodec::Mp3) && !registry.available(LossyCodec::Aac));

    BufferSink sink;
    error.clear();
    CHECK(registry.start(LossyCodec::Mp3, sink, 48000, 2, 192000, error) == nullptr);
    CHECK(contains(error, "MP3") && contains(error, "rejected"));
    return 0;
}

int test_registered_codec_runs_through_the_seam() {
    LossyRegistry registry;
    FakeLog log;
    std::string error;
    CHECK(registry.add(LossyCodec::Opus, fake_factory(log), error));
    CHECK(!registry.add(LossyCodec::Opus, fake_factory(log), error));
    CHECK(contains(error, "already"));
    CHECK(registry.available(LossyCodec::Opus));

    BufferSink sink;
    error.clear();
    std::unique_ptr<LossyEncoder> enc =
        registry.start(LossyCodec::Opus, sink, 48000, 2, 160000, error);
    CHECK(enc != nullptr && error.empty());
    CHECK(log.rate == 48000 && log.channels == 2 && log.bitrate == 160000);

    const float block[2 * 100] = {};
    CHECK(enc->write(block, 100, error));
    CHECK(enc->write(block, 28, error));
    CHECK(enc->finish(error));
    CHECK(log.frames == 128 && log.finished);
    const std::vector<uint8_t> want = {'F', 'A', 'K', 'E', 100, 28, 'E', 'O', 'S'};
    CHECK(sink.bytes == want);
    return 0;
}

int test_bad_requests_refused_before_the_encoder() {
    LossyRegistry registry;
    FakeLog log;
    std::string error;
    CHECK(registry.add(LossyCodec::Vorbis, fake_factory(log, 48000), error));
    BufferSink sink;

    CHECK(registry.start(LossyCodec::Vorbis, sink, 48000, 0, 128000, error) == nullptr);
    CHECK(contains(error, "channels"));
    CHECK(registry.start(LossyCodec::Vorbis, sink, 48000, 9, 128000, error) == nullptr);
    CHECK(registry.start(LossyCodec::Vorbis, sink, 0, 2, 128000, error) == nullptr);
    CHECK(registry.start(LossyCodec::Vorbis, sink, 48000, 2, 1000, error) == nullptr);
    CHECK(contains(error, "bitrate") && contains(error, "1000"));
    CHECK(registry.start(LossyCodec::Vorbis, sink, 48000, 2, 900000, error) == nullptr);
    CHECK(log.rate == 0 && sink.bytes.empty());

    // The encoder's own refusal reaches the operator under the codec's name.
    error.clear();
    CHECK(registry.start(LossyCodec::Vorbis, sink, 44100, 2, 128000, error) == nullptr);
    CHECK(contains(error, "Ogg Vorbis") && contains(error, "48000"));
    CHECK(sink.bytes.empty());
    return 0;
}

}  // namespace

int main() {
    if (test_licence_record() != 0) return 1;
    if (test_unavailable_codec_refused_at_start() != 0) return 1;
    if (test_rejected_licence_cannot_register() != 0) return 1;
    if (test_registered_codec_runs_through_the_seam() != 0) return 1;
    if (test_bad_requests_refused_before_the_encoder() != 0) return 1;
    std::printf("lossy_registry: ok\n");
    return 0;
}
