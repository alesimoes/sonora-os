# ADR-0007 — Module boundaries, contracts, and parallel squads

- Status: Accepted by the owner 2026-10-12, including §3 (process split),
  which supersedes ADR-0006's in-process shape
- Date: 2026-10-12
- Decided by: architect, accepted by the owner. The two questions the owner
  settled personally are §3 and opening Phase 3/4 work early (§10); both
  were answered 2026-10-12, put to the owner directly by the coordinating
  agent in that session.
- Amended by the owner 2026-10-13: the §10 early-scope limit is lifted for
  the Complete DAW milestone (dated note at the end of §10).

## Context

The owner wants several squads (one agent on the interface, one on drivers,
one on plugin integration) working on the product at the same time. That
works only if each squad can build, test and merge its part without waiting
on another squad's code. Today nothing about the tree supports that:

- Everything is one static library, `sonora_core` (`wav_writer`, `track`,
  `disk_thread`, `session`, `jack_backend`, `keymap`), plus `main.cpp`.
  `jack_backend.h` includes `<jack/jack.h>` in a public header and
  `sonora_core` exports the JACK include path, so anything that links the core
  also gets JACK. A Qt interface built on that library would compile against
  JACK headers.
- `JackBackend::process_cb` calls `Session::process` directly. Nothing in the
  engine runs without a JACK server, so the only engine loop that does not
  need hardware is `tools/test-engine-docker.sh` (a real PipeWire in a
  container).
- The key map work (uncommitted in the working tree) puts the command
  vocabulary (`command.h`), a `std::function`-based `CommandRegistry` and the
  JSON key map loader into `sonora_core`, next to the code that runs on the
  audio thread.
- `main.cpp` is the interface. It reads `Track` atomics, calls
  `Session::start_recording`, and owns the command handlers.
- ADR-0006 put the Qt UI in the engine's address space, while listing "whether
  the GUI is in-process or split across a shared-memory boundary" as open.
  The plugin-engineer charter and `brain/specs/plugin-latency-compensation.md`
  already require plugins to run out of process, which means we have to build
  lock-free shared-memory IPC whatever we decide about the UI.

Squads map to the existing agent roles: `rt-engineer` (engine core),
`ui-engineer` (Qt UI, key maps, workflow layer), `driver-engineer` (audio
backends, control surfaces, sync), `plugin-engineer` (plugin hosting,
interchange), and `distro-engineer` (`iso/`, `audio-stack/`, `tools/`).

The project is in Phase 1. The UI is Phase 3, and plugins and interchange are
Phase 4.

## Decision

### 1. Modules, directories and the dependency graph

All C++ moves under one CMake root at the repository top level. It is
organised by owner, so a path tells you who owns the code and whether it may
run on the audio thread. Modules marked *(Pn)* are reserved. Their directory
is not created until that phase starts, because an empty scaffold invites
code written before the design exists.

| Directory | CMake target | Owns | Owner |
|---|---|---|---|
| `contracts/api/` | `sonora_api` | Command vocabulary, control messages, the shared state block layout, `EngineControl`, wire codec | ui-engineer + rt-engineer jointly |
| `contracts/backend/` | `sonora_backend_api` (interface library) | `AudioBackend`, `ProcessCallback`, `AudioBlock` | rt-engineer + driver-engineer jointly |
| `contracts/plugin/` *(P4)* | `sonora_plugin_protocol` | Shared-memory layout between the engine and a plugin host process | plugin-engineer + rt-engineer jointly |
| `common/base/` | `sonora_base` | RT-safe primitives: `RingBuffer`, fixed-capacity containers, the `SONORA_RT` marker | rt-engineer |
| `common/audiofile/` | `sonora_audiofile` | `WavWriter` (BWF), later RF64 and readers | rt-engineer |
| `common/model/` *(P2)* | `sonora_model` | Session/timeline document, undo operations, native session file format | rt-engineer; ui- and plugin-engineer review |
| `engine/core/` | `sonora_core` | `Session`, `Track`, `Transport`, `DiskThread`, later the processing graph and PDC plan | rt-engineer |
| `engine/control/` | `sonora_control` | Engine side of the control API: command handlers, the state publisher thread, the control server | rt-engineer |
| `engine/app/` | `sonora-engine` (exe) | Wiring, options, headless `--record` mode | rt-engineer |
| `engine/plugin_client/` *(P4)* | `sonora_plugin_client` | Engine side of the plugin boundary, RT half included | plugin-engineer writes; rt-engineer and rt-safety-auditor gate |
| `engine/sync/` *(P5)* | `sonora_sync` | LTC/MTC/word clock as clock sources | driver-engineer writes; rt-safety-auditor gates |
| `drivers/jack/` | `sonora_backend_jack` | `JackBackend`, `port_order.h`, `probe_capture_channels`. The **only** target that may include `<jack/jack.h>` | driver-engineer |
| `drivers/fake/` | `sonora_backend_fake` | Deterministic backend for tests and hardware-free runs | driver-engineer |
| `drivers/surfaces/` *(P5)* | `sonora-surfaces` (exe) | HUI and Mackie Control, as a control API client | driver-engineer |
| `ui/keymap/` | `sonora_keymap` | `KeyMap`, `KeyChord`, `TerminalInput`, `ensure_essentials` | ui-engineer |
| `ui/client/` | `sonora_client` | UI side of the control API: transport, `StateView`, reconnect | ui-engineer |
| `ui/tui/` | `sonora-tui` (exe, after M7; a library before) | The terminal interface, the diagnostic of last resort (ADR-0006) | ui-engineer |
| `ui/qt/` *(P3)* | `sonora-ui` (exe) | The Qt 6 interface. The **only** target that may include Qt | ui-engineer |
| `plugins/host/` *(P4)* | `sonora-plugin-host` (exe) | Loads LV2/VST3 out of process. The only target that links plugin SDKs | plugin-engineer |
| `interchange/` *(P4)* | `sonora_interchange`, `sonora-convert` (exe) | AAF, `.ptx`, SMF import and export to and from `sonora_model` | plugin-engineer |
| `keymaps/`, `iso/`, `audio-stack/`, `tools/`, `.github/` | — | Unchanged | as today |

**Allowed dependencies.** An arrow means "may link and include". Any edge not
drawn here is forbidden.

```
layer 0   common/base
layer 1   contracts/backend   contracts/api   contracts/plugin(P4)
          common/audiofile    common/model(P2)
layer 2   engine/core ──> contracts/backend, common/audiofile, common/model,
                          engine/plugin_client(P4), engine/sync(P5)
          drivers/jack, drivers/fake ──> contracts/backend      (JACK: jack only)
          ui/keymap ──> contracts/api
          ui/client ──> contracts/api, common/model
          plugins/host(P4) ──> contracts/plugin                  (plugin SDKs)
          interchange(P4) ──> common/model, common/audiofile     (libaaf, libptformat)
layer 3   engine/control ──> engine/core, contracts/api, common/model
          ui/tui, ui/qt, drivers/surfaces(P5) ──> ui/client, ui/keymap  (Qt: ui/qt only)
layer 4   engine/app ──> engine/control, engine/core, drivers/jack, drivers/fake
```

The same graph in words, because a diagram drifts:

- `common/base` depends on nothing but the standard library.
- `contracts/*` depend only on `common/base`. They contain interfaces, POD
  layouts and the wire codec, never behaviour that needs a thread.
- `engine/core` depends on `common/base`, `common/audiofile`,
  `contracts/backend` (it *implements* `ProcessCallback`), and later
  `common/model`, `engine/plugin_client` and `engine/sync`. **It does not
  depend on `contracts/api`.** The core knows nothing about the wire protocol.
- `engine/control` depends on `engine/core` and `contracts/api`. It is the
  adapter between the two, and it is the only place they meet.
- `drivers/jack` and `drivers/fake` depend on `contracts/backend` and
  `common/base`, **never on `engine/core`**.
- `ui/*` and `drivers/surfaces` depend on `contracts/api`, `ui/client` and
  `ui/keymap`, and from Phase 2 on `common/model` for the session replica.
  **They never depend on `engine/*` or `drivers/jack|fake`.**
- Every module may use `common/base`; the diagram leaves those edges out.
- **Nothing depends on `ui/`.** Nothing outside `drivers/jack` includes JACK
  headers. Nothing outside `ui/qt` includes Qt. Nothing outside
  `plugins/host` links a plugin SDK. Nothing outside `interchange/` links
  libaaf or libptformat.
- `plugins/host` and `interchange` never link `engine/core`. They see the
  engine only through a contract or through the native session format.

**Enforcement is mechanical, not a convention.** Each module exposes only its
own `include/` directory, publicly, and links its dependencies with
`target_link_libraries`. So an include across a forbidden edge fails to
compile. Each module's headers stay under `include/sonora/...`, and that
spelling does not change, so moving files touches no `#include` line. CI adds
a dependency check (migration step M5) that fails the build when a forbidden
header shows up in a forbidden directory: `<jack/` outside `drivers/jack`,
`<Q` outside `ui/qt`, any `engine/` include from `ui/`.

### 2. Where the real-time boundary sits

The real-time path is everything reachable from `ProcessCallback::process()`.
Today that is `JackBackend::process_cb` → `Session::process`. CLAUDE.md's rules
apply to all of it. **Code may enter that call graph only from these
modules:**

| Module | What it may contribute |
|---|---|
| `common/base` | Everything in it must be RT-safe; that is its definition |
| `engine/core` | `Session::process` and its callees, the plan swap, RT event rings |
| `drivers/jack`, `drivers/fake` | The callback trampoline and port-buffer fetch, nothing more |
| `engine/plugin_client` *(P4)* | `submit`/`collect` on the shared-memory channel |
| `engine/sync` *(P5)* | Clock decode that has to be sample-accurate |

Everything else is forbidden on the audio thread, including `contracts/api`.
Its POD structs may be *copied* by RT code, but no function in it may be
called there. The same goes for `engine/control`, `common/audiofile`,
`common/model`, all of `ui/`, and `interchange/`.

Rules that follow:

- **RT entry points are marked.** Functions callable from the audio thread are
  declared with `SONORA_RT`. For now that expands to `noexcept`, and the
  existing `// --- real-time safe ---` header comment stays. The
  `rt-safety-auditor` starts its call graph from every `SONORA_RT` symbol, not
  just from `process_cb`. If Clang's `[[clang::nonblocking]]` and RTSan become
  available (see Unverified), the macro grows into the attribute and a CI job
  catches allocation at runtime. That way the rule is checked by the
  compiler, not by whether each squad remembers it.
