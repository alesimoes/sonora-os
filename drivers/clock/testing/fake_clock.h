// SPDX-License-Identifier: GPL-3.0-or-later
// Header-only fakes for the clock seams (ADR-0125). FakeClockMixer stands in
// for the card's mixer under AlsaClockControl; FakeClockControl stands in for
// a whole ClockSourceControl under ClockGuard, with a scripted lock sequence
// so tests step lock state by poll count, never by time.
#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sonora/clock/clock_mixer.h"
#include "sonora/clock/clock_source.h"

namespace sonora::clock::testing {

class FakeClockMixer final : public ClockMixer {
public:
    void add_enum(const std::string& name, std::vector<std::string> items, std::size_t value = 0) {
        elements_[name] = Element{{name, MixerElementKind::Enum, std::move(items)}, value, false};
    }
    void add_bool(const std::string& name, bool value) {
        elements_[name] = Element{{name, MixerElementKind::Bool, {}}, 0, value};
    }
    // Lets a test change what the card reports, as a cable pulled would.
    void set_bool(const std::string& name, bool value) { elements_.at(name).boolean = value; }
    void refuse_writes(bool refuse) { refuse_writes_ = refuse; }
    int writes() const { return writes_; }

    std::vector<MixerElement> elements() const override {
        std::vector<MixerElement> out;
        for (const auto& entry : elements_) out.push_back(entry.second.element);
        return out;
    }

    std::optional<std::size_t> read_enum(const std::string& name) const override {
        const Element* e = find(name, MixerElementKind::Enum);
        if (e == nullptr) return std::nullopt;
        return e->index;
    }

    bool write_enum(const std::string& name, std::size_t index) override {
        Element* e = find(name, MixerElementKind::Enum);
        if (e == nullptr || refuse_writes_ || index >= e->element.items.size()) return false;
        e->index = index;
        ++writes_;
        return true;
    }

    std::optional<bool> read_bool(const std::string& name) const override {
        const Element* e = find(name, MixerElementKind::Bool);
        if (e == nullptr) return std::nullopt;
        return e->boolean;
    }

    bool write_bool(const std::string& name, bool value) override {
        Element* e = find(name, MixerElementKind::Bool);
        if (e == nullptr || refuse_writes_) return false;
        e->boolean = value;
        ++writes_;
        return true;
    }

private:
    struct Element {
        MixerElement element;
        std::size_t index = 0;
        bool boolean = false;
    };

    const Element* find(const std::string& name, MixerElementKind kind) const {
        auto it = elements_.find(name);
        if (it == elements_.end() || it->second.element.kind != kind) return nullptr;
        return &it->second;
    }
    Element* find(const std::string& name, MixerElementKind kind) {
        return const_cast<Element*>(static_cast<const FakeClockMixer*>(this)->find(name, kind));
    }

    std::map<std::string, Element> elements_;
    bool refuse_writes_ = false;
    int writes_ = 0;
};

class FakeClockControl final : public ClockSourceControl {
public:
    explicit FakeClockControl(ClockCaps caps = internal_only()) : caps_(caps) {}

    static ClockCaps internal_only() {
        ClockCaps caps;
        caps.offer(ClockSource::Internal);
        return caps;
    }

    void set_caps(ClockCaps caps) { caps_ = caps; }
    void fail_select(std::string reason) { select_failure_ = std::move(reason); }
    // Each lock() returns the next scripted state; the last one then sticks.
    void script_lock(std::vector<ClockLock> states) {
        lock_script_.assign(states.begin(), states.end());
    }
    int select_calls() const { return select_calls_; }
    int lock_reads() const { return lock_reads_; }

    ClockCaps caps() const override { return caps_; }

    ClockResult select(ClockSource source) override {
        ++select_calls_;
        if (!caps_.offers(source))
            return ClockResult::failure(std::string(to_string(source)) + " is not offered by this card");
        if (select_failure_) return ClockResult::failure(*select_failure_);
        selected_ = source;
        return ClockResult::success();
    }

    ClockSource selected() const override { return selected_; }

    ClockLock lock() const override {
        ++lock_reads_;
        if (lock_script_.empty()) return caps_.reports_lock ? ClockLock::Locked : ClockLock::Unknown;
        ClockLock state = lock_script_.front();
        if (lock_script_.size() > 1) lock_script_.pop_front();
        return state;
    }

private:
    ClockCaps caps_;
    ClockSource selected_ = ClockSource::Internal;
    std::optional<std::string> select_failure_;
    mutable std::deque<ClockLock> lock_script_;
    mutable int lock_reads_ = 0;
    int select_calls_ = 0;
};

}  // namespace sonora::clock::testing
