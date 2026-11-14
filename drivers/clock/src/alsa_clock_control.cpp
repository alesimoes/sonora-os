// SPDX-License-Identifier: GPL-3.0-or-later
// AlsaClockControl over the ClockMixer seam (ADR-0125 §3). No libasound here:
// the real mixer adapter is later work.
//
// Element names follow sound/usb/mixer.c in upstream Linux (read from the
// source, not observed on a UMC1820):
// - a UAC2 clock selector is an Enum named "<iSelector or terminal name or
//   USB> Clock Source"; its items are the sources' iClockSource strings, or
//   "Unit <clock id>" for an unnamed source;
// - each clock source's validity is a Bool named "<iClockSource> Validity",
//   or "Clock Source <clock id> Validity" when the source has no string.
// "Clock Selector" is kept as a candidate for descriptors that name the unit
// so. An item whose name matches no source keyword is not offered: guessing
// would record on a clock nobody chose.
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "sonora/clock/clock_mixer.h"

namespace sonora::clock {

namespace {

const char* const kSelectorSuffix = "Clock Source";
const char* const kSelectorNames[] = {"Clock Selector"};
const char* const kValiditySuffix = " Validity";
const char* const kUnnamedItemPrefix = "Unit ";
const char* const kUnnamedValidityPrefix = "Clock Source ";

struct Keyword {
    const char* normalized;
    ClockSource source;
};

// Matched against item names lowercased with everything but letters and
// digits dropped, so "S/PDIF", "SPDIF" and "S-PDIF" are one keyword.
// "external" and "optical" are left out on purpose: either could be ADAT or
// S/PDIF, and a wrong guess is a silent clock error.
const Keyword kKeywords[] = {
    {"internal", ClockSource::Internal},
    {"adat", ClockSource::Adat},
    {"spdif", ClockSource::Spdif},
    {"wordclock", ClockSource::WordClock},
    {"wclk", ClockSource::WordClock},
    {"bnc", ClockSource::WordClock},
};

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

std::string normalize(const std::string& name) {
    std::string out;
    for (char c : name) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) out.push_back(static_cast<char>(std::tolower(u)));
    }
    return out;
}

std::optional<ClockSource> classify(const std::string& item) {
    const std::string n = normalize(item);
    for (const Keyword& k : kKeywords)
        if (n.find(k.normalized) != std::string::npos) return k.source;
    return std::nullopt;
}

bool is_selector_name(const std::string& name) {
    if (ends_with(name, kSelectorSuffix)) return true;
    for (const char* candidate : kSelectorNames)
        if (name == candidate) return true;
    return false;
}

bool is_validity_name(const std::string& name) { return ends_with(name, kValiditySuffix); }

std::optional<MixerElement> find_selector(const ClockMixer& mixer) {
    for (const MixerElement& e : mixer.elements())
        if (e.kind == MixerElementKind::Enum && is_selector_name(e.name)) return e;
    return std::nullopt;
}

std::vector<std::string> validity_names(const ClockMixer& mixer) {
    std::vector<std::string> out;
    for (const MixerElement& e : mixer.elements())
        if (e.kind == MixerElementKind::Bool && is_validity_name(e.name)) out.push_back(e.name);
    return out;
}

std::optional<std::size_t> item_index(const MixerElement& selector, ClockSource source) {
    for (std::size_t i = 0; i < selector.items.size(); ++i)
        if (classify(selector.items[i]) == source) return i;
    return std::nullopt;
}

std::optional<std::string> current_item(const ClockMixer& mixer, const MixerElement& selector) {
    std::optional<std::size_t> index = mixer.read_enum(selector.name);
    if (!index || *index >= selector.items.size()) return std::nullopt;
    return selector.items[*index];
}

// The validity control that belongs to a selector item, by the kernel's two
// naming rules: "<item> Validity" for a named source, and "Unit N" pairing
// with "Clock Source N Validity" for an unnamed one.
std::optional<std::string> validity_for_item(const std::vector<std::string>& validities,
                                             const std::string& item) {
    std::vector<std::string> wanted{item + kValiditySuffix};
    if (starts_with(item, kUnnamedItemPrefix))
        wanted.push_back(kUnnamedValidityPrefix + item.substr(std::string(kUnnamedItemPrefix).size()) +
                         kValiditySuffix);
    for (const std::string& name : wanted)
        if (std::find(validities.begin(), validities.end(), name) != validities.end()) return name;
    return std::nullopt;
}

// With no selector the card has one clock in use. Only a lone validity
// control can be tied to it; with several there is no telling which.
std::optional<std::string> active_validity(const ClockMixer& mixer) {
    const std::vector<std::string> validities = validity_names(mixer);
    if (std::optional<MixerElement> selector = find_selector(mixer)) {
        std::optional<std::string> item = current_item(mixer, *selector);
        if (!item) return std::nullopt;
        return validity_for_item(validities, *item);
    }
    if (validities.size() == 1) return validities.front();
    return std::nullopt;
}

}  // namespace

AlsaClockControl::AlsaClockControl(ClockMixer& mixer) : mixer_(mixer) {}

ClockCaps AlsaClockControl::caps() const {
    ClockCaps caps;
    caps.reports_lock = !validity_names(mixer_).empty();
    std::optional<MixerElement> selector = find_selector(mixer_);
    if (!selector) {
        // No selector means no switch: the card runs from its own clock, so
        // Internal is the one source that can be asked for and be true.
        caps.offer(ClockSource::Internal);
        return caps;
    }
    for (const std::string& item : selector->items)
        if (std::optional<ClockSource> source = classify(item)) caps.offer(*source);
    return caps;
}

ClockResult AlsaClockControl::select(ClockSource source) {
    const std::string name = to_string(source);
    std::optional<MixerElement> selector = find_selector(mixer_);
    if (!selector) {
        if (source == ClockSource::Internal) return ClockResult::success();
        return ClockResult::failure("the card exposes no clock selector, so " + name +
                                    " cannot be chosen");
    }
    std::optional<std::size_t> index = item_index(*selector, source);
    if (!index)
        return ClockResult::failure("'" + selector->name + "' offers no " + name + " item");
    if (!mixer_.write_enum(selector->name, *index))
        return ClockResult::failure("the card refused writing " + name + " to '" + selector->name + "'");
    // Read back: a write the card accepted but did not apply is still the
    // wrong clock, and only the read shows it.
    if (mixer_.read_enum(selector->name) != index)
        return ClockResult::failure("'" + selector->name + "' did not change to " + name);
    return ClockResult::success();
}

ClockSource AlsaClockControl::selected() const {
    std::optional<MixerElement> selector = find_selector(mixer_);
    if (!selector) return ClockSource::Internal;
    std::optional<std::string> item = current_item(mixer_, *selector);
    std::optional<ClockSource> source = item ? classify(*item) : std::nullopt;
    // The interface has no "unknown" source. An unreadable or unclassified
    // item reads as Internal here, but lock() reads the item itself, never
    // this value, so ClockGuard's decision does not rest on the guess.
    return source.value_or(ClockSource::Internal);
}

ClockLock AlsaClockControl::lock() const {
    std::optional<std::string> validity = active_validity(mixer_);
    if (!validity) return ClockLock::Unknown;
    std::optional<bool> valid = mixer_.read_bool(*validity);
    if (!valid) return ClockLock::Unknown;
    return *valid ? ClockLock::Locked : ClockLock::Unlocked;
}

}  // namespace sonora::clock
