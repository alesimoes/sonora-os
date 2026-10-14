# ADR-0007 "Unverified": measurements and sources

Status: research note, written 2026-10-12. It answers the six items in the
"Unverified" section of
[ADR-0007](../decisions/ADR-0007-modules-contracts-and-squads.md). The ADR's
decided text is left as written; what changes in it is listed under
"Consequences for ADR-0007" at the end, and was appended to the ADR as its
dated section "Update, 2026-10-12".

Every claim below carries one of three labels:

- **[measured]**: run here. The note names the machine, the container or VM,
  and the command.
- **[source]**: read from a specification, a man page, published source code
  or upstream documentation. The URL is given.
- **[inferred]**: reasoned from the two kinds above. It has not been checked
  directly.

## The machine

All measurements ran on one laptop: Windows 11 Enterprise 10.0.26200, an
11th Gen Intel Core i7-11800H at 2.30 GHz, 16 logical CPUs. Docker Desktop
ran Docker Engine 20.10.14 on the WSL2 kernel `6.6.87.2-microsoft-standard-WSL2`.
That is a generic kernel inside a Hyper-V VM, **not** the RT kernel we ship.
QEMU 11.1.0 ran with WHPX acceleration (the guest logs "Hypervisor detected:
Microsoft Hyper-V").

Containers used `debian:trixie` (13.7) with trixie-backports enabled. The
harness and scripts lived in the session scratch area and are **not** in the
repository. The method for each item is described closely enough to rebuild
it.

## Summary

| ADR-0007 item | Verdict | Evidence |
|---|---|---|
| 3. RTSan / `[[clang::nonblocking]]` on trixie | **Confirmed available.** `clang-22` is in trixie main and `clang-21` is in trixie-backports. Both catch an allocation and a mutex at runtime and warn at compile time. trixie's default `clang` (19) does neither. | measured |
| 6. `std::atomic<float/double/uint64_t/int64_t>` always lock-free | **Confirmed** with GCC 14.2 on amd64 and on arm64 (cross-compiled, run under qemu-user). 16-byte atomics are **not** lock-free. | measured |
| 1. Socket round trip and state-block read under RT load | **Assumption holds in the container.** The 64 B `SOCK_SEQPACKET` round trip is p50 60 µs, p99 158 µs, max 484 µs. A 64 KiB seqlock read is p50 3–9 µs and max under 100 µs. **New finding:** a writer that publishes without pause starves the reader. | measured (container, rough) |
| 2. Plugin round-trip wake mechanism | **Open for hardware.** In the container a futex round trip is p50 61 µs, p99 209 µs, max 3.6 ms, and eventfd is about the same. Spinning is about 0.3 µs, but under `SCHED_FIFO` it **hits RT throttling: stalls of about 50 ms**. | measured (container, rough) |
| 4. Virtual 18-channel ALSA card in QEMU | **Partly confirmed.** `snd-dummy model=rme9652` shows 26 ordered `capture_AUX0..25` ports, and `snd-aloop` shows 2×32. Exactly 18 ordered `AUX0..AUX17` needs a WirePlumber rule that sets **both** `audio.channels` and `audio.position`. With channels alone the ports are named `capture_1..capture_18`. | measured (image kernel 6.12.111-rt) |
| 5. X11 plugin editors under `cage` + Xwayland | **Works, with constraints that matter.** A parentless X11 window is forced to full screen over the UI. A window with `WM_TRANSIENT_FOR` is centred at its own size. An override-redirect popup was drawn at (0,0) instead of where it asked to be. | measured (cage 0.2.0 headless) + source |

---

## 1. RealtimeSanitizer and `[[clang::nonblocking]]` on Debian trixie

### What Debian ships [measured, `apt-cache madison`, 2026-10-12]

| Package | Version | Suite |
|---|---|---|
| `clang` (default) → `clang-19` | 1:19.1.7-3+b1 | trixie/main |
| `clang-17`, `clang-18` | 17.0.6-22+b2, 18.1.8-18+b1 | trixie/main |
| `clang-22`, `libclang-rt-22-dev`, `llvm-22` | 1:22.1.8-1~deb13u4 | trixie/main (a stable update, `~deb13u4`) |
| `clang-21`, `libclang-rt-21-dev`, `llvm-21` | 1:21.1.8-3~bpo13+2 | trixie-backports |
| `g++-14` | 14.2.0-19 | trixie/main |

The RTSan runtime is in `libclang-rt-<N>-dev`
(`/usr/lib/llvm-22/lib/clang/22/lib/linux/libclang_rt.rtsan-x86_64.a`).
Symbolized reports need `llvm-<N>`, which provides `llvm-symbolizer-<N>`.
Without it RTSan prints raw addresses and "invalid path to external
symbolizer".

### What each compiler does [measured]

Test program: a function marked `[[clang::nonblocking]]` calls a
non-inline helper that does `std::vector::push_back`. A second marked
function takes a `std::lock_guard<std::mutex>`. The flags were
`-std=c++17 -O1 -g -fsanitize=realtime`.

| Compiler | `-fsanitize=realtime` | Runtime report | `-Wfunction-effects` (compile time) |
|---|---|---|---|
| clang 19.1.7 | **rejected**: "unsupported argument 'realtime'" | none | attribute accepted silently, **no diagnostics** |
| clang 21.1.8 (bpo) | works | `malloc` and `pthread_mutex_lock` intercepted, exit code **43** | warns on the call to `push_back`, the non-nonblocking `grow()` and `lock_guard` |
| clang 22.1.8 | works | same as 21 | same as 21 |
| g++ 14.2 | n/a | n/a | `'clang::nonblocking' scoped attribute directive ignored [-Wattributes]`, compiles and runs |

Other runtime behaviour [measured, clang 22]:

- `RTSAN_OPTIONS=halt_on_error=false` reports the violation and lets the
  program continue.
- `RTSAN_OPTIONS=suppressions=<file>` with `call-stack-contains:grow`
  suppresses the report. A `function-name-matches:` line matches the
  *intercepted* function, which is `malloc`, not the caller.
- `-fsanitize=realtime,address` is rejected by the driver ("not allowed
  with"). RTSan therefore needs its own build, separate from any ASan or TSan
  job.

A macro of the ADR's shape works on both compilers [measured]:

```cpp
#if defined(__clang__) && defined(__has_cpp_attribute)
#  if __has_cpp_attribute(clang::nonblocking)
#    define SONORA_RT [[clang::nonblocking]]
#  endif
#endif
#ifndef SONORA_RT
#  define SONORA_RT
#endif
virtual void process(float* b, int n) noexcept SONORA_RT = 0;      // pure virtual
void process(float* b, int n) noexcept SONORA_RT override { ... }  // override
```

The attribute sits after `noexcept` and before `override`/`= 0`, because it
is a function-*type* attribute. A virtual call from one `SONORA_RT` function
to another passes the static check. An allocation inside the override is
reported both by `-Wfunction-effects` and at runtime. GCC accepts the
`defined(__clang__)` form without a warning. With the bare attribute, GCC
warns under `-Wattributes`.

With `-Wfunction-effects`, clang 22 tries to infer `nonblocking` for inline
libstdc++ functions. When that fails, it reports the *transitive* cause: "cannot be inferred 'nonblocking' because it calls
`std::__throw_length_error`". Expect most `std::` containers to warn
statically even where the call path never allocates in practice. That noise
is a reason to treat `-Wfunction-effects` as advisory at first and the
runtime check as the gate.

[source] Clang's documentation describes RTSan as treating any
`[[clang::nonblocking]]` function as real-time and exiting non-zero on a
violation. It describes `ScopedDisabler` / `__rtsan_disable()`, the two
suppression forms, and recommends using RTSan together with the compile-time
function-effect analysis: <https://clang.llvm.org/docs/RealtimeSanitizer.html>.
The compile-time analysis covers direct calls and infers only within a
translation unit. Calls through virtual functions and function pointers are
flagged unless the target is declared with the attribute:
<https://clang.llvm.org/docs/FunctionEffectAnalysis.html>. The ADR says
"Clang 20". This note did not open the Clang 20 release notes. What is
established here is that 19 lacks RTSan and that 21 and 22 have it.

### Sketch: a CI job on the `SONORA_RT` call graph [inferred, not built]

1. **Image.** Start from `debian:trixie-slim` with `clang-22`,
   `libclang-rt-22-dev` and `llvm-22`. All three are in trixie main, so the
   job needs no backports pin. Add the existing build dependencies (`cmake`,
   `pkg-config`, `libjack-jackd2-dev`). GCC 14 stays the compiler of the
   release build. Clang is a second configuration, never the shipped one.
2. **Configure.** Set `CC=clang-22 CXX=clang++-22` and
   `-DCMAKE_CXX_FLAGS="-fsanitize=realtime -g -O1 -Wfunction-effects"`, plus
   `-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=realtime`. Use a separate build
   directory, e.g. `build-rtsan`.
3. **Run.** Run `ctest` with
   `RTSAN_OPTIONS=halt_on_error=true:suppressions=<repo>/tools/rtsan.supp`
   and `RTSAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-22`. Any test that
   drives `Session::process()` offline exercises the whole marked call graph:
   the fake or offline backend tests, ring-buffer tests, and later the
   `drivers/fake` conformance suite. Exit code 43 fails the job.
4. **What it catches.** It catches only paths that tests execute. That is
   why the conformance suite and the fault injections matter. The
   `rt-safety-auditor` still reads the static graph.
5. **Static half.** Keep `-Wfunction-effects` as a warning at first, not
   `-Werror`, because of the libstdc++ inference noise above. Promote it to
   an error per module once that module's `SONORA_RT` functions are clean.
6. **Escape hatch.** A blocking call that is deliberate and audited (none
   should exist) goes in a `__rtsan::ScopedDisabler` scope. Each one needs a
   reviewer's sign-off, as an RT exception would.

One question this did not settle: whether `libjack` from pipewire-jack,
called from inside a marked callback (`jack_port_get_buffer` and so on),
trips an interceptor. That needs the real engine built under RTSan against
the PipeWire container.

### Settled 2026-10-12, with M2 [measured]

- **Setup.** `sonora-engine` was built with `clang-22 -fsanitize=realtime`
  from branch `contract/backend`. Disassembly shows
  `__rtsan_realtime_enter` at the entry of `JackBackend::Impl::process_cb`,
  `Session::process` and `Track::report_peak`, and a probe program
  confirmed the runtime catches `malloc` there.
- **Normal recording.** The engine recorded 18 tracks through
  `tools/test-engine-entrypoint.sh` (PipeWire 1.4.2, quantum 128) with no
  RTSan report, `xruns=0`, and every track carrying its own tone. The
  callback's libjack calls (`jack_port_get_buffer`, `jack_last_frame_time`,
  `jack_get_sample_rate` every period) did not trip an interceptor.
- **Refusal paths.** Forcing quantum 64 and rate 96000 mid-take was also
  clean under RTSan.
- **Limit.** These results cover only the paths those runs executed.
  `-Wfunction-effects` still warns on the libjack calls, because they are
  unannotated C.

**Rate change in pw-jack, as the rt-safety-auditor read it** [source:
pipewire-jack 1.4.2, `pipewire-jack.c` lines 5328–5349, not measured
here]:
- `jack_get_sample_rate()` is lock-free while the client is active, so it
  may be called from the callback.
- With `jack.locked-process=true`, the default, the notification and the
  periods are serialised, so no period at a new rate runs before the
  notification.

The engine checks the rate every period anyway, which keeps it correct
without that default and under jackd2.

**What the container measured:**
- With 18 tracks and `clock.force-rate 96000` mid-take, no 96 kHz period
  landed in any file. The check was the zero crossings of the 5639 Hz track
  in every 128-frame period: 29–30 throughout, where a 96 kHz period would
  show about 15.
- That held for the per-period check and also for the earlier code, which
  relied on the notification alone (3 runs). The container therefore
  confirms the auditor's reading. It cannot single out the per-period
  check; only `test_period_guard` does.
- **Trap.** `clock.force-rate` is silently ignored when the configuration's
  `clock.allowed-rates` lacks the rate. A runtime image built from an
  `audio-stack/` that allows only 48000 Hz makes this test pass without
  testing anything.

### The CI job, built 2026-10-13 [measured, clang-22.1.8]

`tools/build-rtsan.sh` (CI job `rtsan`, `-DSONORA_RTSAN=ON`) builds the
whole tree as the sketch above says and runs every ctest with
`halt_on_error=true`. What the first runs showed:

- **`syscall()` is intercepted** (ADR-0012 Unverified 10). A raw
  `syscall(SYS_futex, FUTEX_WAKE, ...)` from a nonblocking function is
  reported as `unsafe-library-call` in `syscall`. `wake_all` now runs its
  one call between `__rtsan_disable()` and `__rtsan_enable()`, declared
  `noexcept` locally: `<sanitizer/rtsan_interface.h>` does not mark them
  so, and `__rtsan::ScopedDisabler` in a noexcept function pulls
  `std::terminate` into the object, which the A3 `nm` test refuses.
- **A shared module cannot link with `--no-undefined`** under RTSan: Clang
  puts the runtime in executables only, and a bundle takes `__rtsan_*` from
  its host. The native bundles drop the flag in this build only.
- **No violation in product code** on the paths the tests drive:
  `Session::process` through `drivers/fake`, plugin_client's InsertChain
  step, the Bridge in-process and through every LV2/VST3 bundle binary,
  and the `sonora-dsp-measure` harness. The one runtime report outside
  `wake_all` was a test recorder that allocates in its callback, by design.
- **`-Wfunction-effects` is not clean**: about 15 warnings, all SONORA_RT
  functions calling unmarked ones (BACKLOG, ADR-0007 follow-ups).

## 2. `std::atomic` lock-freedom (ADR item 6)

[measured] `g++ 14.2.0-19 -std=c++17 -O2` printed
`is_always_lock_free` / `is_lock_free()` for each type, on amd64 natively
and on arm64 with `aarch64-linux-gnu-g++` 14.2.0-19, run under `qemu-aarch64`
(qemu-user from trixie):

| Type | Size | amd64 always / runtime | arm64 always / runtime |
|---|---|---|---|
| `float` | 4 | 1 / 1 | 1 / 1 |
| `double` | 8 | 1 / 1 | 1 / 1 |
| `uint64_t` | 8 | 1 / 1 | 1 / 1 |
| `int64_t` | 8 | 1 / 1 | 1 / 1 |
| `uint32_t`, `bool` | 4, 1 | 1 / 1 | 1 / 1 |
| 16-byte struct (`alignas(16)`) | 16 | **0 / 0** | **0 / 0** |

`ATOMIC_LLONG_LOCK_FREE` is 2 on both. A 16-byte atomic needs `-latomic` to
link at all, and then it goes through libatomic's lock table. Anything wider
than 8 bytes must therefore never sit as a `std::atomic` in shared memory.
The ADR's `static_assert(std::atomic<uint64_t>::is_always_lock_free)` is
correct and passes. The same assert for `float` passes too, so it can be
added.

The arm64 result comes from user-mode emulation. It shows what the compiler
and libstdc++ decide (a compile-time constant) and does not exercise real
arm64 hardware. Address-freedom is not a property `is_always_lock_free`
reports. For GCC on these targets it follows from the atomics being plain
instructions on the object's own address [inferred].

## 3 and 4. IPC costs under a periodic real-time load (ADR items 1 and 2)

### Method [measured]

There was one C++17 harness, built with `g++ -O2` in a `debian:trixie`
container started with `--cap-add=SYS_NICE --ulimit rtprio=99 --ulimit
memlock=-1`. Without `SYS_NICE`, `chrt -f` fails with EPERM. With it,
`SCHED_FIFO` works. `kernel.sched_rt_runtime_us` is 950000, the default.

- **`rtload`** stands in for the audio callback: a separate process at
  `SCHED_FIFO` 80 with `mlockall`. It wakes on an absolute
  `clock_nanosleep` every 2 666 667 ns (128 frames at 48 kHz), mixes 18×128
  floats, and records how late it woke and whether the block finished after
  the next deadline. It ran for the whole duration of the tests below.
- **socket**: `socketpair(AF_UNIX, SOCK_SEQPACKET)` between two forked
  processes, both `SCHED_OTHER`. The server echoes. The client times
  send→recv. 100 warm-up iterations are dropped.
- **seqlock**: a 64 KiB `MAP_SHARED` block between two processes. The writer
  is `SCHED_OTHER` and stands in for the publisher thread: odd sequence,
  release fence, 8 184 relaxed 8-byte stores, even sequence with release. The
  reader times the whole read including retries, copying either with relaxed
  atomic loads (well-defined) or with `memcpy` (formally a data race, which
  is what most seqlocks in the wild do). A torn-read check compares the
  first and last word. The ADR's actual block is smaller (per-track atomics
  plus a seqlock over the transport only), so 64 KiB is a deliberately
  pessimistic size.
