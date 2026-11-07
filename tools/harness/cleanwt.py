"""Clean a gate checkout whose build trees a container or a late-exiting
process may still be writing into.

`git clean` exits non-zero ("Directory not empty") when a file appears
between its listing and its rmdir; a second pass finds the tree stable.
Only after the retries is the failure reported, so a real error still
stops the run.
"""
import time

RETRIES = 4
PAUSE_S = 2.0


def clean(run_git, wt, keep, sleep=time.sleep):
    """run_git(args, cwd) -> CompletedProcess. Returns the last result."""
    args = ["clean", "-fdxq"] + [x for k in keep for x in ("-e", k)]
    r = run_git(args, wt)
    for _ in range(RETRIES):
        if not r.returncode:
            break
        sleep(PAUSE_S)
        r = run_git(args, wt)
    return r