- **The audio thread does not write to shared memory a client can see.**
  It updates atomics on its own objects (`Track::report_peak`,
  `note_overrun`, `Transport::advance`) and pushes POD records into an SPSC
  `RingBuffer<RtEvent>`. A **publisher thread** in `engine/control`, at normal
  priority, copies that into the shared state block (§4.1). The wire layout
  can then change without touching the RT path, and no client can ever
  stall or corrupt the callback.
- **Commands reach the audio thread in three forms only:** an atomic store for
  a flag or a scalar (arm, mute, gain: what `Track` already does); a POD
  record in an SPSC `RingBuffer<RtCommand>` that the callback drains at block
  start, for sample-accurate events such as locate and punch points; and a
  **plan swap** for structural changes such as graph, routing, plugin chain
  or the PDC delays. In a plan swap, `engine/control` builds an immutable
  `ProcessPlan` off-thread and publishes it through an atomic pointer. The
  callback acquires it at block start and stores the epoch it saw. The
  control thread frees retired plans only after that epoch has passed. The
  callback never computes a plan and never frees one, which is the PDC spec's
  "swapped in atomically" requirement made concrete.

### 3. Process shape: engine, UI and plugin host are separate processes

**Decided by the owner, 2026-10-12.** The engine is a headless process. It owns the audio
backend, the disk thread, the session and the RT graph. Every interface is a
**client** of it: the Qt UI, the terminal UI, and later the control-surface
bridge. Plugins run in a third kind of process, the plugin host. Untrusted
interchange files are parsed in a fourth, short-lived process
(`sonora-convert`).

- Control channel: a Unix domain socket (`SOCK_SEQPACKET`) under
  `$XDG_RUNTIME_DIR/sonora/`. Commands and parameter changes go to the engine;
  replies and discrete events come back. Rates are human or 100 Hz, never
  per block.
- State channel: one `memfd` that the engine creates, sized at session
  creation and passed to each client over the socket (`SCM_RIGHTS`). **Clients
  map it `PROT_READ`.** A crashing or buggy UI cannot write engine state,
  because the kernel enforces the boundary.
- Bulk data (waveform overviews) uses neither channel. It goes through files
  or a separate read-only mapping, and is decided with the Phase 2 model.
- systemd: `sonora-engine.service` keeps `LimitRTPRIO=95`,
  `LimitMEMLOCK=infinity` and `Restart=always`. The UI runs under the kiosk
  compositor in its own unit, with no RT limits and its own restart.
- If the UI dies mid-take, the engine keeps recording. The restarted UI
  reattaches and finds the transport in `Recording`. If the engine dies, the
  UI says so on screen, in red, and shows meters as **no data** rather than
  frozen values (a control that lies is worse than one that is missing). The
  heartbeat in the state block is how it knows.

**This amends ADR-0006 § "Process shape: the GUI is in the engine's address
space"**, which that ADR itself left open. Everything else in ADR-0006 stands
and is strengthened. The UI still pulls at a fixed 60 Hz. The audio thread
still never pushes. No Qt object is reachable from the callback, now because
it is in another process.

**Why this went to the owner.** The split costs a state-replication
protocol, which is the single largest piece of work it adds (see
Consequences), and it reverses a stated consequence of an owner-decided ADR.
The owner accepted that cost on 2026-10-12. ADR-0006 carries a dated note
pointing here.

**How the transition stays safe.** Everything is written against
`EngineControl`. Until M7 lands, the terminal UI runs through
`InProcessEngine`, which runs the control server on a thread in the same
process. Every message still goes through the wire codec, and the state block
lives in anonymous memory. So M7 swaps one class, not the UI, and the shape
could still be reversed at that cost if a measurement in "Unverified"
argued for it.

### 4. The contracts

These are the seams. They are sketches of shape. The rt-engineer and the
owning parties settle the exact signatures when each contract lands, and a
change after that follows §6.

#### 4.1 Engine control API (`contracts/api/`)

Two kinds of traffic, deliberately kept apart:

- **Continuous values** (meters, transport position, arm/mute state, xrun and
  overrun counters, per-track PDC latency) are *state*. Clients **pull** them
  from the state block at their own rate.
- **Discrete facts** (a command's result, a take closed with this file, the
  server went away, a plugin dropped this block) are *events*. They arrive in
  order over the control channel.

```cpp
namespace sonora::api {

constexpr uint32_t kProtocolVersion = 1;    // wire messages
constexpr uint32_t kStateLayoutVersion = 1; // the shared state block

// --- client -> engine ---
struct CommandRequest {        // named, key-mappable, discrete
    uint64_t seq;
    std::string command;       // an id from command_catalog()
    int32_t target = -1;       // track index, -1 = selection
    std::string argument;
};
struct ParameterChange {       // continuous: fader drags, surface faders
    uint64_t seq;
    uint32_t track;
    ParamId param;             // Gain, Pan, ... (enum, append-only)
    float value;
};

// --- engine -> client ---
struct Hello {                 // first message after connect; mismatch = refuse
    uint32_t protocol_version, state_layout_version;
    uint32_t sample_rate, buffer_frames, track_count;
    std::vector<std::string> implemented_commands; // feeds ensure_essentials()
};
struct CommandReply { uint64_t seq; DispatchResult result; std::string message; };
struct EngineEvent  { EventKind kind; uint32_t track; uint64_t frame; std::string text; };

// --- the seam every interface codes against ---
class EngineControl {
public:
    virtual ~EngineControl() = default;
    virtual bool connect(std::string& error) = 0;  // fails loudly on version mismatch
    virtual const Hello& hello() const = 0;
    // Never blocks the caller: queues and returns the seq to match a reply.
    virtual uint64_t send(const CommandRequest&) = 0;
    virtual uint64_t send(const ParameterChange&) = 0;
    // Called from the client's own tick (60 Hz in the UI). Non-blocking.
    virtual void poll(std::vector<CommandReply>& replies,
                      std::vector<EngineEvent>& events) = 0;
    virtual StateView& state() = 0;
};
// Implementations: SocketEngineClient (ui/client, after M7),
// InProcessEngine (engine/control), FakeEngine (contracts/api/testing, owned
// by the provider -- rt-engineer -- so it cannot drift from the real one).

// Shared, fixed-size, created once per session. Writer: the engine's
// publisher thread only. Readers: any number of clients, read-only.
struct alignas(64) TrackState {
    std::atomic<uint32_t> flags;       // armed, muted, solo, input monitor
    std::atomic<float>    gain;
    std::atomic<uint32_t> overruns;
    std::atomic<uint32_t> clip_count;  // monotonic; a reader compares, never clears
    std::atomic<uint32_t> latency_frames;  // PDC, "reported honestly"
    PeakRing              peaks;       // last N published peaks + monotonic tick
};
struct EngineStateBlock {
    uint32_t magic, layout_version, capacity_tracks;
    std::atomic<uint32_t> track_count;
    std::atomic<uint64_t> heartbeat;   // publisher tick; a stall means engine trouble
    std::atomic<uint64_t> seq;         // seqlock over the transport snapshot
    TransportFields transport;         // state, playhead, take
    std::atomic<uint32_t> xruns, health_flags;  // server_gone, no_inputs, ...
    TrackState tracks[1];              // capacity_tracks entries follow
};
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "atomics in shared memory must be address-free");

// Reader side. The peak cursor is private to each reader, so the UI, the TUI
// and a control surface can all meter at once without stealing each other's
// peaks -- which Track::take_peak()'s exchange would do with two readers.
class StateView {
public:
    bool attach(const void* block, std::size_t bytes, std::string& error);
    TransportSnapshot transport() const;   // seqlock read, retries, never waits on the writer
    TrackSnapshot track(uint32_t i) const;
    float take_peak(uint32_t i);           // max since this reader's last call
    bool engine_alive(uint64_t now_ns) const;
};

}  // namespace sonora::api
```

The publisher thread is the single caller of `Track::take_peak()`, so the
existing exchange-to-clear semantics stay correct and no peak is lost between
publishes.

#### 4.2 Audio backend (`contracts/backend/`)

```cpp
namespace sonora::backend {

struct AudioBlock {
    const float* const* inputs;  uint32_t input_count;
    float* const*       outputs; uint32_t output_count;
    uint32_t nframes;
    uint64_t frame_time;   // backend's monotonic frame counter; gaps reveal xruns
};

class ProcessCallback {          // implemented by engine/core's Session
public:
    virtual void process(const AudioBlock& block) noexcept = 0;  // SONORA_RT
protected:
    ~ProcessCallback() = default;
};

struct PortInfo { std::string name; uint32_t channel; };  // channel order, natural_less
struct BackendConfig {
    std::string client_name;
    uint32_t inputs, outputs;
    uint32_t required_rate;      // ADR-0003: mismatch is a start-up failure
};

class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    // Usable before open(), so the session can be sized to the interface.
    virtual std::vector<PortInfo> physical_capture_ports() = 0;
    virtual std::vector<PortInfo> physical_playback_ports() = 0;
    virtual bool open(const BackendConfig&, ProcessCallback&, std::string& error) = 0;
    virtual bool activate(std::string& error) = 0;
    virtual int  connect_inputs_in_order() = 0;  // returns connections made; 0 must not be ignored
    virtual int  connected_input_count() const = 0;
    // After return, process() will not be called again; safe to tear down.
    virtual void deactivate() noexcept = 0;
    virtual uint32_t sample_rate() const = 0;
    virtual uint32_t buffer_frames() const = 0;
    virtual uint32_t xruns() const = 0;
    // Server loss, or a rate or buffer size change mid-session. The engine
    // closes the take cleanly and says why; it never adapts silently.
    virtual bool fatal() const = 0;
    virtual const std::string& fatal_reason() const = 0;
};

std::unique_ptr<AudioBackend> make_backend(std::string_view name); // "jack", "fake"
}
```

`drivers/fake` is a real implementation, not a mock. It has two drive modes:
synchronous `run_blocks(n)` on the test thread, which is deterministic, and
its own thread at block rate. It defaults to 18 in and 20 out, the UMC1820
shape. It can be configured with distinct per-channel test tones (the
channel-identity check, in-process), shuffled port names (`AUX10` before
`AUX2`), late enumeration (ports appearing seconds after start, as a USB
interface does at boot), and injected xrun, server loss, and rate or quantum
change. `sonora-engine --backend fake` runs the whole engine with no audio
server at all, so the UI squad needs neither hardware nor PipeWire.

