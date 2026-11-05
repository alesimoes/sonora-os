"""End to end: the real Broker (in a background loop, port 0) against the real
client library, harness.py hooks and harness_mcp.py over stdio."""
import asyncio
import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(TOOLS, "harness"))
sys.path.insert(0, os.path.dirname(TOOLS))

from client import Client, HarnessError  # noqa: E402
from tools.harness import protocol as P  # noqa: E402
from tools.harness.broker import Broker  # noqa: E402

TOKEN = "e2e-token"


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.clock = Clock()
        self.events = []
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()
        self.broker = Broker(os.path.join(self.tmp.name, ".harness"), token=TOKEN,
                             port=0, clock=self.clock, tick_s=3600,
                             on_event=lambda k, **d: self.events.append((k, d)))
        self.on_loop(self.broker.start())
        self.clients = []
        self.addCleanup(self.shutdown)

    def shutdown(self):
        for c in self.clients:
            c.close()
        self.on_loop(self.broker.stop())
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(5)
        self.loop.close()

    def on_loop(self, coro):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(10)

    def in_loop(self, fn, *a, **k):
        async def run():
            return fn(*a, **k)
        return self.on_loop(run())

    def client(self, name, token=TOKEN):
        c = Client(self.broker.port, name, token, ping=False, timeout=5, reconnect_window=0)
        c.connect()
        self.clients.append(c)
        return c

    def env(self, name, port=None):
        e = dict(os.environ)
        e.update(SONORA_HARNESS_PORT=str(port or self.broker.port), SONORA_AGENT_NAME=name,
                 SONORA_HARNESS_TOKEN=TOKEN)
        return e

    def cli(self, name, *args, stdin="", port=None):
        return subprocess.run([sys.executable, os.path.join(TOOLS, "harness.py"), *args],
                              input=stdin, capture_output=True, text=True, timeout=15,
                              env=self.env(name, port))

    def wait_inbox(self, c):
        for _ in range(100):
            if c._inbox:
                return
            threading.Event().wait(0.05)


