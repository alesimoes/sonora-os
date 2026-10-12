# ADR-0006 — Qt 6 as the user interface toolkit

- Status: Accepted
- Date: 2026-10-11
- Decided by: project owner

## Context

This is a professional recording, editing and mixing system. That framing is
load-bearing rather than aspirational, and it decides arguments that would
otherwise be matters of taste:

- **A dropped sample is a defect, not a performance characteristic.** No
  interface frame rate, no visual effect and no convenience is worth an xrun.
- **Losing a take is the worst outcome in the product.** It outranks crashes,
  it outranks a wrong reading on screen, and it outranks every feature.
- **A control that lies is worse than a control that is missing.** An engineer
  trusts the meter and finds out after the take; this is why the engine refuses
  to record with nothing connected rather than showing a plausible zero.
- **The target hardware is a studio machine,** not an arbitrary one. That is
  what makes the GPU requirement below reasonable.


The engine has no graphical interface. What exists is a terminal display in
`engine/src/main.cpp`: `draw()` clears the screen, prints the transport clock
and one ASCII meter per track at 20 fps, and the main loop reads raw keystrokes
from stdin. It proves the audio path and nothing more. `sonora-session.service`
owns tty1, `getty@tty1` is disabled, and the image has no X11, no Wayland
compositor and no display manager at all — `set-default multi-user.target`.

The stated goal for the interface is a reverse-engineered Pro Tools interface,
*improved and modern* in appearance and usability, with the same functionality
and commands: edit, mix and multitrack record — everything Pro Tools and Logic
Pro offer. That is a large, conventional, deeply nested desktop application:
dozens of dialogs, docked windows, menus, keyboard-driven everything — plus two
or three surfaces that are not conventional at all and must redraw at video
rate over 18 channels (the UMC1820 count the engine already records).

Three options were put to the owner with their trade-offs. The owner chose
Qt 6.

## Decision

**Qt 6, Widgets and QML, as the user interface toolkit**, from the Debian
trixie packages, dynamically linked, LGPL-3.0.

The division of labour is fixed, not a matter of taste:

- **Qt provides the shell.** Window, menus, docking, dialogs (file choosers
  included), input handling, focus and keyboard routing, accessibility, high-DPI
  scaling, internationalisation. These are the parts that are expensive to write
  and uninteresting to own, and they are the reason for choosing Qt over the
  alternatives below.
- **We draw everything audio-critical ourselves.** Meters, the timeline, the
  waveform display and the playhead are **custom `QQuickItem` subclasses
  rendering in the Qt Quick GPU scene graph** — via `updatePaintNode()` and the
  scene graph's node classes, or a direct rendering hook. They are **not
  `QWidget`s, not `QPainter`, and not stacks of QML `Rectangle`s.**

**The UI pulls; the audio thread never pushes.** The interface samples engine
state from a single **60 Hz timer** on the GUI thread and reads the existing
atomics: `Track::take_peak()`, `rec_armed()`, `muted()`, `overruns()`,
`Transport::playhead()`. There must be **no Qt signal emitted per sample, per
block, or per metering update**, and no `QObject` reachable from
`Session::process()` or anything it calls.

This is binding. An emit from the audio thread allocates (the queued-connection
event), takes the receiving object's lock, and queues onto another thread's
event loop — all three are forbidden on the real-time path by `CLAUDE.md`, and
`take_peak()` is already an `exchange` precisely so that none of them is needed.

## Rationale

- The application is mostly conventional. A Pro Tools-class feature set is
  hundreds of dialogs, menus and docked panels before it is a waveform. Qt is
  the only one of the three options that gives us those for free, and the
  per-widget cost we pay for them is paid in windows that are not on the
  real-time path.
- It is the only option with real accessibility, native file dialogs, and
  keyboard/focus semantics an engineer's hands already expect. ADR-0004 puts
  Pro Tools' *commands and key bindings* in scope; a toolkit that does not do
  serious keyboard routing makes that harder than it needs to be.
- Qt Quick's scene graph means choosing Qt does not cost us GPU rendering. The
  meters and timeline get the same OpenGL path Dear ImGui would have given us,
  inside an application that also has a menu bar. That is what makes the hybrid
  defensible rather than a compromise.
