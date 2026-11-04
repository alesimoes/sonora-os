"""Agent-side connection to the harness broker (ADR-0047 4b).

Wire shape used here (newline-delimited JSON over 127.0.0.1): a request is
{"op", "rid", ...}, its reply {"rid", "ok", ...}; the broker also pushes
{"op": "deliver", "msg": {...}} frames. Op names are plain strings used only
in the verbs below, so aligning with eng-broker's protocol.py is a local edit.
"""
import json
import os
import socket
import threading
import time

PING_S = 15.0
RECONNECT_WINDOW_S = 60.0


class HarnessError(Exception):
    pass


def env_config():
    """Read the launch environment; a missing variable is a loud failure."""
    cfg = {}
    for key, var in (("port", "SONORA_HARNESS_PORT"), ("name", "SONORA_AGENT_NAME"),
                     ("token", "SONORA_HARNESS_TOKEN")):
        val = os.environ.get(var)
        if not val:
            raise HarnessError(f"{var} is not set")
        cfg[key] = val
    cfg["port"] = int(cfg["port"])
    return cfg


class Client:
    def __init__(self, port, name, token, host="127.0.0.1", ping=True, timeout=10.0,
                 reconnect_window=RECONNECT_WINDOW_S):
        self.host, self.port, self.name, self.token = host, port, name, token
        self.timeout = timeout
        self.reconnect_window = reconnect_window
        self.last_acked_seq = None   # None: let the broker use its stored ack
        self.info = {}
        self._sock = None
        self._wlock = threading.Lock()
        self._cv = threading.Condition()
        self._replies = {}
        self._inbox = []
        self._seen = set()        # ids already queued (delivery is at-least-once)
        self._rid = 0
        self._alive = False
        self._closed = False
        self._ping = ping
        self._pinger = None

    @classmethod
    def from_env(cls, **kw):
        c = env_config()
        return cls(c["port"], c["name"], c["token"], **kw)

    # -- connection -------------------------------------------------------
    def connect(self):
        s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        s.settimeout(None)
        self._sock = s
        self._alive = True
        threading.Thread(target=self._reader, args=(s,), daemon=True).start()
        extra = {} if self.last_acked_seq is None else {"last_acked_seq": self.last_acked_seq}
        r = self._request("register", _raw=True, name=self.name, token=self.token, **extra)
        if not r.get("ok"):
            self.close()
            raise HarnessError("register refused: " + str(r.get("error", r)))
        self.info = r
        for m in r.get("pending", []):
            self._queue(m)
        if self._ping and self._pinger is None:
            self._pinger = threading.Thread(target=self._ping_loop, daemon=True)
            self._pinger.start()
        return r

    def close(self):
        self._closed = True
        self._drop()

    def _drop(self):
        self._alive = False
        try:
            if self._sock:
                # The reader thread's makefile() keeps the fd open past close().
                self._sock.shutdown(socket.SHUT_RDWR)
                self._sock.close()
        except OSError:
            pass
        with self._cv:
            self._cv.notify_all()

    def _reader(self, sock):
        try:
            for line in sock.makefile("rb"):
                try:
                    fr = json.loads(line)
                except ValueError:
                    continue
                if fr.get("op") == "deliver":
                    self._queue(fr.get("msg", {}))
                elif "rid" in fr:
                    with self._cv:
                        self._replies[fr["rid"]] = fr
                        self._cv.notify_all()
        except OSError:
            pass
        if sock is self._sock:
            self._drop()

    def _queue(self, m):
        with self._cv:
            mid = m.get("id")
            if mid is not None:
                if mid in self._seen:
                    return
                self._seen.add(mid)
            self._inbox.append(m)
            self._cv.notify_all()

    def _ping_loop(self):
        while not self._closed:
            time.sleep(PING_S)
            try:
                self._request("ping")
            except HarnessError:
                pass

    def _reconnect(self):
        deadline = time.time() + self.reconnect_window
        delay = 0.2
        while not self._closed:
            try:
                self.connect()
                return
            except (OSError, HarnessError):
                if time.time() > deadline:
                    break
                time.sleep(delay)
                delay = min(delay * 2, 5)
        raise HarnessError("broker unreachable")

    def _request(self, op, _raw=False, **fields):
        if not _raw and not self._alive:
            self._reconnect()
        with self._wlock:
            self._rid += 1
            rid = self._rid
            data = json.dumps(dict(fields, op=op, rid=rid)).encode() + b"\n"
            try:
                self._sock.sendall(data)
            except (OSError, AttributeError) as e:
                self._drop()
                raise HarnessError(f"broker connection lost: {e}")
        end = time.time() + self.timeout
        with self._cv:
            while rid not in self._replies:
                left = end - time.time()
                if left <= 0 or not self._alive:
                    self._drop()
                    raise HarnessError("no reply from broker")
                self._cv.wait(left)
            return self._replies.pop(rid)

    def call(self, op, **fields):
        r = self._request(op, **fields)
        if not r.get("ok"):
            raise HarnessError(str(r.get("error", "refused")))
        return r

    # -- verbs ------------------------------------------------------------
    def whoami(self):
        keep = ("feature", "task", "role", "channels")
        return dict({"name": self.name}, **{k: v for k, v in self.info.items() if k in keep})

    def send(self, to, type, body=None, reply_to=None):
        return self.call("send", to=to, type=type, body={} if body is None else body,
                         reply_to=reply_to)

    def recv(self, limit=20, wait_s=0.0):
        """Return up to `limit` pending messages and ack them."""
        end = time.time() + wait_s
        with self._cv:
            while not self._inbox and time.time() < end and self._alive:
                self._cv.wait(end - time.time())
            out, self._inbox = self._inbox[:limit], self._inbox[limit:]
        seq = max((m.get("seq", 0) for m in out), default=0)
        if seq:
            self.ack(seq)
        return out

    def ack(self, seq):
        self.call("ack", seq=seq)
        self.last_acked_seq = max(self.last_acked_seq or 0, seq)

    def claim(self, paths):
        return self.call("claim", paths=list(paths))

    def release(self, paths):
        return self.call("release", paths=list(paths))

    def check(self, path):
        """Return (allowed, reason)."""
        r = self._request("check", path=path)
        if not r.get("ok"):
            return False, str(r.get("error", "refused"))
        return bool(r.get("allowed", True)), r.get("reason", "")


def oneshot(port, name, token, op, host="127.0.0.1", timeout=5.0, **fields):
    """Token-authenticated single request on a short connection that does not
    register, so it works beside the agent's MCP connection (used by hooks)."""
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.settimeout(timeout)
        frame = dict(fields, op=op, rid=1, name=name, token=token)
        s.sendall(json.dumps(frame).encode() + b"\n")
        line = s.makefile("rb").readline()
    if not line:
        raise HarnessError("broker closed the connection")
    return json.loads(line)


def brief(m):
    body = m.get("body")
    text = body["text"] if isinstance(body, dict) and "text" in body else (
        body if isinstance(body, str) else json.dumps(body, ensure_ascii=False))
    return f"{m.get('seq', '?')} {m.get('type', '?')} from {m.get('from', '?')}: {text}"
