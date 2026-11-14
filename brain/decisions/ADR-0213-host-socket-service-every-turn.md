# ADR-0213 - A host socket is watched only while its holder is serviced every turn

- Status: Accepted
- Date: 2026-11-14
- Item: `plugins.host.oop` (follow-ups: editor gestures by POLLIN, the retiring-chain WaitSet spin)
- Relates to: ADR-0012 (plugin hosting, section 4 and M3 on #78), ADR-0064 (`plugins.host.oop` closed), PR #201 (retiring chains serviced while they wait for their epoch)

## Context

The engine's control thread sleeps on a `platform::WaitSet` between turns
(`EngineControlServer::run`): 50 ms when idle, 2 ms while playing. Since M3 on
#78 it lists, per plug-in host, the host's exit handle (pidfd) and its control
socket for POLLOUT while frames wait to be sent. Two lines stayed open under
`plugins.host.oop` and share one cause:

1. **Gestures wait for the turn.** The socket was never listed for reading,
   so a knob grabbed in a plug-in's own editor (`GestureBegin`,
   `ParamTouched`) reached the document only at the next 50 ms turn when the
   transport was idle.
2. **A holder not serviced spins the WaitSet.** Readiness is level-triggered
   (`wait_set.h`). A chain in `PluginHosting::retiring_`, awaiting its epoch,
   was not pumped, yet `HostSupervisor::watch` listed its host Writable while
   `wants_write()`; POLLOUT ended every sleep at once and the control thread
   spun (the #198 review). PR #201 fixed that instance by calling
   `SupervisedChain::poll_parked()` for every retiring chain each turn
   (scenario `control_parked`). Listing sockets for reading as well, as
   item 1 needs, widens the same trap to every host whose datagrams nobody
   reads.

Checked against `origin/main` at 823e82dd: #201 is there; nothing listed a
socket for reading.

## Decision

**A socket is in the WaitSet only if the holder that drains it is serviced
on the turn after a wake.** Concretely:

- Every holder of a `HostProcess` pumps it every turn: a chain's `poll()`
  (bound, retiring-after-restart and starting hosts), `poll_parked()` for
  `PluginHosting::retiring_`, `stopped()` for `stopping_` and for the hosts a
  chain let go (`reap_let_go`), and `HostSupervisor::reap()` for the hosts the
  supervisor adopted. A new holder of a host either pumps it every turn or
  the host is not listed.
- `HostProcess::watch` lists the open socket **Readable** (a gesture ends the
  sleep; the turn's `pump()` reads it and `take_messages()` hands it to the
  document in that same turn), and **Writable** only while frames wait.
- A socket that is closed, or whose host broke the contract, is not listed
  (nobody pumps it any more).
- A host **signalled to die** (`signal_kill`, a fault, a stop grace that
  passed) is not listed Readable: a down chain no longer pumps its host, the
  exit handle is the wake that matters, and what a dying host left unread
  must not end every sleep. `kill_signalled_` records it.

Control thread only. Nothing on the audio thread changes.

## Rationale

- Level-triggered readiness makes "listed" a promise that the next turn
  consumes the readiness; the only way to keep that promise is to tie the
  listing to the servicing, not to the state of the socket alone.
- Pumping every holder each turn (the #201 choice) is cheaper and simpler
  than filtering the supervisor's registry by holder: the registry has no
  notion of who owns a host, and a filter would need one more piece of
  shared state that could drift from the real owners.
- The kill exception covers the one holder that deliberately stops pumping
  (`SupervisedChain::poll` once `down_`), without adding a pump to a path
  that has nothing left to do with what the host says.

## Consequences

- An editor gesture is serviced in the turn it arrives instead of up to
  50 ms later, idle or playing.
- A chatty host now costs control-thread wakes in proportion to what it
  sends. That is bounded by the M2 rate limits (touches per parameter,
  GestureEnds per second, a fault past them) and by the per-pump drain
  budget (4 x `kMaxDatagramsPerDrain`), and each wake does real work; it is
  progress, not a spin.
- Tests: `plugin_client_host_control_watch` now expects the socket listed
  for reading when idle; `test_plugin_client_host_watch.cpp` adds
  `gesture` (a gesture ends the sleep and the following turn takes it; a
  quiet or drained socket does not) and `parked_unread` (a parked chain
  with unread datagrams and a pending send on a full socket wakes once,
  then sleeps; a killed host's socket is not listed for reading). Wakes are
  counted per turn, never timed.
- Still open under `plugins.host.oop`, unchanged here: the L6 pid-namespace
  check (waits on the sandbox decision) and the M4 slot `Reconfigure`
  `RtCommand` and `EngineChannel` helpers (contract-side items).

## Alternatives rejected

- **List only hosts whose holder was serviced this turn** (a per-turn
  serviced mark checked in `watch()`): correct, but it adds turn identity to
  `HostProcess` and makes a forgotten pump silently slow instead of visibly
  wrong in the tests; pumping every holder already holds the rule.
- **Keep reading on the 50 ms turn**: leaves the editor gesture late, and
  the 2 ms busy cadence only while playing does not cover the idle case
  where an engineer sets a plug-in up.
- **Edge-triggered polling (EPOLLET)**: would hide the spin rather than
  remove its cause, and loses a wake when a drain stops at its budget with
  datagrams left; `WaitSet` is level-triggered by contract.
