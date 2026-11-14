"""harness/brokerhost: port and token survive a daemon restart, so agents
launched before it keep their hub (2026-11-08: WinError 10061 after every
self-update restart; the edit hook then denied every write)."""
import os
import socket
import stat
import tempfile
import unittest

import support  # noqa: F401  (sys.path)
from harness.brokerhost import BrokerHost, TOKEN_FILE, pinned


class Pin(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="pin-")
        self.hdir = os.path.join(self.tmp, ".harness")

    def tearDown(self):
        support.shutil.rmtree(self.tmp, ignore_errors=True)

    def test_second_host_in_the_same_dir_reuses_port_and_token(self):
        self.assertEqual(pinned(self.hdir), (0, None))
        a = BrokerHost(self.hdir)
        a.start()
        port, token = a.port, a.token
        a.stop()
        self.assertEqual(pinned(self.hdir), (port, token))
        if os.name != "nt":
            mode = stat.S_IMODE(os.stat(
                os.path.join(self.hdir, TOKEN_FILE)).st_mode)
            self.assertEqual(mode, 0o600)
        b = BrokerHost(self.hdir)
        b.start()
        try:
            self.assertEqual((b.port, b.token), (port, token))
        finally:
            b.stop()

    def test_taken_port_falls_back_to_a_free_one(self):
        a = BrokerHost(self.hdir)
        a.start()
        port, token = a.port, a.token
        a.stop()
        s = socket.socket()
        s.bind(("127.0.0.1", port))
        s.listen(1)
        try:
            b = BrokerHost(self.hdir)
            b.start()
            try:
                self.assertNotEqual(b.port, port)
                self.assertEqual(b.token, token)
                self.assertEqual(pinned(self.hdir), (b.port, token))
            finally:
                b.stop()
        finally:
            s.close()

    def test_pin_false_keeps_a_fresh_identity(self):
        a = BrokerHost(self.hdir)
        a.start()
        a.stop()
        b = BrokerHost(self.hdir, pin=False)
        b.start()
        try:
            self.assertNotEqual(b.token, a.token)
        finally:
            b.stop()


if __name__ == "__main__":
    unittest.main()
