// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/wav_writer.h"

#include "sonora/wav_reader.h"

#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <vector>

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

std::vector<uint8_t> slurp(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> out(n > 0 ? static_cast<std::size_t>(n) : 0);
    // A short read would hand the checks a zero-filled tail that looks like
    // silence; an empty result fails them visibly instead.
    if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    std::fclose(f);
    return out;
}

// Finds a RIFF chunk by tag, returning the offset of its payload.
long find_chunk(const std::vector<uint8_t>& b, const char tag[4]) {
    for (std::size_t i = 12; i + 8 <= b.size();) {
        uint32_t size = 0;
        std::memcpy(&size, b.data() + i + 4, 4);
        if (std::memcmp(b.data() + i, tag, 4) == 0) {
            return static_cast<long>(i + 8);
        }
        i += 8 + size + (size & 1);
    }
    return -1;
}

uint32_t read_u32(const std::vector<uint8_t>& b, long off) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

// 2026-03-07 09:05:02, picked so every field needs its leading zero.
bool fixed_clock(std::tm& out) {
    out = std::tm{};
    out.tm_year = 2026 - 1900;
    out.tm_mon = 3 - 1;
    out.tm_mday = 7;
    out.tm_hour = 9;
    out.tm_min = 5;
    out.tm_sec = 2;
    return true;
}

bool broken_clock(std::tm&) { return false; }

// RIFF ckSize of a file with no audio: "WAVE", then the bext, fmt and data
// chunk headers and payloads up to the first sample.
constexpr uint32_t kEmptyRiffSize = 4 + (8 + 602) + (8 + 16) + 8;

}  // namespace

