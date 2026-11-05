import json
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(TOOLS, "harness"))

from stub_broker import StubBroker, TOKEN  # noqa: E402
from client import Client, HarnessError  # noqa: E402


def env(port, name="eng-1", token=TOKEN):
    e = dict(os.environ)
    e.update(SONORA_HARNESS_PORT=str(port), SONORA_AGENT_NAME=name, SONORA_HARNESS_TOKEN=token)
    return e


def cli(port, *args, stdin="", name="eng-1", token=TOKEN):
    return subprocess.run([sys.executable, os.path.join(TOOLS, "harness.py"), *args],
                          input=stdin, capture_output=True, text=True, timeout=15,
                          env=env(port, name, token))


class Base(unittest.TestCase):
    def setUp(self):
        self.b = StubBroker()
        self.addCleanup(self.b.stop)


class ClientTests(Base):
    def test_register_and_recv_acks(self):
        self.b.put("eng-1", 1)
        self.b.put("eng-1", 2)
        c = Client(self.b.port, "eng-1", TOKEN, ping=False)
        c.connect()
        self.assertEqual([m["seq"] for m in c.recv()], [1, 2])
        self.assertEqual(self.b.acked["eng-1"], 2)
        self.assertEqual(c.recv(), [])
        c.close()

    def test_bad_token_refused(self):
        with self.assertRaises(HarnessError):
            Client(self.b.port, "eng-1", "nope", ping=False).connect()

    def test_reconnect_sends_last_acked_seq(self):
        self.b.put("eng-1", 1)
        c = Client(self.b.port, "eng-1", TOKEN, ping=False)
        c.connect()
        c.recv()
        c._sock.close()
        c._alive = False
        self.b.put("eng-1", 2)
        c.send("lead", "ask", "hi")  # triggers reconnect
        self.assertEqual(self.b.registers[-1], ("eng-1", 1))
        self.assertEqual([m["seq"] for m in c.recv()], [2])
        c.close()

    def test_claim_conflict(self):
        a = Client(self.b.port, "eng-1", TOKEN, ping=False)
        a.connect()
        b = Client(self.b.port, "eng-2", TOKEN, ping=False)
        b.connect()
        a.claim(["x.py"])
        with self.assertRaises(HarnessError):
            b.claim(["x.py"])
        self.assertEqual(b.check("x.py"), (False, "claimed by eng-1"))
        a.release(["x.py"])
        b.claim(["x.py"])
        a.close()
        b.close()


class McpTests(Base):
    def start(self):
        p = subprocess.Popen([sys.executable, os.path.join(TOOLS, "harness_mcp.py")],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                             env=env(self.b.port))
        self.addCleanup(p.kill)
        return p

    def rpc(self, p, id, method, params=None):
        p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": id, "method": method,
                                  "params": params or {}}) + "\n")
        p.stdin.flush()
        return json.loads(p.stdout.readline())

    def test_handshake_and_tools(self):
        self.b.put("eng-1", 5, body={"text": "fix it"})
        p = self.start()
        r = self.rpc(p, 1, "initialize", {"protocolVersion": "2025-06-18"})["result"]
        self.assertEqual(r["protocolVersion"], "2025-06-18")
        self.assertIn("tools", r["capabilities"])
        p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        names = [t["name"] for t in self.rpc(p, 2, "tools/list")["result"]["tools"]]
        self.assertEqual(names, ["whoami", "send", "recv", "claim", "release"])

        def call(i, n, a=None):
            res = self.rpc(p, i, "tools/call", {"name": n, "arguments": a or {}})["result"]
            return res.get("isError", False), json.loads(res["content"][0]["text"]) \
                if not res.get("isError") else res["content"][0]["text"]

        err, who = call(3, "whoami")
        self.assertEqual((err, who["name"]), (False, "eng-1"))
        err, rv = call(4, "recv")
        self.assertEqual(rv["messages"][0]["seq"], 5)
        self.assertEqual(call(5, "recv")[1]["messages"], [])
        self.assertFalse(call(6, "send", {"to": "lead", "type": "ask", "body": "q"})[0])
        self.assertEqual(self.b.sent[0][1]["to"], "lead")
        self.assertFalse(call(7, "claim", {"paths": ["a"]})[0])
        self.assertFalse(call(8, "release", {"paths": ["a"]})[0])
        self.assertTrue(call(9, "send", {})[0])  # missing args -> isError, not a crash
        p.stdin.close()
        self.assertEqual(p.wait(10), 0)

    def test_exit_nonzero_when_broker_down(self):
        port = self.b.port
        self.b.stop()
        r = subprocess.run([sys.executable, os.path.join(TOOLS, "harness_mcp.py")],
                           input="", capture_output=True, text=True, timeout=15, env=env(port))
        self.assertNotEqual(r.returncode, 0)


