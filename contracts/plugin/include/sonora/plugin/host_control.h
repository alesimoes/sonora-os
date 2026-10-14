// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ============================================================================
//  HostControl, wire version 1 (ADR-0012 §4, §5.5; checklist item 1).
//
//  Owned jointly by plugin-engineer and rt-engineer (ADR-0007 §4.3, §6), like
//  protocol.h. A change to a message, a field, a cap or an ordering rule
//  below moves kHostWireVersion, and tests/test_host_control.cpp's golden
//  encodings move with it, or the build's tests fail.
// ============================================================================
//
// The engine's control thread and one host process talk over ONE connected
// AF_UNIX SOCK_SEQPACKET socket pair per host (common/platform
// ControlChannel::pair). The host's end is inherited as descriptor 3 and
// named on its command line (`--control-fd 3`). Messages only: this channel
// carries no descriptor and no shared memory. The channel and clock regions
// stay inherited descriptors named on the command line (--shm-fd,
// --clock-fd), exactly as in layout 1 (the clock as the sealed read-only
// reopen, auditor M1). ADR-0012 §4's SCM_RIGHTS passing is not used, and
// the engine speaks first: both recorded, with the reasons, in ADR-0012's
// amendment of 2026-10-13 ("HostControl transport and handshake").
//
// Not on any audio thread. Everything here allocates. The engine side is
// the control thread only, at human or 100 Hz rates, never per block; the
// host side is its main thread and a reader thread.
//
// ---------------------------------------------------------------------------
// Framing. A message is a type byte, then its fields in declaration order:
// integers little-endian whatever the host, a float as its IEEE-754 bits in
// a u32, a bool as one byte 0 or 1, a string or byte block as a u32 length
// and its bytes. A message crosses as one or more FRAMES, each one datagram:
// a u32 length of the whole message, a u32 offset of this frame's bytes in
// it, then those bytes (at most kMaxFrameBytes per datagram, under the
// default socket send buffer). SOCK_SEQPACKET keeps order and boundaries, so
// the receiver reassembles one message at a time and never holds two.
//
// Untrusted input, in both directions, and the engine's side above all (the
// host runs third-party code). Decoding refuses, with a reason and never a
// partial message: a short buffer, trailing bytes, the wrong type byte, an
// enum value this version does not know, a bool other than 0 or 1, a string
// or byte block past its cap, a count past its cap, a non-finite value, a
// plugin id that is not one. A frame out of place (not where the message
// being reassembled goes on, a length over that direction's cap
// (kMaxHostMessageBytes host -> engine, kMaxMessageBytes engine -> host), a frame over
// kMaxFrameBytes, an empty message) is a protocol error, and a peer that
// frames wrongly cannot be resynchronised: the engine kills the host and
// follows the crash path (§5.3); the host exits kHostExitUsage.
//
// What the engine checks beyond decoding, on every host message: an
// `instance` below the chain's count; a `param` that is a dense index of
// that instance (param_in_chain) or kBypassParam; a `request` that matches
// one it has outstanding, and a reply type that matches that request. And
// per message (rt-engineer low on PR #51):
//  - Hello: the three versions are the engine's, and `pid` is the pid it
//    forked.
//  - Loaded: instances.size() is the header's instance_count; each
//    instance's params.size() is instance_params[i] (the key-to-index map
//    depends on it); latency_frames passes checked_latency, and so does
//    each instance's; the region's figures win where they differ.
//  - State: `instance` is the one requested; with status Ok, the bytes
//    decode as a StateBlob whose format and id are that instance's.
//  - Every datagram carries no descriptor: one that does is a protocol
//    fault (common/platform's receive refuses it when no region is asked).
// A message that fails any of these is a protocol fault: kill, crash path.
//
// ---------------------------------------------------------------------------
// Handshake and versioning (ADR-0012 §5.5).
//
//  1. The engine creates and initialises both regions (init_header, seeded
//     table, init_clock, sealed), creates the pair, forks the host from its
//     long-lived control thread and writes EngineHello as the FIRST datagram
//     on its end. It may queue Load right behind it.
//  2. The host reads EngineHello first (at most kHelloTimeoutMs). A wire
//     version it does not speak: one stderr line naming both versions, exit
//     kHostExitIncompatible (3), having sent NOTHING on the socket and
//     written nothing to either region. The same for the layout and clock
//     versions, which it also checks against the regions' version words.
//     A first message that is not an EngineHello: exit kHostExitUsage.
//  3. The host sends Hello (its versions and pid): the first datagram it
//     ever sends. The engine refuses a Hello whose versions are not its own
//     (kill, reported as an install fault naming the binary), which can only
//     happen if the binary changed since the start-up `--version` check.
//  4. The host waits for Load, loads and validates the chain, and answers
//     Loaded or Refused (below). Exactly one Load per host (ADR-0012 §2: one
//     chain per host); a second one is a protocol error.
//  The first bytes of both hellos never move in any version: the frame
//  header, the type byte, then the u32 wire version (decode_wire_version),
//  so either side can name a mismatch whatever changed after them.
//
// The start-up check (ADR-0012 §5.5): `sonora-plugin-host --version` prints
// format_host_version_line, "sonora-plugin-host layout 1 clock 1 wire 1",
// and the engine parses it with parse_host_version_line, which requires all
// three; a mismatch disables plugin hosting for the run, loudly.
//
// ---------------------------------------------------------------------------
// Ordering with the shared-memory rules of protocol.h. THE REGION IS THE
// TRUTH; a socket message is a wake-up and the text.
//
//  - Loaded is sent only after the host has published, in the region, its
//    latency (latency_frames relaxed, then latency_generation release) and
//    then state = Ready (release). Refused only after refusal,
//    refusal_instance and state = Refused. Fault{Plugin} only after state =
//    Faulted. LatencyChanged and Reconfigured only after the
//    latency_generation bump they carry.
//  - On any of them the control thread re-reads the region by protocol.h's
//    rule: state (acquire); on Ready, latency_generation (acquire), then
//    latency_frames through checked_latency. Where a message's figure and
//    the region's differ, the region's wins: it is newer, and both are
//    untrusted. The audio thread never reads either; a plan carries the
//    figure the builder used (§5.4).
//  - The 50 ms poll of latency_generation and heartbeat stays: a host whose
//    main thread a plugin has wedged sends nothing, and a crashed host sends
//    nothing at all. The pidfd and the region say so.
//  - Load comes after the engine has seeded the table (seed_value) and the
//    bypass words: the host applies them before validation, so the latency
//    in Loaded is the one of those values. State in Load is restored BEFORE
//    the table is applied: parameters the table holds win over the same
//    parameters in a state (the model's values are authoritative, ADR-0012
//    §6.1); a state carries what the table cannot (internal state,
//    parameters past kMaxParamsPerChain).
//  - The engine submits no block before it has seen Ready in the region
//    (protocol.h); Loaded is how it learns to look without polling.
//
// ---------------------------------------------------------------------------
// Parameter touches and gestures (PR #46 review H1, option a).
//
//  - A value moved in the plugin's own editor (VST3 performEdit; LV2 UI
//    touch once editors are hosted) reaches the engine as ParamTouched,
//    inside GestureBegin ... GestureEnd for that (instance, param). The host
//    NEVER applies a touched value itself: the audio thread is the ring's
//    only producer (§5.1), so the engine sends it back tagged, through an
//    RtCommand, like any other change.
//  - Every touch is inside a gesture: a touch with none open is wrapped by
//    the host in its own GestureBegin/GestureEnd. Touches are coalesced per
//    (instance, param) to at most one per kTouchIntervalMs; the last value
//    of a gesture is never coalesced away (it is in GestureEnd).
//  - GestureEnd's value is the plugin's current value when the gesture
//    ended (VST3: the controller's, read at endEdit), not the last touch
//    the host happened to queue, so a touch lost on the way never makes it
//    stale (rt-engineer M3 on PR #51). A format that cannot say uses the
//    last touch.
//  - No gesture stays open because an event was lost (M3). A plugin's touch
//    can be dropped (the host's queue full, or taken by the main thread at
//    that instant; counted in Fault{EventsDropped}). When the host sees any
//    drop, it ends every gesture still open at once, each with the plugin's
//    current value read on its main thread, before it sends that Fault. A
//    later touch of such a gesture starts a new one; a later end of it is
//    ignored. So every GestureBegin gets its GestureEnd, and the engine
//    records SetInsertParams for every parameter that moved.
//  - The engine does not trust the host to coalesce (M2). It enforces, per
//    host: at most kMaxTouchesPerSecondPerParam ParamTouched per (instance,
//    param), and at most kMaxGestureEndsPerSecond GestureEnd in any
//    one-second window. Past either, the host is a protocol fault (kill,
//    crash path, PluginFault naming the insert): each GestureEnd costs a
//    model mutation, a broadcast and an undo entry, and the drain budget
//    gives fairness between hosts, not a rate limit. Implemented with the
//    control-server wiring (ADR-0012 checklist 5).
//  - The control server applies SetInsertParams for an instance at every
//    GestureEnd and at EditorClosed, so the document, every client and the
//    saved values follow the editor. Before EditorClosed for an instance,
//    and before it stops for any reason it can report, the host sends
//    GestureEnd for every gesture still open on it. A host that dies leaves
//    gestures open: the engine ends them itself with the last ParamTouched
//    value it saw.
//  - `param` is the dense index (protocol.h, "Parameters"). A touch on the
//    plugin's designated bypass parameter (VST3 kIsBypass, LV2 lv2:enabled)
//    is sent as kBypassParam with 1 = bypassed, 0 = not, so the engine
//    records SetInsertBypass, never a parameter. A touch on a parameter the
//    host has no dense index for (read-only, unknown) is dropped and counted
//    (Fault{EventsDropped}). Values are format-native and finite.
//  - Gesture events are sent only after Loaded; earlier ones are held.
//  - Keys. Loaded lists every instance's dense parameters with their stable
//    keys and ranges (ParamDesc). The engine resolves a client's
//    PluginParameterChange{insert, key} (contracts/api protocol 3) against
//    that table of the RUNNING instance, and maps a touch's dense index back
//    to its key for SetInsertParams. A host that replaces it by
//    make-before-break brings its own table in its own Loaded.
//
// Editors. OpenEditor carries the opaque parent-window token (ADR-0007
// update 1 item 6), empty on the appliance (ADR-0010: parentless X11
// toplevels placed by labwc). The host answers EditorOpened or EditorFailed;
// it sends EditorClosed exactly once for every EditorOpened, whether the
// engine asked (CloseEditor), the operator closed the window, or the host is
// stopping. CloseEditor for an editor that is not open is ignored. This
// build hosts no editor yet (BACKLOG Track 4 item 5): every OpenEditor is
// answered with EditorFailed and its reason.
//
// State (ADR-0012 §5.3, §6.1). contracts/api's ModelAction::FlushPluginState
// is a GetState for every running instance, then the files, then
// SetInsertState and SetInsertParams, then the reply. GetState asks one
// instance for its state,
// on the host's main thread while the processing thread runs (VST3
// IComponent::getState, a main-thread call; LV2 state:interface save(),
// which the spec allows concurrently with run()). The bytes are a StateBlob
// (format, plugin id, the format's sections), opaque to the engine except
// for its identity. The engine writes them, as they are, to
// "Plugin State/<sha256>.state" beside the session (state_file_name; the
// saver thread, durably, before the session file that names it) and later
// hands the same bytes back in Load. Restore happens only in Load, before
// the processing thread starts: a running host's state is never replaced
// (make-before-break, §5.3). A state the plugin refuses does not refuse the
// chain: Loaded says StateResult::Refused for that instance and the engine
// shows it loaded with defaults plus its params, as for a hash mismatch.
//
// Reconfigure (BACKLOG N1: a latency-changing value moved during playback).
// The host pauses its processing thread at a block boundary (it keeps
// bumping heartbeat), re-activates every plugin on its main thread,
// validates the chain again, re-applies bypass, publishes the latency
// (region first), resumes, and answers Reconfigured. A plugin that will not
// re-activate faults the host (Fault{Plugin}, exit kHostExitFaulted): it
// must not run inactive.
//
// The audio side of it (rt-engineer M4 on PR #51; implemented with
// ADR-0012 checklist 5). The audio thread never sees the socket, so:
//  - BEFORE sending Reconfigure, the control thread sets an "excused" flag
//    on the InsertChain node's channel slot through an RtCommand. While it
//    is set, blocks the node does not collect are misses all the same
//    (§3: faded, silent, counted as misses) but do not count toward the
//    8-miss removal.
//  - The flag is cleared by the plan built from the latency_generation that
//    Reconfigured carries (read from the region, §5.4), when that plan is
//    swapped in, not when the message arrives. A Reconfigured that never
//    comes (kReconfigureTimeoutMs) clears it with the kill.
//  - The host resumes processing BEFORE it sends Reconfigured, and its
//    blocks then carry the new latency. Until the new plan is in, the old
//    alignment stands (§5.4): that window plays the chain MISALIGNED by the
//    latency change, not silent. The plan swap realigns it; the delay lines
//    are kept per (from, to) pair, so the swap restarts none of them.
//
// Shutdown. The engine sends Shutdown, or closes its end (the host treats
// end-of-file as Shutdown), or sets the region's `shutdown` word (the path
// the RT side has). The host sends GestureEnd for open gestures and
// EditorClosed{HostStopping} for open editors, stores state = Stopped and
// exits kHostExitClean.
//
// Timeouts the engine applies (control thread, never a wait on any audio
// path): Hello within kHelloTimeoutMs of the fork; Loaded or Refused within
// kLoadTimeoutMs of Load (ADR-0012 §5.3's 10 s); a GetState, DescribeValue
// or OpenEditor reply within kReplyTimeoutMs; Reconfigured within
// kReconfigureTimeoutMs. Past one, the host is hung: SIGKILL, crash path.
// The same for a host whose queue of unsent frames would pass
// kMaxQueuedBytes (it is not reading its socket), and for one with a
// message half received that has not grown for kStalledMessageMs
// (Link::stalled: it is pinning engine memory, M1).
//
// Non-blocking use. Link (below) holds one side's frames to send and the
// message being reassembled; it never touches the OS. The engine's control
// thread polls the socket (ControlChannel::native_handle) beside the
// host's pidfd, and on POLLIN calls drain() with a receive that never
// waits, on POLLOUT (asked for only while wants_write()) flush() with a send
// that never waits. drain() takes at most a bounded number of datagrams per
// call, so one chatty host cannot starve the others.
// ---------------------------------------------------------------------------

