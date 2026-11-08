"""Runs the real Broker on its own asyncio thread for the synchronous daemon.

The broker is single-loop by design (no locks), so every call into it is
marshalled onto that loop. Broker events fire on the loop thread; they are
queued here and the daemon drains them on its own thread, so the daemon's
state store is never touched from two threads.

Port and token are pinned across daemon restarts (2026-11-08: a self-update
restart gave the broker a new port and token, so every agent running at that
moment lost the hub: WinError 10061, the edit hook denied every write, the
agents reported blocked). The port is in broker.json already; the token goes
to broker.token, owner-readable only, beside the run files that carry it
anyway. When the old port cannot be bound the host falls back to a free one.
"""
import asyncio
import os
import queue
import threading

from . import protocol as P
from .broker import Broker
from .mailbox import Mailbox

TOKEN_FILE = "broker.token"


def pinned(harness_dir):
    """(port, token) of the previous broker in this directory, or (0, None)."""
    box = Mailbox(harness_dir)
    port = box.load_state("broker.json", {}).get("port") or 0
    try:
        with open(os.path.join(harness_dir, TOKEN_FILE), encoding="utf-8") as f:
            token = f.read().strip() or None
    except OSError:
        token = None
    return (int(port) if token else 0), token


def save_token(harness_dir, token):
    path = os.path.join(harness_dir, TOKEN_FILE)
    os.makedirs(harness_dir, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(token)


class BrokerHost:
    def __init__(self, harness_dir, pin=True, **broker_kw):
        self.events = queue.Queue()
        self.dir = harness_dir
        if pin:
            port, token = pinned(harness_dir)
            broker_kw.setdefault("port", port)
            broker_kw.setdefault("token", token)
        self.broker = Broker(harness_dir, on_event=self._on_event, **broker_kw)
        self.loop = asyncio.new_event_loop()
        self._thread = None
        self._known = set()

    @property
    def port(self):
        return self.broker.port

    @property
    def token(self):
        return self.broker.token

    def _on_event(self, kind, **data):
        self.events.put((kind, data))

    def drain_events(self):
        out = []
        while True:
            try:
                out.append(self.events.get_nowait())
            except queue.Empty:
                return out

    def _run_loop(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def _await(self, coro, timeout=10):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    def _call(self, fn, *a, **kw):
        async def run():
            return fn(*a, **kw)
        return self._await(run())

    def start(self):
        self._thread = threading.Thread(target=self._run_loop, daemon=True,
                                        name="broker")
        self._thread.start()
        try:
            self._await(self.broker.start())
        except OSError:
            if not self.broker.port:
                raise
            self.broker.port = 0   # the pinned port is taken: a fresh one
            self._await(self.broker.start())
        save_token(self.dir, self.broker.token)

    def stop(self):
        if self._thread is None:
            return
        try:
            self._await(self.broker.stop())
        finally:
            self.loop.call_soon_threadsafe(self.loop.stop)
            self._thread.join(timeout=5)
            self._thread = None
            self.loop.close()

    # -- what the daemon needs ---------------------------------------------
    def expect(self, *names):
        self._call(self.broker.expect, *names)

    def open_channel(self, feature, members):
        self._call(self.broker.open_channel, feature, members)

    def claim_for(self, owner, paths):
        return self._call(self.broker.claim_for, owner, paths)

    def release_for(self, owner, paths=()):
        return self._call(self.broker.release_for, owner, paths)

    def tell(self, to, type_, body=None, feature=None, task=None):
        """Daemon message. A role name (lead, architect) is registered as
        expected so the message waits in its mailbox until it is launched."""
        if to not in self._known and P.valid_name(to):
            self._known.add(to)
            self.expect(to)
        return self._call(self.broker.send, P.DAEMON, to, type_, body,
                          feature, task)


class NullBroker:
    """No broker (--no-broker): same surface, does nothing."""
    port = 0

    def __init__(self):
        self.token = P.new_token()

    def start(self):
        pass

    def stop(self):
        pass

    def drain_events(self):
        return []

    def expect(self, *names):
        pass

    def open_channel(self, feature, members):
        pass

    def claim_for(self, owner, paths):
        return []

    def release_for(self, owner, paths=()):
        return 0

    def tell(self, to, type_, body=None, feature=None, task=None):
        pass
