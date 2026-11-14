"""tools/build-job.sh: a lock script killed from outside (the supervisor's
taskkill, a tool call's tree kill) is never reported as exit 0
(2026-11-14: waiters "finished: exit 0" with only the lock-wait line)."""
import os
import shutil
import subprocess
import tempfile
import time
import unittest

import support  # noqa: F401
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BASH = shutil.which("bash")


def _run(env, *args, timeout=60):
    return subprocess.run([BASH, str(ROOT / "tools/build-job.sh"), *args], env=env,
                          capture_output=True, text=True, timeout=timeout)


@unittest.skipUnless(BASH, "needs bash")
class KilledRunner(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="bj-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.env = dict(os.environ, SONORA_HEAVY_DIR=f"{self.tmp}/lock",
                        SONORA_BUILD_JOBS_DIR=f"{self.tmp}/jobs",
                        SONORA_HEAVY_POLL="1", SONORA_BUILD_WAIT_POLL="1")
        self.env.pop("SONORA_HEAVY_HELD", None)

    def _wait(self):
        return _run(self.env, "wait", "--max", "30")

    def test_exit_status_of_the_command_is_kept(self):
        _run(self.env, "start", "sh", "-c", "exit 3")
        r = self._wait()
        self.assertEqual(r.returncode, 3, r.stdout)

    def test_killed_lock_script_is_not_exit_0(self):
        slot = Path(self.tmp, "lock/slot-0")
        slot.mkdir(parents=True)
        # A live holder: this test's own pid, so the waiter keeps waiting.
        (slot / "owner").write_text(
            f"pid={os.getpid()}\nkind=linux\nstart={int(time.time())}\n")
        _run(self.env, "start", "echo", "hi")
        waiters = Path(self.tmp, "lock/wait")
        for _ in range(60):
            if waiters.is_dir() and any("cmd=" in t.read_text() and "pid=" in t.read_text()
                                        for t in waiters.iterdir()):
                break
            time.sleep(0.5)
        ticket = next(waiters.iterdir()).read_text()
        pid = int(ticket.split("pid=")[1].split()[0])
        if os.name == "nt":
            # What the supervisor does; MSYS `kill -9` is reported correctly.
            win = next(l for l in ticket.splitlines()
                       if l.startswith("winpid=")).split("=")[1]
            subprocess.run(["taskkill", "/F", "/PID", win], capture_output=True)
        else:
            subprocess.run(["kill", "-KILL", str(pid)])
        r = self._wait()
        self.assertNotEqual(r.returncode, 0, r.stdout)


if __name__ == "__main__":
    unittest.main()
