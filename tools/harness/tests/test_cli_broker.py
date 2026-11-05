"""The CLI fallback against the real broker: it must not take the agent's
name, and its sends carry the run's feature and task."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

import support
from harness.brokerhost import BrokerHost
from harness.client import Client

TOOLS = support.TOOLS


class CliBrokerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="orch-cli-")
        self.host = BrokerHost(os.path.join(self.tmp, ".harness"))
        self.host.start()
        self.host.expect("eng-1", "lead")

    def tearDown(self):
        self.host.stop()

    def cli(self, *args):
        env = dict(os.environ, SONORA_AGENT_NAME="eng-1",
                   SONORA_HARNESS_PORT=str(self.host.port),
                   SONORA_HARNESS_TOKEN=self.host.token,
                   SONORA_FEATURE="feat-a", SONORA_TASK="a")
        return subprocess.run([sys.executable, os.path.join(TOOLS, "harness.py"),
                               *args], env=env, capture_output=True, text=True,
                              timeout=60)

    def test_cli_beside_the_agents_own_connection(self):
        mcp = Client(self.host.port, "eng-1", self.host.token, ping=False)
        mcp.feature, mcp.task = "feat-a", "a"  # as from_env sets them
        mcp.connect()  # the MCP server owns the name
        try:
            r = self.cli("send", "lead", "note", '{"text": "hello"}')
            self.assertEqual(r.returncode, 0, r.stderr)
            r = self.cli("claim", "a/**")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(self.host.broker.claims, {"a/**": "eng-1"})
            # the registered connection is still the MCP one and still works
            self.assertTrue(self.host.broker.is_alive("eng-1"))
            mcp.send("lead", "note", "from mcp")
            msgs = self.host.broker.box.unacked("lead", 0)
            self.assertEqual([(m["feature"], m["task"]) for m in msgs],
                             [("feat-a", "a"), ("feat-a", "a")])
            self.host.tell("eng-1", "note", {"text": "ping"})
            r = self.cli("recv")
            self.assertEqual([m["body"]["text"] for m in json.loads(r.stdout)],
                             ["ping"])
        finally:
            mcp.close()


if __name__ == "__main__":
    unittest.main()