#include "sonora/plugin/protocol.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace sonora::plugin::control {

inline constexpr uint32_t kHostWireVersion = 1;

// --- caps (untrusted input is checked against these before anything is held)
inline constexpr std::size_t kMaxIdBytes = 1024;            // LV2 URI; VST3 is 32 hex digits
inline constexpr std::size_t kMaxTextBytes = 4096;          // refusal, fault and failure text
inline constexpr std::size_t kMaxTokenBytes = 256;          // the editor's parent-window token
inline constexpr std::size_t kMaxValueTextBytes = 256;      // DescribeValue's answer
inline constexpr std::size_t kMaxKeyBytes = 256;            // a parameter's stable key
inline constexpr uint32_t kMaxParamsPerInstance = 65536;    // dense parameters one instance may list
inline constexpr std::size_t kMaxStateBytes = std::size_t{16} << 20;  // one instance's StateBlob
inline constexpr std::size_t kMaxStateSections = 4;
inline constexpr std::size_t kMaxFrameBytes = std::size_t{64} << 10;  // one datagram
inline constexpr std::size_t kFrameHeaderBytes = 8;
// Caps per direction (rt-engineer M1 on PR #51): what one host can make the
// engine hold is bounded by what the engine's side needs, never by the
// largest message either side may send.
//
// Engine -> host: one Load of a whole chain's states, with room for its
// fixed fields. A chain whose states together pass it cannot be loaded:
// Link::queue refuses it, and the engine reports a PluginFault naming the
// insert ("the chain's plugin states exceed the 64 MiB Load cap").
inline constexpr std::size_t kMaxMessageBytes = std::size_t{64} << 20;
// Host -> engine: a State at its cap, or a Loaded whose key tables fit,
// with room for the fixed fields. The engine's Reassembler refuses a
// longer length at the first frame, before anything is held. A host whose
// Loaded would pass it refuses the chain instead of sending it
// (Refusal::UnsupportedFeature, "parameter table too large").
inline constexpr std::size_t kMaxHostMessageBytes = kMaxStateBytes + (std::size_t{1} << 20);
// Unsent frames the engine holds for one host before the host counts as not
// reading its socket: one Load at its cap and the small requests behind it.
// A host sends blocking (it has one peer), so it keeps no queue.
inline constexpr std::size_t kMaxQueuedBytes = kMaxMessageBytes + (std::size_t{1} << 20);
inline constexpr uint32_t kMaxDatagramsPerDrain = 64;

