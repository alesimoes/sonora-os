"""Stopping a detached process and everything it started (an agent run, a
docker client): the dashboard's run.stop and the supervisor use it. Best
effort: the process may already be gone."""
import os
import signal
import subprocess
import sys


def kill_tree(pid):
    if not pid:
        return
    try:
        if sys.platform == "win32":
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)],
                           capture_output=True, timeout=30)
        else:
            os.killpg(pid, signal.SIGTERM)
    except (OSError, subprocess.SubprocessError):
        pass