#### 4.3 Plugin host boundary (`contracts/plugin/`, Phase 4)

Fixed now, because a later change is expensive:

- **The engine never links a plugin SDK** and never calls plugin code.
- The audio crosses the boundary **once per block per host process**, not once
  per plugin (PDC spec).
- **The engine never waits on a plugin past a deadline.** A missed block is
  dropped and counted. A host that misses repeatedly is removed from the plan,
  and the operator is told which plugin.
- **`latency_frames` reported by the host includes any delay the IPC itself
  adds.** If the design pipelines (the host processes block N while the engine
  runs N+1), that block of delay is declared as latency and PDC absorbs it like
  any plugin's look-ahead. One mechanism, no special case.
- Declared versus actual channel counts are validated host-side before the
  instance is offered to the engine (plugin-engineer charter).

```cpp
namespace sonora::plugin {
struct ChannelHeader {           // one per host process, in a memfd
    uint32_t magic, layout_version, max_frames, max_channels, instance_count;
    std::atomic<uint64_t> submitted_block;  // engine -> host
    std::atomic<uint64_t> completed_block;  // host -> engine
    std::atomic<uint64_t> host_heartbeat;
    // audio slots and a parameter SPSC ring follow
};
class HostChannel {              // engine/plugin_client, RT half
public:
    bool submit(uint64_t block, const AudioBlock& in) noexcept;           // SONORA_RT
    CollectResult collect(uint64_t block, float* const* out,
                          uint64_t deadline_ns) noexcept;                // SONORA_RT
    uint32_t latency_frames() const noexcept;
};
class HostControl {              // engine/control side, non-RT, over a socket
    // scan, instantiate(descriptor) -> id, remove, set_state, host_alive()
};
}
```

**Not decided here, and left for a Phase 4 ADR after measurement:** how the
engine wakes the host (a futex or eventfd write is a syscall on the audio
thread and needs the auditor's ruling; spinning burns a core), whether there
is one host per plugin, per track or per chain (crash blast radius against
process count), who owns the worker threads (PDC spec, open question 1), and
how plugin editor windows appear under a single-client kiosk compositor.

#### 4.4 Control surface boundary (Phase 5)

A control surface is **just another client of §4.1.** It translates MIDI into
`CommandRequest` and `ParameterChange`, and renders feedback (motor faders,
LEDs, meters) from its own `StateView` at a fixed tick. It gets no privileged
path into the engine.

```cpp
namespace sonora::surfaces {
class SurfaceProtocol {          // HUI, MCU
public:
    virtual ~SurfaceProtocol() = default;
    virtual void on_midi(const uint8_t* bytes, std::size_t n, api::EngineControl&) = 0;
    virtual void render(const api::StateView&, MidiOut&) = 0;  // fixed tick, diffed
};
}
```

Timecode *sync* (LTC/MTC/word clock) is not a surface. It is sample-accurate
and belongs in `engine/sync` behind a `ClockSource` interface on the RT path.
Its contract is written in Phase 5.

#### 4.5 Interchange boundary (Phase 4)

Interchange converts between other formats and **our native session format**.
It never touches the running engine.

```cpp
namespace sonora::interchange {
struct ConversionReport {        // honest about what did not survive
    std::vector<std::string> dropped, approximated;
};
class Importer {
public:
    virtual bool can_read(const std::string& path) const = 0;
    virtual bool read(const std::string& path, model::Session& out,
                      ConversionReport&, std::string& error) = 0;
};
class Exporter {
public:
    virtual bool write(const model::Session&, const std::string& path,
                       ConversionReport&, std::string& error) = 0;
};
}
```

It runs inside `sonora-convert`, a separate short-lived process. A malformed
AAF or `.ptx` crashing libaaf or libptformat then kills a converter, not a
session. The engine and the UI only ever open native session files. The
contract therefore depends on the session format spec from Phase 2, and
interchange cannot start before that spec exists.

### 5. Where the command vocabulary and the key map live

- **`command_catalog()`, `CommandSpec`, `CommandContext`, `DispatchResult` and
  `CommandRegistry` move to `contracts/api`.** The vocabulary is the contract
  between three parties: key map data, the interface, and the engine. It
  cannot sit in either side's module.
- `CommandSpec` gains a **domain**: `Engine` (transport.*, track.*, edit
  operations, app.save) or `Interface` (tool.*, zoom.*, edit.mode.*). An
  interface dispatches `Interface` commands to its own local registry and sends
  `Engine` commands as `CommandRequest`. The engine installs handlers only for
  `Engine` commands, in `engine/control`. That is where `main.cpp`'s
  `install_commands` goes.
- The two failure modes `command.h` defines survive the move unchanged. An id
  missing from the catalog is a key map load error. An id in the catalog with
  no handler is `NotImplemented`, reported to the operator. Across processes,
  `ensure_essentials()` takes its `implemented` predicate from `Hello`'s
  `implemented_commands` together with the client's local registry.
- **`KeyMap`, `KeyChord`, `TerminalInput` and the JSON loader move to
  `ui/keymap`.** Turning chords into commands is an input-device concern, and
  each client has its own device: a tty cannot send Cmd, and Qt can. The Qt UI
  adds a `chord_from_qt_key_event` there. Nothing in the engine process
  links `ui/keymap`.
- `app.quit` changes meaning under §3. Quitting the interface does not stop
  the engine. On the appliance the operator must never reach a shell, so what
  `app.quit` does there (close the session, or power off) is a ui-engineer
  decision, recorded when it is made.
- Which interaction state is engine state (selection, edit mode, zoom) is
  decided with the Phase 2 model. The default: anything saved in the session
  is engine state; anything that is only how the operator is looking at it is
  interface state.

### 6. Ownership and how contracts change

Ownership is the table in §1. A squad writes only in its own directories. Work
that needs a change in someone else's directory goes to that squad as a
request. The other squad does not edit the code itself.

**Contract files** are everything under `contracts/`, the public headers of
`common/model`, and `keymaps/README.md`'s schema section. Changes to them fall
into two classes:

- **Additive.** A new command id, a new message or event kind, a field
  appended to the state block with `kStateLayoutVersion` bumped, a new
  `ParamId`. These need review by **every owning party** named in §1 for that
  contract, recorded in the PR thread, plus the usual `code-reviewer` pass.
  An additive change updates the fake in the same PR.
- **Breaking.** A rename or removal, a change to which thread may call
  something, a change to who writes a field, a change in memory ownership, or
  anything that moves code into or out of the RT boundary of §2. These need
  **an amendment to this ADR, or a new ADR, before the code.**

Exception: adding a command id to the catalog is additive and low-risk,
because an unhandled id is `NotImplemented` by design. The ui-engineer may do
it alone, with `code-reviewer`. Renaming or removing an id is breaking and
updates every file in `keymaps/` in the same PR.

Mechanics, since every agent commits as the same GitHub account and
CODEOWNERS cannot tell squads apart:

- `contracts/CONTRACTS.lock` holds a hash of every contract file. CI fails
  when a contract file changes and the lock was not regenerated. This forces
  the change to be deliberate. It does not stop it.
- `integrator` refuses to merge a PR that changes the lock without the named
  parties' reviews in the thread, or a breaking change without the ADR.
- `code-reviewer`, on any diff under `contracts/`, checks that the version
  constant moved if the wire or shared-memory layout did.

### 7. The testing rule

**Every module builds and tests on its own, against fakes of its neighbours,
in the plain build container: no audio server, no hardware, no display.**

| Module | Tests against |
|---|---|
| `common/*` | Nothing; pure unit tests (`ringbuffer`, `wav_writer` as today) |
| `engine/core` | `drivers/fake`: record 18 channels, verify identity, length and overruns; injected xrun and server loss must close the take cleanly |
| `engine/control` | `engine/core` + `drivers/fake`; drives it only through `EngineControl` and the wire codec |
| `drivers/jack` | A **backend conformance suite** in `contracts/backend/conformance/`. It runs against `drivers/fake` in ctest and against `drivers/jack` inside the PipeWire container (`tools/test-engine-docker.sh`), so the fake cannot drift from the real backend |
| `ui/keymap` | Standalone (`test_keymap` as today, plus the shipped profiles) |
| `ui/client`, `ui/tui`, `ui/qt` | `FakeEngine`: scripted state block, scripted replies, engine death. Qt tests run under `tools/Dockerfile.ui` (Xvfb, llvmpipe). That covers correctness and layout only; ADR-0006 forbids treating it as a frame-rate measurement |
| `plugins/host` *(P4)* | A fake engine driver, plus in-tree test plugins: a gain, one that crashes, one that sleeps past the deadline, and one that declares 2 channels and writes 8 |
| `interchange` *(P4)* | Fixture files under a licence that allows redistribution; read, write, re-read through `sonora_model` |

- Each module's tests carry a ctest label (`ctest -L core`, `-L ui`) so a squad
  can run its own quickly. **The gate is still the full `ctest` plus the
  harnesses**, not the squad's label.
- The provider of a contract owns its fake: the driver squad owns
  `drivers/fake`, and the engine squad owns `FakeEngine`. The consumer never
  writes a private mock of a neighbour, because a private mock agrees with
  whatever its author already believes.
- The existing rule still holds: a bug in the RT path gets a regression test
  first.

### 8. Parallel work mechanics

- **One worktree per running agent, never two agents in one working tree.**
  Today's tree already shows the hazard: several agents have uncommitted work
  in `engine/` and `keymaps/` at once. Each squad works in
  `git worktree add <path> -b feature/<squad>-<topic> main`. Each worktree has
  its own `build-container/`.
- **Contract first.** A feature that needs a contract change lands that change
  on `main` as its own small PR (`contract/<topic>`) with the §6 reviews, and
  then the provider and consumer branches build on it. A consumer never codes
  against a contract change that exists only on someone else's branch.
- **Integration order within a feature:** contract → provider with updated fake
  → consumer. Across squads, `integrator` merges one PR at a time. After each
  merge, every other ready PR rebases and re-runs CI before it can merge. A
  stale green check does not count.
- **"Push only validated features" stays the rule.** A squad commits locally on
  its branch and pushes when the feature works. "Works" means: its module
  tests, the full `tools/build-engine.sh`, `tools/test-engine-docker.sh` if it
  touched anything that can reach the RT path or the backend, and
  `tools/test-boot-qemu.sh` if it touched `iso/` or `audio-stack/`. Then
  `code-reviewer`, plus `rt-safety-auditor` for any diff in a module listed in
  §2. Then the PR, then `integrator`. An implementer never reviews its own
  diff.
- **Keep branches short.** A squad branch that lives for weeks against moving
  contracts turns integration into archaeology. If a feature cannot be
  validated within days, it is split.
- **The shared files are hotspots:** `planning/STATUS.md`, `brain/INDEX.md` and
  `planning/BACKLOG.md`. Squads append lines and edit only their own rows in
  "In flight". The integrator resolves conflicts in them at merge.
- **Throughput is capped by review, not by writing.** `code-reviewer`,
  `rt-safety-auditor` and `integrator` are each a single serial gate, and the
  owner's attention is a fourth. Three squads writing do not make three times
  the merged work. Plan squad count against review capacity, not against
  agent count.

### 9. Migration plan

Each step is one commit (or one small PR). After each one,
`tools/build-engine.sh` is green, `ctest` passes, `tools/test-engine-docker.sh`
still records 18 channels clean, and `sonora-engine` behaves identically.

**Prerequisites. Squads do not start in parallel until these have landed:**

- **M0 — Commit the key map work as it stands.** `command.h`, `keymap.h`,
  `keymap.cpp`, `test_keymap.cpp`, the profiles and the `main.cpp` changes,
  after `code-reviewer`. Nothing moves while it is uncommitted.
- **M1 — Restructure into directories and targets, with no source change.**
  Run it in a quiet window with no other branch open on `engine/`, because it
  conflicts with every in-flight change there. It does three things:
  - `git mv` files into the §1 layout, keeping every `include/sonora/*.h`
    spelling.
  - Add a root `CMakeLists.txt` that sets `CMAKE_RUNTIME_OUTPUT_DIRECTORY` to
    the build root. `build/sonora-engine` therefore stays where
    `tools/build-iso.sh` and `tools/test-engine-docker.sh` already look.
  - Switch `-S engine` to `-S .` in `tools/build-engine.sh` and
    `.github/workflows/ci.yml` (the distro-engineer makes that edit in the same
    commit), and update the paths in `.claude/skills/rt-audit/SKILL.md`.

  One edge is temporarily illegal: `drivers/jack` → `engine/core`, because
  `process_cb` still calls `Session::process`. It is named here so nobody
  copies it, and M2 removes it.

  *This paragraph undercounts: there are three illegal edges until M4. See
  "Update, 2026-10-12: lessons from M1" below.*
- **M2 — Backend contract.** Add `contracts/backend`. `Session` implements
  `ProcessCallback`, and `JackBackend` implements `AudioBackend`.
  `<jack/jack.h>` leaves every public header (it goes to a private
  implementation struct), and the illegal M1 edge is gone. rt-engineer and
  driver-engineer, with `rt-safety-auditor`, because this rewires the
  callback.
- **M3 — `drivers/fake` and the backend conformance suite.** Add an
  in-process 18-channel identity test of `engine/core`, and
  `sonora-engine --backend fake`. From here engine work and UI work need no
  PipeWire for their inner loop.
- **M4 — The control API.** Move the catalog and registry into
  `contracts/api` and add domains. Add the messages and the wire codec, the
  state block, `StateView`, the publisher thread, `EngineControl`,
  `InProcessEngine` and `FakeEngine`. Move `install_commands` into
  `engine/control`. The terminal UI in `main.cpp` stops touching `Track` and
  `Session` and drives everything through `InProcessEngine`, which makes it
  the protocol's first real client and its test. `ui/keymap` loses its last
  link to the engine.
- **M5 — Enforcement in CI.** The forbidden-include check from §1 and
  `contracts/CONTRACTS.lock`. It comes after M2 so it lands green.
- **M6 — Ownership written where agents read it.** Add the §1 table to
  `.claude/OPERATIONS.md`, and update each agent charter's "What you own" to
  the new paths. The CLAUDE.md layout table also changes, and that edit is the
  owner's to make or approve.

After M6, these can run in parallel: the **UI squad** (shell, transport,
meters, arming, key routing in Qt against `FakeEngine` and
`--backend fake`), the **driver squad** (§10), the **distro squad** (as
today), and the **engine squad** (Phase 1 closure, then Phase 2).

**Later, each at the phase that needs it:**

- **M7 — Process split (§3, confirmed by the owner 2026-10-12).** Add
  `SocketEngineClient`, the control server on a socket, the memfd state
  block, `sonora-tui` as a separate binary, and two systemd units. It must
  land before the Qt UI enters the image, which is the start of Phase 3. The
  in-engine headless `--record` mode stays, because CI and the QEMU smoke
  test depend on it.
- **M8 — `common/model` and the session format spec** (Phase 2, spec in
  `brain/specs/` first, as the roadmap already requires). Undo operations are
  designed as the replication format for M7's UI replica, so one mechanism
  serves both.
- **M9 — Plugin boundary** (Phase 4). The ADR comes after the measurements in
  "Unverified", then `contracts/plugin`, `engine/plugin_client` and
  `plugins/host`.
- **M10 — Interchange and `sonora-convert`** (Phase 4, after M8).
- **M11 — Surfaces and sync** (Phase 5).

### 10. Honest constraints

**No hardware has been attached.** The UMC1820 has never been near this
project. What a driver squad can usefully do without it:

- Build and own `drivers/fake` and the conformance suite (M3). That is the
  most valuable driver work available. It turns every assumption the engine
  makes about a backend into a test, and the fault injections are exactly the
  failures that cost this project defects before: silent ports, ordering,
  late enumeration, a server that disappears.
- Harden `drivers/jack` against what the PipeWire container can actually show:
  a virtual 18-channel source with Pro Audio-style `AUX` port names, ports
  that appear late, rate and quantum mismatches.
- Find out whether the QEMU image can present a *virtual ALSA card* with 18
  capture channels (`snd-aloop` or `snd-dummy`) that PipeWire's Pro Audio
  profile exposes as `AUX0..AUX17`. If it can, the real ALSA → PipeWire → JACK
  path, the udev rule and the WirePlumber rule are all exercised in CI with no
  device attached. This is unverified, and it goes to the researcher first.
- Write the HUI and MCU codecs against documented protocol behaviour, with
  tests from documented message traces (ADR-0004 allows this).

What it **cannot** do: produce a latency figure, an xrun figure from
hardware, USB bus or clock-drift behaviour, or the Phase 1 exit criterion.
None of those exist until the device is attached. **When the owner attaches
it, the hardware measurement outranks everything else on the driver squad's
list.** Claims stay in the charter's form: "decodes MCU fader messages per the
documented protocol", never "works with surface X".

**Starting Phase 3 and Phase 4 while in Phase 1.** `planning/ROADMAP.md`
runs phases in order. **Decided by the owner, 2026-10-12: Phase 3 and Phase 4
work opens now, limited to exactly the scope in the mitigations below.** The
UI squad builds only what has engine state today, with no timeline editing.
Plugin work is host-side and measurement only, with nothing in `engine/`.
Anything beyond that scope waits for its phase as before, and the roadmap
says so. The risks that set the scope:

- *The UI invents a data model.* Phase 2's timeline model does not exist yet.
  A UI squad that builds an editor now will hold regions and playlists in Qt
  objects, and that becomes a second model to reconcile. Mitigation: until M8,
  the UI squad builds only what has engine state today (shell, window model,
  transport, meters, arming, key routing, and the rendering-path detection
  ADR-0006 requires). No timeline editing.
- *Plugin hosting fixes the graph's shape before the graph exists.* The PDC
  spec's open questions (who owns the worker threads, whether shared memory
  plus a deadline beats in-process) decide `HostChannel`'s shape. Mitigation:
  before Phase 4, the plugin squad does host-side work only (scanning,
  validation, the crash and liar test plugins, the out-of-process host loop)
  and the **measurement** of a shared-memory round trip under RT load. That
  measurement answers the spec's open question and feeds M9. Nothing goes
  into `engine/`.
