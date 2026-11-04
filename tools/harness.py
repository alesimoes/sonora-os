#!/usr/bin/env python3
"""CLI fallback and hook entry point for the harness broker (ADR-0047 4b).

  harness.py whoami | send TO TYPE [BODY] [--reply-to ID] | recv [--max N] [--wait S]
             | claim PATH... | release PATH... | check PATH
  harness.py inbox --brief --hook    PostToolUse: prints nothing when empty
  harness.py check --hook            PreToolUse (Edit|Write|MultiEdit): reads the
                                     hook JSON on stdin, exit 2 to deny, fail closed
One short broker connection per call; name, port and token from SONORA_* env.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "harness"))
from client import Client, HarnessError, brief, env_config, oneshot  # noqa: E402


def hook_path():
    try:
        data = json.load(sys.stdin)
    except ValueError:
        return None
    ti = data.get("tool_input") or {}
    return ti.get("file_path") or ti.get("path")


def cmd_check(c, a):
    if not a.path:
        print("check needs PATH", file=sys.stderr)
        return 2
    ok, why = c.check(a.path)
    if ok:
        return 0
    print(f"harness: {a.path} denied: {why}", file=sys.stderr)
    return 2


def run_hook(a, hook_inbox):
    """Hooks run beside the agent's MCP connection (which owns the registered
    name), so they use token-authenticated one-shot requests, not register."""
    try:
        cfg = env_config()
        if hook_inbox:
            r = oneshot(cfg["port"], cfg["name"], cfg["token"], "peek")
            msgs = r.get("messages", []) if r.get("ok") else []
            if msgs:  # never block the agent on a read-only notice
                text = "Harness messages:\n" + "\n".join(brief(m) for m in msgs)
                print(json.dumps({"hookSpecificOutput": {
                    "hookEventName": "PostToolUse", "additionalContext": text}}))
            return 0
        path = hook_path()
        if not path:
            print("harness: no file path in hook input", file=sys.stderr)
            return 2
        r = oneshot(cfg["port"], cfg["name"], cfg["token"], "check", path=path)
        if r.get("ok") and r.get("allowed"):
            return 0
        why = r.get("reason") or r.get("error") or "claim it first"
        print(f"harness: edit of {path} denied: {why}", file=sys.stderr)
        return 2
    except (HarnessError, OSError, ValueError) as e:
        if hook_inbox:
            return 0
        print(f"harness: broker unavailable ({e}); edits denied", file=sys.stderr)
        return 2


def main(argv):
    ap = argparse.ArgumentParser(prog="harness.py")
    sub = ap.add_subparsers(dest="verb", required=True)
    sub.add_parser("whoami")
    s = sub.add_parser("send")
    s.add_argument("to")
    s.add_argument("type")
    s.add_argument("body", nargs="?", default="")
    s.add_argument("--reply-to")
    for name in ("recv", "inbox"):
        r = sub.add_parser(name)
        r.add_argument("--max", type=int, default=20)
        r.add_argument("--wait", type=float, default=0)
        r.add_argument("--brief", action="store_true")
        r.add_argument("--hook", action="store_true")
    for name in ("claim", "release"):
        sub.add_parser(name).add_argument("paths", nargs="+")
    k = sub.add_parser("check")
    k.add_argument("path", nargs="?")
    k.add_argument("--hook", action="store_true")
    a = ap.parse_args(argv)

    hook_inbox = a.verb == "inbox" and a.hook
    if hook_inbox or (a.verb == "check" and a.hook):
        return run_hook(a, hook_inbox)
    try:
        c = Client.from_env(ping=False, timeout=5.0, reconnect_window=0)
        c.connect()
    except (HarnessError, OSError, ValueError) as e:
        print(f"harness: {e}", file=sys.stderr)
        return 1
    try:
        if a.verb == "whoami":
            print(json.dumps(c.whoami()))
        elif a.verb == "send":
            try:
                body = json.loads(a.body) if a.body.startswith(("{", "[")) else a.body
            except ValueError:
                body = a.body
            print(json.dumps(c.send(a.to, a.type, body, a.reply_to)))
        elif a.verb in ("recv", "inbox"):
            msgs = c.recv(a.max, a.wait)
            if hook_inbox:
                if msgs:
                    text = "Harness messages:\n" + "\n".join(brief(m) for m in msgs)
                    print(json.dumps({"hookSpecificOutput": {
                        "hookEventName": "PostToolUse", "additionalContext": text}}))
            elif a.brief:
                for m in msgs:
                    print(brief(m))
            else:
                print(json.dumps(msgs))
        elif a.verb == "claim":
            print(json.dumps(c.claim(a.paths)))
        elif a.verb == "release":
            print(json.dumps(c.release(a.paths)))
        elif a.verb == "check":
            return cmd_check(c, a)
    except HarnessError as e:
        print(f"harness: {e}", file=sys.stderr)
        return 1
    finally:
        c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
