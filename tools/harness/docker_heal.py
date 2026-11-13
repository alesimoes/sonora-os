"""Supervisor check: Docker down is told and, on Windows, healed a bounded
number of times (2026-11-13: Docker Desktop died at 02:23 and the whole
cycle stood 3 h 40 with zero agents until the coordinator restarted it by
hand; status.json said only "paused").

While the guard (harness/docker_guard.py) sees Docker down:
  - one alert, `docker:down`, naming the held pools and how long; a warning
    until `docker_restart_after_s`, then an alert (one desktop toast, as
    every alert);
  - after `docker_restart_after_s` of outage, Docker Desktop is restarted
    the way that works by hand (restart_docker_desktop), at most
    `docker_restart_max` times per outage and per rolling hour, at least
    `docker_restart_gap_s` apart. Each attempt is a supervisor event.
  - when the attempts are spent the alert says so and nothing more is
    tried until Docker answers again: no infinite loop (owner, 2026-11-09).
The sys surface is docker_restart() -> bool | None (None: not supported on
this host); a sys without it (old fakes, Linux) only alerts.
"""
import os
import subprocess
import time

DOCKER_EXE = r"C:\Program Files\Docker\Docker\Docker Desktop.exe"
# Every process the Desktop runs; a half-dead backend keeps the new one
# from starting if it is left behind.
DOCKER_PROCS = ("Docker Desktop", "com.docker.backend", "com.docker.proxy",
                "com.docker.extensions", "com.docker.dev-envs", "docker")
STEP_TIMEOUT_S = 60
HOUR_S = 3600.0
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def restart_docker_desktop(run=subprocess.run, popen=subprocess.Popen,
                           exe=DOCKER_EXE, is_windows=None):
    """Stops Docker Desktop and its WSL VM, then starts it again. Does not
    wait for the engine (~30 s): the guard's next probe sees it. None when
    the host is not Windows or Docker Desktop is not installed."""
    win = os.name == "nt" if is_windows is None else is_windows
    if not win or not os.path.exists(exe):
        return None
    for name in DOCKER_PROCS:
        _quiet(run, ["taskkill", "/F", "/T", "/IM", name + ".exe"])
    _quiet(run, ["wsl", "--shutdown"])
    popen([exe], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
          creationflags=0x00000008 | 0x00000200)   # detached, own group
    return True


def _quiet(run, cmd):
    """A process that is not running is not an error here."""
    try:
        run(cmd, capture_output=True, timeout=STEP_TIMEOUT_S,
            creationflags=NO_WINDOW)
    except (OSError, subprocess.SubprocessError):
        pass


def _mins(s):
    return f"{int(max(s, 0) // 60)} min"


def _clock(ts):
    return time.strftime("%H:%M", time.localtime(ts))


def check_docker(sv, now):
    guard = getattr(sv.o, "docker", None)
    since = getattr(guard, "down_since", None)
    mem = sv.mem.setdefault("docker", {"since": None, "outage": [],
                                       "hour": []})
    mem["hour"] = [t for t in mem["hour"] if now - t < HOUR_S]
    if since != mem["since"]:   # a new outage gets its own attempts
        mem["since"], mem["outage"] = since, []
    if since is None:
        return
    down = now - since
    pools = ", ".join(sorted(guard.status()["docker_held"]))
    text = (f"Docker down since {_clock(since)} ({_mins(down)}): pools "
            f"{pools} held, no build or test can start")
    th = sv.th
    if down < th.docker_restart_after_s:
        sv.alert("docker:down", text, level="warn", check="docker")
        return
    restart = getattr(sv.sys, "docker_restart", None)
    if not th.docker_restart or restart is None:
        sv.alert("docker:down", text + "; restart it by hand", check="docker")
        return
    tried = max(len(mem["outage"]), len(mem["hour"]))
    if tried >= th.docker_restart_max:
        sv.alert("docker:down", text + f"; {tried} restart(s) "
                 "of Docker Desktop did not bring it back, no more tries "
                 "until it answers: restart it by hand", check="docker")
        if mem.get("gave_up") != since:
            mem["gave_up"] = since
            sv.act("docker", "stop restarting docker desktop",
                   f"{tried} attempt(s) spent, down {_mins(down)}")
        return
    sv.alert("docker:down", text + f"; restart attempts "
             f"{len(mem['outage'])}/{th.docker_restart_max:g}", check="docker")
    last = max(mem["outage"] or [-HOUR_S])
    if now - last < th.docker_restart_gap_s:
        return
    mem["outage"].append(now)
    mem["hour"].append(now)
    n = len(mem["outage"])
    sv.act("docker", f"restart docker desktop ({n}/"
           f"{th.docker_restart_max:g})",
           f"docker info failing for {_mins(down)}",
           lambda: restart() is not None)
