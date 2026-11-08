# Context: engine and real-time

Local CI context: `sonora-engine`. Agents: rt-engineer, rt-safety-auditor.

## Where
- `engine/core` (audio core, `Session::process()`), `engine/control`,
  `engine/app`, `engine/plugin_client`, `drivers/{alsa,jack,fake,support,surfaces}`.
- `common/audiofile` (BWF), `common/platform`, `common/base`, `common/json`.
- Contracts: `contracts/{api,backend,plugin,surfaces}`; lock `contracts/CONTRACTS.lock`.

## Bound by
- ADR-0007: modules, DAG, RT boundary by module, contract change rules.
- ADR-0009: immutable edit state, undo = pointer, plan swap, takes close in-band.
- ADR-0012: plugin hosting; audio thread never waits.
- ADR-0016: loop-record take model. ADR-0022/0023: sends, track structure.
- ADR-0063: the graph by levels, help-first workers (`graph_workers`, default
  0 = serial), drops ramped and counted. A worker abandoned at a deadline
  may run blocks later: nothing writes its node's index, buffer or track until
  it returns (`graph_session.cpp` fence); test late workers through
  `SessionTestAccess::claim`, never with time.
- ADR-0008: neutral modules are standard C++17 only.
- RT rules: `CLAUDE.md` "Rules for the real-time path".

## Build and test
- Module loop: `ctest -L core|control|app|audiofile|drivers|plugin_client`
  (labels from `sonora_label_module_tests`, root `CMakeLists.txt`).
- Gate: the micro build (`tools/micro-build.sh`, ADR-0061): the targets the
  diff reaches and their tests, then RTSan on them. TSan, record, BWF and
  Windows are the nightly on main (`.github/workflows/nightly.yml`).
- Narrow build: `tools/build-slot.sh tools/build-engine.sh`.

## Traps
- A short write is an overrun: count it, never block.
- Protocol, session format and ADR numbers are reserved by the coordinator.
- Timing tests carry label `timing` and run serial.
- Loop/bounce tests under TSan are timing-sensitive: wait on state, not time.
