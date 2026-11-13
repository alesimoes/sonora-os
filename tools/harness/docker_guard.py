"""No launches that need Docker while Docker is down (owner, 2026-11-09).

Docker Desktop stopped; the daemon kept launching engineers, testers, fixers
and integrators whose builds died at once, then relaunched them: a loop that
spent runs for nothing ("não pode ter esse loop infinito se não conseguir
iniciar o docker"). The guard probes `docker info` at most once a minute;
while it fails, the pools whose runs build or test are held and one event
says so. When Docker answers again they resume by themselves.

The hold is reported as its own cause (2026-11-13: status.json listed the
held pools under `paused` and the gap read "paused", the same words as an
owner pause, while Docker stood down 3 h 40 with nobody told). status.json
carries `docker_down_since` and `docker_held`, each held pool row gets
`docker_down`, and the scheduler's gap reads "docker down". Restarting
Docker is the supervisor's bounded job (harness/docker_heal.py), not the
guard's.
"""
import subprocess

PROBE_EVERY_S = 60.0
PROBE_TIMEOUT_S = 15.0
# Pools whose runs build or test in containers (scheduler.POOL_OF_KIND).
DOCKER_POOLS = frozenset({"build", "qa", "fix", "merge"})


def docker_up(timeout=PROBE_TIMEOUT_S):
    try:
        r = subprocess.run(["docker", "info", "--format", "{{.ServerVersion}}"],
                           capture_output=True, timeout=timeout,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return r.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


class Guard:
    def __init__(self, store, probe=docker_up):
        self.store, self.probe = store, probe
        self.up, self.checked = True, None
        self.down_since = None   # first failed probe of the current outage

    def held(self, now):
        """The pools to hold now: DOCKER_POOLS while Docker is down."""
        if self.checked is None or now - self.checked >= PROBE_EVERY_S:
            self.checked = now
            up = bool(self.probe())
            if up != self.up:
                self.store.event("daemon", "docker_up" if up else "docker_down",
                                 "builds resume" if up else
                                 "engineer, tester, fixer and integrator launches held "
                                 "until Docker answers")
            if not up and self.down_since is None:
                self.down_since = now
            if up:
                self.down_since = None
            self.up = up
        return frozenset() if self.up else DOCKER_POOLS

    def status(self, pools=None):
        """status.json fields; marks the held rows of `pools` in place."""
        held = sorted(DOCKER_POOLS) if not self.up else []
        for name, row in (pools or {}).items():
            row["docker_down"] = name.split(":")[0] in held
        return {"docker_down_since": self.down_since, "docker_held": held}
