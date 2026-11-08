"""Stopping a detached process and everything it started (an agent run, a
docker client): the dashboard's run.stop and the supervisor use it. Best
effort: the process may already be gone."""
import os
import signal
import subprocess
import sys


def kill_tree(pid):
    if not pid or int(pid) <= 1 or int(pid) == os.getpid():
        return   # never init or ourselves (see launcher.kill_tree)
    try:
        if sys.platform == "win32":
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)],
                           capture_output=True, timeout=30)
        else:
            pg = os.getpgid(pid)
            if pg <= 1 or pg == os.getpgrp():
                os.kill(pid, signal.SIGTERM)
            else:
                os.killpg(pg, signal.SIGTERM)
    except (OSError, subprocess.SubprocessError):
        pass
