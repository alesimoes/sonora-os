# CI compiles with the shipped toolchain, on pinned runners

## What happened

GitHub moved `ubuntu-latest` to Ubuntu 26.04, which uses the amd64v3 package
variant: GCC 15.2, `-march=x86-64-v3` by default, FMA on. The engine and
sanitizer jobs installed `build-essential` on the runner, so from one run to
the next CI compiled with a different compiler, for a different instruction
set, than the appliance ships, and nothing said so. PR #154 found it when a
glibc override in `playback_test.h` stopped building (`<random>` pulls in
`<mm_malloc.h>` on x86-64-v3) and the mix's bits moved with `-march`.

The appliance (ADR-0002) and `tools/build-engine.sh` use Debian trixie: GCC
14.2, baseline x86-64.

## The rule (2026-10-26, PR #155)

1. **Every job that compiles C++ does so in a trixie image from `tools/`**,
   never with the runner's compiler. The engine, sanitizer (TSan, ASan,
   UBSan), record and interop jobs and the nightly slow and CPU jobs use
   `tools/Dockerfile.build`, the image `tools/build-engine.sh` builds. The
   UI, plugins and RTSan jobs already used `Dockerfile.ui`,
   `Dockerfile.plugins` and `Dockerfile.rtsan`, all trixie.
2. **No job that compiles C++ runs on a floating label.** They are pinned to
   `ubuntu-24.04`. The host still supplies the kernel, and a container
   shares it: the ASLR setting TSan needs (`vm.mmap_rnd_bits=28`), RT limits
   and memlock come from the host, so a new label is a deliberate change.
3. **Each such job prints its compiler** (`c++ --version`, or
   `clang++-22 --version` for RTSan) in a step, so a log says which compiler
   a green run means.
4. Jobs that compile nothing (shellcheck, executable bits, progress, the
   quantum pin) stay on `ubuntu-latest`.

The image is built with `docker/build-push-action` and its layers kept in
the Actions cache (`scope=sonora-build`), as the UI, plugins and RTSan images
already were; the engine and sanitizer jobs write the cache, the others read
it. No registry is used.

## What this does not cover

- The RTSan job uses clang-22 (trixie main) because GCC has no RealtimeSanitizer;
  GCC 14 stays the compiler of the shipped build (ADR-0007).
- `sonora_core` is built without `-ffp-contract=off`, unlike `sonora_dsp`
  (ADR-0011 §2.3). With trixie's baseline GCC the default does not fuse, but
  another `-march` would; that decision is a BACKLOG line from #154.
- A Debian point release can still move GCC 14's patch level under the
  image; it is the same move the appliance makes when it updates.