- *Phase 1 loses focus.* Its exit criteria are hardware-blocked, and parallel
  squads make that easy to forget. The roadmap's rule, that a phase closes when
  measured, does not change because other work is moving.
- *Contracts written early are wrong early.* Versioning, fakes owned by the
  provider and the §6 change classes exist to make that cheap. They do not
  make it free.

**The minimum contracts that keep that risk low:** `contracts/api` (M4),
`contracts/backend` (M2) and a *written*, not coded, draft of the §4.3
invariants. Nothing for interchange until the session format spec exists.

**Owner amendment, 2026-10-13.** Given directly to the coordinating agent:
"I want to see the product — the DAW running, recording, including plugins,
like Logic and Pro Tools; then comes the OS to install", corrected the same
day to "a complete product" with "all the features we have in Pro Tools and
Logic". **The early Phase 3/4 scope limit above is lifted entirely for the
Complete DAW milestone** (`planning/ROADMAP.md`). Concretely:

- The UI squad may build timeline editing, playlists, comping, the mixer
  and automation against `common/model` and `FakeEngine`. The mitigation
  for "the UI invents a data model" still holds by other means: the UI
  holds no model of its own, only `common/model` snapshots replicated by
  ADR-0009's operations.
- Plugin work may go into `engine/plugin_client` once M4 exists. §4.3's
  invariants, the auditor's gate on anything the callback calls, and the
  M9 plugin ADR after the IPC measurement all still apply.
- "Phase 1 loses focus" no longer applies as written: the owner has put the
  appliance (image, kiosk, USB stick, hardware exit) after the Complete DAW.
  The hardware measurement rule above stands: when the UMC1820 is attached,
  it outranks everything on the driver squad's list.

The rest of this ADR is unchanged. Phase numbers cited here map to the
roadmap's stages through its "Phase numbers before 2026-10-13" table.

## Rationale

- **Decided by what each choice costs later.** A module boundary added after
  three squads have written against a monolith is a rewrite of all three.
  Added now, it is M1 to M6: mechanical steps on a tree of a few thousand
  lines, most of which only moves.
- **The process boundary is the only boundary that holds under crash.** A
  linker boundary keeps squads out of each other's headers. It does not keep
  a GPU driver bug out of the process holding the take. ADR-0006 measured the
  display side at 375 MB, 124 MB of it LLVM pulled in by Mesa. That is the
  largest body of third-party code on the image after plugins, and the
  project already requires plugins out of process for exactly this reason.
  The same argument applies to the UI. Losing a take outranks crashes, a wrong
  reading, and every feature (ADR-0006).
