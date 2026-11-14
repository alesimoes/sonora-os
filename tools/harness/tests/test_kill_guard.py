"""kill_tree never signals init or the daemon's own process group
(2026-11-08: a fake run with pid=1 made the supervisor killpg the CI
runner; the Harness job hung 20 minutes three times with no log)."""
import os
import unittest
from unittest import mock

import support  # noqa: F401  (sets sys.path)
from harness import launcher, proctree


class KillGuard(unittest.TestCase):
    def test_init_and_self_are_never_signalled(self):
        for kill in (launcher.kill_tree, proctree.kill_tree):
            with mock.patch("os.kill") as k, \
                    mock.patch("subprocess.run") as run:
                if hasattr(os, "killpg"):
                    with mock.patch("os.killpg") as kpg:
                        for pid in (0, 1, os.getpid()):
                            kill(pid)
                        kpg.assert_not_called()
                else:
                    for pid in (0, 1, os.getpid()):
                        kill(pid)
                k.assert_not_called()
                run.assert_not_called()

    @unittest.skipIf(os.name == "nt", "process groups are POSIX")
    def test_a_child_in_our_group_is_killed_alone(self):
        with mock.patch("os.getpgid", return_value=os.getpgrp()), \
                mock.patch("os.killpg") as kpg, mock.patch("os.kill") as k:
            launcher.kill_tree(424242)
            proctree.kill_tree(424242)
        kpg.assert_not_called()
        self.assertEqual(k.call_count, 2)


if __name__ == "__main__":
    unittest.main()
