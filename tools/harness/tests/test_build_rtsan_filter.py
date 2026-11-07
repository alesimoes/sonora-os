"""build-rtsan.sh must not drop a defined target as "not defined": a
`printf | grep -q` under pipefail failed by SIGPIPE on a long target list
(test_active_folder was skipped, then ctest could not find it)."""
import os
import re
import shutil
import subprocess
import unittest

SCRIPT = os.path.join(os.path.dirname(__file__), "..", "..", "build-rtsan.sh")


def _filter_block():
    with open(SCRIPT, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"    rtsan_targets=\(\)\n(.*?)    cmake --build \"\$build\" -j\"\$\(nproc\)\" --target", text, re.S)
    return "rtsan_targets=()\n" + m.group(1)


@unittest.skipUnless(shutil.which("bash"), "needs bash")
class TargetFilter(unittest.TestCase):
    def test_every_defined_target_is_kept(self):
        prog = (
            "set -euo pipefail\n"
            "defined=(); for i in $(seq 1 20000); do defined+=(t$i); done\n"
            "defined+=(test_active_folder)\n"
            "wanted=(t1 test_active_folder nope)\n" + _filter_block() +
            'echo "KEPT:${rtsan_targets[*]}"\n'
        )
        for _ in range(5):
            out = subprocess.run(["bash", "-s"], input=prog.encode(), capture_output=True)
            self.assertEqual(out.returncode, 0, out.stderr.decode())
            out = out.stdout.decode()
            self.assertIn("KEPT:t1 test_active_folder", out)
            self.assertIn("skip nope", out)
            self.assertNotIn("skip test_active_folder", out)


if __name__ == "__main__":
    unittest.main()