- **wake**: two processes share a mapping. The "engine" side is itself a
  2.667 ms periodic loop at `SCHED_FIFO` 80. Each period it signals `req`
  and waits for `done`. The "host" (`SCHED_FIFO` 70, no DSP) waits for
  `req`, timestamps, and signals `done`. Three mechanisms were tried: a
  shared (non-`PRIVATE`) futex, a pair of eventfds, and busy-spin with
  `pause`. One-way is the time from submit to the host's wake. Round trip is
  from submit to the engine seeing `done`.

**These are container figures on a Hyper-V guest running a generic kernel.**
They put an order of magnitude on the costs and expose qualitative
behaviour. They are not latency figures for the appliance.

### Raw numbers (µs; run 2, all under the concurrent `rtload`)

| Test | n | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| SEQPACKET 64 B, paced 1 kHz | 20 000 | 60.1 | 158.1 | 240.4 | 484.0 |
| SEQPACKET 64 B, back-to-back | 200 000 | 39.3 | 82.0 | 136.1 | 1 086.7 |
| SEQPACKET 1 KiB, paced 1 kHz | 20 000 | 55.6 | 195.8 | 256.7 | 772.3 |
| seqlock 64 KiB, atomic copy, writer 1 kHz, reader back-to-back (cache-hot) | 20 000 | 2.4 | 3.8 | 14.5 | 923.6 |
| seqlock 64 KiB, memcpy, writer 1 kHz, reader back-to-back | 20 000 | 1.1 | 1.2 | 8.5 | 79.8 |
| seqlock 64 KiB, atomic copy, writer 1 kHz, reader 500 Hz (fresh data each read) | 5 000 | 3.3 | 16.5 | 32.9 | 78.0 |
| seqlock 64 KiB, memcpy, writer 1 kHz, reader 500 Hz | 5 000 | 2.6 | 12.1 | 42.4 | 89.5 |
| seqlock 64 KiB, atomic copy, writer 100 Hz, reader 60 Hz (the UI case) | 600 | 8.7 | 25.4 | 47.5 | 52.8 |
| futex, one-way (both FIFO) | 7 449 | 35.2 | 131.9 | 220.0 | 3 520.1 |
| futex, round trip (both FIFO) | 7 449 | 60.6 | 209.2 | 344.7 | 3 565.6 |
| futex, round trip (both `SCHED_OTHER`) | 5 574 | 60.7 | 223.3 | 279.7 | 303.8 |
| eventfd, one-way (FIFO) | 7 449 | 38.1 | 149.8 | 368.9 | 41 755.5 |
| eventfd, round trip (FIFO) | 7 449 | 63.8 | 244.5 | 489.3 | 41 794.2 |
| spin, one-way (FIFO) | 7 449 | 0.20 | 3.3 | 46 213 | 51 456 |
| spin, round trip (FIFO) | 7 449 | 0.30 | 3.9 | 46 214 | 51 456 |