- Debian ships Qt 6, so ADR-0002's frozen-package-set argument applies to the
  toolkit as well: we do not maintain a Qt build.

### Why the 60 Hz pull, stated plainly

The headline risk of Qt, shown to the owner as a stated con, is that **meters
at 60 Hz fight the Qt event loop**. With 18 channels this is not theoretical.
Eighteen `QWidget` meters repainting at 60 Hz means ~1080 paint events per
second through `QPainter`, each a CPU-side rasterisation on the GUI thread,
plus whatever signal traffic drove them. On a machine where the audio callback
must complete inside 2.7 ms (128 frames at 48 kHz), that load competes for the
same cores and the same memory bandwidth as the callback. **Ordinary Qt widgets
for meters would produce xruns caused by the interface itself.** The custom
scene-graph rendering is not premature optimisation; it is the mitigation for
the one risk that was named when the decision was made, and anyone who
"simplifies" it back to widgets or QML `Rectangle` stacks has reintroduced the
defect this ADR exists to prevent.

The terminal `draw()` already has the correct shape — pull, fixed rate, clear
the peak on read. The GPU version is the same contract at 60 Hz with one draw
call per surface instead of one per channel.

## Consequences

### The image grows, and gains a display server

Qt 6 was presented as roughly 200 MB. That is only part of the cost: today's
image has **nothing to display on**. A Qt GUI needs a display server, which
this ADR names as a real addition rather than an implementation detail:

- A Wayland compositor in kiosk form (one fullscreen client, no desktop) plus
  Mesa, `libdrm` and input stack. This is the direction: a kiosk compositor
  matches ROADMAP Phase 1's "fullscreen, no desktop visible" better than a full
  X11 session, and keeps the operator away from any shell.
- `iso/config/package-lists/sonora.list.chroot` gains a Qt block and a display
  block. The `sonora` account is already in `video`, so device access is
  provided for.
- `sonora-session.service` is rewritten. Today it is a tty program: `PAMName=login`,
  `TTYPath=/dev/tty1`, `StandardInput=tty`, `Conflicts=getty@tty1.service`.
  A compositor-hosted GUI needs none of that and needs a seat instead. The
  real-time properties of the unit — `LimitRTPRIO=95`, `LimitMEMLOCK=infinity`,
  `Restart=always` — are not negotiable and carry over unchanged.

**What we accept:** an image up to **1.5 GB** and a boot to a usable interface
of up to **20 seconds**, against today's measured 867 MB and ~13 seconds to a
running engine. If a built image exceeds either figure, that is a trigger to
revisit this ADR, not a number to quietly absorb. A USB appliance at 1.5 GB is
still a cheap stick; one at 4 GB is a different product.

### The terminal UI stays

`draw()` and the headless `--record` path are not replaced. The headless mode
is how the engine is tested (`tools/test-engine-docker.sh`, the QEMU smoke
test, CI), and ROADMAP Phase 1's exit criteria depend on it. The terminal
display is also the diagnostic of last resort when the GUI or the compositor
will not start: an appliance that can still record from a tty is worth more
than a tidier `main.cpp`. The GUI must never become the only way to run the
engine.

### Measured: the runtime cost is 375 MB, not 200 MB

Measured 2026-10-11 in a Debian trixie container, installing the runtime set
the image would need -- `libqt6gui6`, `libqt6widgets6`, `libqt6quick6`, the
QtQuick and QtQuick.Controls QML modules, `libgl1-mesa-dri`, `libglx-mesa0`,
`libegl-mesa0`, `cage`, `seatd` and a font -- with no development packages:

    before:  127 MB
    after:   502 MB
    delta:   375 MB   (166 packages)

That is uncompressed on-disk size. The image ships it through squashfs, which
on this package mix should land somewhere near half, so expect the ISO to grow
from 867 MB to roughly 1.0--1.1 GB. The compressed figure is an expectation,
not a measurement; the 375 MB is measured.

