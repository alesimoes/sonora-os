"""Wire contract of the harness broker (ADR-0047 section 4b).

Transport: TCP on 127.0.0.1, newline-delimited JSON, one object per line,
UTF-8, at most MAX_FRAME bytes. Every frame has "v" (PROTOCOL_VERSION) and
"t" (frame type).

Client -> broker requests (optional "rid" is echoed in the reply):
  register {name, token, last_acked_seq}   first frame of a connection;
        reply ok {name, last_acked_seq}, then one "deliver" frame per
        unacked message with seq > max(last_acked_seq, stored .acked).
  ping {}                                   reply ok {}
  ack {seq}                                 cumulative, no reply; marks every
        message with seq <= N delivered for this name.
  send {to, type, body, feature?, task?, reply_to?, needs_ack?}
        to = a name | "feat/<id>" (channel) | "role:<r>" (live names
        starting "<r>-" or equal to "<r>"). reply ok {id, recipients}.
  claim {paths}     all-or-nothing; reply ok {claimed} or "claim_denied"
        {denied: [{path, owner}]}.
  release {paths}   reply ok {released}. paths=[] releases all of the caller.
  check {path}      reply "check_result" {allow, owner, reason}. May be sent
        without register on a short connection by adding "name" and "token".

Broker -> client:
  ok / error {code, msg} / claim_denied / check_result  (replies, carry rid)
  deliver {msg}     server push; msg is the message envelope below.

Message envelope (also what is stored in mail/<name>/ and log/messages.jsonl):
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
    if frame.get("v") != PROTOCOL_VERSION:
        raise ProtocolError(E_BAD_FRAME, "unsupported version")
    if not isinstance(frame.get("t"), str):
        raise ProtocolError(E_BAD_FRAME, "missing t")
    return frame


def _need(frame, key, typ):
    if not isinstance(frame.get(key), typ):
        raise ProtocolError(E_BAD_FRAME, f"{frame['t']}: {key} must be "
                            f"{typ.__name__}")


def valid_name(name):
    return bool(name) and len(name) <= 64 and all(
        c.isalnum() or c in "-_." for c in name)


def validate_request(frame):
    t = frame["t"]
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
    return {"t": "ok", "rid": rid, **fields}


def reply_error(rid, code, msg=""):
    return {"t": "error", "rid": rid, "code": code, "msg": msg}


def env_connection(environ=None):
    """(host, port, token, name) from the environment; raises KeyError naming
    the missing variable. Clients exit non-zero on KeyError."""
    e = os.environ if environ is None else environ
    return (LOOPBACK_HOST, int(e[ENV_PORT]), e[ENV_TOKEN], e[ENV_NAME])
