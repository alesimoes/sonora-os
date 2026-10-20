// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ============================================================================
//  The probe record, record version 1 (ADR-0012, amendment of 2026-10-19,
//  item 2).
//
//  Owned jointly by plugin-engineer and rt-engineer (ADR-0007 §4.3, §6),
//  like protocol.h and host_control.h. A change to a field, its meaning or
//  a cap below moves kProbeRecordVersion, and tests/test_probe_record.cpp's
//  golden line moves with it, or the build's tests fail.
// ============================================================================
//
// What `sonora-plugin-host --probe ID --channels N` prints, as one line of
// JSON on stdout, and what the engine's plugin catalog cache stores per
// plugin (engine/control plugin_scanner.h). It carries everything
// contracts/api's PluginInfo needs, so the engine maps a record to a
// catalog entry without asking the host again.
//
// Versioning. The record carries its own version (`record`) and the host's
// `--version` line (`host`). The host's --version line names the record
// version it writes as `probe <kProbeRecordVersion>`, after layout, clock
// and wire (parse_probe_version). A record of another version is refused
// by decode(), never guessed at: for the engine's cache it is a miss, and
// the plugin is probed again.
//
// The line. One JSON object, UTF-8, no newline inside, its keys in this
// order (a reader must not depend on the order; the tests that grep the
// host's output do):
//   record, host, uri, format, bundle, name, vendor, version, categories,
//   has_editor, audio_in, audio_out, sidechain_in, ok, reason (only when
//   ok is false), channels, replicas, latency_frames, sample_rate, outcome,
//   bypass, params
// and each parameter's
//   key, symbol, name, min, max, default, flags, unit, steps, labels.
// Numbers are integers or finite floats; a float is written shortest
// round-trip (std::to_chars), so a record read back is the record written.
//
// Who writes which outcome. The host writes Ok, Refused (it validated the
// plugin and refused it: the reason says why) and NotInstalled (no such
// plugin in its paths). The engine writes Blocked itself, for a probe that
// crashed or did not answer within its timeout: the host cannot say that
// of itself. A Blocked plugin is cached as blocked, so it is not run again
// at every boot (amendment item 4).
//
// No OS calls and no dependency beyond the standard library (ADR-0008,
// ADR-0007: contracts depend on common/base only). Not for the audio
// thread: everything here allocates.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sonora::plugin::probe {

inline constexpr uint32_t kProbeRecordVersion = 1;

// Caps a reader enforces: a record over any of them is refused whole.
inline constexpr std::size_t kMaxRecordBytes = std::size_t{16} << 20;
inline constexpr std::size_t kMaxRecordParams = 65536;   // host_control.h kMaxParamsPerInstance
inline constexpr std::size_t kMaxRecordLabels = 4096;    // per parameter
inline constexpr std::size_t kMaxRecordCategories = 16;
inline constexpr std::size_t kMaxRecordText = std::size_t{64} << 10;  // any one string

enum class Outcome : uint8_t {
    Ok = 0,            // validated; usable at `channels`
    Refused = 1,       // the host refused it, `reason` says why
    NotInstalled = 2,  // no such plugin in the host's paths
    Blocked = 3,       // the engine's: its probe crashed or timed out
};

// The same bits as contracts/api's PluginParamFlag; engine/control asserts
// that they agree.
enum ParamFlag : uint32_t {
    kParamAutomatable = 1u << 0,
    kParamToggle = 1u << 1,
    kParamEnum = 1u << 2,
    kParamLogarithmic = 1u << 3,
    // Past protocol.h's kMaxParamsPerChain: not automatable from Sonora,
    // still in the plugin's own state.
    kParamPastCap = 1u << 4,
};
inline constexpr uint32_t kKnownParamFlags = (1u << 5) - 1;

struct Param {
    // The stable key the session file stores (ADR-0012 §6.1): LV2 port
    // symbol, VST3 ParamID in decimal. The dense index is the position in
    // Record::params.
    std::string key;
    // What the host's --set takes: LV2 port symbol, VST3 title.
    std::string symbol;
    std::string name;
    // Format-native: LV2 plain, VST3 normalised [0, 1].
    float min = 0.f;
    float max = 1.f;
    float default_value = 0.f;
    uint32_t flags = 0;  // ParamFlag
    std::string unit;    // "dB", "Hz", "ms", "%", or empty
    // 0: continuous; otherwise the number of values, evenly spaced from min
    // to max (2 for a toggle). Never 1.
    uint32_t steps = 0;
    // Empty, or one per step, from min.
    std::vector<std::string> labels;
};

struct Record {
    uint32_t record = kProbeRecordVersion;
    // The --version line of the host that wrote it, without the newline.
    std::string host;
    // As the host takes it: an LV2 URI, or "vst3:" and the class id.
    std::string uri;
    std::string format;  // "lv2" or "vst3"
    // The bundle the plugin was found in, absolute: what the engine's cache
    // fingerprints. Empty for NotInstalled.
    std::string bundle;
    std::string name;
    std::string vendor;
    std::string version;
    // LV2 class labels or VST3 subCategories, broadest first.
    std::vector<std::string> categories;
    // Whether this host can open the plugin's own editor.
    bool has_editor = false;
    // Main audio as the plugin declares it; a key input apart.
    uint32_t audio_in = 0;
    uint32_t audio_out = 0;
    uint32_t sidechain_in = 0;
    Outcome outcome = Outcome::Refused;
    std::string reason;  // when outcome is not Ok
    // What it was probed at: the chain's channels, the replicas that took
    // (a mono plugin on 2 channels runs twice), its latency then.
    uint32_t channels = 0;
    uint32_t replicas = 0;
    uint32_t latency_frames = 0;
    uint32_t sample_rate = 0;
    // Bypass through the plugin's own parameter (VST3 kIsBypass, LV2
    // lv2:enabled), or host-side.
    bool plugin_bypass = false;
    std::vector<Param> params;

    bool ok() const { return outcome == Outcome::Ok; }
};

// One line, no newline. Non-finite floats are written as 0: JSON has none,
// and no range or default a reader could use is one.
std::string encode(const Record& r);

// False, with `error`, for a line that is not a version-1 record within the
// caps: another `record` version, a missing or mistyped field, an unknown
// outcome, ok disagreeing with outcome, steps of 1, more labels than steps.
// Unknown keys are refused too: a record is the contract's or it is not.
bool decode(std::string_view line, Record& out, std::string& error);

// The record version a host's --version line names (`probe N`), or false
// when it names none: a host from before records were versioned.
bool parse_probe_version(std::string_view version_line, uint32_t& probe);

}  // namespace sonora::plugin::probe
