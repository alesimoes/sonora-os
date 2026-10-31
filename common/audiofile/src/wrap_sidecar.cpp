// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/wrap_sidecar.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include "sonora/platform/file.h"

namespace sonora {

namespace {

constexpr char kMagic[4] = {'S', 'N', 'W', 'R'};

void put_le(unsigned char* p, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}

uint64_t get_le(const unsigned char* p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= uint64_t{p[i]} << (8 * i);
    return v;
}

}  // namespace

WrapSidecar::~WrapSidecar() { close(); }

bool WrapSidecar::open(const std::string& path, uint64_t loop_from, uint64_t loop_to,
                       uint32_t latency) {
    if (file_) return false;
    file_ = platform::create_exclusive(path);
    if (!file_) return false;
    path_ = path;
    failure_.clear();
    unsigned char h[kHeaderBytes] = {};
    std::memcpy(h, kMagic, 4);
    put_le(h + 4, kVersion, 4);
    put_le(h + 8, loop_from, 8);
    put_le(h + 16, loop_to, 8);
    put_le(h + 24, latency, 4);
    if (std::fwrite(h, 1, sizeof h, file_) != sizeof h || std::fflush(file_) != 0) {
        const int err = errno;
        std::fclose(file_);
        file_ = nullptr;
        std::remove(path.c_str());
        errno = err;
        return false;
    }
    return true;
}

bool WrapSidecar::fail() {
    if (failure_.empty()) failure_ = std::strerror(errno);
    return false;
}

bool WrapSidecar::append(uint64_t file_frame) {
    if (!file_ || !failure_.empty()) return false;
    unsigned char r[8];
    put_le(r, file_frame, 8);
    if (std::fwrite(r, 1, sizeof r, file_) != sizeof r || std::fflush(file_) != 0) return fail();
    return true;
}

bool WrapSidecar::checkpoint() {
    if (!file_ || !failure_.empty()) return false;
    // A file the OS cannot sync counts as synced (platform::sync_data()).
    if (!platform::sync_data(file_) && errno != ENOSYS) return fail();
    return true;
}

bool WrapSidecar::close() {
    if (!file_) return true;
    const bool ok = std::fclose(file_) == 0;
    file_ = nullptr;
    if (!ok) return fail();
    return failure_.empty();
}

bool read_wrap_sidecar(const std::string& path, WrapList& out, std::string& error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = path + ": " + std::strerror(errno);
        return false;
    }
    unsigned char h[WrapSidecar::kHeaderBytes];
    const bool whole = std::fread(h, 1, sizeof h, f) == sizeof h;
    if (!whole || std::memcmp(h, kMagic, 4) != 0) {
        std::fclose(f);
        error = path + ": not a wrap list";
        return false;
    }
    const uint64_t version = get_le(h + 4, 4);
    // Version 1 is version 2 without kBeforeFile, which it never wrote: it
    // reads the same.
    if (version < 1 || version > WrapSidecar::kVersion) {
        std::fclose(f);
        error = path + ": wrap list version " + std::to_string(version) +
                ", this engine reads 1 to " + std::to_string(WrapSidecar::kVersion);
        return false;
    }
    WrapList list;
    list.loop_from = get_le(h + 8, 8);
    list.loop_to = get_le(h + 16, 8);
    list.latency = static_cast<uint32_t>(get_le(h + 24, 4));
    unsigned char r[8];
    while (std::fread(r, 1, sizeof r, f) == sizeof r) list.wraps.push_back(get_le(r, 8));
    std::fclose(f);
    out = std::move(list);
    return true;
}

}  // namespace sonora