class E2E(Base):
    def test_register_whoami_duplicate_and_bad_token(self):
        a = self.client("eng-1")
        self.assertEqual(a.whoami()["name"], "eng-1")
        with self.assertRaises(HarnessError):
            self.client("eng-1")
        with self.assertRaises(HarnessError):
            self.client("eng-2", token="wrong")

    def test_addressed_delivery_reaches_only_the_addressee(self):
        a, b, c = self.client("eng-1"), self.client("eng-2"), self.client("eng-3")
        a.send("eng-2", "ask", {"text": "which flag?"})
        got = b.recv(wait_s=3)
        self.assertEqual([(m["from"], m["type"], m["body"]["text"]) for m in got],
                         [("eng-1", "ask", "which flag?")])
        self.assertEqual(c.recv(), [])
        self.assertEqual(a.recv(), [])

    def test_channel_delivery_and_membership(self):
        a, b, c = self.client("eng-1"), self.client("eng-2"), self.client("eng-3")
        self.in_loop(self.broker.open_channel, "f1", ["eng-1", "eng-2"])
        a.send("feat/f1", "note", "hello team")
        self.assertEqual(b.recv(wait_s=3)[0]["body"], "hello team")
        self.assertEqual(a.recv(), [])  # not echoed to the sender
        self.assertEqual(c.recv(), [])
        with self.assertRaises(HarnessError):
            c.send("feat/f1", "note", "intruder")

    def test_ack_and_redelivery_after_reconnect(self):
        a, b = self.client("eng-1"), self.client("eng-2")
        a.send("eng-2", "note", "one")
        self.assertEqual([m["body"] for m in b.recv(wait_s=3)], ["one"])  # acked
        a.send("eng-2", "note", "two")  # reaches the socket, never recv'd
        self.wait_inbox(b)
        self.assertEqual(len(b._inbox), 1)
        b.close()
        for _ in range(100):  # the broker notices the closed socket
            if not self.broker.is_alive("eng-2"):
                break
            threading.Event().wait(0.05)
        a.send("eng-2", "note", "three")  # while b is away
        b2 = self.client("eng-2")
        self.assertEqual([m["body"] for m in b2.recv(wait_s=3)], ["two", "three"])
        b2.close()
        for _ in range(100):
            if not self.broker.is_alive("eng-2"):
                break
            threading.Event().wait(0.05)
        b3 = self.client("eng-2")
        self.assertEqual(b3.recv(), [])  # everything acked now

    def test_claim_race_has_one_winner(self):
        cs = [self.client("eng-%d" % i) for i in range(1, 7)]
        barrier = threading.Barrier(len(cs))
        results = {}

        def go(c):
            barrier.wait()
            try:
                c.claim(["engine/mixer.cpp"])
                results[c.name] = True
            except HarnessError:
                results[c.name] = False

        ts = [threading.Thread(target=go, args=(c,)) for c in cs]
        for t in ts:
            t.start()
        for t in ts:
            t.join(10)
        self.assertEqual(sum(results.values()), 1, results)
        winner = next(n for n, ok in results.items() if ok)
        loser = next(c for c in cs if c.name != winner)
        with self.assertRaises(HarnessError) as cm:
            loser.claim(["engine/*.cpp"])  # glob overlapping a held file
        self.assertIn(winner, str(cm.exception))

    def hook_in(self, path):
        return json.dumps({"tool_name": "Edit", "tool_input": {"file_path": path}})

    def test_check_hook_deny_and_allow(self):
        a = self.client("eng-1")  # connected: hooks must work beside the MCP link
        self.client("eng-2")
        a.claim(["src/a.cpp"])
        ok = self.cli("eng-1", "check", "--hook", stdin=self.hook_in("src/a.cpp"))
        self.assertEqual(ok.returncode, 0, ok.stderr)
        foreign = self.cli("eng-2", "check", "--hook", stdin=self.hook_in("src/a.cpp"))
        self.assertEqual(foreign.returncode, 2)
        self.assertIn("eng-1", foreign.stderr)
        free = self.cli("eng-2", "check", "--hook", stdin=self.hook_in("src/b.cpp"))
        self.assertEqual(free.returncode, 2)
        self.assertIn("claim", free.stderr)
        self.assertEqual(self.cli("eng-1", "check", "--hook", stdin="not json").returncode, 2)

    def test_check_hook_denies_when_broker_is_gone(self):
        r = self.cli("eng-1", "check", "--hook", stdin=self.hook_in("x"), port=1)
        self.assertEqual(r.returncode, 2)
        self.assertIn("broker", r.stderr)

    def test_inbox_hook_brief_output(self):
        a, b = self.client("eng-1"), self.client("eng-2")
        empty = self.cli("eng-2", "inbox", "--brief", "--hook")
        self.assertEqual((empty.returncode, empty.stdout), (0, ""))
        a.send("eng-2", "bug", {"text": "mixer clicks"})
        r = self.cli("eng-2", "inbox", "--brief", "--hook")
        self.assertEqual(r.returncode, 0, r.stderr)
        out = json.loads(r.stdout)["hookSpecificOutput"]
        self.assertEqual(out["hookEventName"], "PostToolUse")
        self.assertIn("bug from eng-1: mixer clicks", out["additionalContext"])
        self.assertEqual(self.cli("eng-2", "inbox", "--brief", "--hook").stdout, "")
        # The hook only shows; the MCP side still gets and acks the message.
        self.assertEqual(b.recv(wait_s=3)[0]["body"]["text"], "mixer clicks")

    def test_inbox_hook_silent_when_broker_is_gone(self):
        r = self.cli("eng-1", "inbox", "--brief", "--hook", port=1)
        self.assertEqual((r.returncode, r.stdout), (0, ""))

    def test_mcp_server_over_stdio(self):
        a = self.client("eng-1")
        argv = [sys.executable, os.path.join(TOOLS, "harness_mcp.py")]
        p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             text=True, env=self.env("eng-2"))
        self.addCleanup(p.wait)
        self.addCleanup(p.stdin.close)
        n = [0]

        def rpc(method, params=None):
            n[0] += 1
            msg = {"jsonrpc": "2.0", "id": n[0], "method": method, "params": params or {}}
            p.stdin.write(json.dumps(msg) + "\n")
            p.stdin.flush()
            return json.loads(p.stdout.readline())["result"]

        def call(tool, **args):
            r = rpc("tools/call", {"name": tool, "arguments": args})
            text = r["content"][0]["text"]
            return r.get("isError", False), (text if r.get("isError") else json.loads(text))

        info = rpc("initialize", {"protocolVersion": "2024-11-05"})
        self.assertEqual(info["serverInfo"]["name"], "harness")
        names = {t["name"] for t in rpc("tools/list")["tools"]}
        self.assertEqual(names, {"whoami", "send", "recv", "claim", "release"})
        self.assertEqual(call("whoami")[1]["name"], "eng-2")
        self.assertFalse(call("send", to="eng-1", type="note", body="from mcp")[0])
        self.assertEqual(a.recv(wait_s=3)[0]["body"], "from mcp")
        a.send("eng-2", "note", "to mcp")
        self.assertEqual(call("recv", wait_s=3)[1]["messages"][0]["body"], "to mcp")
        self.assertFalse(call("claim", paths=["x/y.py"])[0])
        with self.assertRaises(HarnessError):
            a.claim(["x/*.py"])  # the MCP server's claim is visible to others
        self.assertFalse(call("release", paths=["x/y.py"])[0])
        p.stdin.close()
        self.assertEqual(p.wait(10), 0)


