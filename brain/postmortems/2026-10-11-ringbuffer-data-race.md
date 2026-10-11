# 2026-10-11 — data race in the ring buffer, found by CI

## Symptom

The first CI run failed. ThreadSanitizer reported a data race inside the
`memcpy` in `RingBuffer`, between the producer thread and the consumer thread.

The test itself printed `test_ringbuffer: ok`. Every assertion passed. The
sequence check that verifies nothing was lost or reordered passed too, on this
machine, on this run.

## Root cause

The producer and the consumer need opposite memory orderings, and both were
using the consumer's.

`write_space()` was implemented as `capacity_ - read_space() - 1`, and
`read_space()` loads `write_` with `acquire` and `read_` with `relaxed`. That
is correct for the consumer: the acquire on `write_` pairs with the producer's
release store and makes the written data visible.

For the producer the roles reverse. It needs `acquire` on `read_`, because the
consumer stores `read_` with `release` *after* finishing its `memcpy`. Loading
`read_` relaxed lets the producer observe the advanced index without
synchronising with that copy, and then overwrite bytes the consumer is still
reading.

## Why the test passed anyway

On x86-64 loads are acquire in practice, so the generated code happened to be
correct on the hardware this ran on. The defect is in what the C++ memory
model permits the compiler and a weaker architecture to do: reorder the load
of `read_` ahead of the consumer's copy. It would appear as occasional
corrupted samples on ARM, or after an optimiser change, and it would be
blamed on anything but memory ordering.

## Fix

`write_space()` no longer borrows `read_space()`. It loads `read_` with
acquire and `write_` with relaxed, which is the producer's correct pairing.
Each function now says in a comment which thread it is for and why its
orderings are that way round.

## What this says about the harness

This is the first defect found by something other than running the program.
Fifteen before it were found by compiling, by building the image, by
inspecting the ISO, or by recording audio. None of those would ever have found
this one: the program works.

The sanitizer job was added speculatively in the first commit, before anything
had been compiled. It paid for itself on its first run.

The lesson for the review gate is the one already written into
`.claude/agents/code-reviewer.md`: "a relaxed load that another thread depends
on for ordering is a bug that appears only on hardware you do not own." That
was the right instinct and it still did not catch this by reading. Keep TSan
in CI and do not let it be disabled when it is inconvenient.

## Secondary issue

Running TSan in a container aborted with "FATAL: ThreadSanitizer: unexpected
memory mapping" before any test ran. That is ASLR randomising more address
bits than TSan's shadow mapping expects, not a defect in our code. CI now sets
`vm.mmap_rnd_bits=28`; locally, `setarch -R` does the same. Worth knowing,
because it looks exactly like a test failure.
