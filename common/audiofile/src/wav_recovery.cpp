// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/wav_recovery.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "sonora/platform/file.h"

namespace sonora {
namespace {

namespace fs = std::filesystem;

// What WavWriter writes in bext Originator (EBU Tech 3285 §2.3, 32 bytes,
// NUL-padded).
constexpr char kOriginator[] = "Sonora Engine";
constexpr uint64_t kRiffSizeLimit = 0xFFFFFFFFu;

uint32_t u32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct Layout {
    bool ours = false;
    uint32_t block_align = 0;
    uint64_t data_body = 0;  // offset of the first sample
    uint64_t data_bytes = 0;
};

// Walks the chunks up to the data chunk. Empty `why` on success.
Layout read_layout(std::FILE* f, std::string& why) {
    Layout l;
    unsigned char head[12];
    if (std::fread(head, 1, 12, f) != 12 || std::memcmp(head, "RIFF", 4) != 0 ||
        std::memcmp(head + 8, "WAVE", 4) != 0) {
        why = "not a RIFF/WAVE file";
        return l;
    }
    uint64_t at = 12;
    unsigned char ck[8];
    for (;;) {
        if (std::fread(ck, 1, 8, f) != 8) {
            why = "no data chunk";
            return l;
        }
        const uint32_t size = u32(ck + 4);
        const uint64_t body = at + 8;
        if (std::memcmp(ck, "data", 4) == 0) {
            l.data_body = body;
            l.data_bytes = size;
            break;
        }
        if (std::memcmp(ck, "bext", 4) == 0 && size >= 256 + 32) {
            unsigned char bext[256 + 32];
            if (std::fread(bext, 1, sizeof bext, f) != sizeof bext) {
                why = "short bext chunk";
                return l;
            }
            l.ours = std::memcmp(bext + 256, kOriginator, sizeof kOriginator) == 0;
        } else if (std::memcmp(ck, "fmt ", 4) == 0 && size >= 16) {
            unsigned char fmt[16];
            if (std::fread(fmt, 1, sizeof fmt, f) != sizeof fmt) {
                why = "short fmt chunk";
                return l;
            }
            l.block_align = static_cast<uint32_t>(fmt[12]) | (static_cast<uint32_t>(fmt[13]) << 8);
        }
        at = body + size + (size & 1);
        if (at > static_cast<uint64_t>(LONG_MAX) ||
            std::fseek(f, static_cast<long>(at), SEEK_SET) != 0) {
            why = "truncated chunk";
            return l;
        }
    }
    if (l.block_align == 0) why = "no fmt chunk before the data";
    return l;
}

bool put_u32(std::FILE* f, long at, uint64_t v) {
    unsigned char b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xFF);
    return std::fseek(f, at, SEEK_SET) == 0 && std::fwrite(b, 1, 4, f) == 4;
}

std::string frames_text(uint64_t n) { return std::to_string(n) + (n == 1 ? " frame" : " frames"); }

}  // namespace

TakeRecovery recover_take(const std::string& path) {
    TakeRecovery r;
    r.path = path;
    std::error_code ec;
    const uint64_t size = fs::file_size(path, ec);
    if (ec) {
        r.why = ec.message();
        return r;
    }
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        r.why = std::strerror(errno);
        return r;
    }
    const Layout l = read_layout(f, r.why);
    std::fclose(f);
    if (!r.why.empty()) return r;
    if (!l.ours) {
        r.outcome = TakeRecovery::Outcome::NotOurs;
        return r;
    }

    r.header_frames = l.data_bytes / l.block_align;
    const uint64_t finished = l.data_body + l.data_bytes + (l.data_bytes & 1);
    if (size == finished) {
        r.outcome = TakeRecovery::Outcome::Intact;
        r.frames = r.header_frames;
        return r;
    }
    if (size < l.data_body + l.data_bytes) {
        r.outcome = TakeRecovery::Outcome::Short;
        r.frames = r.header_frames;
        r.why = "its header counts " + frames_text(r.header_frames) + ", the file holds " +
                frames_text((size > l.data_body ? size - l.data_body : 0) / l.block_align);
        return r;
    }

    // Whole frames only: a frame the engine died writing is not audio.
    r.frames = (size - l.data_body) / l.block_align;
    const uint64_t data = r.frames * l.block_align;
    const uint64_t pad = data & 1;
    const uint64_t riff = l.data_body + data + pad - 8;
    r.outcome = TakeRecovery::Outcome::Failed;
    if (riff > kRiffSizeLimit) {
        r.why = "the file is past the 4 GB WAV limit";
        return r;
    }
    // The tail first: cut back to the audio, then grown by the pad, which
    // a resize fills with zero. Interrupted here, the header still
    // describes less than the file, and recovery runs again next time.
    fs::resize_file(path, l.data_body + data, ec);
    if (!ec && pad) fs::resize_file(path, l.data_body + data + pad, ec);
    if (ec) {
        r.why = ec.message();
        return r;
    }
    f = std::fopen(path.c_str(), "r+b");
    if (!f) {
        r.why = std::strerror(errno);
        return r;
    }
    bool ok = put_u32(f, static_cast<long>(l.data_body - 4), data) && put_u32(f, 4, riff);
    // Without a sync where the platform has none (ENOSYS): the repair is no
    // less right, only not yet durable.
    ok = ok && (platform::sync_data(f) || errno == ENOSYS);
    const int err = errno;
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
        r.why = std::strerror(err);
        return r;
    }
    r.outcome = TakeRecovery::Outcome::Repaired;
    return r;
}

std::vector<TakeRecovery> recover_takes(const std::string& audio_dir) {
    std::vector<TakeRecovery> out;
    std::error_code ec;
    std::vector<std::string> paths;
    for (fs::directory_iterator it(audio_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".wav" && it->is_regular_file(ec)) {
            paths.push_back(it->path().string());
        }
    }
    std::sort(paths.begin(), paths.end());
    for (const std::string& p : paths) {
        TakeRecovery r = recover_take(p);
        if (r.outcome == TakeRecovery::Outcome::Repaired ||
            r.outcome == TakeRecovery::Outcome::Short ||
            r.outcome == TakeRecovery::Outcome::Failed) {
            out.push_back(std::move(r));
        }
    }
    return out;
}

}  // namespace sonora
