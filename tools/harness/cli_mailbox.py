"""CLI commands that change the store, run by the daemon (2026-11-07).

While a daemon runs (harness/daemonlock.py), `orchestrator.py add`,
`import-queue` and `bundle` do not open a second Orchestrator on
state.json: they drop a request under .harness/cli-requests/ and the
daemon's next tick applies it, so state.json keeps one writer. `adopt` has
had the same door since #461 (harness/autoadopt.py). The result of each
request is an event (`cli_request` or `cli_request_failed`).
"""
import json
import os
import time

from . import dash_door
from .status import write_atomic

REQUEST_DIR = "cli-requests"
KINDS = ("add", "import-queue", "bundle", "regate", "assign")


def request_dir(hdir):
    return os.path.join(hdir, REQUEST_DIR)


def post(hdir, kind, **args):
    """Queue one request for the daemon. Returns the file."""
    if kind not in KINDS:
        raise ValueError(f"unknown request {kind}")
    d = request_dir(hdir)
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, f"{time.time_ns()}-{os.getpid()}.json")
    write_atomic(p, {"kind": kind, "args": args})
    return p


def take(hdir):
    """[(kind, args)] of the pending requests, oldest first; files removed."""
    d = request_dir(hdir)
    try:
        names = sorted(n for n in os.listdir(d) if n.endswith(".json"))
    except OSError:
        return []
    out = []
    for n in names:
        p = os.path.join(d, n)
        try:
            with open(p, encoding="utf-8") as f:
                req = json.load(f)
            os.remove(p)
        except (OSError, ValueError):
            continue    # half-written: the next tick sees it whole
        if isinstance(req, dict) and req.get("kind") in KINDS:
            out.append((req["kind"], req.get("args") or {}))
    return out


def apply(orch, kind, args):
    """Runs one request on the daemon's own store; returns what it did."""
    if kind in dash_door.KINDS:
        return dash_door.apply(orch, kind, args)
    if kind == "add":
        orch.store.add_task(
            args["id"], args["track"], args["agent"],
            files=args.get("files") or [], brief=args.get("brief") or "",
            depends_on=args.get("depends_on") or [],
            wants=args.get("wants") or [])
        return f"task {args['id']}"
    if kind == "import-queue":
        made = orch.flow.import_queue(args["text"],
                                      wants=args.get("wants") or [])
        return f"imported {len(made)}: {', '.join(made)}"
    return "bundle " + orch.train.bundle(args["name"], args["prs"])


def tick(orch):
    for kind, args in take(orch.hdir):
        try:
            done = apply(orch, kind, args)
        except Exception as e:   # a bad request must not stop the tick
            orch.store.event(None, "cli_request_failed",
                             f"{kind}: {str(e)[:200]}")
            continue
        orch.store.event(None, "cli_request", f"{kind}: {done[:200]}")
