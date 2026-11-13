#include <algorithm>
#include <cctype>
#include <utility>

#include "sonora/loops/loop_scan.h"

namespace sonora::loops {
namespace {

constexpr std::size_t kHeaderBytes = 64 * 1024;
constexpr std::size_t kSidecarBytes = 64 * 1024;

std::string join(const std::string& a, const std::string& b) {
    return a.empty() ? b : a + "/" + b;
}

bool ends_with_ci(const std::string& s, const std::string& suf) {
    if (s.size() < suf.size()) return false;
    for (std::size_t i = 0; i < suf.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - suf.size() + i])) != suf[i])
            return false;
    return true;
}

std::string stem_of(const std::string& name) {
    auto dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string last_dir(const std::string& rel) {
    auto slash = rel.rfind('/');
    if (slash == std::string::npos) return "";
    auto prev = rel.rfind('/', slash - 1);
    return rel.substr(prev == std::string::npos ? 0 : prev + 1,
                      slash - (prev == std::string::npos ? 0 : prev + 1));
}

}  // namespace

LoopScanner::LoopScanner(const ILoopFileSystem& fs, std::string root)
    : fs_(fs), root_(std::move(root)) {}

void LoopScanner::walk(const std::string& rel, std::map<std::string, Cached>& next) {
    std::vector<FsEntry> items;
    if (!fs_.list(join(root_, rel), items)) return;
    std::sort(items.begin(), items.end(),
              [](const FsEntry& a, const FsEntry& b) { return a.name < b.name; });
    for (const auto& it : items) {
        if (it.is_dir) {
            walk(join(rel, it.name), next);
            continue;
        }
        if (!ends_with_ci(it.name, ".wav")) continue;
        const std::string path = join(rel, it.name);
        const std::string side = stem_of(it.name) + ".json";
        std::int64_t side_mtime = -1;
        for (const auto& s : items)
            if (!s.is_dir && s.name == side) side_mtime = s.mtime;

        auto old = cache_.find(path);
        if (old != cache_.end() && old->second.size == it.size &&
            old->second.mtime == it.mtime && old->second.side_mtime == side_mtime) {
            next[path] = old->second;
            continue;
        }
        ++files_read_;
        Cached c;
        c.size = it.size;
        c.mtime = it.mtime;
        c.side_mtime = side_mtime;
        LoopEntry& e = c.entry;
        e.id = path;
        e.path = path;
        e.name = stem_of(it.name);
        e.category = last_dir(path);
        parse_filename_tokens(e.name, e);
        std::string bytes;
        if (fs_.read(join(root_, path), kHeaderBytes, bytes)) parse_wav_header(bytes, e);
        // The sidecar is the curator's word: it wins over anything inferred.
        std::string text;
        if (side_mtime >= 0 && fs_.read(join(root_, join(rel, side)), kSidecarBytes, text))
            parse_sidecar_json(text, e);
        next[path] = std::move(c);
    }
}

void LoopScanner::refresh() {
    std::map<std::string, Cached> next;
    walk("", next);
    cache_ = std::move(next);
    entries_.clear();
    for (const auto& kv : cache_) entries_.push_back(kv.second.entry);
}

}  // namespace sonora::loops
