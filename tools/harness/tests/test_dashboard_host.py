"""The console posts from the owner's phone through `tailscale serve`
(2026-11-08): this machine's Tailscale name is accepted, any other host is
not."""
import os
import sys
import unittest

import support  # noqa: F401
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
import dashboard  # noqa: E402


class Host(unittest.TestCase):
    def ok(self, h, **kw):
        return dashboard.host_ok(h, hostname="DESKTOP-BNLF3FL", env=kw.get("env", {}))

    def test_local_names(self):
        self.assertTrue(self.ok("127.0.0.1:8765"))
        self.assertTrue(self.ok("localhost"))

    def test_this_machines_tailnet_name(self):
        self.assertTrue(self.ok("desktop-bnlf3fl.tail96e086.ts.net:8765"))

    def test_other_names_are_refused(self):
        for h in ("evil.example.com", "other-pc.tail96e086.ts.net",
                  "desktop-bnlf3fl.evil.com", "desktop-bnlf3fl.ts.net.evil.com", ""):
            self.assertFalse(self.ok(h), h)

    def test_extra_hosts_from_env(self):
        self.assertTrue(self.ok("dash.lan", env={"SONORA_DASH_HOSTS": "dash.lan, x"}))


if __name__ == "__main__":
    unittest.main()