// --- timing
inline constexpr int kHelloTimeoutMs = 10000;
inline constexpr int kLoadTimeoutMs = 10000;
inline constexpr int kReplyTimeoutMs = 5000;
inline constexpr int kReconfigureTimeoutMs = 10000;
inline constexpr int kTouchIntervalMs = 10;  // 100 Hz, ADR-0012 §4
// A message half received that has not grown for this long: the host is
// pinning engine memory, and is killed (rt-engineer M1 on PR #51).
inline constexpr int kStalledMessageMs = kReplyTimeoutMs;
// The engine's own limits on what a host's touches may cost it (M2): past
// either, in any one-second window, the host is a protocol fault.
inline constexpr uint32_t kMaxTouchesPerSecondPerParam = 1000 / kTouchIntervalMs + 10;
inline constexpr uint32_t kMaxGestureEndsPerSecond = 50;

enum class Type : uint8_t {
    // engine -> host
    EngineHello = 1,
    Load = 2,
    GetState = 3,
    DescribeValue = 4,
    OpenEditor = 5,
    CloseEditor = 6,
    Reconfigure = 7,
    Shutdown = 8,
    // host -> engine
    Hello = 64,
    Loaded = 65,
    Refused = 66,
    State = 67,
    ValueText = 68,
    EditorOpened = 69,
    EditorFailed = 70,
    EditorClosed = 71,
    GestureBegin = 72,
    ParamTouched = 73,
    GestureEnd = 74,
    LatencyChanged = 75,
    Reconfigured = 76,
    Fault = 77,
};