- **We are building the shared-memory machinery anyway.** Out-of-process
  plugins need a lock-free shared-memory handoff with deadlines. The UI's
  needs are a strict subset: read-only, non-RT writer, human-rate commands. So
  the split adds a client of machinery we already have to build, not a second
  kind of machinery.
- **The audio thread stays ignorant of every boundary but one.** The publisher
  thread means no squad's protocol change can reach the callback. The only
  boundaries the callback touches are the backend's buffers and, in Phase 4,
  the plugin channel. Both are owned jointly with rt-engineer and gated by the
  auditor.
- **Fakes owned by providers, and conformance suites that run against both the
  real and the fake implementation**, are what let a squad trust a green test
  that never touched the neighbour's real code.
- **Fail loudly at start-up** is applied at every seam. Protocol and layout
  versions are checked on connect. A rate or quantum change is fatal, not an
  adaptation. A command missing from the catalog is a load error. A dead
  engine shows "no data", not stale meters.

## Consequences

- **The layout changes once, visibly.** Every path in agent charters, skills,
  CI and CLAUDE.md that says `engine/src` changes in M1 and M6. After that, a
  path says who owns the code and whether it may run on the audio thread.
- **State replication is real work (§3).** The UI needs a
  replica of the session model to draw a timeline. Designing undo operations
  as the replication format (M8) makes that one mechanism instead of two, but
  it is still the largest single cost of the split. It lands in Phase 2–3, not
  now.
- **Two or more processes to start, and in order.** The UI must handle the
  engine being absent, starting or restarting, and say which. The distro squad
  owns two units in place of one.
- **Command latency becomes a socket round trip.** This is assumed to be well
  under a millisecond and irrelevant at human rates (unverified; see below).
  Automation written from a UI or surface fader is timestamped by the engine on
  receipt, not by the client.
- **More review traffic on the seams.** Contract PRs need two or three
  parties. That is the intended friction.
- **§3 protects a take from a UI crash, not from an engine crash.** Leaving a
  valid file behind when the engine itself dies (the disk thread periodically
  patching the WAV header) is Phase 5's crash recovery, and nothing here
  replaces it.

## Unverified

*All six items were measured on 2026-10-12. The results and the changes
they make are in "Update, 2026-10-12" below. The list is kept as
written.*

Stated so nobody treats them as measured. Each line names the measurement that
settles it, for the researcher:

1. **Socket round trip and state-block read cost under RT load.** The
   assumption is that a command round trip is well under 1 ms and that a
   60 Hz `StateView` read is negligible. Measure p50/p99/max of a
   `SOCK_SEQPACKET` request/reply between two processes while the engine
   records 18 channels at quantum 128, in the PipeWire container and then on
   real hardware. Record any xrun the reader causes.
2. **Shared-memory plugin round trip.** The PDC spec's open question 3: the
   time from `submit` to `completed_block` for one host process doing no DSP,
   with a futex wake versus spinning, under the same load. This settles the
   wake mechanism and whether pipelining is needed.
3. **RTSan availability.** RealtimeSanitizer and `[[clang::nonblocking]]` are
   described as shipping with Clang 20. Whether Debian trixie or
   trixie-backports packages a Clang that has them is not established.
   Determine that, and whether the build can carry a Clang job beside GCC 14.
4. **Virtual ALSA card in QEMU.** Whether `snd-aloop` or `snd-dummy` can
   present 18 capture channels that PipeWire's Pro Audio profile exposes as
   ordered `AUX` ports (§10).
5. **Plugin editor windows under a kiosk compositor.** LV2 and VST3 editors on
   Linux are largely X11. Whether `cage` (ADR-0006's direction) with Xwayland
   can show a plugin host's editor window over the UI's fullscreen surface is
   unknown, and the answer may constrain the compositor choice.
6. **`std::atomic` in shared memory.** It is correct only for lock-free,
   address-free atomics. The `static_assert` above enforces lock-freedom.
   Confirm on the target toolchain that `std::atomic<float>` and
   `std::atomic<uint64_t>` are always lock-free on x86-64 (expected, not
   checked).

## Update, 2026-10-12: the Unverified items measured

Source: `brain/research/adr-0007-measurements.md`. Those figures come from a
container on a Hyper-V guest with a generic kernel. They give orders of
magnitude and show qualitative behaviour. They are not appliance figures.
The decided text above stands. The changes below are additions and
tightenings. None of them changes an owner decision.

1. **`SONORA_RT` becomes the attribute (item 3, resolved).** RTSan and
   `[[clang::nonblocking]]` work with `clang-22` from trixie main and
   `clang-21` from trixie-backports. They do not work with trixie's default
   `clang` 19. The rule in §2 is refined as follows:
   - RT entry points are declared `noexcept SONORA_RT`, in that order, before
     `override` or `= 0`. The attribute is a function-*type* attribute, so
     its position matters.
   - `SONORA_RT` expands to `[[clang::nonblocking]]` under a Clang that
     supports it, and to nothing elsewhere. GCC 14 stays the compiler of the
     shipped build.
   - The new CI job is an **RTSan build of its own** (`clang-22`,
     `-fsanitize=realtime`). It cannot be combined with ASan or TSan.
   - `-Wfunction-effects` is **advisory at first**, because libstdc++ inference
     produces noise. It is promoted to an error per module once that module's
     RT functions are clean.
   - The runtime check is the gate, and it catches only what tests execute.
     That raises the value of the `drivers/fake` conformance suite and its
     fault injections.
   - A deliberate `ScopedDisabler` is an RT exception and needs the auditor's
     sign-off.
   - Still open: whether `pipewire-jack`'s libjack trips RTSan from inside
     the callback.
2. **Shared-memory atomics (item 6, resolved for amd64 and the arm64
   compiler).** `float`, `double`, `uint32_t`, `uint64_t`, `int64_t` and
   `bool` are always lock-free with GCC 14 on both. 16-byte atomics are
   **not**: they go through libatomic's lock table. So:
   - every atomic type used in `EngineStateBlock` and `ChannelHeader` gets
     its own `static_assert(...::is_always_lock_free)`, not just
     `uint64_t`;
   - **no `std::atomic` wider than 8 bytes may sit in shared memory.** A
     wider value goes under the seqlock.
3. **The state publisher must be paced (item 1: the assumption holds, and
   there is a new constraint).**
   - In the container, a 64-byte `SOCK_SEQPACKET` round trip measured p50
     60 µs, p99 158 µs, max 484 µs.
   - A pessimistic 64 KiB seqlock read at the UI's rates measured p99 25 µs.
   - But a writer that republished without pause starved the reader: p99 41
     ms, reads abandoned after 100 ms.

   This binds `engine/control` and `StateView`:
   - The publisher thread runs **on a fixed timer** (100 Hz–1 kHz measured
     clean). Each write must be short compared with the period. It never
     republishes in a loop and never publishes "on change" without a
     minimum interval.
   - `StateView` reads use a **bounded** number of seqlock retries. When the
     bound is reached, the reader reports the snapshot as stale for that
     tick, and the interface shows it as such. The UI thread never spins
     indefinitely on engine state. This is an additive change to §4.1's
     `StateView`: snapshots carry a `stale` flag.
   - M4 includes a regression test: a paced writer at the maximum rate must
     not starve a 60 Hz reader.
4. **Plugin wake mechanism (item 2): still open, and narrowed.**
   - Futex and eventfd were equivalent in the container: a round trip p50
     about 60 µs and p99 210–245 µs, about 8 % of a 2.67 ms period at p99.
   - **Spinning at `SCHED_FIFO` is ruled out.** It ran into RT throttling,
     with stalls of about 50 ms.
   - Making spinning work would need `sched_rt_runtime_us=-1`, which removes
     the kernel's protection against a runaway RT thread. On an appliance
     that must never hang, that trade is rejected here.
   - The futex-versus-eventfd choice, and the auditor's ruling on a wake
     syscall from the audio thread, wait for the RT kernel on bare metal.
     They feed the Phase 4 plugin ADR (M9).
   - A futex worst case above one period was seen on the generic kernel.
     That confirms `collect(..., deadline_ns)` must drop the block, never
     wait.
5. **Virtual multichannel card (item 4, partly resolved).**
   - `snd-dummy model=rme9652` presents **26** ordered
     `capture_AUX0..AUX25` ports through the real ALSA → PipeWire → JACK
     path in the image kernel, with no extra configuration. It is the
     preferred stand-in for the driver squad's virtual-card CI test, with
     `--tracks 18` taking the first 18.
   - Exactly 18 ports needs `snd-aloop` plus a test-only WirePlumber rule
     that sets **both** `audio.channels` and `audio.position`. With the
     channel count alone, the ports silently become `capture_1..capture_18`.
   - Registry ids were *not* in channel order for monitor ports. That
     confirms `natural_less` over names is the rule.

   Consequences for §4.2 and §10:
   - `drivers/fake` adds two injections: ports named without `AUX`
     (`capture_1..N`), and ids out of channel order.
   - Any virtual-card assertion counts ports **per node**, not across nodes.
6. **Plugin editors under the kiosk compositor (item 5): a real constraint,
   open.**
   - Under `cage` 0.2.0 with Xwayland, a parentless X11 window is forced
     full screen over the UI and its size hints are ignored.
   - A window with `WM_TRANSIENT_FOR` is centred at its own size.
   - An override-redirect popup was drawn at the screen origin.
   - An X11 editor cannot name a native Wayland surface as its parent. The
     options are therefore:
     - (a) the Qt UI runs on X11 under Xwayland (`QT_QPA_PLATFORM=xcb`), so
       it can be the editor's transient parent;
     - (b) the plugin host creates its own X11 anchor window, which cage then
       shows full screen over the UI whenever an editor is open;
     - (c) a different kiosk compositor, or a small custom policy.

   None was tested with real Qt or a real plugin. This is recorded as an
   **open constraint on the Phase 4 plugin ADR and on the ADR that chooses
   the compositor** (ADR-0006 leaves the compositor open). It also
   constrains ADR-0006's stated Wayland-kiosk direction.

   **Effect on ADR-0007's contracts:** none to code now. The plugin contract
   (§4.3) is Phase 4 and not yet written. It does fix one requirement the
   Phase 4 ADR must meet. Under (a), and possibly (c), the plugin host needs
   a window reference that belongs to the UI. The UI is a client of the
   engine, not of the host. So `HostControl`'s editor operation must accept
   an **opaque parent-window token** that the UI supplies and that is
   carried UI → engine → host. Its meaning depends on the display server,
   which the protocol does not interpret. No contract in §4.1 has to change
   today: the token fits `CommandRequest::argument`, or a later additive
   message.

**What still needs bare metal and the RT kernel:** every IPC figure above,
attribution of any xrun to a reader, the wake syscall's cost on the audio
thread, RTSan on arm64, and the engine actually recording from the virtual
cards. These are in `planning/BACKLOG.md`.

## Update, 2026-10-12: lessons from M1

Source: the rt-engineer's report on executing M1 (branch
`feature/module-migration`, not yet merged when this was written). Where §9
is wrong or says nothing, this section corrects it. The decided text above
stands as written. Nothing here touches the owner decisions in §3 and §10.

