"""Is a heavy-lock holder or waiter alive, and which pid may be killed for it.

tools/build-slot.sh writes `pid=$$`. Under Git bash that is an MSYS pid, a
number of its own that is not a Windows pid; the Windows supervisor took it
for one, found no such process and reaped live holders, so the next waiter
started a second build beside the first (2026-11-13: 430 holders and 195
waiters reaped as "pid dead", among them builds that went on to finish).
build-slot.sh now writes `winpid=` too; without it a Windows host cannot
probe the entry and leaves it to build-slot.sh's own reaper, which probes
MSYS pids from Git bash correctly. A holder whose docker run outlives its
shell (`container=`) is alive while that container runs.
"""


def probe_pid(entry, host_kind):
    """The pid this host can probe or kill for the entry, or None."""
    if entry.get("kind") != host_kind:
        return None
    pid = entry.get("winpid") if host_kind == "win" else entry.get("pid")
    return pid if isinstance(pid, int) and pid > 1 else None


def entry_alive(entry, host_kind, pid_alive, container_running):
    """True alive, False dead, None cannot tell (never reaped).

    container_running(name) -> True, False, or None when docker did not
    answer (a hung docker is a loaded machine, not a free one)."""
    pid = probe_pid(entry, host_kind)
    if pid is None:
        return None
    if pid_alive(pid):
        return True
    c = entry.get("container")
    if c and container_running(c) is not False:
        return True
    return False


def kill_entry(entry, host_kind, kill_tree, container_kill):
    """Kills what the entry left running; False when nothing could be
    reached (the slot is then kept: removing it would let a second build
    start beside one still running)."""
    did = False
    pid = probe_pid(entry, host_kind)
    if pid is not None:
        kill_tree(pid)
        did = True
    c = entry.get("container")
    if c:
        container_kill(c)
        did = True
    return did