// True for the types the engine sends; the host refuses any other.
constexpr bool engine_to_host(Type t) {
    return static_cast<uint8_t>(t) >= 1 && static_cast<uint8_t>(t) <= 8;
}
// True for the types the host sends; the engine refuses any other.
constexpr bool host_to_engine(Type t) {
    return static_cast<uint8_t>(t) >= 64 && static_cast<uint8_t>(t) <= 77;
}

enum class Format : uint8_t { Lv2 = 0, Vst3 = 1 };

// --- engine -> host ---------------------------------------------------------

struct EngineHello {
    uint32_t wire_version = kHostWireVersion;
    uint32_t layout_version = kLayoutVersion;
    uint32_t clock_version = kClockLayoutVersion;
};

// One chain instance. `id` is the LV2 URI, or the VST3 class id as 32 hex
// digits (ADR-0012 §6.1, without the "vst3:" the command line uses).
// `params` must equal the header's instance_params[i]. `state` is a
// StateBlob from an earlier GetState, or empty.
struct LoadInstance {
    Format format = Format::Lv2;
    std::string id;
    uint32_t params = 0;
    std::string state;
};

inline constexpr uint32_t kLoadReactivate = 1u << 0;  // re-activate after the first validation
inline constexpr uint32_t kLoadKnownFlags = kLoadReactivate;

