"""Wire contract of the harness broker (ADR-0047 section 4b).

Transport: TCP on 127.0.0.1, newline-delimited JSON, one object per line,
UTF-8, at most MAX_FRAME bytes. Every frame has "v" (PROTOCOL_VERSION).
Requests carry "op" and "rid"; replies carry the same "rid" and "ok"; the
broker's pushes are {"op": "deliver", "msg": {...}}.

Replies: {rid, ok: true, ...} or {rid, ok: false, code, error}. "code" is
machine readable (E_* below), "error" is a human sentence.

Requests (client -> broker):
  register {name, token, last_acked_seq?}  first frame of a connection. Reply
        ok {name, last_acked_seq, pending: [msg...]}: every stored message
        with seq > max(last_acked_seq, stored ack), in seq order. Omit
        last_acked_seq to use the stored ack. Bad token / duplicate / unknown
        name: ok false and the connection is closed.
  ping {}                                   reply ok
  ack {seq}                                 cumulative; reply ok
  send {to, type, body, feature?, task?, reply_to?, needs_ack?}
        to = a name | "feat/<id>" (channel) | "role:<r>" (live names equal to
        or starting "<r>-"). Reply ok {id, recipients}.
  claim {paths}     all-or-nothing. Reply ok {claimed}, or ok false with
        code claim_denied, error "<path> claimed by <owner>", denied
        [{path, owner}].
  release {paths}   reply ok {released}. paths=[] releases all of the caller.
  check {path}      reply ok {allowed, owner, reason}. A short connection may
        skip register by adding "name" and "token" to the check.

Pushes (broker -> client): {"op": "deliver", "msg": <envelope>}, sent after
the register reply for messages that arrive while connected.

Envelope (also stored in mail/<name>/ and log/messages.jsonl):
  {v, id, seq, ts, from, to, feature, task, type, reply_to, needs_ack, body}
  "seq" is per recipient and strictly increasing; delivery is at-least-once,
  handlers must be idempotent on "id".
"""
import json
import os
import secrets
import time

PROTOCOL_VERSION = 1
LOOPBACK_HOST = "127.0.0.1"
MAX_FRAME = 1 << 20

ENV_TOKEN = "SONORA_HARNESS_TOKEN"
ENV_PORT = "SONORA_HARNESS_PORT"
ENV_NAME = "SONORA_AGENT_NAME"
ENV_DIR = "SONORA_HARNESS"

DAEMON = "daemon"
CHANNEL_PREFIX = "feat/"
ROLE_PREFIX = "role:"

MSG_TYPES = frozenset({
    "assign", "ask", "answer", "blocked", "handoff", "done", "qa_report",
    "bug", "note",
    # wake events, sent by the daemon
    "plan_request", "plan_ready", "task_done", "qa_fail", "agent_dead",
    "pool_under_floor",
})
# Frame types a client may send.
REQUESTS = frozenset({"register", "ping", "ack", "send", "claim", "release",
                      "check"})

# Error codes
E_BAD_FRAME = "bad_frame"
E_BAD_TOKEN = "bad_token"
E_DUPLICATE = "duplicate_name"
E_UNKNOWN_NAME = "unknown_name"
E_NOT_REGISTERED = "not_registered"
E_BAD_TYPE = "bad_type"
E_NO_RECIPIENT = "no_recipient"
E_BAD_PATH = "bad_path"
E_CLAIM_DENIED = "claim_denied"
E_NOT_MEMBER = "not_member"   # send to a channel the sender is not in

# Event names passed to Broker's on_event callback.
EV_REGISTERED = "registered"
EV_SUSPECT = "suspect"        # connection of a registered name closed
EV_MESSAGE = "message"        # a message addressed to "daemon"
EV_UNKNOWN_TYPE = "unknown_type"


class ProtocolError(Exception):
    def __init__(self, code, msg=""):
        super().__init__(f"{code}: {msg}")
        self.code = code
        self.msg = msg


def new_id():
    """Sortable unique id (ulid-like): 12 hex ms timestamp + 16 hex random."""
    return f"{int(time.time() * 1000):012x}{secrets.token_hex(8)}"


def new_token():
    return secrets.token_hex(32)


def now_iso():
    ms = int(time.time() * 1000)
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(ms / 1000)) + \
        f".{ms % 1000:03d}Z"


def encode(frame):
    frame = dict(frame)
    frame.setdefault("v", PROTOCOL_VERSION)
    data = json.dumps(frame, separators=(",", ":"), sort_keys=True).encode()
    if len(data) + 1 > MAX_FRAME:
        raise ProtocolError(E_BAD_FRAME, "frame too large")
    return data + b"\n"


def decode(line):
    """Parse one line into a frame dict or raise ProtocolError(E_BAD_FRAME)."""
    try:
        frame = json.loads(line)
    except (ValueError, UnicodeDecodeError) as e:
        raise ProtocolError(E_BAD_FRAME, f"not json: {e}")
    if not isinstance(frame, dict):
        raise ProtocolError(E_BAD_FRAME, "frame must be an object")
    if frame.get("v", PROTOCOL_VERSION) != PROTOCOL_VERSION:  # v optional
        raise ProtocolError(E_BAD_FRAME, "unsupported version")
    return frame


def _need(frame, key, typ):
    if not isinstance(frame.get(key), typ):
        raise ProtocolError(E_BAD_FRAME, f"{frame['op']}: {key} must be "
                            f"{typ.__name__}")


def valid_name(name):
    return bool(name) and len(name) <= 64 and all(
        c.isalnum() or c in "-_." for c in name)


def validate_request(frame):
    t = frame.get("op")
    if t not in REQUESTS:
        raise ProtocolError(E_BAD_FRAME, f"unknown request {t}")
    if t == "register":
        _need(frame, "name", str)
        _need(frame, "token", str)
        if not valid_name(frame["name"]):
            raise ProtocolError(E_BAD_FRAME, "bad name")
        if not isinstance(frame.get("last_acked_seq", 0), int):
            raise ProtocolError(E_BAD_FRAME, "last_acked_seq must be int")
    elif t == "ack":
        _need(frame, "seq", int)
    elif t == "send":
        _need(frame, "to", str)
        _need(frame, "type", str)
        if not isinstance(frame.get("body", {}), (dict, str)):
            raise ProtocolError(E_BAD_FRAME, "body must be object or string")
    elif t in ("claim", "release"):
        _need(frame, "paths", list)
        if not all(isinstance(p, str) for p in frame["paths"]):
            raise ProtocolError(E_BAD_PATH, "paths must be strings")
    elif t == "check":
        _need(frame, "path", str)
    return frame


def reply_ok(rid, **fields):
    return {"rid": rid, "ok": True, **fields}


def reply_error(rid, code, msg="", **fields):
    return {"rid": rid, "ok": False, "code": code,
            "error": f"{code}: {msg}" if msg else code, **fields}


def env_connection(environ=None):
    """(host, port, token, name) from the environment; raises KeyError naming
    the missing variable. Clients exit non-zero on KeyError."""
    e = os.environ if environ is None else environ
    return (LOOPBACK_HOST, int(e[ENV_PORT]), e[ENV_TOKEN], e[ENV_NAME])