1. **Three edges are temporarily illegal, not one.** §9's M1 step names only
   `drivers/jack` → `engine/core`. It missed that `main.cpp` is still the
   terminal interface until M4, and that `test_keymap` exercises the real
   thing. The full list after M1:

   | Edge | Why it exists | Removed by |
   |---|---|---|
   | `drivers/jack` → `engine/core` | `process_cb` calls `Session::process` | M2 |
   | `engine/app` → `ui/keymap`, `ui/tui` | `main.cpp` is still the terminal UI: it decodes keys and sets the tty mode | M4 |
   | `test_keymap` (test only) → `engine/control`, `ui/tui` | It drives the handlers `sonora-engine` really installs on a real `Session`, and checks bytes against the terminal mode `main.cpp` sets | M4 |

   The second and third break "nothing depends on `ui/`" and "`ui/*` never
   depends on `engine/*`". Each one is commented at its
   `target_link_libraries` line in the M1 tree, so nobody copies it.

   **Ordering this forces:** M5's check enforces the §1 graph, so it would
   flag the second and third edges. **M4 must merge before M5.** §9 says M5
   "comes after M2 so it lands green". That reason is now incomplete: M5
   comes after M2 *and* M4. If M4 slips, M5 waits. It does not land with an
   allow-list, because an allow-list entry outlives the reason for it.

   Cost of having missed this: none in code, since M1 landed the edges with
   comments. The cost would have come at M5, as a red CI run on a check
   everyone assumed was ready to land.

2. **Where a test lives.** §7 says what each module tests against. It does
   not say where the test file goes. The rule M1 applied, adopted here:
   - A test lives with its subject module, in `<module>/tests/`, and is
     registered in that module's `CMakeLists.txt`.
   - A test that needs a neighbour's *real* implementation, not a fake,
     lives in the **higher** module of the pair, the one allowed to depend
     on the other. Example: `test_takes` (a take on disk is never
     overwritten) drives a real `Session` through the installed command
     handlers, so it lives in
     `engine/control`, not `engine/core`. A test never creates an edge its
     directory's module could not have.
   - Placements M1 made: `tty_mode` is in `ui/tui`, and `quit_signals` with
     its test is in `engine/app`.
   - `test_keymap` is the one exception (item 1). It moves to its proper
     shape in M4, when it can drive handlers through `FakeEngine` or
     `InProcessEngine`.

3. **Moving the CMake source directory breaks existing build trees.** A
   build directory configured with `-S engine` records `engine/` as its
   source, and cmake refuses to reuse it for `-S .`. Every checkout and
   every agent worktree that built before M1 fails once. On the M1 branch,
   `tools/build-engine.sh` deletes a build tree whose `CMakeCache.txt` names
   `engine/` as the source directory, natively and inside the container.
   That is safe because a build tree holds only build output.

   **Migration hazard for any future root move:** whoever moves the CMake
   root again (or renames the build directory CI caches) ships the same
   kind of cleanup in the same commit, and keys it on the old source path,
   never on "the cache exists". CI caches no build tree today; if it ever
   does, that cache's key changes in the same commit. A future ADR that
   moves the root names this step.

4. **The §7 ctest labels were not added in M1.** M1 was "no source change",
   and labels were not in its list. **They are assigned to M5.** It is the
   step that already touches every module's test registration for
   enforcement, and the item 2 placement rule makes the labels mechanical
   (one label per top-level directory: `base`, `audiofile`, `api`, `core`,
   `control`, `app`, `jack`, `keymap`, `tui`, then each new module's label
   as it is created). The gate stays the full `ctest`, as §7 says.

5. **M2 is its own branch and PR, and it is a contract PR.**
   - M2 starts from `main` *after* M1 merges. It is not stacked on the M1
     branch. §9's quiet-window reasoning applies to M1, and stacking M2 on
     an unmerged M1 would extend that window.
   - M2 adds `contracts/backend`, so under §8 it is a
     `contract/backend` PR. It needs reviews from both owning parties,
     rt-engineer and driver-engineer, recorded in the thread, plus
     `code-reviewer`. **`rt-safety-auditor` gates it**, because it rewires
     the callback.
   - It applies the first update's declaration rule to the new signature
     from the first commit. §4.2's sketch shows
     `void process(const AudioBlock&) noexcept = 0;  // SONORA_RT`, with the
     marker as a comment. That predates the update. The landed declaration
     is
     `virtual void process(const AudioBlock& block) noexcept SONORA_RT = 0;`,
     and implementations are `... noexcept SONORA_RT override;`. Because
     `[[clang::nonblocking]]` is part of the function type, an override that
     omits it does not match under the RTSan build. Getting this wrong
     shows up only in the Clang job, not in GCC.
   - `contracts/CONTRACTS.lock` does not exist until M5. So for M2 the §6
     review requirement is enforced by `integrator` reading the thread, not
     by CI.

**Doc paths.** The M1 branch updates `keymaps/README.md`, the rt-audit
skill and CI. The docs in `brain/` that name `engine/src`,
`engine/include` or `engine/tests` are correct on `main` until M1 merges,
so they were not edited here. Each is listed in
`planning/BACKLOG.md` to be updated in the M1 PR or immediately after it. ADRs
and postmortems keep their old paths, because they record what was true when
they were written. `keymaps/default.json`'s `"comment"` field is data, and a
copy of the same profile is embedded in `ui/keymap/src/keymap.cpp`. Both are
fixed together in M4, when `main.cpp` stops being where commands are handled.

## Update, 2026-10-12: portability (ADR-0008)

ADR-0008, accepted by the owner, adds `common/platform` to §1's graph at
layer 0, beside `common/base`. Any module may link it except `common/base`
and `contracts/*`. It also extends M5's check with an include allowlist for
the platform-neutral modules, and adds a required Windows (MSVC) CI job after
M5. §1's dependency rules are otherwise unchanged.

## Update, 2026-10-12: the backend contract as landed (M2)

Source: branch `contract/backend`, after reviews by the code-reviewer, the
rt-safety-auditor and the driver-engineer as co-owner. Where §4.2's sketch
and the landed headers differ, the headers in `contracts/backend/` are the
contract. Nothing here touches §3 or §10.

1. **`make_backend()` lives in `engine/app`, not in the contract.** A
   factory names every driver, and contracts may not depend on drivers. It
   is a function local to `main.cpp`, the one place that names a concrete
   backend.
2. **A new module, `drivers/support`** (`sonora_driver_support`, header
   only, driver squad). It holds `natural_less`, `PeriodGuard`,
   `FrameClock` and `describe_ports()`, which `drivers/jack` uses and
   `drivers/fake` will use. It is not `common/base`, because everything
   there must be RT-safe and `natural_less` allocates. Edges:
   `drivers/support` → `contracts/backend`, `common/base`; `drivers/jack`
   and `drivers/fake` → `drivers/support`.
3. **Ports carry a device.** `PortInfo { device, name, channel, usb }`,
   where `channel` counts within the device. `BackendConfig::capture_device`
   chooses the device. Pro Audio applies to every ALSA card, so onboard
   audio plus the UMC1820 in one sorted list shifted channels. The rule is
   stated once, in the contract, as the pure function `select_device()`; an
   empty name follows the device policy in item 8. A device that is not
   present yet selects nothing and is not an error, so a USB interface may
   still enumerate late. `sonora-engine --device NAME` sets it.
4. **What is fatal.** Server loss; `DeviceGone`, when a physical capture
   port that feeds an input is unregistered; `Stalled`, when no period
   arrives for one second while active, checked by `fatal()` against the
   control thread's clock; and a rate or buffer-size change. Item 9 says
   which sources count.
5. **Guarantees added to §4.2.**
   - A period whose rate or size differs from the first is refused before
     `process()`. The rate is read in the period itself, not only from
     the server's notification.
   - No `process()` call starts after `fatal()` first returns true. One
     already running may finish, and `deactivate()` is the barrier.
   - The callback writes every output, silence included.
   - `inputs[i]` may be null, and non-null does not mean connected.
   - `frame_time` never decreases; the backend widens JACK's 32-bit
     counter.
   - `buffer_frames()` is the size the first period set.
   - `connect_inputs_in_order()` is idempotent.