// The chain, once. `key_channels`: the last N of the region's channels are
// a side-chain key, not signal (every plugin with a key input reads them;
// their output planes stay silent). Fewer than the region's channels.
struct Load {
    uint32_t key_channels = 0;
    uint32_t flags = 0;
    std::vector<LoadInstance> instances;  // 1 .. kMaxInstances, the header's instance_count
};

struct GetState {
    uint32_t request = 0;
    uint32_t instance = 0;
};

struct DescribeValue {
    uint32_t request = 0;
    uint32_t instance = 0;
    uint32_t param = 0;  // dense index
    float value = 0.f;   // format-native, finite
};

struct OpenEditor {
    uint32_t request = 0;
    uint32_t instance = 0;
    std::string parent_token;  // opaque; empty on the appliance
};

struct CloseEditor {
    uint32_t instance = 0;
};

struct Reconfigure {
    uint32_t request = 0;
};

struct Shutdown {};

// --- host -> engine ---------------------------------------------------------

struct Hello {
    uint32_t wire_version = kHostWireVersion;
    uint32_t layout_version = kLayoutVersion;
    uint32_t clock_version = kClockLayoutVersion;
    uint64_t pid = 0;
};

enum class StateResult : uint8_t {
    None = 0,      // no state was given
    Restored = 1,  // the plugin took it
    Refused = 2,   // the plugin refused it, or it is another plugin's: defaults plus params
};

enum class BypassBy : uint8_t { Host = 0, Plugin = 1 };

// One dense parameter of a running instance, in dense order: the key the
// session file stores (LV2 port symbol, VST3 ParamID in decimal; ADR-0012
// §6.1) and the format-native range. The engine resolves a client's
// PluginParameterChange{insert, key} against THIS table (contracts/api,
// protocol 3), not the catalog's, so a rescan that reordered a plugin's
// parameters can never move another one; and it names the key of a touch's
// dense index when it records SetInsertParams. Untrusted: keys are 1 ..
// kMaxKeyBytes bytes and unique within the instance; min <= max and every
// figure finite.
struct ParamDesc {
    std::string key;
    float min = 0.f;
    float max = 1.f;
    float default_value = 0.f;
};

