// SPDX-License-Identifier: GPL-3.0-or-later
// The full-disk cases of test_wav_writer. A file-size limit (RLIMIT_FSIZE)
// stands in for a full disk, which only POSIX has (ADR-0008 V3).
#include "sonora/wav_writer.h"

#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
    if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    std::fclose(f);
    return out;
}

long find_chunk(const std::vector<uint8_t>& b, const char tag[4]) {
    for (std::size_t i = 12; i + 8 <= b.size();) {
        uint32_t size = 0;
        std::memcpy(&size, b.data() + i + 4, 4);
        if (std::memcmp(b.data() + i, tag, 4) == 0) return static_cast<long>(i + 8);
        i += 8 + size + (size & 1);
    }
    return -1;
}

uint32_t read_u32(const std::vector<uint8_t>& b, long off) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

// Same value as test_wav_writer: the RIFF size of a file with no audio.
constexpr uint32_t kEmptyRiffSize = 4 + (8 + 602) + (8 + 16) + 8;

}  // namespace

int main() {
    const std::string path = "test_out_full_disk.wav";

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

    // --- a take the disk refuses mid-way counts only what reached the file ---
    //
    // write() counted frames once fwrite had taken them into stdio's buffer.
    // When the disk then refused the flush, up to a buffer's worth of
    // counted frames never reached it: the header claimed 87424 frames on a
    // file that held 87163 (tools/failed-take-quit.py, PR #246), and the
    // grow close() tried cannot succeed on a full disk. A file-size limit
    // stands in for the full disk, and stays in force through close().
    {
        const auto refused_take = [&](uint16_t channels, sonora::WavWriter::Format format,
                                      uint32_t bpf, uint64_t room, uint64_t want_frames) {
            std::remove(path.c_str());
            const uint64_t header_end = 8 + kEmptyRiffSize;
            rlimit before{};
            if (::getrlimit(RLIMIT_FSIZE, &before) != 0) return false;
            const auto old_handler = std::signal(SIGXFSZ, SIG_IGN);
            rlimit tiny = before;
            tiny.rlim_cur = header_end + room;

            sonora::WavWriter w;
            bool ok = w.open(path, 48000, channels, format, 0);
            ok = ok && ::setrlimit(RLIMIT_FSIZE, &tiny) == 0;
            // 0.3 packs to 66 66 26: a part frame left behind is not zero,
            // so a pad written over it is seen to be.
            const std::vector<float> block(64 * channels, 0.3f);
            int refused_at = -1;
            for (int i = 0; ok && i < 1000 && refused_at < 0; ++i) {
                if (!w.write(block.data(), 64)) refused_at = i;
            }
            w.close();
            const uint64_t counted = w.frames_written();
            const bool failed = w.failed();
            ::setrlimit(RLIMIT_FSIZE, &before);
            std::signal(SIGXFSZ, old_handler);

            const auto b = slurp(path);
            const long data = find_chunk(b, "data");
            if (!ok || refused_at < 0 || !failed || data <= 0) return false;
            const uint64_t data_bytes = read_u32(b, data - 4);
            std::fprintf(stderr, "refused take: %u ch, counted %llu, header %llu bytes, "
                         "file %zu bytes past the header\n", channels,
                         static_cast<unsigned long long>(counted),
                         static_cast<unsigned long long>(data_bytes),
                         b.size() - static_cast<std::size_t>(data));
            return counted == want_frames && data_bytes == want_frames * bpf &&
                   read_u32(b, 4) == b.size() - 8 &&
                   b.size() == static_cast<std::size_t>(data) + data_bytes + data_bytes % 2 &&
                   (data_bytes % 2 == 0 || b.back() == 0);
        };
        // Stereo float: 8 bytes a frame. The 5 bytes past the last whole
        // frame are part of one, and are cut off.
        CHECK(refused_take(2, sonora::WavWriter::Format::Float32, 8, 10005, 1250));
        // Mono 24-bit: 3333 frames fill the room exactly, odd-sized, and the
        // pad byte cannot be added past the limit. The last frame goes, so
        // the data is even and the RIFF size is the file's.
        CHECK(refused_take(1, sonora::WavWriter::Format::Int24, 3, 9999, 3332));
        // One byte more, a part frame's first: the pad goes over it.
        CHECK(refused_take(1, sonora::WavWriter::Format::Int24, 3, 10000, 3333));
    }

    std::remove(path.c_str());
    std::printf("test_wav_writer_full_disk: ok\n");
    return 0;
}
