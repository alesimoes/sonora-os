#!/usr/bin/env python3
"""CLI fallback and hook entry point for the harness broker (ADR-0047 4b).

  harness.py whoami | send TO TYPE [BODY] [--reply-to ID] | recv [--max N] [--wait S]
             | claim PATH... | release PATH... | check PATH
  harness.py inbox --brief --hook    PostToolUse: prints nothing when empty
  harness.py check --hook            PreToolUse (Edit|Write|MultiEdit): reads the
                                     hook JSON on stdin, exit 2 to deny, fail closed
One short, unregistered broker connection per call (it never takes the
agent's name, which its MCP connection owns); name, port and token from the
SONORA_* env. send tags the message with SONORA_FEATURE and SONORA_TASK.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "harness"))
from client import HarnessError, brief, env_config, oneshot  # noqa: E402
from hookretry import with_one_retry  # noqa: E402
from pathnorm import candidates  # noqa: E402

def hook_input():
    try:
        data = json.load(sys.stdin)
    except ValueError:
        return {}
    return data if isinstance(data, dict) else {}


def hook_path(data):
    ti = data.get("tool_input") or {}
    return ti.get("file_path") or ti.get("path")


def bash_guard(data):
    """PreToolUse for Bash: 2 (deny) when a git command would move the
    daemon's own checkout (the root of the tools/ this file lives in)."""
    from gitguard import MESSAGE, violates
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    cmd = (data.get("tool_input") or {}).get("command", "")
    if violates(cmd, data.get("cwd") or os.getcwd(), root):
        print(f"harness: {MESSAGE}", file=sys.stderr)
        return 2
    return 0


def call(cfg, op, **fields):
    """One token-authenticated request on a short connection that does not
    register: the CLI must never take the agent's name, which its MCP
    connection owns."""
    r = oneshot(cfg["port"], cfg["name"], cfg["token"], op, **fields)
    if not r.get("ok"):
        raise HarnessError(str(r.get("error", "refused")))
    return r


def cmd_check(cfg, a):
    if not a.path:
        print("check needs PATH", file=sys.stderr)
        return 2
    r = call(cfg, "check", path=a.path)
    if r.get("allowed", True):
        return 0
    print(f"harness: {a.path} denied: {r.get('reason', '')}", file=sys.stderr)
    return 2


def cmd_recv(cfg, a):
    end = time.time() + a.wait
    while True:
        msgs = call(cfg, "inbox", limit=a.max, ack=True).get("messages", [])
        if msgs or time.time() >= end:
            return msgs
        time.sleep(0.5)


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
        data = hook_input()
        if data.get("tool_name") == "Bash":
            return bash_guard(data)
        path = hook_path(data)
        if not path:
            print("harness: no file path in hook input", file=sys.stderr)
            return 2
        # The run's own result.json is never anyone else's file, and a
        # merge-train run works alone in a worktree of its own: no claim.
        res = os.environ.get("SONORA_RESULT_PATH")
        if res and os.path.normcase(os.path.abspath(path)) ==                 os.path.normcase(os.path.abspath(res)):
            return 0
        if os.environ.get("SONORA_UNCLAIMED_OK") == "1":
            return 0
        r = {}
        for cand in candidates(path):
            r = with_one_retry(lambda: oneshot(
                cfg["port"], cfg["name"], cfg["token"], "check", path=cand))
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
    s.add_argument("--feature", help="default: SONORA_FEATURE")
    s.add_argument("--task", help="default: SONORA_TASK")
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
        cfg = env_config()
    except (HarnessError, ValueError) as e:
        print(f"harness: {e}", file=sys.stderr)
        return 1
    feature = a.feature if a.verb == "send" and a.feature else \
        os.environ.get("SONORA_FEATURE") or None
    task = a.task if a.verb == "send" and a.task else \
        os.environ.get("SONORA_TASK") or None
    try:
        if a.verb == "whoami":
            r = call(cfg, "whoami")
            print(json.dumps({"name": cfg["name"], "feature": feature,
                              "task": task, "channels": r.get("channels", [])}))
        elif a.verb == "send":
            try:
                body = json.loads(a.body) if a.body.startswith(("{", "[")) else a.body
            except ValueError:
                body = a.body
            print(json.dumps(call(cfg, "send", to=a.to, type=a.type, body=body,
                                  reply_to=a.reply_to, feature=feature,
                                  task=task)))
        elif a.verb in ("recv", "inbox"):
            msgs = cmd_recv(cfg, a)
            if a.brief:
                for m in msgs:
                    print(brief(m))
            else:
                print(json.dumps(msgs))
        elif a.verb == "claim":
            print(json.dumps(call(cfg, "claim", paths=a.paths)))
        elif a.verb == "release":
            print(json.dumps(call(cfg, "release", paths=a.paths)))
        elif a.verb == "check":
            return cmd_check(cfg, a)
    except (HarnessError, OSError, ValueError) as e:
        print(f"harness: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