A repeat (run 3) gave eventfd round trip p50 58.3, p99 212.1, max 358.2.
The 41 ms maximum in run 2 did not reproduce, so treat it as host noise. Spin
with the host at `SCHED_OTHER` gave round trip p50 0.33, p99 7.2, p99.9 34.3,
max 673.8.

`rtload` wake lateness (µs):

| Condition | n | p50 | p99 | p99.9 | max | blocks finished after the next deadline |
|---|---|---|---|---|---|---|
| alone, 20 s | 7 499 | 114.9 | 331.0 | 396.0 | 845.9 | 0 |
| concurrent with everything, 260 s (run 2) | 97 499 | 52.9 | 252.0 | 379.4 | 15 642 | 46 |
| concurrent with everything except spin, 175 s (run 3) | 65 624 | 53.5 | 266.1 | 366.1 | 3 258 | 8 |
| concurrent with the FIFO spin test only, 25 s | 9 374 | 95.5 | 245.7 | 296.0 | 2 434 | 0 |

Block work itself is 0.5–1 µs p50 and at most 130 µs.

### Reading the numbers

- **Socket round trip: the ADR assumption holds.** A command round trip
  "well under 1 ms" held at p99 (158–196 µs paced) and at the paced max
  (484–772 µs). Back-to-back produced one 1.09 ms outlier in 200 000. At
  human or 100 Hz rates this is irrelevant [measured, container].
