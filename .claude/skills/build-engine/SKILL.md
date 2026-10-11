---
name: build-engine
description: Build and test the SonoraOS engine. Use whenever engine C++ code changes, before reporting any engine work as done, and when asked whether the engine compiles.
---

# Build the engine

The engine needs Linux and the JACK development headers. On Windows the build
runs in a container.

## Run it

    tools/build-engine.sh

That configures CMake into `build/`, compiles, and runs `ctest`. It is
incremental; re-running after an edit is cheap.

## What to report

Report what the command printed. Specifically:

- whether it configured, compiled and linked
- the exact text of any warning or error, not a summary of it
- the `ctest` line showing how many tests passed

`-Wall -Wextra` is on and warnings in this code base are usually real bugs in
the making. Do not report success while warnings remain unexplained.

If the build cannot run at all — no Docker daemon, no Linux — say that plainly
and say that the code is therefore unverified. Never describe code you could
not compile as working.

## Common failures

**`Package jack was not found`** — the JACK headers are missing. In the
container that means the image lacks `libjack-jackd2-dev`; on Debian install
it. We link against the API, so either JACK2 or `pipewire-jack` development
headers satisfy it.

**Test hangs** — `test_ringbuffer` runs a producer thread against a consumer.
A hang there means the ring buffer lost or reordered data and one side is
waiting forever. That is a real defect, not a flaky test.
