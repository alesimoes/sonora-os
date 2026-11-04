#!/usr/bin/env python3
"""Per-agent stdio MCP server for the harness broker (ADR-0047 4b).

Newline-delimited JSON-RPC on stdin/stdout, stdlib only. Holds one long-lived
broker connection (its liveness is the agent's heartbeat). Exits non-zero at
start-up if the broker is unreachable, so a failed tool server is loud.
"""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "harness"))
from client import Client, HarnessError  # noqa: E402

TOOLS = [
    {"name": "whoami", "description": "Your harness name, task and feature.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "send", "description": "Send a message to a name, feat/<id> or role:<r>.",
     "inputSchema": {"type": "object", "required": ["to", "type"], "properties": {
         "to": {"type": "string"}, "type": {"type": "string"},
         "body": {"description": "string or object"},
         "reply_to": {"type": "string"}}}},
    {"name": "recv", "description": "Return pending messages and acknowledge them.",
     "inputSchema": {"type": "object", "properties": {
         "max": {"type": "integer"}, "wait_s": {"type": "number"}}}},
    {"name": "claim", "description": "Claim file paths or globs before editing (all or nothing).",
     "inputSchema": {"type": "object", "required": ["paths"], "properties": {
         "paths": {"type": "array", "items": {"type": "string"}}}}},
    {"name": "release", "description": "Release claimed paths.",
     "inputSchema": {"type": "object", "required": ["paths"], "properties": {
         "paths": {"type": "array", "items": {"type": "string"}}}}},
]


def run_tool(c, name, a):
    if name == "whoami":
        return c.whoami()
    if name == "send":
        return c.send(a["to"], a["type"], a.get("body"), a.get("reply_to"))
    if name == "recv":
        return {"messages": c.recv(int(a.get("max", 20)), min(float(a.get("wait_s", 0)), 30))}
    if name == "claim":
        return c.claim(a["paths"])
    if name == "release":
        return c.release(a["paths"])
    raise KeyError(name)


def handle(c, req):
    method, rid = req.get("method"), req.get("id")
    if rid is None:  # notification
        return None
    if method == "initialize":
        ver = (req.get("params") or {}).get("protocolVersion", "2024-11-05")
        res = {"protocolVersion": ver, "capabilities": {"tools": {}},
               "serverInfo": {"name": "harness", "version": "1"}}
    elif method == "tools/list":
        res = {"tools": TOOLS}
    elif method == "tools/call":
        p = req.get("params") or {}
        try:
            out = run_tool(c, p.get("name"), p.get("arguments") or {})
            res = {"content": [{"type": "text", "text": json.dumps(out, ensure_ascii=False)}]}
        except KeyError as e:
            res = {"isError": True, "content": [{"type": "text", "text": f"bad tool or argument: {e}"}]}
        except HarnessError as e:
            res = {"isError": True, "content": [{"type": "text", "text": f"harness error: {e}"}]}
    elif method == "ping":
        res = {}
    else:
        return {"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": "method not found"}}
    return {"jsonrpc": "2.0", "id": rid, "result": res}


def main():
    try:
        c = Client.from_env()
        c.connect()
    except (HarnessError, OSError, ValueError) as e:
        print(f"harness_mcp: cannot reach broker: {e}", file=sys.stderr)
        return 1
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except ValueError:
            continue
        resp = handle(c, req)
        if resp is not None:
            sys.stdout.write(json.dumps(resp) + "\n")
            sys.stdout.flush()
    c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
