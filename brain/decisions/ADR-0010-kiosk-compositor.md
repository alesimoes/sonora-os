# ADR-0010 — The display stack: labwc as a configured kiosk, Qt on Wayland

- Status: **Proposed**. It shapes the image, so the owner confirms it before
  the image work in "Migration" starts.
- Date: 2026-10-13
- Decided by: architect, pending the owner.
- Numbering: ADR-0008 (portability) is on `docs/adr-0008-portability`, and
  ADR-0009 may be taken on another branch. This one is 0010 so that neither
  collides.

## Context

ADR-0006 chose Qt 6 and left two things open: **which compositor**, and the
process shape. ADR-0007 §3 settled the process shape (owner, 2026-10-12): the
engine is headless, and the Qt UI, the terminal UI and the plugin host are
separate processes. "The UI runs under the kiosk compositor in its own unit,
with no RT limits and its own restart." This ADR picks that compositor and
the units around it.

What forces the choice:

1. **Plugin editors are mostly X11.** Phase 4 hosts plugins out of process.
   Their editors are X11 windows on Linux, so they reach a Wayland session
   through Xwayland. Under `cage` 0.2.0 (ADR-0007, update 1 item 6;
   `brain/research/adr-0007-measurements.md` §6, measured):
   - a parentless X11 window is forced full screen over the UI, and its size
     hints are ignored;
   - an override-redirect popup is drawn at (0,0), not where it asked to be;
   - an X11 window cannot name a Wayland surface as its parent.
2. **The GPU is a requirement, but the image boots on hardware we have never
   seen** (ADR-0006). The Qt shell was measured on llvmpipe: 18 meters at
   60 Hz cost about one full core. The figure is the same under `cage` (wlroots,
   pixman) and on Xvfb (`brain/research/qt-shell-render-cost.md` on
   `feature/qt-shell`, 65f54d1, not yet pushed). The shell drops to 30 Hz and
   says so in red when it finds a CPU rasteriser.
3. **The engine keeps recording if the UI or the compositor dies** (ADR-0007
   §3).
4. **Budgets** (ADR-0006): ISO ≤ 1.5 GB (867–909 MB today), and ≤ 20 s to a
   usable interface (about 13 s to a running engine today).
5. **Keys.** The Pro Tools and Logic maps use F1–F12 and modifier chords. Any
   chord the compositor takes for itself never reaches the UI. On tty1 that
   has already happened: Alt+Fn switches consoles (BACKLOG, "Known gaps").
6. **Phase 6** (installed system, Secure Boot) must not be made harder.

Today's image has no display stack at all. `sonora-session.service` runs the
engine, which is also the terminal UI, on tty1 with `PAMName=login`. The image
manifest (`live/filesystem.packages` in `iso/sonora.iso`, 2026-10-11) has
`dbus` and `systemd`, but **not `libpam-systemd`**. So that unit does not
register a logind session today.

## Decision

### 1. Compositor: labwc, configured as a kiosk

**labwc 0.8.3** (trixie main, wlroots 0.18.2) runs one client, the Qt UI. The
configuration we ship, `/usr/share/sonora/labwc/`, makes it a kiosk:

