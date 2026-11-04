"""Runs the real Broker on its own asyncio thread for the synchronous daemon.

The broker is single-loop by design (no locks), so every call into it is
marshalled onto that loop. Broker events fire on the loop thread; they are
queued here and the daemon drains them on its own thread, so the daemon's
sqlite store is never touched from two threads.
"""
import asyncio
import queue
import threading

from . import protocol as P
from .broker import Broker


class BrokerHost:
    def __init__(self, harness_dir, **broker_kw):
        self.events = queue.Queue()
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
        self._await(self.broker.start())

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
