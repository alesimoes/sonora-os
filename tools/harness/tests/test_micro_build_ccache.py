"""micro-build.sh must make the shared ccache volume writable by the caller
uid on Linux, or the plan-only RTSan job dies with "ccache: Permission denied"."""
import os
import unittest

SCRIPT = os.path.join(os.path.dirname(__file__), "..", "..", "micro-build.sh")


class CcacheVolumeOwner(unittest.TestCase):
    def test_named_volume_is_chowned_to_caller(self):
        with open(SCRIPT, encoding="utf-8") as f:
            text = f.read()
        self.assertIn("-v sonora-ccache:/ccache --entrypoint chown", text)
        self.assertIn('"$(id -u):$(id -g)" /ccache', text)


if __name__ == "__main__":
    unittest.main()
