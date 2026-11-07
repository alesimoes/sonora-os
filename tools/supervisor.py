#!/usr/bin/env python3
"""Watchdog over the orchestrator daemon (harness/supervisor_outer.py).

    python tools/supervisor.py [--root DAEMON_CHECKOUT] [--once] [--interval S]

Run it once per machine next to the daemon (a second terminal, Task
Scheduler or a systemd user unit). Every interval it checks that the daemon
is alive and on origin/main's harness code, restarts it when it died or
hung, and tells the owner (desktop notification) what it could not heal.
Alerts: planning/orchestrator/supervisor-alerts.json; log: .harness/supervisor.log.
--once runs one sweep and prints the alerts as JSON (exit 1 when any stands).
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import supervisor_outer, supervisor_sys  # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(prog="supervisor.py")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))),
        help="the daemon's checkout (default: this one)")
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--interval", type=float, default=None,
                    help="seconds between sweeps (default: interval_s of "
                         "[supervisor] in orchestrator.toml)")
    args = ap.parse_args(argv)
    outer = supervisor_outer.Outer(args.root, supervisor_sys.RealSys())
    if args.once:
        rows = outer.tick()
        print(json.dumps(rows, indent=1))
        return 1 if rows else 0
    print(f"supervisor: watching {outer.root} every "
          f"{args.interval or outer.th.interval_s:g}s", flush=True)
    try:
        outer.run(interval_s=args.interval)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