class Gaps(Base):
    """Timers are driven through the injected clock and tick()."""

    def advance(self, s):
        self.clock.t += s
        self.in_loop(self.broker.tick)

    def test_needs_ack_redelivered_after_interval(self):
        self.client("lead")
        b = self.client("eng-2")
        self.in_loop(self.broker.send, "lead", "eng-2", "bug", {"text": "x"}, needs_ack=True)
        self.wait_inbox(b)
        self.assertEqual(len(b._inbox), 1)
        b._inbox.clear()
        b._seen.clear()  # a client that lost the message without acking
        self.advance(599)
        self.assertEqual(b.recv(wait_s=0.3), [])
        self.advance(2)
        self.assertEqual(len(b.recv(wait_s=3)), 1)  # redelivered, then acked
        self.advance(1000)
        self.assertEqual(self.broker.ack_wait, [])

    def test_unacked_twice_is_reported_to_lead(self):
        lead = self.client("lead")
        self.client("eng-2")
        self.in_loop(self.broker.send, "lead", "eng-2", "ask", {"text": "?"}, needs_ack=True)
        self.advance(601)
        self.advance(601)
        self.assertIn(P.EV_UNACKED, [k for k, _ in self.events])
        notes = [m for m in lead.recv(wait_s=3) if m["type"] == "note"]
        self.assertIn("eng-2", notes[0]["body"]["text"])

    def test_ask_gets_default_answer_after_interval(self):
        a = self.client("eng-1")
        self.client("lead")
        ask = a.send("lead", "ask", {"text": "A or B?", "default": "take A"})
        self.advance(1199)
        self.assertEqual(a.recv(wait_s=0.3), [])
        self.advance(2)
        ans = a.recv(wait_s=3)[0]
        self.assertEqual((ans["type"], ans["reply_to"], ans["body"]["text"], ans["from"]),
                         ("answer", ask["id"], "take A", "daemon"))

    def test_answered_ask_gets_no_default(self):
        a, lead = self.client("eng-1"), self.client("lead")
        ask = a.send("lead", "ask", {"text": "?"})
        lead.send("eng-1", "answer", "B", reply_to=ask["id"])
        self.assertEqual(a.recv(wait_s=3)[0]["body"], "B")
        self.advance(1300)
        self.assertEqual(a.recv(wait_s=0.3), [])

    def test_done_refused_while_blocking_messages_unacked(self):
        a, lead = self.client("eng-1"), self.client("lead")
        lead.send("eng-1", "bug", {"text": "regression"})
        with self.assertRaises(HarnessError) as cm:
            a.send("lead", "done", {"text": "finished"})
        self.assertIn("blocked", str(cm.exception))
        a.recv(wait_s=3)  # acknowledges the bug
        a.send("lead", "done", {"text": "finished"})
        self.assertEqual(lead.recv(wait_s=3)[-1]["type"], "done")


if __name__ == "__main__":
    unittest.main()