- **State-block read: negligible, as assumed.** Even the pessimistic 64 KiB
  read costs single-digit µs at p50 and under 100 µs at max in the paced
  cases. Zero torn reads were seen in any run [measured, container].
- **New: a seqlock writer must not publish back-to-back.** With the writer
  republishing 64 KiB without pause, the reader validated only 97 of 100
  reads. Three exceeded 100 ms and were abandoned, the p99 read took 41 ms,
  and there were 47 418 failed validations [measured; reproduced in run 3:
  2 starved of 100, max 41.8 ms]. The publisher thread must run on a timer
  (100 Hz–1 kHz is fine: zero starvation, at most 5–54 failed validations).
  Its write must be short compared with the period. This is a constraint on
  `engine/control`'s publisher, and the ADR does not state it.
- **New: spinning at `SCHED_FIFO` hits RT throttling.** The spin host at
  FIFO 70 shows a gap of about 46–51 ms at p99.9. That matches the
  50 ms per second that `sched_rt_runtime_us=950000` takes back from RT
  tasks that never sleep [inferred from the number and the setting; the
  throttling message itself was not captured]. The same spin at
  `SCHED_OTHER` had a 0.67 ms worst case. Either way a spinning host burns a
  whole core. The rejected-alternative argument in the ADR ("spinning burns
  a core") is right, and the throttling makes spinning at FIFO worse than
  useless unless `sched_rt_runtime_us=-1`. Our `audio-stack/sysctl-sonora.conf`
  would then have to set that, and doing so removes the system's protection
  against a runaway RT thread.
- **futex vs eventfd: no difference that matters here.** Both are about
  60 µs p50 and 210–245 µs p99 for a round trip in this environment. Against
  a 2 667 µs period, the p99 round trip is about 8 % of the budget. The
  futex worst case under FIFO (3.6 ms) exceeds a period. On this generic
  Hyper-V kernel that is noise from the host, not a property of futex, but it
  shows why the ADR's `collect(..., deadline_ns)` must drop the block rather
  than wait [inferred].
- **The load process missed deadlines in every concurrent run, spin or
  not** (46 and 8 misses, worst wake 15.6 ms and 3.3 ms). It missed none
  when alone for 20 s, and none in either 25 s run beside the spin test. On
  a WSL2 guest these cannot be attributed to the readers. Hyper-V
  scheduling the VM's vCPUs is at least as likely a cause. **No xrun
  attribution is possible from this environment**, which is exactly what
  ADR item 1 asks for. That needs the RT kernel on bare metal.

## 5. A virtual multichannel ALSA card in QEMU (ADR item 4)

### Method [measured]

- The kernel and initrd were extracted read-only from `iso/sonora.iso`
  (dated 2026-10-11 13:35; kernel `6.12.111+deb13-rt-amd64`). The ISO was
  attached as a CD-ROM. Boot used `-kernel/-initrd`, as
  `tools/test-boot-qemu.sh` does, with **no** `intel-hda` device.
- The modules came from the kernel command line:
  `modules-load=snd-dummy snd_dummy.model=rme9652`, or
  `modules-load=snd-aloop snd_aloop.pcm_substreams=1`.
  `snd-aloop.ko.xz` and `snd-dummy.ko.xz` are both present in the image's
  module tree.
- To run diagnostics and add a WirePlumber rule without modifying `iso/`,
  a scratch live-boot persistence disk was attached as IDE: one MBR
  partition, ext4 labelled `persistence`, `persistence.conf` = `/ union`.
  **The overlay files must sit under `rw/` on that partition.** Files at the
  partition root are ignored. On that disk, `/usr/local/bin/sonora-smoketest`
  was replaced with a diagnostic that prints `/proc/asound`, `wpctl status`,
  `pw-link -o -I`, the node properties from `pw-dump`, and `hw_params`
  while a `pw-record` runs against the capture node.

### Results [measured, image kernel 6.12.111-rt, PipeWire 1.4.2]

| Card | Pro Audio capture nodes | Ports per node | Names | hw_params while running |
|---|---|---|---|---|
| `snd-dummy model=rme9652` | 1 (`pro-input-0`) | 26 | `capture_AUX0..capture_AUX25` | 26 ch, S32_LE, MMAP_INTERLEAVED, period 128, buffer 256 |
| `snd-aloop` (no rule) | 2 (`pro-input-0`, `pro-input-1`) | 32 each | `capture_AUX0..capture_AUX31` | not captured |
| `snd-aloop` + rule `audio.channels = 18` only | 2 | 18 each | **`capture_1..capture_18`**: the AUX naming is lost | 18 ch, FLOAT_LE, MMAP_NONINTERLEAVED |
| `snd-aloop` + rule `audio.channels = 18` and `audio.position = "AUX0,…,AUX17"` | 2 | 18 each | `capture_AUX0..capture_AUX17` | 18 ch, FLOAT_LE, MMAP_NONINTERLEAVED |

In every case the capture ports of a node had increasing registry ids in AUX
order: for example, 189..206 for AUX0..AUX17. That is **not** true of
`monitor_` ports on the output node, where `monitor_AUX0` was id 46 and
`monitor_AUX1` id 45. Registry order is therefore not a channel order, and
sorting by name with `natural_less` (`engine/include/sonora/port_order.h`)
remains the right rule.

[source] Why no driver gives exactly 18 natively: `snd-dummy`'s models fix
the channel count. `rme9652` is 26..26, `ice1712` is 10..10, and the default
is 1..2. `snd-aloop` allows 1..32, and the first stream to prepare fixes the
count for the whole cable. See
<https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/drivers/dummy.c>
and <https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/drivers/aloop.c>.

Consequences for a CI test:

- `snd-dummy model=rme9652` is the simplest stand-in for "more than 18
  ordered AUX capture ports through the real ALSA → PipeWire → JACK path".
  It needs no rule at all. `--tracks 18` then takes the first 18.
- If a test needs exactly 18, use `snd-aloop` plus a test-only WirePlumber
  rule with both properties. Setting only the count silently changes the
  port names, and the shipped smoke test's `grep -c capture_AUX` would then
  count 0.
- The shipped smoke test counts `capture_AUX` across *all* nodes. On
  `snd-aloop` that gives 64 (two nodes). A virtual-card assertion needs to
  count per node.
- Not exercised: the engine actually recording from these ports, and the
  udev rule (the dummy card is a `platform-` device, not USB).

## 6. X11 plugin editors under `cage` with Xwayland (ADR item 5)

### What trixie ships [measured]

`cage 0.2.0-2` depends on `libwlroots-0.18 (0.18.2-3)` and on `xwayland`
(2:24.1.6-1), so it is built with Xwayland support. The image built on
2026-10-11 does not contain cage yet.

### Headless test [measured]

Container `debian:trixie` with `cage xwayland python3-xlib grim weston`, and
`WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1`. That
gives a 1280×720 output. The primary client was the native Wayland
`weston-simple-shm`, standing in for the Qt UI. A python-xlib client then
mapped X11 windows with `PMinSize`/`PMaxSize` set to their size, as a
fixed-size plugin editor does. Screenshots came from `grim` (cage supports
screencopy), with pixel bounding boxes taken from them.

| X11 window | Requested | Where it ended up |
|---|---|---|
| A: parentless toplevel | 400×300 at (10,10) | **resized to 1280×720 at (0,0)**, over the Wayland UI, and focused. The size hints were ignored. |
| B: `WM_TRANSIENT_FOR` = A | 300×200 | 300×200, **centred** (rendered bbox 490,260–788,458), above A |
| C: override-redirect (a popup or menu) | 200×100 at (50,60) | X reports (50,60), but it was **rendered at (0,0)** (bbox 0,0–198,98) |
| after the X client exits | n/a | the Wayland UI is visible again |

[source] This matches cage's code at tag v0.2.0. In `xwayland.c`,
`is_primary()` is `parent == NULL`. In `view.c`, `view_position()` maximizes
primary views and centres the rest. Each newly mapped view takes focus. There
is no way for the user to switch between views:
<https://raw.githubusercontent.com/cage-kiosk/cage/v0.2.0/xwayland.c>,
<https://raw.githubusercontent.com/cage-kiosk/cage/v0.2.0/view.c>.
`is_transient_for()` for an X11 child returns true only when the parent is
an XDG-shell view, but positioning never consults it. The README says only
"A kiosk runs a single, maximized application":
<https://github.com/cage-kiosk/cage>.

What this means for SonoraOS [inferred]:

- An editor window from an out-of-process plugin host *does* appear over the
  fullscreen UI, but as a full-screen window with the editor drawn at its
  fixed size in the corner. That is unusable as it stands.
- To get a centred, correctly sized editor, the host's X11 window needs an X11
  parent through `WM_TRANSIENT_FOR`. An X11 window cannot name a native
  Wayland surface as its parent. So either the Qt UI also runs on X11
  (`QT_QPA_PLATFORM=xcb` under Xwayland), or the plugin host creates a hidden
  X11 "anchor" toplevel and parents editors to it. The anchor itself would be
  primary and full-screen: a full-screen anchor (black or transparent)
  appears over the UI whenever an editor is open. Neither variant was tested.
- The override-redirect misplacement means X11 popup menus inside plugin UIs
  would open at the screen origin, not at the mouse. This was observed on
  cage 0.2.0 only. Whether upstream has fixed it was not determined.
- Another option is a different kiosk compositor (a wlroots compositor with
  a small custom policy, or a configured `labwc`/`sway`). That is the
  "may constrain the compositor choice" outcome the ADR anticipated, and it
  is now a real constraint.

## Still unverified

1. Any of the IPC figures on the RT kernel and bare metal, with the real
   engine recording 18 channels at quantum 128 through PipeWire. That
   includes attributing xruns to a reader, which this environment cannot do.
2. The futex vs eventfd choice on the RT kernel. Also not measured: the cost
   of the syscall on the audio thread itself (the auditor's ruling the ADR
   asks for), and whether `sched_rt_runtime_us=-1` is acceptable if spinning
   is ever wanted.
3. ~~Whether `pipewire-jack`'s libjack calls trip RTSan interceptors from
   inside a `nonblocking` callback.~~ They did not, on the paths exercised;
   see "Settled 2026-10-12, with M2" in §1.
4. RTSan on arm64 (`libclang_rt.rtsan-aarch64`). It was not checked.
5. Atomics on real arm64 hardware: only the compile-time answer was checked.
6. Plugin editors with a real Qt 6 Wayland UI, a real LV2/VST3 editor, and
   input focus returning to the UI when the editor closes or loses
   interest. Also the `xcb` and anchor-window variants above, and whether
   upstream cage still misplaces override-redirect windows.
7. The engine actually recording from the `snd-dummy`/`snd-aloop` ports in
   QEMU, and whether quantum 128 holds there.

## Consequences for ADR-0007 (for its owner to apply by a new ADR or an amendment)

- Unverified item 3 is resolved. The `SONORA_RT` macro can become the
  attribute now. The CI job uses `clang-22` from trixie main, and the
  default `clang` (19) is not enough.
- Item 6 is resolved for amd64 and for arm64's compiler. Add a
  `static_assert` for `float`. Forbid atomics wider than 8 bytes in shared
  memory.
- Item 1 holds in the container. Add the publisher pacing constraint.
- Item 2: futex and eventfd are equivalent in the container. Spinning at FIFO
  is ruled out unless RT throttling is disabled. The decision waits for the
  RT kernel.
- Item 4: use `snd-dummy model=rme9652` (26 ports), or `snd-aloop` with a
  rule that sets both channels and positions.
- Item 5: cage maximizes parentless X11 windows. Plugin editors need an X11
  transient parent or a different compositor policy.