- `rc.xml`:
  - **No compositor key bindings.** labwc loads its default bindings
    (Alt-Tab, Super-Return, and so on) whenever `rc.xml` declares no keybind
    at all, `<default/>` or not (labwc-config(5), 0.8.3). So the file
    declares exactly one inert binding on a key we never use
    (`XF86LaunchB` → `None`) and no `<default/>`.
  - No root-menu mouse bindings. `menu.xml` is an empty `openbox_menu`. The
    desktop shows for about a second between a UI crash and its restart, and
    nothing on it may lead anywhere.
  - Window rule for `identifier="sonora-ui"` (the UI's Wayland `app_id`):
    `serverDecoration="no"`, `fixedPosition="yes"`, actions `Maximize` and
    `ToggleAlwaysOnBottom`. The UI covers the output and stays **below**
    every other window. Clicking it focuses it but never raises it over an
    open editor.
  - Every other window, meaning plugin editors and Qt's own dialogs, keeps
    server-side decorations. A title bar to move it and a close button that
    sends `WM_DELETE_WINDOW` are how an operator gets rid of a misbehaving
    editor. Placement policy is `center`.
  - Xwayland is **lazy**. It is the default with wlroots ≥ 0.18.2
    (labwc-config(5)), and Debian ships 0.18.2-3. Boot pays nothing for X11;
    the first editor opened pays Xwayland's start.
- `environment`: written at start-up by the wrapper
  `/usr/local/bin/sonora-compositor`. It maps `/etc/default/keyboard` to
  `XKB_DEFAULT_LAYOUT/VARIANT/MODEL`, and sets
  `XKB_DEFAULT_OPTIONS=srvrkeys:none` (see §5).

### 2. Qt runs as a native Wayland client

- `QT_QPA_PLATFORM=wayland`, with no `;xcb` fallback list. If the Wayland
  connection fails, the UI exits and the unit's failure handling takes over
  (§4). It does not quietly come up on another path.
- `QT_WAYLAND_DISABLE_WINDOWDECORATION=1`, and `app_id` = `sonora-ui` via
  `QGuiApplication::setDesktopFileName`.
- The UI does not request full screen. The compositor rule maximizes it.
  `--windowed` and development runs are unaffected.
- Qt's menus, combo boxes and tooltips are `xdg_popup`s, placed by the
  compositor relative to their parent. None of them goes through Xwayland.
- The plugin host's opaque parent-window token (ADR-0007, update 1 item 6)
  stays in the Phase 4 contract. On the appliance its value is **empty**.
  Editors are parentless X11 toplevels and labwc places them, so the UI never
  has to be an X11 window.

### 3. Seat and session: logind, not seatd

- `libpam-systemd` is listed explicitly in the package list.
- A PAM service `/etc/pam.d/sonora-ui` has `pam_unix` account and session
  plus `pam_systemd`, and **no `pam_limits`**. The compositor then gets a
  logind session on seat0 and VT1, and libseat opens DRM and input through
  logind. That is the documented shape for cage, and it applies unchanged to
  any wlroots compositor
  (<https://github.com/cage-kiosk/cage/wiki/Starting-Cage-on-boot-with-systemd>).
- `pam_limits` is left out so that `/etc/security/limits.d/95-audio.conf`
  (rtprio 95 for `audio`) does not reach the UI. `LimitRTPRIO=0` on the
  unit then holds.

### 4. Units

Five units. **None of the others names `sonora-engine.service`, and it names none
of them.** That is the whole of (3) in the Context, expressed as unit
dependencies.

| Unit | Runs | Key properties |
|---|---|---|
| `sonora-engine.service` | `sonora-engine` headless (ADR-0007 M7) | `User=sonora`, no TTY, `StandardOutput=journal`, `ExecStartPre=sonora-wait-for-audio`, `Restart=always`, `RestartSec=1`, `LimitRTPRIO=95`, `LimitMEMLOCK=infinity`, `Nice=-10`, `OOMScoreAdjust=-900`. `WantedBy=multi-user.target`. |
| `sonora-compositor.service` | `sonora-compositor` → `exec labwc -C /usr/share/sonora/labwc` | `User=sonora`, `PAMName=sonora-ui`, `TTYPath=/dev/tty1`, `TTYReset`, `TTYVHangup`, `TTYVTDisallocate`, `StandardInput=tty-fail`, `UtmpIdentifier=tty1`, `Conflicts=getty@tty1.service`, `After=systemd-user-sessions.service systemd-logind.service`, `Wants=sonora-ui.service`, `Restart=always`, `RestartSec=1`, `StartLimitIntervalSec=60`, `StartLimitBurst=5`, `OnFailure=sonora-display-fallback.service`, `LimitRTPRIO=0`, `OOMScoreAdjust=500`. `WantedBy=multi-user.target`. |
| `sonora-ui.service` | `sonora-ui` | `User=sonora`, `BindsTo=` + `After=sonora-compositor.service`, `ExecStartPre=sonora-wait-for-display` (waits for `$XDG_RUNTIME_DIR/wayland-0`), `Environment=WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland …`, `Restart=always`, `RestartSec=1`, the same start limit and `OnFailure=`, `RestartPreventExitStatus=` the UI's "unfit display" exit code, `LimitRTPRIO=0`, `OOMScoreAdjust=500`. No `[Install]`: the compositor pulls it in. |
| `sonora-tui.service` | `sonora-tui` (ADR-0007 M7), a client like the Qt UI | tty2, the TTY properties of today's `sonora-session.service`, `Conflicts=getty@tty2.service`, `Restart=always`, `LimitRTPRIO=0`. `WantedBy=multi-user.target`. |
| `sonora-display-fallback.service` | `chvt 2`, as root | `Type=oneshot`. Started only by `OnFailure=`. |

The behaviour this gives:

- **UI crash:** systemd restarts the UI in 1 s. The engine is untouched, and
  the UI reattaches and finds `Recording` (ADR-0007 §3).
- **Compositor crash:** `BindsTo` stops the UI, systemd restarts the
  compositor, and its `Wants=` starts a fresh UI. The engine is untouched.
- **Five failures in 60 s, or the UI exiting with its "unfit display" code:**
  the unit fails, and `OnFailure` switches the console to tty2. There, the
  terminal UI has been attached to the same engine all along. That is
  ADR-0006's "falls back to the terminal UI", built from units rather than
  from code in the UI.
- When to exit with the "unfit display" code is ADR-0006's call: the
  rendering path the UI finds at start-up. Today's shell stays up at 30 Hz,
  in red, on llvmpipe. Whether llvmpipe counts as "no usable driver" is a
  ui-engineer decision under ADR-0006, and this ADR does not make it. It only
  provides the exit.
- Once the UI implements it, `sonora-ui.service` becomes `Type=notify` with
  `WatchdogSec=5`. A UI that hangs without crashing is then killed and goes
  down the same path. The `sd_notify` write goes behind `common/platform`
  (ADR-0008: `ui/qt` uses only Qt).
- `multi-user.target` stays the default target. A graphical target buys us
  nothing.

### 5. Keyboard

- The compositor binds no keys (§1), so every chord except the one inert key
  reaches the focused client.
- **VT switching is off.** labwc 0.8.3 acts on the `XF86Switch_VT_n` keysyms
  unconditionally (`src/input/keyboard.c`, `handle_change_vt_key`). XKB's
  default rules produce those keysyms for Ctrl+Alt+F1–F12. The option
  `srvrkeys:none` makes those chords plain function keys, so a profile can
  use them. A key that is listed in a profile but switches the console away
  from the UI mid-take is a control that lies.
- The cost: an operator cannot reach tty2 by hand. The terminal UI shows up
  when the display stack has failed, which is when it is needed, and it is
  always running for anyone with a serial or SSH path in the future.
- When a plugin editor has focus, keys go to the editor. Whether transport
  keys must reach the engine while an editor is focused (Pro Tools'
  keyboard-focus behaviour) is for the Phase 4 plugin ADR. The compositor
  cannot route a key to a client that is not focused, and it should not try.

### 6. What the GPU situation does under this stack

Source: wlroots 0.18.2 `render/wlr_renderer.c`, `renderer_autocreate`. With
`WLR_RENDERER` unset, it tries GLES2 on the DRM device. It falls back to
pixman **only if the backend has no render node**.

| Machine | Compositor | Qt UI | Result |
|---|---|---|---|
| Working Mesa driver | GLES2 | GL on the GPU | The supported configuration. |
| Firmware framebuffer only (`simpledrm`, no render node) | GLES2 through Mesa's software path, or pixman | llvmpipe, shm buffers | It runs, on the CPU. The UI says so in red (ADR-0006) and may choose the "unfit display" exit. |
| Render node with a broken driver | GLES2 fails and pixman is **not** tried, so labwc exits | n/a | Five restarts, then tty2. It fails loudly. |

This is the same wlroots and the same path as the cage figures in the Qt
shell note. Choosing a compositor does not settle ADR-0006's open LLVM
question: Mesa is needed for any of these options.

## Rationale

The trade is about what each option costs us later.

- **labwc is the only candidate that does what we need with configuration
  alone.** Measured here, headless labwc 0.8.3 with Xwayland and the rule
  set above:
  - a fixed-size 400×300 parentless X11 window kept its size;
  - it was drawn above a maximized, always-on-bottom Wayland client;
  - an override-redirect 200×100 popup was drawn exactly at its requested
    (50,60);
  - the Wayland client was intact after the X clients exited.

  Cage fails the first and third of those (measured in §6 of the research
  note). Fixing cage means carrying a patched compositor. The policy we need
  (UI at the bottom, editors floating, popups where they ask) is ordinary
  stacking-window-manager behaviour, and labwc is a stacking window manager.
- **The UI stays native Wayland.** Option (a) of the ADR-0007 update would
  run Qt on X11 to be the editors' transient parent. That makes the UI depend
  on Xwayland, and it hands Qt's own popups to the same override-redirect
  handling that cage drew at (0,0). Under labwc, editors need no parent, so
  nothing in `ui/qt` depends on X11. That also keeps ADR-0008's edge clean:
  the display server is an OS concern in `iso/`, not in the UI.
- **Losing a take is the worst outcome.** In this layout no display failure
  reaches the engine's unit. The cgroup, restart and OOM priority are all
  separate.
- **Fail loudly at start-up.** No `xcb` fallback list, no silent pixman
  fallback on a broken driver, and a bounded restart count ending on a
  visible terminal.
- **Same boot on unknown hardware.** One compositor, one Qt platform, logind
  for the seat, and in-kernel DRM drivers only. Nothing is probed or chosen
  at run time except what wlroots' renderer selection does, and that is
  stated above.
- **Size:** labwc costs 18 MB more than cage (below). That is the price of
  not owning a compositor fork, and it is 1–2 % of the budget.

## Consequences

### Measured: package availability and size

Run 2026-10-13 in `debian:trixie`, from apt's own accounting with
`--no-install-recommends` (the image uses `--apt-recommends false`).
Installed, uncompressed. Each delta is over the same "Qt runtime + Mesa" set
as ADR-0006's measurement: `libqt6gui6 libqt6widgets6 libqt6quick6`, the QML
modules `qtquick`, `-controls`, `-window`, `-templates`, `libgl1-mesa-dri
libglx-mesa0 libegl-mesa0 fonts-dejavu-core`.

| Set | Packages | Installed | Delta over Qt + qt6-wayland |
|---|---:|---:|---:|
| Qt runtime + Mesa, no display server | 133 | 365 MB | — |
| + `qt6-wayland` 6.8.2-4 | 140 | 370 MB | — |
| + `cage` 0.2.0-2 (pulls `xwayland`) | 173 | 386 MB | +16 MB |
| + **`labwc` 0.8.3-1** (pulls `xwayland`) | 193 | 404 MB | **+34 MB** |
| + `sway` 1.10.1-2 + `xwayland` | 191 | 396 MB | +26 MB |
| + `weston` 14.0.2-1 + `xwayland` | 296 | 641 MB | +271 MB |

- All five are in trixie **main**, section `x11`. `xwayland` is 2:24.1.6-1.
- Weston's size comes from Debian building in its remote backends: it pulls
  `libfreerdp3-3`, `libneatvnc0`, GStreamer and VA-API.
- Without `libpam-systemd` listed, apt satisfies `libseat1`'s
  `Depends: seatd | logind` with the `seatd` daemon. With it listed, logind
  satisfies the dependency and seatd is not installed (measured both ways).
- These figures are over a bare container. The image already carries some
  of the shared libraries, so the real delta is at most this. Squashfs should
  roughly halve it, which is an expectation, not a measurement. Expected ISO:
  about 909 MB + ~200 MB ≈ 1.1 GB, inside the 1.5 GB budget.

### Boot time

- Nothing measured yet. Xwayland is lazy, so it is not on the boot path.
- The 20 s budget to a drawn frame is checked in the QEMU smoke test at
  migration stage 2. Exceeding it triggers a revisit of ADR-0006, as that
  ADR says.

### Secure Boot and Phase 6

- Unaffected. The compositor is user space and uses only in-kernel DRM
  drivers, signed with the kernel.
- This ADR **excludes the NVIDIA proprietary driver**. It is out-of-tree
  DKMS modules, unsigned, needing MOK enrolment, and from non-free. NVIDIA
  hardware gets nouveau or nothing, and whether nouveau is good enough is
  unverified.
- A graphical installer in Phase 6 (Calamares is Qt) runs as another client
  of the same compositor unit. It needs no second display stack.

### Contracts

- No change. The Phase 4 parent-window token stays as ADR-0007 described it.
  It is empty on the appliance.

### Smaller consequences

- The image gains a PAM service file, a labwc configuration directory, two
  small wrapper scripts and five units.
- `sonora-session.service` is removed at stage 2 (below).
- `getty@tty2` is disabled at stage 2, because the terminal UI owns tty2.

## Migration

**Stage 1: lands now, before M7; the shipped boot is unchanged.**

distro-engineer:
- Package list, a "display" block:
  `libpam-systemd labwc xwayland qt6-wayland`, plus the Qt runtime set
  above. Packages for the UI binary come with `ui/qt`'s install rule.
- `includes.chroot`:
  - `/usr/share/sonora/labwc/{rc.xml,menu.xml}`;
  - `/etc/pam.d/sonora-ui`;
  - `/usr/local/bin/sonora-compositor` and `/usr/local/bin/sonora-wait-for-display`,
    both with their `chmod` in the hook;
  - the compositor, UI and fallback units.
- The compositor unit carries `ConditionKernelCommandLine=sonora.gui`, and
  the UI unit runs `sonora-ui --demo` (FakeEngine). Under `sonora.gui`,
  `sonora-session.service` gets `ConditionKernelCommandLine=!sonora.gui`.
  Without the parameter the image boots exactly as today.
- QEMU smoke-test checks under `sonora.gui`:
  - ISO size;
  - time to the UI's first frame (it logs one);
  - `kill -9` of the UI, then of labwc, recovers within 3 s;
  - an injected repeated UI failure ends with `fgconsole` = 2.
- Record the renderer labwc picks on QEMU's `-vga std`, which has no render
  node.

ui-engineer:
- `setDesktopFileName("sonora-ui")`. No full-screen request when not
  `--windowed`.
- Log one line at the first frame, for the boot-time check.
- A distinct "unfit display" exit code, documented in `ui/qt`.
- Later: `sd_notify` READY and WATCHDOG from the 60 Hz tick, through
  `common/platform`.

**Stage 2: after ADR-0007 M7 (headless engine, `sonora-tui` client).**

distro-engineer:
- Replace `sonora-session.service` with `sonora-engine.service` and
  `sonora-tui.service` (tty2), and enable the compositor without the
  condition.
- Move the smoke test's engine checks to the new unit name.
- Add one test: start a take, `kill -9` labwc and the UI, and assert that the
  engine PID is unchanged and the take file keeps growing.

**Stage 3: before the Phase 4 plugin ADR.**

researcher:
- The unverified items 3 and 6 below, with a real Qt 6 UI and a real
  LV2/VST3 X11 editor under this labwc configuration.

## Unverified

Stated so that nobody treats them as measured. Each has an owner and a
measurement.

1. **Placement with real clients.** The labwc test used `foot` as the
   Wayland stand-in and python-xlib windows as editors. Not yet seen:
   - a real Qt 6 UI and a real plugin editor;
   - focus returning to the UI when an editor closes;
   - Qt's dialogs under `ToggleAlwaysOnBottom`.

   labwc honoured the editor's own requested position (10,10, title bar
   added). A rule that centres editors that ask for (0,0) is untested.
   Owner: researcher.
2. **The inert keybind suppresses all defaults.** This rests on
   labwc-config(5). Test: send Alt-Tab and Super-Return to the UI through a
   virtual keyboard in QEMU and check that they arrive. Owner:
   distro-engineer, stage 1.
3. **Renderer on a machine with no render node, and its CPU cost**: GLES2 on
   llvmpipe or pixman, measured on QEMU `-vga std` and then on a laptop with
   the GPU driver blacklisted. Owner: researcher.
4. **Restart semantics.** `BindsTo` + `Wants` + `StartLimitBurst` +
   `OnFailure` + `RestartPreventExitStatus` are expected to reach tty2 within
   about 10 s, and to do nothing to the engine. That is reasoned from
   systemd.unit(5), not run. Owner: distro-engineer, stage 1.
5. **Logind session from a system unit with `PAMName=` and `pam_systemd`.**
   Documented for cage, not yet run in our image. Owner: distro-engineer,
   stage 1.
6. **Xwayland start latency** when the first editor opens, under lazy start.
   If it is visible, set `<xwaylandPersistence>yes` and pay at boot instead.
   Owner: researcher.
7. **Boot time and compressed ISO size** with this stack. Owner:
   distro-engineer, stage 1.
8. **Bare metal only:**
   - whether a GPU reset or hang under the compositor costs the audio
     callback anything;
   - xruns with the UI on the GPU versus on llvmpipe (ADR-0006 measurements
     2 and 3);
   - nouveau on NVIDIA hardware.

   BACKLOG, under "blocked on hardware".

## Alternatives rejected

**cage 0.2.0, Qt on Wayland.** The smallest option (+16 MB), built for
exactly this purpose, and already measured. Rejected on the measurements:
- parentless X11 editors are forced full screen with their size ignored;
- override-redirect popups, which is how many plugin UIs draw their menus,
  appear at (0,0);
- there are no decorations, so a broken editor cannot be moved or closed;
- every new view takes focus, and there is no way to switch between views.

Each of these is in cage's positioning code (`view.c`, `xwayland.c` at
v0.2.0), not in its configuration. So the fix is a fork we would maintain.