int main() {
    const std::string path = "test_out.wav";

    // --- a 24-bit mono file has the sizes and payload we expect ---
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        std::vector<float> frames{0.0f, 0.5f, -0.5f, 1.0f, -1.0f};
        CHECK(w.write(frames.data(), frames.size()));
        CHECK(w.frames_written() == 5);
        CHECK(w.close());

        const auto b = slurp(path);
        CHECK(!b.empty());
        CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
        CHECK(std::memcmp(b.data() + 8, "WAVE", 4) == 0);

        const long data = find_chunk(b, "data");
        CHECK(data > 0);
        CHECK(read_u32(b, data - 4) == 5 * 3);  // 5 frames, 3 bytes each
        // The RIFF size field must match the real file length minus 8.
        CHECK(read_u32(b, 4) == b.size() - 8);

        // 15 bytes of data is odd, so RIFF wants one zero pad byte after it
        // that the data chunk's own size does not count. Without it a reader
        // walking the chunks lands one byte short of the next one.
        CHECK(b.size() == static_cast<std::size_t>(data) + 15 + 1);
        CHECK(b.back() == 0x00);

        // Full-scale positive clamps to 0x7FFFFF, not a wrapped negative.
        const long fs = data + 3 * 3;
        CHECK(b[fs] == 0xFF && b[fs + 1] == 0xFF && b[fs + 2] == 0x7F);
        // Full-scale negative clamps to 0x800000.
        const long fsn = data + 4 * 3;
        CHECK(b[fsn] == 0x00 && b[fsn + 1] == 0x00 && b[fsn + 2] == 0x80);
    }

    // --- the bext chunk carries the timecode origin, including the high word
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        const uint64_t origin = (1ull << 32) | 2ull;
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, origin));
        CHECK(w.close());

        const auto b = slurp(path);
        const long bext = find_chunk(b, "bext");
        CHECK(bext > 0);
        CHECK(read_u32(b, bext + 338) == 2);  // low word
        CHECK(read_u32(b, bext + 342) == 1);  // high word
    }

    // --- an origin learnt after open() is the one close() leaves, and the
    // audio after the header is untouched by the patch
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 5));
        std::vector<float> frames{0.5f, -0.5f};
        CHECK(w.write(frames.data(), frames.size()));
        w.set_time_reference((3ull << 32) | 4ull);
        CHECK(w.close());

        const auto b = slurp(path);
        const long bext = find_chunk(b, "bext");
        CHECK(bext > 0);
        CHECK(read_u32(b, bext + 338) == 4);
        CHECK(read_u32(b, bext + 342) == 3);
        const long data = find_chunk(b, "data");
        CHECK(read_u32(b, data - 4) == 2 * 3);
        CHECK(b.size() == static_cast<std::size_t>(data) + 2 * 3);
        CHECK(read_u32(b, 4) == b.size() - 8);
    }

    // --- the Description given before open() is the bext's first 256 bytes,
    // NUL-padded, and reads back; one too long is cut at 256, never spilling
    // into Originator
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        w.set_description("sonora take=3 track=7 session=Song punch=96000-144000");
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        CHECK(w.close());
        const auto b = slurp(path);
        const long bext = find_chunk(b, "bext");
        CHECK(bext > 0);
        const char want[] = "sonora take=3 track=7 session=Song punch=96000-144000";
        CHECK(std::memcmp(b.data() + bext, want, sizeof want) == 0);
        sonora::WavReader r;
        std::string error;
        CHECK(r.open(path, error));
        CHECK(r.description() == want);
        r.close();

        std::remove(path.c_str());
        w.set_description(std::string(300, 'x'));
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        CHECK(w.close());
        const auto c = slurp(path);
        const long bext2 = find_chunk(c, "bext");
        CHECK(c[bext2 + 255] == 'x');
        CHECK(std::memcmp(c.data() + bext2 + 256, "Sonora Engine", 13) == 0);
        CHECK(r.open(path, error));
        CHECK(r.description() == std::string(256, 'x'));
    }

    // --- a NaN sample becomes silence instead of garbage ---
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        const float nan_sample = std::numeric_limits<float>::quiet_NaN();
        std::vector<float> frames{nan_sample, 0.5f};
        CHECK(w.write(frames.data(), frames.size()));
        CHECK(w.close());

        const auto b = slurp(path);
        const long data = find_chunk(b, "data");
        CHECK(b[data] == 0x00 && b[data + 1] == 0x00 && b[data + 2] == 0x00);
    }

    // --- float32 writes 4 bytes per sample and reports IEEE float ---
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        CHECK(w.open(path, 96000, 2, sonora::WavWriter::Format::Float32, 0));
        std::vector<float> stereo{0.25f, -0.25f, 0.5f, -0.5f};  // 2 frames
        CHECK(w.write(stereo.data(), 2));
        CHECK(w.close());

        const auto b = slurp(path);
        const long fmt = find_chunk(b, "fmt ");
        CHECK(fmt > 0);
        uint16_t audio_format = 0;
        std::memcpy(&audio_format, b.data() + fmt, 2);
        CHECK(audio_format == 3);  // IEEE float
        const long data = find_chunk(b, "data");
        CHECK(read_u32(b, data - 4) == 2 * 2 * 4);
    }

    // --- double close is a no-op, not a crash or a corrupt header ---
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        CHECK(w.close());
        CHECK(w.close());
        CHECK(!w.is_open());
    }

    // --- an existing file is never truncated ---
    //
    // open() used fopen("wb"), so a take numbered like one already on disk
    // replaced it without a word: record take 1, restart the engine, record
    // again, and the first performance was gone.
    {
        std::remove(path.c_str());
        std::FILE* f = std::fopen(path.c_str(), "wb");
        CHECK(f != nullptr);
        std::fputs("the previous take", f);
        std::fclose(f);

        sonora::WavWriter w;
        CHECK(!w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        CHECK(!w.is_open());
        const auto b = slurp(path);
        CHECK(std::string(b.begin(), b.end()) == "the previous take");
    }

    // --- a header that cannot reach the disk fails open(), with its reason ---
    //
    // The header sat in stdio's buffer, so open() reported success on a full
    // disk and the take failed later, in the disk thread, with nobody told.
    // And the reason start_recording() puts on screen is errno, read after
    // open()'s cleanup, which C and POSIX allow to change it. A file-size
    // limit below the header's size stands in for a full disk.
    {
        std::remove(path.c_str());
        rlimit before{};
        CHECK(::getrlimit(RLIMIT_FSIZE, &before) == 0);
        // Past the limit the kernel sends SIGXFSZ, which would kill the test
        // instead of failing the write.
        const auto old_handler = std::signal(SIGXFSZ, SIG_IGN);
        rlimit tiny = before;
        tiny.rlim_cur = 100;
        CHECK(::setrlimit(RLIMIT_FSIZE, &tiny) == 0);

        sonora::WavWriter w;
        errno = 0;
        const bool opened =
            w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0);
        const int err = errno;

        CHECK(::setrlimit(RLIMIT_FSIZE, &before) == 0);
        std::signal(SIGXFSZ, old_handler);
        CHECK(!opened);
        CHECK(!w.is_open());
        CHECK(err == EFBIG);
        // A header with no audio behind it would only hold the take number.
        CHECK(::access(path.c_str(), F_OK) != 0);
    }

    // --- bext carries the creation date and time, from the local clock ---
    //
    // The writer zero-filled OriginationDate and OriginationTime, which EBU
    // Tech 3285 v2 says "shall" hold 10 and 8 ASCII characters, so every
    // reader showed a blank creation date (brain/research/bwf-interop.md).
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        w.set_clock(&fixed_clock);
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        CHECK(w.close());

        const auto b = slurp(path);
        const long bext = find_chunk(b, "bext");
        CHECK(bext > 0);
        CHECK(std::memcmp(b.data() + bext + 320, "2026-03-07", 10) == 0);
        CHECK(std::memcmp(b.data() + bext + 330, "09:05:02", 8) == 0);
        // The fields are fixed-width and not terminated: TimeReference
        // follows at once and must be untouched.
        CHECK(read_u32(b, bext + 338) == 0);
    }

    // --- with no clock the fields stay zero, and the take still records ---
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        w.set_clock(&broken_clock);
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        CHECK(w.close());

        const auto b = slurp(path);
        const long bext = find_chunk(b, "bext");
        CHECK(bext > 0);
        for (long i = 320; i < 338; ++i) CHECK(b[bext + i] == 0);
    }

    // --- the default clock writes a well-formed date and time ---
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        CHECK(w.close());

        const auto b = slurp(path);
        const long bext = find_chunk(b, "bext");
        CHECK(bext > 0);
        const char* shape = "dddd-dd-dddd:dd:dd";
        for (int i = 0; i < 18; ++i) {
            const uint8_t c = b[bext + 320 + i];
            if (shape[i] == 'd') {
                CHECK(c >= '0' && c <= '9');
            } else {
                CHECK(c == static_cast<uint8_t>(shape[i]));
            }
        }
    }

    // --- past the 32-bit size limit the writer stops, it never wraps ---
    //
    // close() cast the RIFF and data sizes to 32 bits unchecked, so a take
    // past 4 GiB wrote a wrapped header and readers saw a short or garbled
    // file. A small limit stands in for 2^32 - 1.
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        // Room for exactly 10 mono 24-bit frames.
        w.set_riff_size_limit(kEmptyRiffSize + 10 * 3);
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        const std::vector<float> block(8, 0.25f);
        CHECK(w.write(block.data(), 8));
        CHECK(!w.failed());
        // Only 2 of these 8 fit. They are written; the rest are refused.
        CHECK(!w.write(block.data(), 8));
        CHECK(w.failed());
        CHECK(w.failure() == "take reached the 4 GB WAV limit");
        CHECK(w.frames_written() == 10);
        // Later blocks change nothing.
        CHECK(!w.write(block.data(), 8));
        CHECK(w.frames_written() == 10);
        CHECK(w.close());
        CHECK(w.failure() == "take reached the 4 GB WAV limit");

        const auto b = slurp(path);
        const long data = find_chunk(b, "data");
        CHECK(data > 0);
        CHECK(read_u32(b, data - 4) == 10 * 3);
        CHECK(read_u32(b, 4) == kEmptyRiffSize + 10 * 3);
        CHECK(read_u32(b, 4) == b.size() - 8);

        // The next take on the same writer starts clean.
        std::remove(path.c_str());
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        CHECK(!w.failed());
        CHECK(w.failure().empty());
        CHECK(w.close());
    }

    // --- the limit counts the pad byte, and never splits a frame ---
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        // 5 mono 24-bit frames are 15 bytes and need a pad byte: 16 do not
        // fit in 15, so only 4 frames may be written.
        w.set_riff_size_limit(kEmptyRiffSize + 15);
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        const std::vector<float> block(5, 0.25f);
        CHECK(!w.write(block.data(), 5));
        CHECK(w.frames_written() == 4);
        CHECK(w.close());
        const auto b = slurp(path);
        CHECK(read_u32(b, 4) == b.size() - 8);
        CHECK(read_u32(b, 4) <= kEmptyRiffSize + 15);

        // Stereo float: 8 bytes a frame, 20 bytes of room, 2 whole frames.
        std::remove(path.c_str());
        sonora::WavWriter s;
        s.set_riff_size_limit(kEmptyRiffSize + 20);
        CHECK(s.open(path, 48000, 2, sonora::WavWriter::Format::Float32, 0));
        const std::vector<float> stereo(2 * 3, 0.25f);
        CHECK(!s.write(stereo.data(), 3));
        CHECK(s.frames_written() == 2);
        CHECK(s.close());
        const auto c = slurp(path);
        const long data = find_chunk(c, "data");
        CHECK(data > 0);
        CHECK(read_u32(c, data - 4) == 2 * 8);
        CHECK(read_u32(c, 4) == c.size() - 8);
    }

    // --- a checkpoint leaves a file any reader can open, mid-take ---
    //
    // Until close() the sizes were zero, so a take whose engine died before
    // closing it read as empty, whatever it held (ADR-0007 Consequences,
    // "the disk thread periodically patching the WAV header").
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        w.set_clock(fixed_clock);
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Float32, 7));
        std::vector<float> ramp(100);
        for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) / 128.0f;
        CHECK(w.write(ramp.data(), 60));
        w.set_time_reference(4800);
        CHECK(w.checkpoint());

        // Another handle, as a reader after a crash would have.
        auto b = slurp(path);
        long data = find_chunk(b, "data");
        CHECK(data > 0);
        CHECK(read_u32(b, data - 4) == 60 * 4);
        CHECK(read_u32(b, 4) == b.size() - 8);
        sonora::WavReader r;
        std::string error;
        CHECK(r.open(path, error));
        CHECK(r.frames() == 60);
        CHECK(r.time_reference() == 4800);
        std::vector<float> got(60);
        CHECK(r.read(0, 60, got.data()) == 60);
        for (int i = 0; i < 60; ++i) CHECK(got[i] == ramp[i]);
        r.close();

        // The take carries on where it was, not over the header.
        CHECK(w.write(ramp.data() + 60, 40));
        b = slurp(path);
        data = find_chunk(b, "data");
        CHECK(read_u32(b, data - 4) == 60 * 4);
        CHECK(w.close());
        b = slurp(path);
        CHECK(read_u32(b, data - 4) == 100 * 4);
        CHECK(read_u32(b, 4) == b.size() - 8);
        CHECK(r.open(path, error));
        CHECK(r.frames() == 100);
        got.resize(100);
        CHECK(r.read(0, 100, got.data()) == 100);
        for (int i = 0; i < 100; ++i) CHECK(got[i] == ramp[i]);
        r.close();
    }

    // --- odd-sized data: the checkpoint's header matches the file ---
    //
    // The pad byte goes on at close(); until then a RIFF size that counted
    // it would claim a byte the file does not have.
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        const std::vector<float> block(5, 0.25f);
        CHECK(w.write(block.data(), 5));
        CHECK(w.checkpoint());
        auto b = slurp(path);
        CHECK(read_u32(b, 4) == b.size() - 8);
        CHECK(read_u32(b, 4) == kEmptyRiffSize + 15);
        CHECK(w.write(block.data(), 2));
        CHECK(w.close());
        b = slurp(path);
        const long data = find_chunk(b, "data");
        CHECK(read_u32(b, data - 4) == 21);
        CHECK(read_u32(b, 4) == kEmptyRiffSize + 22);
        CHECK(read_u32(b, 4) == b.size() - 8);
    }

    // --- flush() hands buffered audio to the OS, the header untouched ---
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        CHECK(!w.flush());
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        const std::vector<float> block(5, 0.25f);
        CHECK(w.write(block.data(), 5));
        CHECK(w.flush());
        const auto b = slurp(path);
        CHECK(b.size() == 8 + kEmptyRiffSize + 15);
        CHECK(read_u32(b, 4) == 0);
        CHECK(w.close());
    }

    // --- a checkpoint on a closed or failed take is harmless ---
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        CHECK(!w.checkpoint());
        w.set_riff_size_limit(kEmptyRiffSize + 6);
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        const std::vector<float> block(4, 0.25f);
        CHECK(!w.write(block.data(), 4));
        CHECK(w.failed());
        // Nothing more to make durable; close() finishes it as before.
        CHECK(!w.checkpoint());
        CHECK(w.close());
        const auto b = slurp(path);
        CHECK(read_u32(b, 4) == b.size() - 8);
        CHECK(w.failure() == "take reached the 4 GB WAV limit");
    }

    std::remove(path.c_str());
    std::printf("test_wav_writer: ok\n");
    return 0;
}
