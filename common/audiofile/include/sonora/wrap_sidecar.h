// SPDX-License-Identifier: GPL-3.0-or-later
//
// A loop take's wrap list on disk (ADR-0016 §2): `<take file>.wraps`, next
// to the take, so a take killed mid-loop still says where each pass ended.
//
//   offset 0   "SNWR"            magic
//          4   u32 LE            version, 1
//          8   u64 LE            A, the loop start, a timeline frame
//         16   u64 LE            B, the loop end
//         24   u32 LE            L, the input latency every wrap below carries
//                                (ADR-0016 C7): an orphan's bext has none
//         28   u32               zero
//         32   u64 LE, repeated  the file frame of each wrap, in wrap order;
//                                kLostWrap for one whose boundary record the
//                                audio thread could not push
//
// Append-only. A record cut short by a crash is not a wrap.
//
// Not for the audio thread: it creates, writes and syncs files.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace sonora {

class WrapSidecar {
public:
    // A wrap the list holds no frame for: its record was lost to a full
    // boundary ring. Its place is kept so the wraps after it keep theirs.
    static constexpr uint64_t kLostWrap = ~uint64_t{0};
    static constexpr uint32_t kVersion = 1;
    static constexpr std::size_t kHeaderBytes = 32;

    static std::string path_for(const std::string& take_path) { return take_path + ".wraps"; }

    WrapSidecar() = default;
    ~WrapSidecar();
    WrapSidecar(const WrapSidecar&) = delete;
    WrapSidecar& operator=(const WrapSidecar&) = delete;

    // Creates `path` exclusively and writes the header, flushed. False with
    // errno set, leaving no file, as WavWriter::open() does.
    bool open(const std::string& path, uint64_t loop_from, uint64_t loop_to, uint32_t latency);
    // One wrap, handed to the OS (a kill -9 after it loses nothing). False,
    // writing nothing more until the next open(), once a write has failed.
    bool append(uint64_t file_frame);
    // Syncs what was appended to the medium: the take's checkpoint calls it
    // after the take's own, so the loss window of the take covers its wraps.
    bool checkpoint();
    bool close();

    bool is_open() const { return file_ != nullptr; }
    const std::string& path() const { return path_; }
    // Empty unless a write or sync failed; then why.
    const std::string& failure() const { return failure_; }

private:
    bool fail();

    std::FILE* file_ = nullptr;
    std::string path_;
    std::string failure_;
};

struct WrapList {
    uint64_t loop_from = 0;
    uint64_t loop_to = 0;
    uint32_t latency = 0;
    std::vector<uint64_t> wraps;   // kLostWrap where a record was lost
};

// False, with `error`, if the file is missing, not a wrap list, or of
// another version. A torn last record is dropped.
bool read_wrap_sidecar(const std::string& path, WrapList& out, std::string& error);

}  // namespace sonora