struct LoadedInstance {
    uint32_t latency_frames = 0;  // the plugin's own, no IPC part
    uint32_t replicas = 0;
    StateResult state = StateResult::None;
    BypassBy bypass = BypassBy::Host;
    std::vector<ParamDesc> params;  // exactly LoadInstance::params entries
};

struct Loaded {
    uint32_t latency_frames = 0;      // the chain's, IPC included, as in the region
    uint32_t latency_generation = 0;  // as in the region when this was sent
    uint32_t host_flags = 0;          // HostFlag bits
    std::vector<LoadedInstance> instances;
};

struct Refused {
    Refusal reason = Refusal::None;
    int32_t instance = -1;
    std::string text;
};

enum class StateStatus : uint8_t {
    Ok = 0,      // bytes is a StateBlob
    Empty = 1,   // the plugin keeps no state of its own; its params are all there is
    Failed = 2,  // text says why
};

struct State {
    uint32_t request = 0;
    uint32_t instance = 0;
    StateStatus status = StateStatus::Ok;
    std::string bytes;  // a StateBlob, at most kMaxStateBytes
    std::string text;
};

struct ValueText {
    uint32_t request = 0;
    bool ok = false;
    std::string text;
};

struct EditorOpened {
    uint32_t request = 0;
    uint32_t instance = 0;
};

struct EditorFailed {
    uint32_t request = 0;
    uint32_t instance = 0;
    std::string text;
};

enum class EditorCloseReason : uint8_t {
    Requested = 0,     // CloseEditor
    ByOperator = 1,    // the window was closed
    HostStopping = 2,  // Shutdown, end-of-file, or the region's shutdown word
};

struct TouchedValue {
    uint32_t param = 0;  // dense index, or kBypassParam
    float value = 0.f;
};

// `moved`: the last value of every parameter touched while the editor was
// open, each once, so the engine can record SetInsertParams for what moved
// without a gesture end (an LV2 UI that never sends one) even had it missed
// a ParamTouched. At most kMaxParamsPerInstance + 1 entries.
struct EditorClosed {
    uint32_t instance = 0;
    EditorCloseReason reason = EditorCloseReason::Requested;
    std::vector<TouchedValue> moved;
};

struct GestureBegin {
    uint32_t instance = 0;
    uint32_t param = 0;  // dense index, or kBypassParam
};

struct ParamTouched {
    uint32_t instance = 0;
    uint32_t param = 0;
    float value = 0.f;
};

struct GestureEnd {
    uint32_t instance = 0;
    uint32_t param = 0;
    float value = 0.f;  // the gesture's last value
};

struct LatencyChanged {
    uint32_t latency_generation = 0;
    uint32_t latency_frames = 0;
};

struct Reconfigured {
    uint32_t request = 0;
    uint32_t latency_generation = 0;
    uint32_t latency_frames = 0;
};

enum class FaultKind : uint8_t {
    Plugin = 0,         // a plugin misbehaved; the host exits kHostExitFaulted
    Protocol = 1,       // the engine broke this contract; the host exits kHostExitUsage
    EventsDropped = 2,  // touches the host could not queue or map; `count` of them
};

struct Fault {
    FaultKind kind = FaultKind::Plugin;
    int32_t instance = -1;
    uint64_t count = 0;
    std::string text;
};

// --- codec ------------------------------------------------------------------
//
// decode() returns false with the reason in `error`; `out` is then
// unspecified and must not be used. A message is never half-accepted.

// The type byte, or false for an empty buffer or a byte no message has.
bool peek_type(std::string_view bytes, Type& out);

// The u32 after the type byte of either hello; the only part of them that
// never moves. False if `bytes` is not a hello or is too short.
bool decode_wire_version(std::string_view bytes, uint32_t& wire_version);

void encode(const EngineHello& m, std::string& out);
void encode(const Load& m, std::string& out);
void encode(const GetState& m, std::string& out);
void encode(const DescribeValue& m, std::string& out);
void encode(const OpenEditor& m, std::string& out);
void encode(const CloseEditor& m, std::string& out);
void encode(const Reconfigure& m, std::string& out);
void encode(const Shutdown& m, std::string& out);
void encode(const Hello& m, std::string& out);
void encode(const Loaded& m, std::string& out);
void encode(const Refused& m, std::string& out);
void encode(const State& m, std::string& out);
void encode(const ValueText& m, std::string& out);
void encode(const EditorOpened& m, std::string& out);
void encode(const EditorFailed& m, std::string& out);
void encode(const EditorClosed& m, std::string& out);
void encode(const GestureBegin& m, std::string& out);
void encode(const ParamTouched& m, std::string& out);
void encode(const GestureEnd& m, std::string& out);
void encode(const LatencyChanged& m, std::string& out);
void encode(const Reconfigured& m, std::string& out);
void encode(const Fault& m, std::string& out);

