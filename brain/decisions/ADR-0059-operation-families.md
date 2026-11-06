# ADR-0059: Operations as families, a two-level variant

Status: Accepted (principal engineer, 2026-11-06; owner's order: no sanitizer check is switched off to make a build pass).
Builds on: ADR-0009 §2 (operations and their codec), ADR-0007 §1 (common/model is shared by the engine and the UI).
Supersedes: PR #453's `-fno-sanitize=alignment` on common/model and the conformance target (closed, never merged).

## Summary

`Operation` and `Member` are no longer one `std::variant` of 62 (61) alternatives. Each is a small class over a `std::variant` of **families**, each family a `std::variant` of at most nine operations, grouped by domain. `apply.cpp` and `op_codec.cpp` are split into one file per family. The wire encoding, the undo window and the session file are byte-identical: an operation is named on the wire by its kind, never by an index.

## Context

Under GCC 14 with `-fsanitize=address,undefined -O2 -g` (the ASan step of `tools/local-ci.sh`), libstdc++ stores alternative N of a variant N `_Variadic_union` levels deep, and UBSan puts a null and an alignment check on `this` at every level. Every visit, copy, move and assignment instantiates code per alternative, each with up to N levels: O(N²) per operation on the variant, with N = 62. Measured before this change: `op_codec.cpp` and `apply.cpp` over 40 minutes each, `test_compound.cpp` over 39, `contracts/api/conformance/src/conformance.cpp` over 10; one function in `op_codec.o` called `__ubsan_handle_type_mismatch_v1` 2,141 times. With `-fno-sanitize=alignment` they took 1.5 to 3 minutes, which is how PR #453 got a green build, by not checking alignment in the code that holds every edit.

The defect is in our code's shape, not in the sanitizer: one variant that every new operation lengthens.

## Decision

1. **Nine families** (`operations.h`): `SessionOp` (sources, monitor bus, settings, markers), `TrackOp`, `RegionOp` (what places, moves, cuts or records audio regions), `ClipOp` (a region's attributes, fades and crossfades), `PlaylistOp`, `InsertOp`, `MixOp` (faders, buses, sends, outputs, automation), `GroupOp` (groups, VCAs, folders), `MidiOp`. A family holds at most about a dozen operations; a new operation joins the family of its domain, and a family that outgrows a dozen is split.
2. **`Member`** is `detail::OpVariant<SessionOp, …, MidiOp>`; **`Operation`** is the same families then `Compound` (`op_variant.h`). Both convert implicitly from any leaf (`Operation op = AddTrack{…}`, `Compound{{SetMix{…}, …}}` read as before).
3. **Access** goes through the family: `get_if<T>(&op)`, `get<T>(op)`, `holds_alternative<T>(op)` and `visit(f, op)` in `sonora::model` replace their `std::` namesakes, which no longer compile on an operation (so no caller kept the flat meaning by accident). `op.index()` is the leaf's flat index in family order and `Operation::kinds` the number of leaves (what `std::variant_size_v` was). `families()` gives the outer variant to code that dispatches per family.
4. **Files by family.** `apply.cpp` keeps the compound fold and the public functions; the `Applier` is declared in `src/apply_detail.h` and each family's `operator()`s live in `src/apply_<family>.cpp`. The codec's shared writers and readers are `src/op_codec_{write,read}.cpp` behind `src/op_codec_detail.h`; each family's encoder and decoder is `src/op_codec_<family>.cpp`; the undo window is `src/op_codec_window_{write,read}.cpp`. No translation unit visits more than one short variant leaf by leaf.
5. **No format change.** The wire names each operation by its `"op"` kind, and the window and the session file hold no operation index, so neither `kProtocolVersion`, `kWindowFormat` nor `kFormatVersion` moves. `common/model/tests/fixtures/operations.bytes` holds the bytes the flat variant wrote for one of each operation (written by origin/main at this change); `model_replication` checks the new code writes exactly those and reads them back.

## Rationale

Two levels cap the depth of every variant at the family's size: the code a visit or a copy costs is the sum of the families' squares plus the outer one's, about a tenth of 62², and it grows linearly as families are added. Splitting the files by family follows the same line and the owner's convention (modular code by domain, files under about 500 lines), and lets the build spread the work across units.

## Consequences

- Callers outside common/model changed only where they used `std::get_if`, `std::get` or `std::holds_alternative` on an operation (engine/control, two tests, one plug-in test). Constructing operations, vectors of members and compounds is unchanged.
- `Operation::index()` is no longer the position in a declaration list, so a test that relied on that order asks `variant_index<T>()` instead.
- The kind's name table (`op_codec.cpp`, `kNames`) is in family order; the frozen-bytes test fails if a name and a kind disagree.
- `kWindowFormat` moved to `src/op_codec_detail.h`; `tools/harness/numbers.py` reads it there.

## Alternatives rejected

1. **Turn off a sanitizer check for these units** (PR #453): hides what the check is for, in the code that holds every edit; the owner forbids it.
2. **A hand-written flat tagged union** (aligned storage, an index and a function table): linear code with no grouping, but our own copy, move and destruction of 62 types to get right and keep right, where `std::variant` already is; and it leaves the 3,000-line files.
3. **Type erasure** (`std::unique_ptr<OpBase>` with virtual apply/encode): one allocation per operation, and the visitors (`apply`, the codec, the UI's checks) turn into virtual methods on the plain value types, which ADR-0009 keeps as data.

## Measurements

Per translation unit, the ASan step's flags (`-fsanitize=address,undefined -fno-omit-frame-pointer -g`, RelWithDebInfo, GCC 14, sonora-build:trixie), two at a time under the machine lock, every check on:

| unit | before (flat variant) | after |
|---|---|---|
| `op_codec.cpp` (now 15 files) | over 40 min | 22 s, every piece 7 to 41 s |
| `apply.cpp` (now 12 files) | over 40 min | 59 s, every piece 9 to 59 s |
| `tests/test_compound.cpp` | over 39 min | 84 s |
| `tests/test_replication.cpp` | not measured | 88 s |
| `contracts/api/conformance/src/conformance.cpp` | over 10 min | 155 s |
| `contracts/api/testing/src/fake_engine.cpp` | 166 s | 107 s |
| `engine/control/src/engine_control_server.cpp` | 206 s | 119 s |