class CliTests(Base):
    def test_inbox_hook_empty_prints_nothing(self):
        r = cli(self.b.port, "inbox", "--brief", "--hook")
        self.assertEqual((r.returncode, r.stdout), (0, ""))

    def test_inbox_hook_injects_message(self):
        self.b.put("eng-1", 17, body="broken test")
        r = cli(self.b.port, "inbox", "--brief", "--hook")
        out = json.loads(r.stdout)["hookSpecificOutput"]
        self.assertEqual(out["hookEventName"], "PostToolUse")
        self.assertIn("17 bug from tester-1: broken test", out["additionalContext"])
        self.assertEqual(cli(self.b.port, "inbox", "--brief", "--hook").stdout, "")

    def test_inbox_hook_broker_down_is_silent(self):
        port = self.b.port
        self.b.stop()
        r = cli(port, "inbox", "--brief", "--hook")
        self.assertEqual((r.returncode, r.stdout), (0, ""))

    def hook_in(self, path):
        return json.dumps({"tool_name": "Edit", "tool_input": {"file_path": path}})

    def test_check_hook_denies_foreign_claim(self):
        self.b.claims["src/a.cpp"] = "eng-2"
        r = cli(self.b.port, "check", "--hook", stdin=self.hook_in("src/a.cpp"))
        self.assertEqual(r.returncode, 2)
        self.assertIn("eng-2", r.stderr)
        self.assertEqual(cli(self.b.port, "check", "--hook", stdin=self.hook_in("src/b.cpp")).returncode, 0)

    def test_check_hook_broker_down_denies(self):
        port = self.b.port
        self.b.stop()
        r = cli(port, "check", "--hook", stdin=self.hook_in("src/a.cpp"))
        self.assertEqual(r.returncode, 2)
        self.assertIn("broker", r.stderr)

    def test_verbs(self):
        self.b.put("eng-1", 3)
        self.assertEqual(json.loads(cli(self.b.port, "whoami").stdout)["name"], "eng-1")
        self.assertEqual(cli(self.b.port, "claim", "f.py").returncode, 0)
        self.assertEqual(self.b.claims["f.py"], "eng-1")
        self.assertEqual(cli(self.b.port, "claim", "f.py", name="eng-2").returncode, 1)
        self.assertEqual(cli(self.b.port, "release", "f.py").returncode, 0)
        self.assertEqual(cli(self.b.port, "send", "lead", "ask", '{"text": "q"}').returncode, 0)
        self.assertEqual(self.b.sent[-1][1]["body"], {"text": "q"})
        self.assertEqual(json.loads(cli(self.b.port, "recv").stdout)[0]["seq"], 3)
        self.assertEqual(json.loads(cli(self.b.port, "recv").stdout), [])

    def test_missing_env_fails(self):
        e = {k: v for k, v in os.environ.items() if not k.startswith("SONORA_")}
        r = subprocess.run([sys.executable, os.path.join(TOOLS, "harness.py"), "whoami"],
                           capture_output=True, text=True, env=e, timeout=15)
        self.assertEqual(r.returncode, 1)


if __name__ == "__main__":
    unittest.main()
