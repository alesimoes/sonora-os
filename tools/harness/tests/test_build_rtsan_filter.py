"""build-rtsan.sh must not drop a defined target as "not defined": a
`printf | grep -q` under pipefail failed by SIGPIPE on a long target list
(test_active_folder was skipped, then ctest could not find it)."""
import os
import re
import shutil
import subprocess
import unittest

SCRIPT = os.path.join(os.path.dirname(__file__), "..", "..", "build-rtsan.sh")


def _filter_function():
    with open(SCRIPT, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"^filter_existing_targets\(\) \{\n.*?^\}\n", text, re.S | re.M)
    return m.group(0)


@unittest.skipUnless(shutil.which("bash"), "needs bash")
class TargetFilter(unittest.TestCase):
    def test_every_defined_target_is_kept(self):
        prog = (
            "set -euo pipefail\n"
            "cmake() { echo 'The following are some of the valid targets:';"
            " for i in $(seq 1 20000); do echo \"... t$i\"; done;"
            " echo '... test_active_folder'; }\n"
            + _filter_function() +
            "echo \"KEPT:$(filter_existing_targets b t1 test_active_folder nope"
            " 2>&1 | tr '\\n' ' ')\"\n"
        )
        for _ in range(5):
            out = subprocess.run(["bash", "-s"], input=prog.encode(), capture_output=True)
            self.assertEqual(out.returncode, 0, out.stderr.decode())
            out = out.stdout.decode()
            self.assertIn("KEPT:t1 test_active_folder", out)
            self.assertIn("skipped", out)
            self.assertNotIn("test_active_folder not in", out)


if __name__ == "__main__":
    unittest.main()
