# Context: model and contracts

Local CI context: `sonora-engine` (runs the whole non-slow suite).
Agents: architect (shape), rt-engineer (model), owner engineer per contract.

## Where
- `common/model` (`include/sonora/model/`: `model.h`, `operations.h`,
  `op_codec.h`; src; tests), `common/session_files`, `common/json`.
- `contracts/{api,backend,plugin,surfaces}`, `contracts/CONTRACTS.lock`
  (`tools/contracts-lock.sh`).

## Bound by
- ADR-0007: contract change rules (a contract is a small PR first, with ADR,
  fake and test); changing a merged contract is an amendment from its owner.
- ADR-0009: operations are a pure `apply`, undo = pointer, canonical JSON.
- ADR-0023: track structure; session format and protocol numbers are
  reserved by the coordinator.

## Build and test
- Module labels as in `engine-rt.md`; conformance tests in `contracts/*/tests`.
- Run `tools/contracts-lock.sh` after any header change under `contracts/`.

## Traps
- `model.h` is 1193 lines and `operations.h` 809: do not append, add new
  headers. `Operation` is a `std::variant` (operations.h ~line 652) with a
  `static_assert` on the member count (~703): every includer pays compile
  time; two model files hung the ASan build for 40 minutes.
- Public include paths stay valid when splitting (forwarding headers).
