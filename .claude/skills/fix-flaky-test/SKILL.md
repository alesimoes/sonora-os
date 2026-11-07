---
name: fix-flaky-test
description: Make a timing-dependent test deterministic at the root. Use when a test fails intermittently, on an unrelated PR, or only under load or TSan.
---

# Fix a flaky test

1. Read the failing log, not the whole test: which assertion, what it waited
   for, how long.
2. Reproduce before touching anything, through `tools/build-slot.sh`:
   `ctest -R NAME --repeat until-fail:50`, a CPU hog beside it, and under the
   sanitizer where it failed. Write down the failure rate.
3. Find the assumption: a sleep, a fixed deadline, "thread X has started by
   now", output order of two threads, a shared file or port.
4. Replace it:
   - wait on an event or an atomic the code under test publishes;
   - bound the wait by progress (fail when the state stops advancing), not by
     a larger number of milliseconds;
   - inject the clock or the thread when the code takes one (interface, fake);
   - give each test its own directory, port and names.
5. If the test exposed a real race in product code, stop and report it with
   the interleaving; do not paper over it in the test.
6. Re-run step 2; report both rates. Zero failures in 50 under load is the bar.
7. Never: raise a timeout, add a sleep, add `slow`/`timing` to hide it, drop
   the test, or edit the gate (`.claude/rules/gates-and-checks.md`).
