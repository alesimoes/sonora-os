"""The orchestrator's command line (split out of tools/orchestrator.py).

    orchestrator.py run [--live] [--once] [--fake-claude PATH]
    orchestrator.py status | drain | stop | resume
    orchestrator.py pause --pool NAME [--resume]
    orchestrator.py add --id ID --track daw --agent rt-engineer ...
    orchestrator.py adopt [PR...] | bundle NAME PR... | import-queue PATH
    orchestrator.py flakes          BACKLOG lines for the flaky tests seen

state.json has one writer (harness/daemonlock.py, 2026-11-07): a command
that changes the store goes to the running daemon's mailbox; with no
daemon it takes the lock itself, so a daemon starting meanwhile waits.
"""
import argparse
import json
import os
import sys

from . import autoadopt, cli_mailbox, daemonlock, launcher, localci_flakes
from . import status
from .brokerhost import NullBroker


def _parser(root_default):
    ap = argparse.ArgumentParser(prog="orchestrator.py")
    ap.add_argument("--root", default=root_default)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--live", action="store_true",
                   help="launch for real (default is dry-run)")
    r.add_argument("--fake-claude")
    r.add_argument("--once", action="store_true")
    r.add_argument("--exit-when-drained", action="store_true")
    r.add_argument("--no-broker", action="store_true")
    r.add_argument("--shadow", action="store_true",
                   help="dry run that logs differences from the live agents")
    r.add_argument("--shadow-agents",
                   help="JSON file of the agents the coordinator runs")
    r.add_argument("--no-self-update", action="store_true",
                   help="do not re-exec when main changes the harness")
    q = sub.add_parser("import-queue")
    q.add_argument("path")
    q.add_argument("--wants", default="adr")
    for c in ("status", "drain", "stop", "resume", "flakes"):
        sub.add_parser(c)
    p = sub.add_parser("pause")
    p.add_argument("--pool", required=True)
    p.add_argument("--resume", action="store_true")
    a = sub.add_parser("add")
    a.add_argument("--id", required=True)
    a.add_argument("--track", required=True)
    a.add_argument("--agent", required=True)
    a.add_argument("--files", default="")
    a.add_argument("--brief", default="")
    a.add_argument("--depends-on", default="")
    a.add_argument("--wants", default="")
    b = sub.add_parser("bundle", help="join open train PRs of one context "
                       "into one PR (a running daemon takes the request)")
    b.add_argument("name")
    b.add_argument("prs", nargs="+", type=int)
    t = sub.add_parser("adopt", help="put open PRs on the merge train "
                       "(a running daemon takes the request on its tick)")
    t.add_argument("prs", nargs="*", type=int,
                   help="PR numbers; none means every open PR")
    return ap


def _csv(s):
    return [x for x in s.split(",") if x]


def daemon_alive(root):
    return daemonlock.daemon_running(os.path.join(root, ".harness")) or \
        autoadopt.daemon_alive(os.path.join(root, "planning", "orchestrator"))


def _switches(args, odir):
    def touch(name):
        open(os.path.join(odir, name), "a").close()

    if args.cmd == "drain":
        touch("STOP")
    elif args.cmd == "stop":
        touch("STOP")
        touch("STOP.NOW")
    elif args.cmd == "resume":
        for n in ("STOP", "STOP.NOW"):
            try:
                os.remove(os.path.join(odir, n))
            except FileNotFoundError:
                pass
    elif args.cmd == "pause":
        path = os.path.join(odir, "paused.json")
        try:
            with open(path, encoding="utf-8") as f:
                cur = set(json.load(f))
        except (OSError, ValueError):
            cur = set()
        cur = cur - {args.pool} if args.resume else cur | {args.pool}
        status.write_atomic(path, sorted(cur))
    return 0


def _request(args):
    """(mailbox kind, args) of a store-changing command, for the daemon."""
    if args.cmd == "add":
        return "add", {"id": args.id, "track": args.track,
                       "agent": args.agent, "files": _csv(args.files),
                       "brief": args.brief,
                       "depends_on": _csv(args.depends_on),
                       "wants": _csv(args.wants)}
    if args.cmd == "import-queue":
        with open(args.path, encoding="utf-8") as f:
            return "import-queue", {"text": f.read(),
                                    "wants": _csv(args.wants)}
    return "bundle", {"name": args.name, "prs": args.prs}


def _post(root, args):
    hdir = os.path.join(root, ".harness")
    if args.cmd == "adopt":
        autoadopt.write_request(hdir, args.prs)
    else:
        kind, req = _request(args)
        cli_mailbox.post(hdir, kind, **req)
    print(f"{args.cmd}: daemon is running; queued for its next tick")
    return 0


def _direct(root, args, make_orch):
    """No daemon: this process is the writer, under the lock."""
    hdir = os.path.join(root, ".harness")
    try:
        daemonlock.acquire(hdir, "cli", wait_s=30)
    except daemonlock.Busy:
        return _post(root, args)   # a daemon started meanwhile
    try:
        o = make_orch(root)
        try:
            if args.cmd == "adopt":
                got = o.train.adopt(set(args.prs) or None)
                print("adopted: " + (" ".join(f"#{n}" for n in got)
                                     or "none"))
            elif args.cmd == "bundle":
                print("bundle: " + o.train.bundle(args.name, args.prs))
            else:
                kind, req = _request(args)
                print(cli_mailbox.apply(o, kind, req))
        finally:
            o.close()
    finally:
        daemonlock.release(hdir)
    return 0


def _run(ap, args, root, orch_cls, make_broker):
    if args.shadow and not args.shadow_agents:
        ap.error("--shadow needs --shadow-agents FILE")
    hdir = os.path.join(root, ".harness")
    try:
        daemonlock.acquire(hdir, "daemon", wait_s=60)
    except daemonlock.Busy as e:
        print(f"orchestrator: refusing to start: {e}", file=sys.stderr)
        return 2
    try:
        try:
            broker = NullBroker() if args.no_broker else make_broker(root)
            o = orch_cls(root, broker=broker, dry_run=not args.live,
                         fake_claude=args.fake_claude,
                         shadow_agents=args.shadow_agents
                         if args.shadow else None,
                         log=lambda m: print(m, flush=True))
        except launcher.ModelError as e:
            print(f"orchestrator: refusing to start: {e}", file=sys.stderr)
            return 2
        if not args.no_self_update and args.live:
            o.enable_self_update(sys.argv)
        try:
            o.run(once=args.once, exit_when_drained=args.exit_when_drained)
        except KeyboardInterrupt:
            pass
        finally:
            o.close()
    finally:
        daemonlock.release(hdir)
    return 0


def main(argv, orch_cls, make_broker, root_default):
    ap = _parser(root_default)
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    odir = os.path.join(root, "planning", "orchestrator")
    os.makedirs(odir, exist_ok=True)
    if args.cmd in ("drain", "stop", "resume", "pause"):
        return _switches(args, odir)
    if args.cmd == "status":
        try:
            with open(os.path.join(odir, "status.json"),
                      encoding="utf-8") as f:
                print(f.read())
        except FileNotFoundError:
            print("no status.json yet; is the daemon running?")
            return 1
        return 0
    if args.cmd == "flakes":
        rows = localci_flakes.from_state_file(os.path.join(odir, "state.json"))
        print("\n".join(localci_flakes.backlog_lines(rows))
              or "no flaky test recorded")
        return 0
    if args.cmd == "run":
        return _run(ap, args, root, orch_cls, make_broker)
    if daemon_alive(root):
        return _post(root, args)
    return _direct(root, args, orch_cls)