bool decode(std::string_view bytes, EngineHello& out, std::string& error);
bool decode(std::string_view bytes, Load& out, std::string& error);
bool decode(std::string_view bytes, GetState& out, std::string& error);
bool decode(std::string_view bytes, DescribeValue& out, std::string& error);
bool decode(std::string_view bytes, OpenEditor& out, std::string& error);
bool decode(std::string_view bytes, CloseEditor& out, std::string& error);
bool decode(std::string_view bytes, Reconfigure& out, std::string& error);
bool decode(std::string_view bytes, Shutdown& out, std::string& error);
bool decode(std::string_view bytes, Hello& out, std::string& error);
bool decode(std::string_view bytes, Loaded& out, std::string& error);
bool decode(std::string_view bytes, Refused& out, std::string& error);
bool decode(std::string_view bytes, State& out, std::string& error);
bool decode(std::string_view bytes, ValueText& out, std::string& error);
bool decode(std::string_view bytes, EditorOpened& out, std::string& error);
bool decode(std::string_view bytes, EditorFailed& out, std::string& error);
bool decode(std::string_view bytes, EditorClosed& out, std::string& error);
bool decode(std::string_view bytes, GestureBegin& out, std::string& error);
bool decode(std::string_view bytes, ParamTouched& out, std::string& error);
bool decode(std::string_view bytes, GestureEnd& out, std::string& error);
bool decode(std::string_view bytes, LatencyChanged& out, std::string& error);
bool decode(std::string_view bytes, Reconfigured& out, std::string& error);
bool decode(std::string_view bytes, Fault& out, std::string& error);

// An LV2 URI (1 .. kMaxIdBytes bytes, none of them a control character or
// a space) or a VST3 class id (exactly 32 hex digits).
bool valid_plugin_id(Format format, std::string_view id);

// The engine's check of a touch's (instance, param) against the chain it
// built: a dense index of that instance, or kBypassParam on an instance
// that exists. Parameters past kMaxParamsPerChain are in the chain (the
// document records them) though the table has no slot for them.
inline bool param_in_chain(const ParamMap& m, uint32_t instance, uint32_t param) noexcept {
    if (instance >= m.instance_count) return false;
    return param == kBypassParam || param < m.count[instance];
}

// --- frames -----------------------------------------------------------------

// Appends `message`'s frames to `frames`. Refuses an empty message and one
// over kMaxMessageBytes, which no receiver would take.
bool split_frames(std::string_view message, std::vector<std::string>& frames, std::string& error);

class Reassembler {
public:
    enum class Result { Partial, Complete, Error };

    // `max_message`: the longest message this direction carries
    // (kMaxHostMessageBytes on the engine's side, kMaxMessageBytes on the
    // host's). A first frame announcing more is refused before anything is
    // held.
    explicit Reassembler(std::size_t max_message = kMaxMessageBytes) : max_(max_message) {}

    // Complete: `message` is the whole message. Error: the frame is out of
    // place, `error` says how, and what was held is dropped. The caller then
    // ends the connection.
    Result add(std::string_view frame, std::string& message, std::string& error);
    bool idle() const { return expected_ == 0; }
    std::size_t held() const { return partial_.size(); }

private:
    Result fail(std::string& error, std::string why);

    std::size_t max_;
    std::string partial_;
    std::size_t expected_ = 0;
};

// --- one side of a connection, without the OS ---------------------------------

enum class Io { Ok, WouldBlock, Closed, Error };

// The defaults are the engine's side of one host: it receives at most
// kMaxHostMessageBytes per message and queues at most kMaxQueuedBytes. So
// one host can make the engine hold at most about 82 MiB, and that only
// while a Load at its cap is in flight; otherwise about 17 MiB.
class Link {
public:
    explicit Link(std::size_t max_receive_message = kMaxHostMessageBytes,
                  std::size_t max_queued_bytes = kMaxQueuedBytes)
        : max_queued_(max_queued_bytes), in_(max_receive_message) {}

