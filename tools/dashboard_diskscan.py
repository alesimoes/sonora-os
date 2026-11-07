"""Where the repo's disk goes: the agent worktrees (count, total, biggest)
and the build-* directories. Walking worktrees is slow, so this runs in a
background thread (dashboard_bg) with a time budget; a walk that runs out of
time reports what it measured and says it is partial."""
import os
import time

BUDGET_S = 240
TOP = 5


def dir_size(path, deadline):
    """(bytes, finished) of a tree, without following links."""
    total, stack = 0, [path]
    while stack:
        if time.time() > deadline:
            return total, False
        try:
            with os.scandir(stack.pop()) as it:
                for e in it:
                    try:
                        if e.is_symlink():
                            continue
                        if e.is_dir(follow_symlinks=False):
                            stack.append(e.path)
                        else:
                            total += e.stat(follow_symlinks=False).st_size
                    except OSError:
                        pass
        except OSError:
            pass
    return total, True


def _children(path, prefix=""):
    try:
        return sorted(os.path.join(path, n) for n in os.listdir(path)
                      if n.startswith(prefix) and os.path.isdir(os.path.join(path, n)))
    except OSError:
        return []


def scan_repo(root, budget=BUDGET_S):
    deadline = time.time() + budget
    complete = True
    sized = []
    for kind, paths in (("worktree", _children(os.path.join(root, ".claude", "worktrees"))),
                        ("build", _children(root, "build"))):
        for p in paths:
            size, done = dir_size(p, deadline)
            complete = complete and done
            sized.append({"kind": kind, "name": os.path.basename(p), "size": size})
    wts = sorted((x for x in sized if x["kind"] == "worktree"), key=lambda x: -x["size"])
    builds = sorted((x for x in sized if x["kind"] == "build"), key=lambda x: -x["size"])
    return {"ok": True, "partial": not complete, "scanned_at": int(time.time()),
            "worktrees": {"count": len(wts), "size": sum(x["size"] for x in wts), "top": wts[:TOP]},
            "builds": {"count": len(builds), "size": sum(x["size"] for x in builds), "top": builds[:TOP]}}
