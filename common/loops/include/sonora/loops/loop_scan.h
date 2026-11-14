#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "sonora/loops/loop_source.h"

namespace sonora::loops {

// Injected so the scan is testable without a disk. Off the audio thread only.
struct FsEntry {
    std::string name;
    bool is_dir = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
};

class ILoopFileSystem {
public:
    virtual ~ILoopFileSystem() = default;
    virtual bool list(const std::string& dir, std::vector<FsEntry>& out) const = 0;
    // Reads at most max_bytes from the start of the file.
    virtual bool read(const std::string& path, std::size_t max_bytes,
                      std::string& out) const = 0;
};

// Metadata parsers; each fills only the fields it finds.
void parse_filename_tokens(const std::string& stem, LoopEntry& e);
bool parse_sidecar_json(const std::string& text, LoopEntry& e);
// Reads sample rate, frames and the ACID tempo from a RIFF/WAVE image.
bool parse_wav_header(const std::string& bytes, LoopEntry& e);

// Pure filter used by every source.
std::vector<LoopEntry> filter(const std::vector<LoopEntry>& all,
                              const LoopQuery& q);

// Scans root recursively. refresh() re-reads only files whose size or mtime
// changed since the last call (stale-checked cache).
class LoopScanner : public ILoopSource {
public:
    LoopScanner(const ILoopFileSystem& fs, std::string root);
    void refresh();
    std::size_t count() const override { return entries_.size(); }
    const LoopEntry* at(std::size_t i) const override {
        return i < entries_.size() ? &entries_[i] : nullptr;
    }
    std::vector<LoopEntry> search(const LoopQuery& q) const override {
        return filter(entries_, q);
    }
    std::size_t files_read() const { return files_read_; }  // cache misses

private:
    struct Cached {
        std::uint64_t size = 0;
        std::int64_t mtime = 0;
        std::int64_t side_mtime = -1;
        LoopEntry entry;
    };
    void walk(const std::string& rel, std::map<std::string, Cached>& next);
    const ILoopFileSystem& fs_;
    std::string root_;
    std::map<std::string, Cached> cache_;
    std::vector<LoopEntry> entries_;
    std::size_t files_read_ = 0;
};

}  // namespace sonora::loops