    // Frames `message` and holds them until flush() sends them. False, with
    // nothing held, for a message no receiver would take, or when the frames
    // held would pass the cap (kMaxQueuedBytes: the peer is not reading).
    bool queue(std::string_view message, std::string& error);
    bool wants_write() const { return !out_.empty(); }
    std::size_t queued_bytes() const { return queued_bytes_; }

    // Sends held frames in order until none is left (Ok) or the transport
    // would block (WouldBlock, frames kept). `send(std::string_view)` returns
    // an Io and never waits; Closed and Error are passed on.
    template <class Send>
    Io flush(Send&& send) {
        while (!out_.empty()) {
            const Io r = send(std::string_view(out_.front()));
            if (r != Io::Ok) return r;
            queued_bytes_ -= out_.front().size();
            out_.pop_front();
        }
        return Io::Ok;
    }

    // Receives at most `max_datagrams` datagrams, reassembling them, and
    // calls `on(std::string_view message)` for each whole one; `on` returns
    // false to refuse it (a decode or protocol failure, its reason in
    // `error`). `receive(std::string& datagram)` returns an Io and never
    // waits. Returns WouldBlock when the transport is drained, Ok when the
    // datagram budget ran out first (call again), Closed when the peer is
    // gone, Error on a framing or protocol failure (the caller ends the
    // connection). `now_ns` is the caller's monotonic clock: every frame
    // taken is progress, for stalled().
    template <class Receive, class On>
    Io drain(Receive&& receive, On&& on, std::string& error, uint64_t now_ns,
             uint32_t max_datagrams = kMaxDatagramsPerDrain) {
        std::string datagram, message;
        for (uint32_t n = 0; n < max_datagrams; ++n) {
            const Io r = receive(datagram);
            if (r != Io::Ok) return r;
            progress_ns_ = now_ns;
            switch (in_.add(datagram, message, error)) {
            case Reassembler::Result::Partial: break;
            case Reassembler::Result::Error: return Io::Error;
            case Reassembler::Result::Complete:
                if (!on(std::string_view(message))) return Io::Error;
                break;
            }
        }
        return Io::Ok;
    }

    // A message is half received and no frame of it has come for
    // `timeout_ns` (kStalledMessageMs): the peer is pinning memory. The
    // engine's control thread checks this with its 50 ms poll and kills
    // the host (M1).
    bool stalled(uint64_t now_ns, uint64_t timeout_ns) const {
        return !in_.idle() && now_ns - progress_ns_ >= timeout_ns;
    }
    std::size_t held_bytes() const { return in_.held(); }

private:
    std::size_t max_queued_;
    std::deque<std::string> out_;
    std::size_t queued_bytes_ = 0;
    Reassembler in_;
    uint64_t progress_ns_ = 0;
};

// --- state ------------------------------------------------------------------

// What GetState returns and Load takes: a plugin's state, whose identity
// both sides can check before a plugin ever sees the bytes. Sections are
// the format's: LV2 one (the state as Turtle, lilv_state_to_string, port
// values not included: they are the table's); VST3 two (IComponent's
// getState, then IEditController's).
struct StateBlob {
    Format format = Format::Lv2;
    std::string id;  // as in LoadInstance
    std::vector<std::string> sections;
};

inline constexpr uint32_t kStateMagic = 0x54534E53u;  // "SNST"
inline constexpr uint32_t kStateBlobVersion = 1;

// False when the blob would pass kMaxStateBytes or kMaxStateSections.
bool encode_state_blob(const StateBlob& blob, std::string& out, std::string& error);
bool decode_state_blob(std::string_view bytes, StateBlob& out, std::string& error);

// SHA-256 of `bytes`, as 64 lowercase hex digits.
std::string sha256_hex(std::string_view bytes);
// "Plugin State/<sha256>.state", relative to the session folder (ADR-0012
// §6.1). Empty when `sha256` is not 64 lowercase hex digits.
std::string state_file_name(std::string_view sha256);
// The check at open: the file's bytes hash to the name the session gives.
bool state_matches(std::string_view bytes, std::string_view sha256);

// --- `sonora-plugin-host --version` -------------------------------------------

// The host's line, without the newline: protocol.h's line with
// `wire <kHostWireVersion>` appended (protocol.h, Versioning). Returns what
// snprintf returns.
int format_host_version_line(char* out, std::size_t size);

// The engine's parser: protocol.h's grammar, requiring layout, clock and
// wire, each once. Keys it does not know are skipped with their value.
bool parse_host_version_line(const char* line, uint32_t& layout, uint32_t& clock, uint32_t& wire);

}  // namespace sonora::plugin::control