**The 200 MB in the options presented to the owner was wrong, and the error
was the display side, exactly where the architect said nobody had costed it.**
Qt itself is the smaller half. `libLLVM.so.19.1` alone is 124 MB, pulled in by
`mesa-libgallium`, because Mesa's Gallium drivers compile shaders through LLVM.

An open question that follows, worth real money in boot time and size: with a
GPU now a stated requirement, which drivers do we actually ship? Intel's
`iris` does not need LLVM. `radeonsi` and the software fallback `llvmpipe` do.
Dropping LLVM would cut a third of this delta, at the cost of not booting
usefully on AMD or on a machine with no driver at all. That decision needs the
target hardware named, and it is not made here.

Either way this stays inside the 1.5 GB budget below.

### A working GPU is a stated hardware requirement

**Decided by the owner, 2026-10-11:** a machine used for professional
recording, editing and mixing is expected to have a GPU, and the UI may depend
on one. This closes what this ADR first listed as an open question.

The reasoning is the system's own framing. This is not a utility that has to
run on whatever is lying around; it is a studio tool, and a studio machine
driving an 18-channel interface with a scrolling waveform and eighteen meters
is already a machine with a graphics adapter. Requiring a working Mesa driver
costs us nothing a real target lacks.

Software rasterisation (`llvmpipe`) will not hold 60 Hz across 18 meters and a
scrolling waveform, and attempting it would burn CPU the audio thread needs.
So the requirement is not a preference to be degraded around; it is the
supported configuration.

What still follows, because the image must behave predictably on hardware we
have never seen:

- The UI **determines its rendering path at start-up and says which one it
  got, loudly.** A machine that silently fell back to software rendering and
  then caused xruns mid-take is the failure this project keeps finding in other
  forms.
- On a machine with no usable driver it falls back to the terminal UI rather
  than thrashing. That is a diagnostic path, not a second supported product:
  the operator is told the machine is not fit for the job, and the engine still
  records, because refusing to record would be worse.
- Lowering the meter rate to protect the callback is always acceptable.
  Dropping samples never is.

### Process shape: the GUI is in the engine's address space

Reading `Track`'s atomics from the UI requires the UI and the audio callback to
share memory. The front end therefore links `sonora_core` and runs in the same
process as the JACK callback, with the GUI on the main thread and audio on the
server's real-time thread. Two consequences follow and are binding:

- A GUI crash takes the take with it. Every code path the GUI can reach must
  leave a valid file behind — the same discipline `main.cpp` already applies
  when the audio server disappears.
- No Qt object, event loop call, or signal may be touched from the audio
  thread, and the GUI thread must not hold anything the audio thread waits on.
  The only traffic across the boundary is atomics and the `RingBuffer`.

Whether the GUI should instead be a separate process over shared memory is left
open below.

### Licensing

The engine is GPL-3.0-or-later (`SPDX-License-Identifier` on every file). Qt 6
under LGPL-3.0 is compatible with that, and shipping a GPL-3.0-or-later
application against LGPL Qt is the ordinary, well-trodden case. The condition
LGPL-3.0 attaches is that the user can **replace the Qt libraries** with their
own build:

- **Dynamic linking only.** Use the Debian Qt 6 packages as shared libraries.
- **Static linking is forbidden** unless we also ship what LGPL-3.0 section 4
  requires to relink against a modified Qt — object files or equivalent, plus
  the installation information. We are not doing that; the rule is simply "do
  not statically link Qt".
- No bundled or vendored Qt build that the operator cannot swap out, and no
  Qt module pulled in without checking its own licence: **not every Qt 6 module
  is LGPL-3.0** — some are GPL-3.0-or-commercial. A GPL-3.0 module is still
  compatible with us, but it forecloses any future non-GPL redistribution, so
  the licence of each module is checked when it is added.

Anything beyond that — the exact text of our offer, or redistributing the
image commercially — is **legal care, not a technical rule**, and this ADR does
not pretend to settle it. The technical rules above are the ones agents must
follow.

### ADR-0004 still draws the line

