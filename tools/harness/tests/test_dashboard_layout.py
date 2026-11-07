"""Layout contract of tools/dashboard.html: the status strip sits right below
the header, and the machine section carries its one-line summary."""
import os
import unittest

TOOLS = os.path.join(os.path.dirname(__file__), "..", "..")


def _read(*p):
    with open(os.path.join(TOOLS, *p), encoding="utf-8") as f:
        return f.read()


class DashboardLayout(unittest.TestCase):
    def test_strip_right_below_header(self):
        h = _read("dashboard.html")
        self.assertLess(h.index("</header>"), h.index('id="usage"'))
        self.assertLess(h.index('id="strip"'), h.index('id="sit"'))
        self.assertIn('id="pausebox"', h)

    def test_machine_summary_and_no_redundant_disk_heading(self):
        h = _read("dashboard.html")
        self.assertIn('id="syssum"', h)
        self.assertNotIn('id="diskhint"', h)

    def test_warning_thresholds_documented_in_js(self):
        j = _read("dashboard-static", "dash-disk.js")
        self.assertIn("LOW_FREE_PCT=15", j)
        self.assertIn("HIGH_RECLAIM=30e9", j)


if __name__ == "__main__":
    unittest.main()
