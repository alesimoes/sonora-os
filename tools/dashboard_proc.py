"""The one place the dashboard's actions start a process or look at one, so a
test replaces these functions and no real command runs."""
import os
import subprocess
import sys

TAIL = 4000


def run_cmd(cmd, timeout, cwd=None):
    """(returncode, merged output tail). 124 when the timeout hit, 127 when the
    program is missing; never raises."""
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=timeout)
        return r.returncode, ((r.stdout or "") + (r.stderr or ""))[-TAIL:]
    except subprocess.TimeoutExpired:
        return 124, "timeout after %ss" % timeout
    except OSError as e:
        return 127, str(e)


def spawn_detached(cmd, cwd, logfile):
    """Starts a process that outlives the dashboard; returns its pid."""
    os.makedirs(os.path.dirname(logfile), exist_ok=True)
    kw = {}
    if sys.platform == "win32":
        kw["creationflags"] = 0x00000008 | 0x00000200 | 0x08000000   # DETACHED | NEW_GROUP | NO_WINDOW
    else:
        kw["start_new_session"] = True
    with open(logfile, "ab") as out:
        p = subprocess.Popen(cmd, cwd=cwd, stdin=subprocess.DEVNULL, stdout=out, stderr=out, **kw)
    return p.pid


def find_processes(needles):
    """[(pid, command line)] of processes whose command line holds every
    string of `needles`; the dashboard's own process is left out."""
    if sys.platform == "win32":
        cmd = ["powershell", "-NoProfile", "-Command",
               "Get-CimInstance Win32_Process | ForEach-Object { \"$($_.ProcessId)`t$($_.CommandLine)\" }"]
    else:
        cmd = ["ps", "-eo", "pid=,args="]
    _, out = run_cmd(cmd, 30)
    rows = []
    for ln in out.splitlines():
        pid, _, args = ln.strip().replace("\t", " ").partition(" ")
        if pid.isdigit() and int(pid) != os.getpid() and all(n in args for n in needles):
            rows.append((int(pid), args.strip()))
    return rows
