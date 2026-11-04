"""Loopback TCP broker of the harness orchestrator (ADR-0047 4b).

Embedding: create a Broker inside the daemon's asyncio loop, `await start()`,
read `.port` and `.token` for the children's environment, call `send()` /
`open_channel()` / `claim_for()` from the same loop thread, `await stop()`.
Everything runs in one event loop and handlers never await between reading
and mutating state, so claims are atomic without locks.
"""
import asyncio
import hmac
import fnmatch
import os
import time

from . import protocol as P
from .mailbox import Mailbox

_WILD = "*?["


def _norm(p):
    p = p.replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    return p


def _prefix(p):
    for i, c in enumerate(p):
        if c in _WILD:
            return p[:i]
    return p


def _is_glob(p):
    return any(c in p for c in _WILD)


def overlaps(a, b):
    """True if claim patterns a and b can cover the same file (conservative
    for two globs: nested static prefixes count as overlap)."""
    if a == b:
        return True
    ga, gb = _is_glob(a), _is_glob(b)
    if not ga and not gb:
        return False
    if ga and not gb:
        return fnmatch.fnmatchcase(b, a)
    if gb and not ga:
        return fnmatch.fnmatchcase(a, b)
    pa, pb = _prefix(a), _prefix(b)
    return pa.startswith(pb) or pb.startswith(pa)


class _Conn:
    def __init__(self, writer):
        self.writer = writer
        self.name = None

    def send(self, frame):
        if not self.writer.is_closing():
            self.writer.write(P.encode(frame))