**cage with Qt on xcb under Xwayland (option a).** Editors can then name the
UI as `WM_TRANSIENT_FOR` and are centred at their own size. Rejected:
- Qt's own menus, combo boxes and tooltips are override-redirect windows
  under xcb, and cage drew those at (0,0). That was measured with
  python-xlib, not Qt, but the code path is the same.
- The UI would depend on Xwayland, and every editor open would carry the
  UI's X11 window id through the engine.

**cage with a host-side X11 anchor window (option b).** The anchor is
parentless, so cage shows it full screen over the UI whenever an editor is
open. That hides the meters during a take. Rejected.

**sway in a kiosk configuration.** A viable second choice: +26 MB, the same
wlroots, and good Xwayland handling (inferred, not tested here). Rejected in
favour of labwc:
- sway is tiling. The shape we want (UI underneath, editors floating above)
  has to be built from `for_window` rules against its defaults.
- A full-screen container hides floating windows other than its own popups,
  so the UI could not simply be full screen.
- Its IPC socket lets any process of the same user drive the compositor,
  `exec` included.

sway is the fallback if labwc fails Unverified 1 or 2.

**weston with kiosk-shell.** kiosk-shell full-screens every toplevel except
those with a parent or a fixed size. That handles fixed-size editors, but
resizable editors would go full screen. Rejected:
- Debian's weston costs +271 MB (measured), most of it remote-desktop
  backends (FreeRDP, neatvnc, GStreamer, VA-API). That is network-facing code
  an appliance does not want, and a fifth of the size budget.
