"""No launches that need Docker while Docker is down (owner, 2026-11-09).

Docker Desktop stopped; the daemon kept launching engineers, testers, fixers
and integrators whose builds died at once, then relaunched them: a loop that
spent runs for nothing ("não pode ter esse loop infinito se não conseguir
iniciar o docker"). The guard probes `docker info` at most once a minute;
while it fails, the pools whose runs build or test are paused and one event
says so. When Docker answers again they resume by themselves. The guard
never starts Docker: the owner or the coordinator does.
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

    def held(self, now):
        """The pools to pause now: DOCKER_POOLS while Docker is down."""
        if self.checked is None or now - self.checked >= PROBE_EVERY_S:
            self.checked = now
            up = bool(self.probe())
            if up != self.up:
                self.store.event("daemon", "docker_up" if up else "docker_down",
                                 "builds resume" if up else
                                 "engineer, tester, fixer and integrator launches paused "
                                 "until Docker answers")
            self.up = up
        return frozenset() if self.up else DOCKER_POOLS
