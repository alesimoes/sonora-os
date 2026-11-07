"""What the dashboard reads from the machine: merge-when-green watchers
(ps), running containers (docker) and the machine-wide heavy lock
(tools/build-slot.sh). Read-only; every reader returns empty on failure."""
import os
import re
import shutil
import subprocess
import time

from dashboard_util import ROOT, norm, orch_root, run

def read_watchers():
    rows = []
    for line in run(["ps", "-ef"]).splitlines():
        m = re.search(r"(?:^|[\s/])sh\s+(?:\./)?tools/merge-when-green\.sh\s+(\d+)", line)
        if m:
            parts = line.split()
            rows.append({"pr": int(m.group(1)), "pid": parts[1], "since": parts[4]})
    seen, uniq = set(), []
    for r in rows:
        if (r["pr"], r["pid"]) not in seen:
            seen.add((r["pr"], r["pid"]))
            uniq.append(r)
    return sorted(uniq, key=lambda r: r["pr"])


def read_containers():
    ids = run(["docker", "ps", "-q"]).split()
    if not ids:
        return []
    fmt = "{{.Id}}|{{.Config.Image}}|{{.State.StartedAt}}|{{range .Mounts}}{{.Source}};{{end}}"
    rows = []
    for line in run(["docker", "inspect", "--format", fmt] + ids).splitlines():
        cid, image, started, mounts = (line.split("|") + ["", "", "", ""])[:4]
        rows.append({"id": cid[:12], "image": image, "started": started,
                     "mounts": [norm(m) for m in mounts.split(";") if m]})
    return rows


# --- the machine-wide heavy lock -------------------------------------------
LOCK_RE = re.compile(r"^(holds|waits)\s+pid=(\d+)\s+kind=(\S+)\s+user=(\S+)\s+prio=(\d+)\s+start=(\d+)\s+cwd=(\S*)\s+cmd=(.*)$")
_lock_cache = {"t": 0.0, "rows": []}


def read_lock(labels):
    """Parsed `build-slot.sh --list`, cached a few seconds: the holder first,
    then the waiters in queue order, each with who owns it."""
    if time.time() - _lock_cache["t"] > 6:
        rows = []
        for ln in heavy_lock_list():
            m = LOCK_RE.match(ln.strip())
            if m:
                rows.append({"role": m.group(1), "pid": m.group(2), "prio": int(m.group(5)), "start": int(m.group(6)),
                             "cwd": m.group(7), "cmd": m.group(8).strip()})
        _lock_cache.update(t=time.time(), rows=rows)
    rows = _lock_cache["rows"]
    for r in rows:
        low = r["cwd"].lower()
        who = None
        for aid, meta in labels.items():
            d = (meta.get("dir") or ("agent-" + meta.get("worktree", aid))).lower()
            if meta.get("status") not in ("done", "paused") and "/worktrees/" + d in low:
                who = meta.get("label", aid)
        if not who and "/.harness/ci" in low:
            who = "Builder do orquestrador (CI local)"
        if not who:
            who = os.path.basename(r["cwd"].rstrip("/")) or "?"
        r["who"] = who
        r["builder"] = "/.harness/ci" in low
    return rows


def _find_slot_script():
    # The lock script lives with the harness branch, which may not be the one
    # this checkout is on: use the first worktree that has it.
    for base in (ROOT, os.path.join(ROOT, ".claude", "worktrees", "harness-pr"), orch_root()):
        f = os.path.join(base, "tools", "build-slot.sh")
        if os.path.exists(f):
            return f
    return os.path.join(ROOT, "tools", "build-slot.sh")


GIT_BASH = next((b for b in (r"C:\Program Files\Git\bin\bash.exe", shutil.which("bash")) if b and os.path.exists(b)), None)
SLOT_SCRIPT = _find_slot_script()


def heavy_lock_list():
    """Who holds the machine-wide heavy lock and who waits (tools/build-slot.sh)."""
    try:
        out = subprocess.run([GIT_BASH or "bash", SLOT_SCRIPT, "--list"],
                             capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=10).stdout
        return out.strip().splitlines()[:20]
    except (OSError, subprocess.SubprocessError):
        return []
