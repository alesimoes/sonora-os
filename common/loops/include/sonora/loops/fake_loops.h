#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

#include "sonora/loops/loop_source.h"

namespace sonora::loops {

inline bool contains_ci(const std::string& hay, const std::string& needle) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != hay.end();
}

// In-memory source whose search defines the query semantics.
class FakeLoopSource : public ILoopSource {
public:
    explicit FakeLoopSource(std::vector<LoopEntry> entries)
        : entries_(std::move(entries)) {}
    std::size_t count() const override { return entries_.size(); }
    const LoopEntry* at(std::size_t i) const override {
        return i < entries_.size() ? &entries_[i] : nullptr;
    }
    std::vector<LoopEntry> search(const LoopQuery& q) const override {
        std::vector<LoopEntry> out;
        for (const auto& e : entries_) {
            if (!q.category.empty() && e.category != q.category) continue;
            if (!q.key.empty() && e.key != q.key) continue;
            if (q.bpm > 0.0 &&
                (e.bpm <= 0.0 || std::fabs(e.bpm - q.bpm) > q.bpm * q.bpm_tolerance))
                continue;
            if (!q.text.empty()) {
                bool hit = contains_ci(e.name, q.text);
                for (const auto& t : e.tags) hit = hit || contains_ci(t, q.text);
                if (!hit) continue;
            }
            out.push_back(e);
        }
        return out;
    }

private:
    std::vector<LoopEntry> entries_;
};

class FakeLoopAudition : public ILoopAudition {
public:
    bool start(const LoopEntry& e, double bpm) override {
        if (e.path.empty()) return false;
        current_ = e.id;
        session_bpm_ = bpm;
        playing_ = true;
        return true;
    }
    void stop() override { playing_ = false; }
    bool playing() const override { return playing_; }
    std::string current_;
    double session_bpm_ = 0.0;

private:
    bool playing_ = false;
};

class FakeLoopInserter : public ILoopInserter {
public:
    struct Call {
        std::string id;
        int track;
        std::uint64_t position;
    };
    bool insert(const LoopEntry& e, int track, std::uint64_t pos) override {
        if (track < 0 || track >= track_count_) return false;
        calls_.push_back({e.id, track, pos});
        return true;
    }
    int track_count_ = 4;
    std::vector<Call> calls_;
};

}  // namespace sonora::loops