Reproducing Pro Tools' workflow, commands, edit modes and key bindings is in
scope (ADR-0004). Its artwork, icons, exact colours and exact visual design are
not. "Improved and modern" is not decoration on the goal — it is what keeps the
work on the right side of that line, and a Qt interface that pixel-matches
Pro Tools would be a violation of ADR-0004 regardless of how it was built.

## What this does not decide

Named as open so the next ADR or the `ui-engineer` knows what is still theirs:

- **QML versus Widgets per surface.** That Qt is the toolkit does not settle
  which half renders the mixer, the editor or the dialogs. Only the
  audio-critical surfaces are fixed (custom `QQuickItem`, scene graph).
- **The window and docking model.** Pro Tools' Edit/Mix window split, a single
  docked shell, or something else.
- **The command vocabulary and key map.** `keymaps/protools.json` and
  `keymaps/logic.json` exist as a plan, not a schema. How the UI binds a
  command name to an action is downstream of this ADR.
- **Which compositor**, and whether the GUI is in-process or split across a
  shared-memory boundary.
- **Where this lands in the roadmap.** The ROADMAP has no UI toolkit item;
  Phase 1's "fullscreen, no desktop visible" is satisfied today by the tty.

## Unverified

Stated so nobody treats it as measured:

- **No Qt has been built or run in this project.** Nothing in this ADR has been
  compiled.
- **The ~200 MB figure is an estimate** from the options presented to the owner,
  not a measurement, and it excludes the display server and Mesa, which may
  well cost more than Qt does.
- **The 60 Hz / 18-channel scene graph claim is reasoned, not benchmarked.**

The measurements that would settle these, for the researcher rather than for
guesswork:

1. Build the image with the Qt and display packages added and record the ISO
   size and the boot time to a drawn frame. Compare against 867 MB / 13 s.
   This settles the size and boot budget above.
2. Run the engine recording 18 channels at 48 kHz, quantum 128, with the GUI
   drawing 18 meters and a scrolling waveform at 60 Hz, and report xruns and
   callback worst-case duration — once with the scene-graph implementation and
   once with `QWidget` meters, so the margin this ADR claims is a number.
3. Determine the rendering path on hardware with no GPU driver, and measure
   what `llvmpipe` actually costs the callback. This settles how aggressive the
   start-up degradation has to be.

Until (1) and (2) exist, the size budget and the mitigation are design intent
backed by argument.

## Alternatives rejected

**Dear ImGui + OpenGL.** GPU-rendered immediate mode. Meters at 60 fps with no
widget cost; timeline and waveform as direct drawing; pure C++ with no extra
runtime; a small binary and a fast appliance boot; total visual control; about
50 MB of dependencies. **This was the recommended option** and it remains the
better fit for the surfaces we care most about — which is exactly why its
rendering model is the one adopted inside Qt above. Rejected because every
complex widget would be ours to write, there is no accessibility, and even file
dialogs must be built. For a Pro Tools-class feature set that is the larger
share of the work, and it is work that buys nothing an operator would notice.

**A web UI served by the engine over local HTTP.** Modern visuals with familiar
tooling, remote access from a tablet for free, and fast design iteration.
Rejected on three counts, any one of which would be enough: a browser in the
appliance is ~400 MB and slow to boot, against an image that currently reaches
a running engine in 13 seconds; editing has measurable input latency; and
meters over websockets are never sample-accurate. It also puts the interface on
a stack we do not control, which is the opposite of ADR-0002's reason for
choosing a frozen base. Remote monitoring from a tablet remains attractive and
can be added later as a *secondary* surface over the same pull-based state,
without being the primary interface.

## Amendment, 2026-10-12

The process shape in "Process shape: the GUI is in the engine's address
space" is **superseded by ADR-0007 §3**, decided by the owner 2026-10-12. The
engine runs headless. The Qt UI, the terminal UI and control surfaces are
separate client processes. They talk to the engine over a Unix socket and
read a shared state block that they map read-only. A UI crash therefore no
longer takes the take with it.

The rest of this ADR stands unchanged:
- the UI pulls at 60 Hz and the audio thread never pushes;
- no Qt object is reachable from the callback;
- scene-graph rendering for meters and timeline;
- the GPU requirement and the licensing rules.

The text above is left as it was decided.
