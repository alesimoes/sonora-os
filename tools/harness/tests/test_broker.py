import asyncio
import os
import tempfile
import unittest
from unittest import mock

from tools.harness import mailbox
from tools.harness import protocol as P
from tools.harness.broker import Broker, overlaps


class Client:
    """Fake agent client speaking the real protocol over loopback."""

    def __init__(self, port):
        self.port = port
        self.n = 0
        self.inbox = []

    async def connect(self):
        self.r, self.w = await asyncio.open_connection("127.0.0.1", self.port)
        return self

    async def _read(self):
        line = await asyncio.wait_for(self.r.readline(), 3)
        return P.decode(line) if line else None

    async def req(self, t, **kw):
        """Send a request, return its reply; pushes are queued in inbox."""
        self.n += 1
        rid = str(self.n)
        self.w.write(P.encode(dict(kw, t=t, rid=rid)))
        await self.w.drain()
        while True:
            f = await self._read()
            if f is None or f.get("rid") == rid:
                return f
            if f["t"] == "deliver":
                self.inbox.append(f["msg"])

    async def register(self, name, token, last=0):
        return await self.req("register", name=name, token=token,
                              last_acked_seq=last)

    async def pushes(self, n):
        while len(self.inbox) < n:
            f = await self._read()
            self.inbox.append(f["msg"])
        return self.inbox

    def ack(self, seq):
        self.w.write(P.encode({"t": "ack", "seq": seq}))

    async def close(self):
        self.w.close()
        try:
            await self.w.wait_closed()
        except Exception:
            pass


class BrokerTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = os.path.join(self.tmp.name, ".harness")
        self.events = []
        self.clients = []
        self.broker = await self.mk()
        self.addCleanup(self.tmp.cleanup)

    async def mk(self, token=None):
        b = Broker(self.dir, token=token, port=0,
                   on_event=lambda k, **d: self.events.append((k, d)))
        await b.start()
        return b

    async def raw(self):
        c = await Client(self.broker.port).connect()
        self.clients.append(c)
        return c

    async def asyncTearDown(self):
        for c in self.clients:
            await c.close()
        await self.broker.stop()

    async def agent(self, name, last=0):
        c = await self.raw()
        r = await c.register(name, self.broker.token, last)
        self.assertEqual(r["t"], "ok", r)
        return c

    async def test_bad_token(self):
        c = await self.raw()
        r = await c.register("eng-1", "wrong")
        self.assertEqual((r["t"], r["code"]), ("error", P.E_BAD_TOKEN))
        self.assertIsNone(await c._read())  # closed
        self.assertNotIn("eng-1", self.broker.conns)

    async def test_duplicate_name(self):
        await self.agent("eng-1")
        c = await self.raw()
        r = await c.register("eng-1", self.broker.token)
        self.assertEqual(r["code"], P.E_DUPLICATE)

    async def test_unexpected_name_refused(self):
        self.broker.expect("eng-1")
        c = await self.raw()
        r = await c.register("eng-9", self.broker.token)
        self.assertEqual(r["code"], P.E_UNKNOWN_NAME)

    async def test_request_before_register(self):
        c = await self.raw()
        r = await c.req("send", to="daemon", type="note")
        self.assertEqual(r["code"], P.E_NOT_REGISTERED)

    async def test_addressed_delivery(self):
        a = await self.agent("eng-1")
        t = await self.agent("tester-2")
        o = await self.agent("eng-2")
        r = await a.req("send", to="tester-2", type="handoff",
                        body={"x": 1}, needs_ack=True)
        self.assertEqual(r["recipients"], ["tester-2"])
        msgs = await t.pushes(1)
        self.assertEqual((msgs[0]["from"], msgs[0]["seq"], msgs[0]["body"]),
                         ("eng-1", 1, {"x": 1}))
        r = await o.req("ping")
        self.assertEqual(r["t"], "ok")
        self.assertEqual(o.inbox, [])

    async def test_unknown_recipient(self):
        a = await self.agent("eng-1")
        r = await a.req("send", to="nobody", type="note")
        self.assertEqual(r["code"], P.E_NO_RECIPIENT)

    async def test_channel_delivery_daemon_member(self):
        self.broker.open_channel("f1", ["eng-1", "tester-1"])
        e = await self.agent("eng-1")
        t = await self.agent("tester-1")
        out = await self.agent("eng-2")
        r = await e.req("send", to="feat/f1", type="note", feature="f1")
        self.assertEqual(r["recipients"], ["daemon", "tester-1"])
        self.assertEqual((await t.pushes(1))[0]["from"], "eng-1")
        self.assertEqual([k for k, _ in self.events if k == P.EV_MESSAGE],
                         [P.EV_MESSAGE])
        r = await out.req("send", to="feat/f1", type="note")
        self.assertEqual(r["code"], P.E_NOT_MEMBER)
        mid, rc = self.broker.send(P.DAEMON, "feat/f1", "plan_ready")
        self.assertEqual(rc, ["eng-1", "tester-1"])
        self.assertEqual((await e.pushes(1))[0]["type"], "plan_ready")

    async def test_ordering_and_redelivery_after_reconnect(self):
        a = await self.agent("eng-1")
        t = await self.agent("tester-1")
        for i in range(5):
            await a.req("send", to="tester-1", type="note", body={"i": i})
        got = await t.pushes(5)
        self.assertEqual([m["seq"] for m in got], [1, 2, 3, 4, 5])
        t.ack(2)
        await t.req("ping")
        await t.close()
        await asyncio.sleep(0.05)
        await a.req("send", to="tester-1", type="note", body={"i": 5})
        t2 = await self.agent("tester-1", last=2)
        got = await t2.pushes(4)
        self.assertEqual([m["seq"] for m in got], [3, 4, 5, 6])
        # a fresh process that remembers nothing still resumes from .acked
        t2.ack(6)
        await t2.req("ping")
        await t2.close()
        await asyncio.sleep(0.05)
        t3 = await self.agent("tester-1", last=0)
        r = await t3.req("ping")
        self.assertEqual((r["t"], t3.inbox), ("ok", []))

    async def test_claim_race_one_winner(self):
        a = await self.agent("eng-1")
        b = await self.agent("eng-2")
        ra, rb = await asyncio.gather(
            a.req("claim", paths=["engine/a.cpp", "engine/b.cpp"]),
            b.req("claim", paths=["engine/b.cpp", "ui/c.cpp"]))
        wins = [r for r in (ra, rb) if r["t"] == "ok"]
        losses = [r for r in (ra, rb) if r["t"] == "claim_denied"]
        self.assertEqual((len(wins), len(losses)), (1, 1))
        self.assertEqual(losses[0]["denied"][0]["path"], "engine/b.cpp")
        # all-or-nothing: the loser holds nothing
        loser = "eng-2" if ra["t"] == "ok" else "eng-1"
        self.assertNotIn(loser, self.broker.claims.values())

    async def test_directory_glob_claim_and_release(self):
        a = await self.agent("eng-1")
        b = await self.agent("eng-2")
        self.assertEqual((await a.req("claim", paths=["engine/mixer/**"]))["t"],
                         "ok")
        r = await b.req("claim", paths=["engine/mixer/pan.cpp"])
        self.assertEqual(r["denied"][0]["owner"], "eng-1")
        self.assertTrue(overlaps("src/**", "src/a/*.cpp"))
        self.assertFalse(overlaps("src/a/*.cpp", "src/b/*.cpp"))
        await a.req("release", paths=[])
        self.assertEqual((await b.req("claim", paths=["engine/mixer/pan.cpp"]))
                         ["t"], "ok")

    async def test_check_allow_deny(self):
        a = await self.agent("eng-1")
        b = await self.agent("eng-2")
        await a.req("claim", paths=["engine/x.cpp"])
        r = await a.req("check", path="engine/x.cpp")
        self.assertTrue(r["allow"])
        r = await b.req("check", path="engine/x.cpp")
        self.assertEqual((r["allow"], r["owner"]), (False, "eng-1"))
        r = await b.req("check", path="engine/free.cpp")
        self.assertFalse(r["allow"])
        self.assertIn("claim it first", r["reason"])
        # short hook-style connection without register
        c = await self.raw()
        r = await c.req("check", path="engine/x.cpp", name="eng-1",
                        token=self.broker.token)
        self.assertTrue(r["allow"])
        c2 = await self.raw()
        r = await c2.req("check", path="engine/x.cpp", name="eng-1",
                         token="bad")
        self.assertEqual(r["t"], "error")

    async def test_suspect_on_close(self):
        a = await self.agent("eng-1")
        await a.close()
        for _ in range(50):
            if any(k == P.EV_SUSPECT for k, _ in self.events):
                break
            await asyncio.sleep(0.02)
        self.assertIn((P.EV_SUSPECT, {"name": "eng-1"}), self.events)
        self.assertFalse(self.broker.is_alive("eng-1"))
        # the name can be taken again by the relaunch
        await self.agent("eng-1")

    async def test_restart_keeps_mail_claims_and_seq(self):
        token = self.broker.token
        self.broker.expect("eng-1", "tester-1")
        a = await self.agent("eng-1")
        await a.req("claim", paths=["engine/x.cpp"])
        await a.req("send", to="tester-1", type="note", body={"k": 1})
        self.broker.open_channel("f1", ["eng-1"])
        await a.close()
        await self.broker.stop()
        self.broker = await self.mk(token=token)
        self.assertEqual(self.broker.claims, {"engine/x.cpp": "eng-1"})
        self.assertIn("f1", self.broker.channels)
        t = await self.agent("tester-1")
        self.assertEqual((await t.pushes(1))[0]["body"], {"k": 1})
        a = await self.agent("eng-1")
        await a.req("send", to="tester-1", type="note")
        self.assertEqual((await t.pushes(2))[1]["seq"], 2)

    async def test_crash_safe_write(self):
        path = os.path.join(self.dir, "mail", "x", "0000000001-note-a.json")
        mailbox.atomic_write(path, b'{"old":1}')
        with mock.patch("os.replace", side_effect=OSError("crash")):
            with self.assertRaises(OSError):
                mailbox.atomic_write(path, b'{"new":')
        with open(path, "rb") as f:
            self.assertEqual(f.read(), b'{"old":1}')
        self.assertTrue(any(n.startswith(".tmp.") for n in
                            os.listdir(os.path.dirname(path))))
        # leftover temp files are invisible to readers
        self.assertEqual([m["old"] for m in
                          self.broker.box.unacked("x", 0)], [1])

    def test_loopback_only_bind(self):
        for host in ("0.0.0.0", "", "192.168.1.5", "localhost"):
            with self.assertRaises(ValueError):
                Broker(self.dir, host=host)

    async def test_bound_address_is_loopback(self):
        host, port = self.broker._server.sockets[0].getsockname()[:2]
        self.assertEqual(host, "127.0.0.1")
        self.assertNotEqual(port, 0)
        with open(os.path.join(self.dir, "broker.json")) as f:
            self.assertNotIn(self.broker.token, f.read())


if __name__ == "__main__":
    unittest.main()