- It is not wlroots, so none of the Qt shell measurements carry over.

**Qt on EGLFS/KMS with no compositor.** Rejected on two counts, either one
sufficient:
- **Debian trixie's Qt 6.8.2 ships no EGLFS plugin** (measured). The
  platform plugins in `libqt6gui6` are `linuxfb`, `minimal`, `offscreen`,
  `vkkhrdisplay` and `xcb`. `qt6-qpa-plugins` adds `vnc`, and `qt6-wayland`
  adds the Wayland ones. `apt-cache search eglfs` returns nothing. Using
  EGLFS means building Qt ourselves, against ADR-0006 ("from the Debian
  trixie packages") and ADR-0002, with the LGPL relinking duty that ADR-0006
  chose to avoid.
- With no display server there is nowhere for an out-of-process X11 editor
  to appear at all.

`linuxfb` is CPU-only, and Qt Quick's software backend cannot draw our
`QSGGeometryNode` meters (Qt shell note). `vkkhrdisplay` is experimental,
needs Vulkan drivers the image does not carry, and has the same X11 problem.

**Our own wlroots compositor with a fixed policy.** A few hundred lines of
policy, but we would own input, the session, VT handling and wlroots' API,
which breaks on every minor release (0.17 → 0.18 → 0.19). Rejected while a
packaged compositor does the job with a configuration file. It is
reconsidered only if both labwc and sway fail.

**seatd for seat access.** It works, but it is one more daemon with its own
group, when logind is already part of the systemd we ship and the existing
unit already uses a PAM session. Rejected. `libpam-systemd` is listed
explicitly so that apt does not choose seatd for us.
