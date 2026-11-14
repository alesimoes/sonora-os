// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's preferences (ADR-0204, protocol 71): one message pair carries
// all five, so adding the rest of the Preferences window costs one bump.
//
// The engine validates a set whole and stores it whole: a value out of range
// refuses the request with a reason the operator sees, and nothing is
// clamped, because a clamped value is one the operator did not ask for and
// would not know about. None of it is ever applied on the audio thread
// (ADR-0204 section 4).
//
// Not on the audio thread: everything here allocates.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sonora::api {

// Ranges, each the engine's own command-line flag's (engine/app --help), so a
// value the Preferences window accepts is one a start-up flag accepts too.
inline constexpr uint32_t kMaxAutosaveIntervalS = 86400;   // 0: autosave off
inline constexpr uint32_t kMaxBackupCount = 1000;          // 0: no backups
inline constexpr uint32_t kMinAutomatchMs = 10;            // SessionConfig::kMinTouchReturnMs
inline constexpr uint32_t kMaxAutomatchMs = 5000;          // SessionConfig::kMaxTouchReturnMs
inline constexpr uint32_t kMinBufferFrames = 16;           // 0: the backend's default
inline constexpr uint32_t kMaxBufferFrames = 8192;
// A PipeWire node name or an ALSA "hw:CARD=...,DEV=..." id; no real one comes
// near it.
inline constexpr std::size_t kMaxDeviceIdBytes = 256;

struct EngineSettings {
    uint32_t autosave_interval_s = 60;
    uint32_t backup_count = 20;   // session_files' kDefaultBackups
    uint32_t automatch_ms = 250;  // SessionConfig::touch_return_ms
    std::string device_id;        // empty: the system's default device
    uint32_t buffer_frames = 0;   // 0: the backend's default period

    bool operator==(const EngineSettings& o) const {
        return autosave_interval_s == o.autosave_interval_s && backup_count == o.backup_count &&
               automatch_ms == o.automatch_ms && device_id == o.device_id &&
               buffer_frames == o.buffer_frames;
    }
    bool operator!=(const EngineSettings& o) const { return !(*this == o); }
};

// Which value refused a set. The first one found, in declaration order; the
// wire carries the number, so a value keeps its number for good.
enum class SettingsRefusal : uint32_t {
    None = 0,
    AutosaveInterval = 1,
    BackupCount = 2,
    AutomatchTime = 3,
    DeviceId = 4,
    BufferFrames = 5,
    // Valid, but the engine could not keep them (its preferences file could
    // not be written); nothing changed.
    NotStored = 6,
};
inline constexpr uint32_t kLastSettingsRefusal = static_cast<uint32_t>(SettingsRefusal::NotStored);

// EngineSettingsReport::pending: stored values the running engine does not
// use yet. AutoMatch is a session's construction-time setting, so it waits
// for the next session open; the device and the buffer wait for the next
// engine start (ADR-0204 section 4).
inline constexpr uint32_t kSettingsPendingSessionOpen = 1u << 0;
inline constexpr uint32_t kSettingsPendingRestart = 1u << 1;
inline constexpr uint32_t kSettingsPendingFlags =
    kSettingsPendingSessionOpen | kSettingsPendingRestart;

// None, with `reason` cleared, when every value is in range; otherwise the
// first value out of range and words naming it, its value and its range.
inline SettingsRefusal validate(const EngineSettings& s, std::string& reason) {
    reason.clear();
    if (s.autosave_interval_s > kMaxAutosaveIntervalS) {
        reason = "autosave interval " + std::to_string(s.autosave_interval_s) +
                 " s is outside 0 (off) to " + std::to_string(kMaxAutosaveIntervalS) + " s";
        return SettingsRefusal::AutosaveInterval;
    }
    if (s.backup_count > kMaxBackupCount) {
        reason = "backup count " + std::to_string(s.backup_count) + " is outside 0 (none) to " +
                 std::to_string(kMaxBackupCount);
        return SettingsRefusal::BackupCount;
    }
    if (s.automatch_ms < kMinAutomatchMs || s.automatch_ms > kMaxAutomatchMs) {
        reason = "AutoMatch time " + std::to_string(s.automatch_ms) + " ms is outside " +
                 std::to_string(kMinAutomatchMs) + " to " + std::to_string(kMaxAutomatchMs) +
                 " ms";
        return SettingsRefusal::AutomatchTime;
    }
    if (s.device_id.size() > kMaxDeviceIdBytes) {
        reason = "audio device id of " + std::to_string(s.device_id.size()) +
                 " bytes is over the cap of " + std::to_string(kMaxDeviceIdBytes);
        return SettingsRefusal::DeviceId;
    }
    for (const char c : s.device_id) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            reason = "audio device id holds control byte " + std::to_string(u);
            return SettingsRefusal::DeviceId;
        }
    }
    if (s.buffer_frames != 0 &&
        (s.buffer_frames < kMinBufferFrames || s.buffer_frames > kMaxBufferFrames)) {
        reason = "buffer of " + std::to_string(s.buffer_frames) +
                 " frames is outside 0 (backend default) or " + std::to_string(kMinBufferFrames) +
                 " to " + std::to_string(kMaxBufferFrames);
        return SettingsRefusal::BufferFrames;
    }
    return SettingsRefusal::None;
}

// --- client -> engine (wire type 11) ----------------------------------------

// All five values; the window sends what it shows, unchanged ones included,
// so a set never depends on what the engine held before. `seq` is assigned
// by EngineControl::send_settings(), like every request's.
struct EngineSettingsSet {
    uint64_t seq = 0;
    EngineSettings settings;
};

// --- engine -> client (wire type 12) ----------------------------------------

// The answer to a set (its `seq`), and, with seq 0, what the engine holds,
// sent after Hello and whenever its settings change from elsewhere.
struct EngineSettingsReport {
    uint64_t seq = 0;
    // What the engine holds after the request: the request's values when it
    // was taken, the unchanged ones when it was refused.
    EngineSettings settings;
    SettingsRefusal refusal = SettingsRefusal::None;
    // Empty exactly when refusal is None; the decoder refuses either half
    // without the other, so a refusal always reaches the operator in words.
    std::string reason;
    uint32_t pending = 0;   // kSettingsPending* flags
};

// Where the engine keeps them, on its control thread. The engine implements
// it; interface and engine tests inject FakeEngineSettings
// (contracts/api/testing).
class EngineSettingsStore {
public:
    virtual ~EngineSettingsStore() = default;
    // Validates `request` whole and stores it whole, or refuses it and keeps
    // what it had. The report's seq is the request's.
    virtual EngineSettingsReport set(const EngineSettingsSet& request) = 0;
    // What is held now, seq 0.
    virtual EngineSettingsReport report() const = 0;
};

}  // namespace sonora::api