6. **`fatal_reason()` returns `std::string` by value.** It runs on the
   control thread only, and its reasons carry specifics ("48000 -> 96000
   Hz", the device). `fatal()` stays callable from any thread.
7. **The engine locks its memory.** `mlockall(MCL_CURRENT | MCL_FUTURE)`
   runs before the session exists, and the engine refuses to start unless
   `RLIMIT_MEMLOCK` is or can be raised to unlimited (see the commit for
   why a finite limit is refused). Tests that start the engine in a
   container need `--ulimit memlock=-1:-1`.

Items 8 to 12 answer the final code review. Each is the **default policy,
2026-10-12**, chosen by the coordinator; the owner can override it, and did
for item 8 on 2026-10-13.

8. **Which device without `--device`: default policy, revised by the owner
   2026-10-13.** The owner connects the UMC1820 only when development as a
   whole is finished; until then the laptop's onboard audio must record.
   The 2026-10-12 rule (onboard only when named, never a fallback) is
   replaced by:
   - **the name**, in order: `--device NAME`; else the first line of
     `~/.config/sonora/device` (on the appliance `/home/sonora`, which is on
     the persistence partition, ADR-0005); else `/etc/sonora/device`. Blank
     lines and `#` lines name nothing. The file lets the choice survive
     reboots without editing the systemd unit. The image ships neither file;
   - a named device is used whatever its bus, and one still absent when the
     wait ends makes the engine exit 1 naming it and listing those present
     (`describe_absent()`);
   - no name: **a USB interface present is recorded from**, unchanged.
     Several USB devices: `open()` fails naming them;
   - no name and no USB device: **wait** (`--wait-for-inputs`), retrying
     the auto-connect every half second; a patch made by hand within the
     wait counts as inputs, and then nothing else is chosen;
   - the wait over with nothing connected: **the one non-USB capture
     device** is recorded from (`select_device(..., wait_over = true)`,
     `fall_back_after_wait()` in `engine/app`), with a stderr line and a
     notice kept on screen for the session: "recording from <device>
     (onboard): no USB interface found". Several non-USB devices: the
     engine exits 1 listing them. No device at all: not an error; the
     terminal UI keeps looking once a second with the reason on screen, so
     the unit never restart-loops on an empty machine. A device that
     appears only later gets the same fallback there, and several are
     named on screen instead of exiting;
   - `--tracks auto` sizes the session to the device chosen this way, so
     the wait and the fallback happen before the session exists: two
     tracks on typical onboard audio, the interface's count on it;
   - a USB interface that appears while recording from onboard **is not
     switched to**, since that would change the channel count and the
     ports under the tracks. The screen adds "USB interface <name> found;
     restart the engine to record from it". The contract gains
     `AudioBackend::choose_capture_device()`, which pins a device only if
     none is chosen yet, so the backend itself cannot move either;
   - a take is refused while no armed track's input is connected
     (`input_connected()`), with the reason on the status line. When some
     inputs are connected but none to an armed track, the reason says so
     ("2 of 8 inputs are connected, none of them to an armed track")
     rather than "nothing is connected".

   Under jackd2, or wherever no device reads as USB, the one device present
   is announced as onboard even if it is the interface; the notice is
   wrong in name only, and naming the device silences it.

   Container-verified (`tools/test-engine-docker.sh`): with only a fake
   onboard device, `--tracks auto` and `--tracks 4` both record from it
   after the wait with the notice; beside a second fake non-USB device both
   are refused, the two named; a fake device patched by name and killed
   mid-take ends the take through `DeviceGone`, the reason naming it, with
   the files closed and full of what was captured.

   *Where the bus comes from.* Nothing in the JACK API names it, and under
   pw-jack 1.4.2 the client part of a port name is the node's
   description ("UMC1820 Pro"), not `node.name` (`alsa_input.usb-...`).
   With `jack.fill-aliases = true` (off by default) a port's first alias is
   its PipeWire `object.path`, `alsa:acp:<card id>:...` (or `alsa:pcm:`
   with ACP off), from `spa/plugins/alsa` and `impl-port.c`. The kernel's
   snd-usb-audio, and only it, creates `/proc/asound/cardN/usbid`, and
   `/proc/asound/<card id>` links to `cardN`. The engine sets
   `jack.fill-aliases` for itself through `PIPEWIRE_PROPS` before any thread
   exists, keeping what the variable held. Measured in the container (a
   JACK client with physical ports and `object.path` set through
   `PIPEWIRE_PROPS`): without the setting the aliases are empty; with it,
   alias 1 is `alsa:acp:PCH:0:capture:output_0`. Under jackd2, or with the
   setting overridden, no device reads as USB: the one device present is
   the fallback after the wait, and with several one must be named. Not
   verified on hardware: that a real UMC1820's ports carry the
   `alsa:acp:UMC1820:...` path, and that `/proc/asound/UMC1820/usbid`
   exists on the target.

   Several USB devices, or several non-USB ones with no USB device, fail
   fast at startup, so under `Restart=always` the unit can reach its start
   limit and leave tty1 on the last error. That is the chosen behaviour,
   confirmed by the owner for the non-USB case: the operator has to pick
   one, in the setting file.
9. **Which sources `DeviceGone` covers.** Physical ones only: `pw-play`
   ending or a soft synth closing is not a device leaving. A source is
   forgotten once unlinked, so an interface unpatched earlier and removed
   later does not end the take. But under pw-jack 1.4.2 a destroyed node's
   link disconnect arrives before its port's unregistration, in the same
   batch (measured in the container), so a source unlinked less than one
   second before its port goes still counts (`SourceWatch`). A removal is
   matched by name as well as id, since jackd2 reuses port ids.
10. **Teardown is bounded.** With the server stalled, the take closed but
    the exit waited in `jack_deactivate()` for good, and `Restart=always`
    could not act. After the take is closed, every exit path arms a
    watchdog thread that writes a message and `_exit(1)`s after five
    seconds. `WatchdogSec=` in the unit is not added: it needs the engine
    to send `WATCHDOG=1` through `sd_notify` from a loop that proves
    progress, which means linking libsystemd and choosing what "alive"
    means; not trivially safe, so it is left for its own change.
11. **`frame_time` across a server clock step.** A step backwards in
    JACK's counter is a clock change, not a forward jump of nearly 2^32
    frames: `FrameClock` holds still and counts a discontinuity. The
    count reaches the operator through
    `AudioBackend::clock_discontinuities()`: beside the xruns on the
    terminal UI's status line ("clock steps") and in the headless
    `RESULT` line (`clock_steps=`). It is reported, not failed: a step puts
    no hole in the audio, only a break in the timestamps after it.
12. **`PeriodGuard::state()`** reads the reason and its detail in one load,
    so `fatal_reason()` cannot print "48000 -> 0 Hz".

*Known, harmless.* `jack_get_sample_rate()` inside pw-jack 1.4.2 writes
`c->sample_rate` back on the calling thread (`pipewire-jack.c:5346`), here
the RT thread, racing the notify thread that updates it. The value is only
used to compare against the expected rate, and the first fatal reason
sticks, so a torn or stale read can at worst delay detection by a period.

## Update, 2026-10-13: drivers/fake and the conformance suite as landed (M3)

Source: branch `feature/fake-backend`. Where §4.2 and §7 describe M3 and
the code differs, this is what landed. No contract header changed.

1. **The suite is a library, not a test.** `contracts/backend/conformance`
   builds `sonora_backend_conformance`: `conformance::run(Rig&, ...)`. A
   `Rig` makes backends, lets periods pass, says whether it is exact (runs
   exactly the periods asked for, which only `run_blocks()` can), and
   causes faults. A fault a rig cannot cause is reported `SKIP`, never
   `ok`. It links only `contracts/backend`, so it stays neutral.
2. **Where it runs.** `drivers/fake/tests/test_conformance_fake.cpp`
   (ctest `backend_conformance_fake`) runs it on six fakes: UMC1820,
   onboard, both, two interfaces, the injected naming and ordering, and
   the threaded drive. `drivers/jack` builds `sonora-conformance-jack`,
   not a ctest; `tools/test-engine-entrypoint.sh` runs it against PipeWire
   and causes the faults from the shell (`pw-metadata` `clock.force-quantum`
   and `clock.force-rate`, `SIGSTOP` on the server, killing the stand-in
   card, killing the server). Measured in the container on 2026-10-13: 10
   ok, 0 FAIL, 4 SKIP. Xrun and clock step cannot be caused on demand
   there. The quantum and rate changes are refused by the shipped pin
   (`audio-stack/pipewire-sonora.conf`): the settings metadata stores
   `clock.force-quantum 256` while `pw-top` shows the running driver still
   at 128. The rig asks the running driver whether the change took
   (`Rig::Injected::Held`), and a held change is skipped with that reason
   after checking that the backend carried on, not fatal. On an unpinned
   PipeWire, before the pin landed, both checks passed. The refusal path
   stays covered by the `PeriodGuard` tests and the fake rigs. The pin is
   not loosened for the test. All of this is pw-jack in a container, not
   hardware.
   After a fatal fault the suite calls `Rig::restore()` before it checks
   that no `process()` follows, so a server that resumes (SIGCONT) cannot
   slip a period past the check. A deliberately leaky backend in
   `test_conformance_fake` proves the check fails it.
3. **Checks.** A refused rate at `open()` and a usable backend after it;
   periods reach `process()` at the first period's size with the declared
   shape and non-null outputs; `frame_time` never decreases, and its gaps
   are no more than xruns plus clock steps (none on an exact rig);
   an absent named device is not an error and is never replaced;
   the default device policy on whichever devices are present;
   `connect_inputs_in_order()` idempotent, `input_connected()` exact;
   `deactivate()` a barrier and idempotent; and per fault: xrun and clock
   step not fatal and counted, the rest fatal with the shared reason, no
   `process()` starting after `fatal()`, and, on an exact rig, the first
   period at a new rate or size refused.
4. **One fatal text for every backend.** `drivers/support/fatal_reason.h`
   (`describe_fatal()`) holds what `JackBackend::fatal_reason()` said, so a
   test against the fake asserts the sentence the appliance prints.
5. **The fake's shapes and signal.** `FakeBackend::umc1820()` is
   "UMC1820 Pro", 18 in / 20 out, USB; `onboard()` is "Built-in Audio
   Pro", 2 / 2. Tones are 0.5 × sin at 250 + 317 c Hz, the frequencies
   `tools/verify-recording.py` checks, computed from the server frame in
   integers so a test compares samples exactly. Unconnected inputs are
   `nullptr` unless configured otherwise; outputs are pre-filled with NaN
   so an output the callback leaves unwritten is counted.
6. **`sonora-engine --backend fake`** (and `fake:onboard`) uses the
   threaded drive. Measured in the container: 18 files, 96000 frames
   for `--record 2`, `xruns=0`, verified by `verify-recording.py` before
   PipeWire is started.
7. **Test placement.** `engine/core/tests/test_record_fake.cpp` links
   `drivers/fake` as a test-only edge, as §7 asks; `sonora_core` links no
   driver.

## Alternatives rejected

**Keep one library and rely on discipline.** It costs nothing today. It costs
a rewrite later, because nothing stops a Qt file from including `session.h`
and reading a `Track` directly. That is the coupling ADR-0006's in-process
shape already invites, and three squads would build on it before anyone
noticed. Conventions do not survive parallel agents. Compile errors do.

**Modules as subdirectories inside `engine/`, keeping `engine/` as the CMake
root.** This avoids touching `tools/build-engine.sh` and CI. Rejected because
the path would stop meaning anything: `engine/ui/qt` would sit beside
`engine/core` under the directory CLAUDE.md describes as the real-time core,
and reviewers apply RT scrutiny by path. The saving is two one-line edits in
M1.

**UI in the engine process, as ADR-0006 currently describes.** This is the
cheapest path to pixels: no replication protocol, no socket, and direct reads
of `Track`'s atomics. Rejected by the owner 2026-10-12 because it puts Qt, Mesa
and LLVM in the address space that holds the take. A GPU driver fault then
ends the recording, which is the one outcome ranked worse than any other. The
argument that took plugins out of process applies with equal force.

**The audio thread writes the shared state block directly.** This saves one
thread and about 10 ms of meter latency. Rejected because it couples the RT
path to the wire layout, so every additive state-block change would need an RT
audit. It also makes a client-visible mapping something the callback writes
under a seqlock. A publisher thread costs nothing the operator can see.

**The engine core implements the control API itself** (no `engine/control`
adapter). It has one fewer module. Rejected because it pulls the protocol,
`std::string` messages and `std::function` handlers into the module that owns
the RT path. The rt-safety-auditor's call-graph search would then start inside
a module full of allocating code.

**A key map inside the engine,** where the uncommitted work put it. Rejected
because key decoding is per input device (the tty cannot send Cmd, and Qt can),
and the engine has no input device of its own under §3.

**CODEOWNERS for contract reviews.** Every agent pushes as the same GitHub
account, so CODEOWNERS cannot distinguish squads. The lock file plus the
integrator's check is the mechanism that actually works here.

## Update, 2026-10-13: `common/midi`

A new neutral module, `common/midi` (layer 1, links only `common/base`):
MIDI event model, Standard MIDI File codec and pure edit primitives, owned by
rt-engineer. Like `common/audiofile` it never runs on the audio thread. The
real-time `MidiBuffer` lives in `common/base` instead, because §2 lets only
`common/base` among the `common/` modules put code on the audio thread.

## Update, 2026-10-13: the control API as landed (M4)

Source: branch `contract/api`. Where §4.1's sketch and the landed headers
differ, the headers in `contracts/api/` are the contract. §3 and §10 are
untouched. This is a contract PR under §6 and §8: it needs the reviews of
both owning parties (ui-engineer and rt-engineer) in the thread, and the
rt-safety-auditor's, before it merges.

1. **What `contracts/api` holds.** `sonora_api` is now a static library
   linking only `common/base`:
   - `command.h`: the catalog with unchanged ids, plus a `CommandDomain`
     (`Engine` or `Interface`) on every `CommandSpec`. `Interface` is
     `tool.*`, `zoom.*`, `edit.mode.*` and `app.quit`. Everything else is
     `Engine`, `track.select*` included, as §5 reads.
     `is_engine_command()` says which.
   - `api/messages.h`: `CommandRequest`, `ParameterChange` (`ParamId::Gain`,
     linear 0..4), `ModelRequest` (`Apply`, `Undo`, `Redo`, `Resync`),
     `Hello` (adds `track_names`), `CommandReply`, `EngineEvent`
     (`Notice`, `InputsMissing`, `TakeFailed`, `Fatal`), `ModelUpdate`
     (`Op`, `Undo`, `Redo`, `Snapshot`, each with its revision and the undo
     floor), and `Inbox`, what one `poll()` hands over. Every enum is
     append-only.
   - `api/wire.h`: the codec. A message is a type byte, then its fields:
     little-endian integers, length-prefixed strings with caps (64 KiB of
     text, 64 MiB of model bytes, 4096 list entries). Decoding refuses
     truncation, trailing bytes, a length past its cap, an unknown enum
     value and the wrong type, each with a reason. Nothing is partially
     decoded. `decode_hello_versions()` reads the two versions first, so a
     newer engine's Hello still names the mismatch.
   - `api/state_block.h` and `api/state_view.h`: item 3.
   - `api/engine_control.h`: `connect`, `hello`, `send(CommandRequest)`,
     `send(ParameterChange)`, `submit(ModelRequest)`, `poll(Inbox&)` and
     `state()`. The implementation assigns the seqs, and they rise across
     all three kinds of request.
2. **The model path (ADR-0009 §2).** An operation crosses as opaque bytes,
   because `contracts/api` may not depend on `common/model`. `common/model`
   gains two public headers:
   - `op_codec.h`: one strict JSON object per operation, with the session
     file's spellings.
   - `replication.h`: `Document` is the engine's half. It applies bytes,
     undoes, redoes and snapshots, and each returns the update to send.
     `Replica` is a client's half. It applies only what it is sent, checks
     that the revision follows on, and evicts down to the engine's floor.
     When it loses track it says so, and the client asks for `Resync`.

   The engine sends a `Snapshot` right after `Hello`. Both headers are
   additive public headers of `common/model`, which is a contract file
   under §6.
3. **The state block, layout version 1.**
   - The header holds magic, layout version, capacity, publish period,
     track count, heartbeat and seq. Under the seq it holds transport
     state, take, playhead, xruns, overruns, clock steps, health flags,
     inputs connected and buffer frames.
   - Each track gets one 64-byte-aligned `TrackState`: flags, gain,
     overruns, clip count, latency, and a 64-slot peak ring.
   - Health flags: `ServerGone`, `NoInputs`, `TakeFailed`.
   - Every atomic type in the block has its own
     `static_assert(is_always_lock_free)`, and none is wider than 8 bytes
     (2026-10-12 update, item 2).
   - **The seqlock uses no standalone fence.** The writer stores every
     field with release after an odd sequence. The reader loads them with
     acquire and checks the sequence again. GCC 14 warns that
     `atomic_thread_fence` is invisible to ThreadSanitizer (`-Wtsan`), and
     a fence-free seqlock is one TSan can check. On x86-64 a release store
     and an acquire load are plain moves.
   - **Peaks are a tagged ring, not a seqlocked value.** Each slot packs
     the publish tick's low 32 bits and the float into one 8-byte atomic. A
     reader keeps a cursor per track and takes the maximum of the slots it
     has not seen. So readers never steal each other's peaks, and the
     publisher stays the only caller of `Track::take_peak()`. A reader more
     than 64 publishes behind loses the oldest peaks, never the newest. A
     copied `StateView` is a second reader.
   - `StateView` retries the seqlock at most 64 times, yielding between
     attempts, then returns the snapshot with `stale` set.
   - `engine_alive()` is judged on the caller's clock. A heartbeat that
     stands still for max(250 ms, ten publish periods) means the engine is
     not answering.
   - `StateBlockMemory` is the block's anonymous, 64-byte-aligned home until
     M7 puts it in a memfd.
4. **`engine/control`** gains three pieces beside `install_session_commands`:
   - `StatePublisher` is a thread on a fixed timer, 100 Hz by default. A
     late publish is followed by the next one a period later, never by a
     burst. It reads only the atomics the audio thread already writes
     (`Transport`, `Track`) and the backend's any-thread counters. The
     control thread hands it the take number, input connections and health
     through atomics. **Nothing was added to `Session::process()` or to
     anything it calls.**
   - `EngineControlServer` owns the block, the publisher, the handlers and
     the `Document`. Its control thread takes encoded requests through
     `deliver()` and answers through an `Outbound` callback, so it knows no
     transport. It refuses an `Interface` command with `NotImplemented` and
     says whose command it is. Every 50 ms it checks `fatal()` and the
     take's failure (the `Fatal` and `TakeFailed` events). Once a second it
     runs the wiring layer's housekeeping.
   - `InProcessEngine` is the client half: an `EngineControl` whose
     requests and answers are encoded bytes through the server, and whose
     `StateView` reads the server's block. M7 replaces it with
     `SocketEngineClient` and gives the server a socket.
5. **`FakeEngine` lives in `contracts/api/testing`** (`sonora_api_fake`), as
   §4.1 said. It links `common/model`, so its model path is the real
   `apply()`. That makes it a test-support library, not the contract:
   `sonora_api` itself still links only `common/base`.
   - It is deterministic: the clock is injectable, and requests are handled
     at `poll()`.
   - It shares the `StateWriter` and the codec with the real engine.
   - It can script death, an event, a peak, xruns and a version mismatch.
6. **One conformance suite for both.** `contracts/api/conformance`
   (`sonora_api_conformance`) drives an engine through `EngineControl` and
   `StateView` only. It has nine checks:
   - Hello;
   - a version mismatch refused by name;
   - dispatch results and seq order;
   - arming and its refusals;
   - a take;
   - parameter changes;
   - the model path, with a real `Replica`;
   - per-reader peak cursors;
   - the heartbeat after the engine dies.

   `api_conformance_fake` runs it on `FakeEngine`. `engine/control`'s
   `control` test runs it on `InProcessEngine`, over a real `Session` that
   `drivers/fake` feeds at real-time pace. That test also fails if
   `FakeEngine::engine_commands()` differs from the handlers this build
   installs.
7. **The edges.**
   - `test_keymap` → `engine/control`, `ui/tui`: **gone.** The handler
     tests moved to `engine/control/tests/test_session_commands.cpp` and
     the termios tests to `ui/tui/tests/test_tty_mode.cpp`. `test_keymap`
     now takes the engine's commands from `FakeEngine`'s Hello, and it is
     platform-neutral (ADR-0008, V4).
   - `engine/app` → `ui/keymap`: **gone**, as a direct link and as code.
     The interactive path in `main.cpp` has no key decoding, no termios, no
     drawing and no track access.
   - `engine/app` → `ui/tui`: **one link remains, as a composition root.**
     The terminal interface (`TerminalView`, `run_terminal()`) lives in
     `ui/tui` and links only `contracts/api` and `ui/keymap`. `main.cpp`
     builds an `InProcessEngine` and calls `run_terminal()`. Until M7 puts
     them in two processes, one executable must contain both, and only a
     module above `engine/control` may link it. M7 removes this link when
     `sonora-tui` becomes its own executable.
   - **So M5's check names this one edge until M7, or M5 waits for M7.**
     That choice is the integrator's. Either way the entry is not an
     allow-list entry without an end date.
8. **The Qt interface** keeps its narrow `EngineAdapter`, so its routing
   tests need no engine, and gains `ControlAdapter`, which forwards every
   call to `EngineControl` and `StateView`.
   - `SyntheticEngine` is deleted. `FakeEngine` drives `sonora-ui` and its
     tests.
   - The Qt UI is **not** given `InProcessEngine`. §3 keeps Qt, Mesa and
     LLVM out of the process that holds the take, so the Qt UI meets the
     real engine through M7's socket.
9. **Measured** in the container on Docker Desktop on the owner's laptop.
   These are not appliance figures.
   - A writer paced at 1 kHz against a 60 Hz reader of 64 tracks, 60 ticks:
     0 stale reads, 0 torn, worst tick 0.015 ms.
   - An unpaced writer: about 600,000 reads in 400 ms, a few dozen stale,
     0 torn.
   - The publisher at a 1 ms period: 200 publishes in 200 ms.