class Broker:
    def __init__(self, harness_dir, token=None, host=P.LOOPBACK_HOST, port=0,
                 on_event=None, expected_names=None):
        if host != P.LOOPBACK_HOST:
            raise ValueError("broker binds 127.0.0.1 only, got %r" % (host,))
        self.dir = os.path.abspath(harness_dir)
        self.token = token or P.new_token()
        self.host = host
        self.port = port
        self.on_event = on_event
        self.expected = set(expected_names) if expected_names else None
        self.box = Mailbox(self.dir)
        self.conns = {}       # name -> _Conn (registered, live)
        self.last_seen = {}   # name -> monotonic time of last frame
        self.seq = {}         # name -> last assigned seq
        self.claims = {}      # pattern -> owner
        self.channels = {}    # feature -> set(members)
        self._server = None
        self._stopping = False

    # ---- lifecycle ----------------------------------------------------
    async def start(self):
        self.box.sweep_tmp()
        self.claims = self.box.load_state("claims.json", {})
        self.channels = {k: set(v) for k, v in
                         self.box.load_state("channels.json", {}).items()}
        for n in self.box.names():
            self.seq[n] = self.box.max_seq(n)
        self._server = await asyncio.start_server(
            self._handle, self.host, self.port, limit=P.MAX_FRAME)
        self.port = self._server.sockets[0].getsockname()[1]
        self.box.save_state("broker.json", {
            "host": self.host, "port": self.port, "pid": os.getpid(),
            "started": P.now_iso()})  # never the token
        return self

    async def stop(self):
        self._stopping = True
        if self._server:
            self._server.close()
        for c in list(self.conns.values()):
            c.writer.close()
        if self._server:
            await self._server.wait_closed()
            self._server = None

    def _event(self, kind, **data):
        if self.on_event:
            try:
                self.on_event(kind, **data)
            except Exception:
                pass

    # ---- state API (daemon side) ---------------------------------------
    def expect(self, *names):
        if self.expected is None:
            self.expected = set()
        self.expected.update(names)

    def open_channel(self, feature, members):
        self.channels[feature] = set(members) | {P.DAEMON}
        self.box.save_state("channels.json",
                            {k: sorted(v) for k, v in self.channels.items()})

    def close_channel(self, feature):
        self.channels.pop(feature, None)
        self.box.save_state("channels.json",
                            {k: sorted(v) for k, v in self.channels.items()})

    def is_alive(self, name):
        return name in self.conns

    def claim_for(self, owner, paths):
        """Claim on behalf of `owner` (the daemon writes a task's globs at
        launch). Returns [] on success or the denied [{path, owner}]."""
        paths = [_norm(p) for p in paths]
        denied = []
        for p in paths:
            for q, o in self.claims.items():
                if o != owner and overlaps(p, q):
                    denied.append({"path": p, "owner": o})
                    break
        if denied:
            return denied
        for p in paths:
            self.claims[p] = owner
        self._save_claims()
        return []

    def release_for(self, owner, paths=()):
        keep = {_norm(p) for p in paths}
        n = 0
        for p in [p for p, o in self.claims.items()
                  if o == owner and (not keep or p in keep)]:
            del self.claims[p]
            n += 1
        self._save_claims()
        return n

    def check_path(self, name, path):
        path = _norm(path)
        if name == P.DAEMON:
            return {"allow": True, "owner": None, "reason": "daemon"}
        mine = False
        for pat, owner in self.claims.items():
            if pat == path or fnmatch.fnmatchcase(path, pat):
                if owner != name:
                    return {"allow": False, "owner": owner,
                            "reason": "claimed by %s" % owner}
                mine = True
        if mine:
            return {"allow": True, "owner": name, "reason": "own claim"}
        return {"allow": False, "owner": None,
                "reason": "not claimed: claim it first"}

    def _save_claims(self):
        self.box.save_state("claims.json", self.claims)

    # ---- routing --------------------------------------------------------
    def _known(self, name):
        return (name in (P.DAEMON, "owner") or name in self.conns
                or name in self.seq
                or (self.expected is not None and name in self.expected))

    def _resolve(self, sender, to):
        if to.startswith(P.CHANNEL_PREFIX):
            members = self.channels.get(to[len(P.CHANNEL_PREFIX):])
            if members is None:
                raise P.ProtocolError(P.E_NO_RECIPIENT, "no channel " + to)
            if sender != P.DAEMON and sender not in members:
                raise P.ProtocolError(P.E_NOT_MEMBER, to)
            return sorted(members - {sender})
        if to.startswith(P.ROLE_PREFIX):
            r = to[len(P.ROLE_PREFIX):]
            names = sorted(n for n in self.conns
                           if n == r or n.startswith(r + "-"))
            if not names:
                raise P.ProtocolError(P.E_NO_RECIPIENT, "no live " + to)
            return names
        if not P.valid_name(to) or not self._known(to):
            raise P.ProtocolError(P.E_NO_RECIPIENT, to)
        return [to]

    def send(self, sender, to, type_, body=None, feature=None, task=None,
             reply_to=None, needs_ack=False):
        """Route one message; durable on disk before this returns."""
        recipients = self._resolve(sender, to)
        if type_ not in P.MSG_TYPES:
            self._event(P.EV_UNKNOWN_TYPE, type=type_, sender=sender)
        mid = P.new_id()
        base = {"v": P.PROTOCOL_VERSION, "id": mid, "ts": P.now_iso(),
                "from": sender, "to": to, "feature": feature, "task": task,
                "type": type_, "reply_to": reply_to,
                "needs_ack": bool(needs_ack),
                "body": {} if body is None else body}
        self.box.append_log(base)
        for r in recipients:
            seq = self.seq.get(r, 0) + 1
            self.seq[r] = seq
            msg = dict(base, seq=seq)
            self.box.put(r, msg)
            if r == P.DAEMON:
                self.box.set_acked(r, seq)
                self._event(P.EV_MESSAGE, msg=msg)
            elif r in self.conns:
                self.conns[r].send({"op": "deliver", "msg": msg})
        return mid, recipients

    # ---- connection handling --------------------------------------------
    async def _handle(self, reader, writer):
        conn = _Conn(writer)
        try:
            while True:
                try:
                    line = await reader.readline()
                except (ValueError, asyncio.LimitOverrunError):
                    conn.send(P.reply_error(None, P.E_BAD_FRAME, "too long"))
                    break
                if not line:
                    break
                rid = None
                try:
                    frame = P.decode(line)
                    rid = frame.get("rid")
                    P.validate_request(frame)
                    if not self._dispatch(conn, frame, rid):
                        break
                except P.ProtocolError as e:
                    conn.send(P.reply_error(rid, e.code, e.msg))
                    if conn.name is None:
                        break
                await writer.drain()
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            if conn.name and self.conns.get(conn.name) is conn:
                del self.conns[conn.name]
                if not self._stopping:
                    self._event(P.EV_SUSPECT, name=conn.name)
            writer.close()

    def _auth(self, token):
        return hmac.compare_digest(str(token).encode(), self.token.encode())

    def _dispatch(self, conn, f, rid):
        """Returns False to close the connection."""
        t = f["op"]
        if t == "register":
            return self._register(conn, f, rid)
        if conn.name is None:
            if t == "check" and self._auth(f.get("token", "")) and \
                    P.valid_name(str(f.get("name", ""))):
                conn.send(self._check_reply(rid, f["name"], f["path"]))
                return True
            raise P.ProtocolError(P.E_NOT_REGISTERED, "register first")
        name = conn.name
        self.last_seen[name] = time.monotonic()
        if t == "ping":
            conn.send(P.reply_ok(rid))
        elif t == "ack":
            self.box.set_acked(name, f["seq"])
            conn.send(P.reply_ok(rid))
        elif t == "send":
            mid, rcpt = self.send(
                name, f["to"], f["type"], f.get("body"), f.get("feature"),
                f.get("task"), f.get("reply_to"), f.get("needs_ack", False))
            conn.send(P.reply_ok(rid, id=mid, recipients=rcpt))
        elif t == "claim":
            paths = [_norm(p) for p in f["paths"]]
            denied = self.claim_for(name, paths)
            if denied:
                d = denied[0]
                conn.send(P.reply_error(
                    rid, P.E_CLAIM_DENIED,
                    "%s claimed by %s" % (d["path"], d["owner"]),
                    denied=denied))
            else:
                conn.send(P.reply_ok(rid, claimed=paths))
        elif t == "release":
            conn.send(P.reply_ok(rid, released=self.release_for(
                name, f["paths"])))
        elif t == "check":
            conn.send(self._check_reply(rid, name, f["path"]))
        return True

    def _check_reply(self, rid, name, path):
        r = self.check_path(name, path)
        return P.reply_ok(rid, allowed=r["allow"], owner=r["owner"],
                          reason=r["reason"])

    def _register(self, conn, f, rid):
        name = f["name"]
        if conn.name is not None:
            raise P.ProtocolError(P.E_BAD_FRAME, "already registered")
        if not self._auth(f["token"]):
            conn.send(P.reply_error(rid, P.E_BAD_TOKEN))
            return False
        if name in self.conns:
            conn.send(P.reply_error(rid, P.E_DUPLICATE, name))
            return False
        if self.expected is not None and name not in self.expected:
            conn.send(P.reply_error(rid, P.E_UNKNOWN_NAME, name))
            return False
        conn.name = name
        self.conns[name] = conn
        self.last_seen[name] = time.monotonic()
        client_acked = f.get("last_acked_seq", 0)
        if client_acked:
            self.box.set_acked(name, client_acked)
        after = max(client_acked, self.box.get_acked(name))
        self.seq.setdefault(name, self.box.max_seq(name))
        conn.send(P.reply_ok(rid, name=name, last_acked_seq=after,
                             pending=self.box.unacked(name, after)))
        self._event(P.EV_REGISTERED, name=name)
        return True
