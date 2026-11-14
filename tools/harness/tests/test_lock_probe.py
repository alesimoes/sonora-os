"""harness/lock_probe.py and the owner file supervisor_sys reads: which pid
of a heavy-lock entry a host may probe or kill, and when a holder is alive."""
import os
import shutil
import tempfile
import unittest

import support  # noqa: F401  (puts tools/ on sys.path)
from harness import lock_probe as P
from harness import supervisor_sys


def alive_set(*pids):
    return lambda pid: pid in pids


def containers(**running):
    return lambda name: running.get(name, False)


class ProbePid(unittest.TestCase):
    def test_windows_host_probes_the_windows_pid_only(self):
        e = {"kind": "win", "pid": 150963, "winpid": 11908}
        self.assertEqual(P.probe_pid(e, "win"), 11908)
        self.assertIsNone(P.probe_pid({"kind": "win", "pid": 150963}, "win"))

    def test_linux_host_probes_the_pid(self):
        self.assertEqual(P.probe_pid({"kind": "linux", "pid": 42}, "linux"), 42)

    def test_other_kind_or_init_is_never_probed(self):
        self.assertIsNone(P.probe_pid({"kind": "linux", "pid": 42}, "win"))
        self.assertIsNone(P.probe_pid({"kind": "linux", "pid": 1}, "linux"))


class EntryAlive(unittest.TestCase):
    def test_live_pid(self):
        e = {"kind": "win", "pid": 7, "winpid": 70}
        self.assertTrue(P.entry_alive(e, "win", alive_set(70), containers()))

    def test_dead_pid_without_container_is_dead(self):
        e = {"kind": "win", "pid": 7, "winpid": 70}
        self.assertFalse(P.entry_alive(e, "win", alive_set(), containers()))

    def test_running_or_unknown_container_keeps_it_alive(self):
        e = {"kind": "win", "pid": 7, "winpid": 70, "container": "c"}
        self.assertTrue(P.entry_alive(e, "win", alive_set(), containers(c=True)))
        self.assertTrue(P.entry_alive(e, "win", alive_set(), lambda n: None))
        self.assertFalse(P.entry_alive(e, "win", alive_set(), containers(c=False)))

    def test_unprobeable_entry_is_unknown(self):
        e = {"kind": "win", "pid": 7}
        self.assertIsNone(P.entry_alive(e, "win", alive_set(), containers()))


def created_at(**times):
    """created(pid) -> process creation time (epoch s) or None (gone)."""
    return lambda pid: times.get(f"p{pid}")


class KillEntry(unittest.TestCase):
    def test_kills_pid_and_container(self):
        killed, stopped = [], []
        e = {"kind": "win", "pid": 7, "winpid": 70, "container": "c",
             "start": 1000}
        self.assertTrue(P.kill_entry(e, "win", killed.append, stopped.append,
                                     created_at(p70=999.5)))
        self.assertEqual((killed, stopped), ([70], ["c"]))

    def test_dead_or_recycled_winpid_is_not_killed_but_container_is(self):
        # The holder's bash was tree-killed at the agent's time limit, its
        # container kept building, and Windows gave its pid to another
        # process (the daemon, Docker, another agent): taskkill /T on it
        # would kill that one.
        for created in (created_at(), created_at(p70=1500.0)):
            killed, stopped = [], []
            e = {"kind": "win", "pid": 7, "winpid": 70, "container": "c",
                 "start": 1000}
            self.assertTrue(P.kill_entry(e, "win", killed.append,
                                         stopped.append, created))
            self.assertEqual((killed, stopped), ([], ["c"]))

    def test_unknown_start_never_kills_the_pid(self):
        killed = []
        e = {"kind": "win", "pid": 7, "winpid": 70}
        self.assertFalse(P.kill_entry(e, "win", killed.append, killed.append,
                                      created_at(p70=1.0)))
        self.assertEqual(killed, [])

    def test_nothing_reachable(self):
        killed = []
        self.assertFalse(P.kill_entry({"kind": "win", "pid": 7}, "win",
                                      killed.append, killed.append,
                                      created_at()))
        self.assertEqual(killed, [])


class Created(unittest.TestCase):
    def test_own_process_was_created_before_now(self):
        t = supervisor_sys.process_created(os.getpid())
        if t is None:
            self.skipTest("no creation time on this host")
        self.assertLessEqual(t, __import__("time").time() + 1)
        self.assertGreater(t, 0)

    def test_missing_pid_has_no_creation_time(self):
        self.assertIsNone(supervisor_sys.process_created(2 ** 22 + 12345))


class OwnerFile(unittest.TestCase):
    def test_winpid_and_last_container_are_read(self):
        d = tempfile.mkdtemp(prefix="lock-")
        self.addCleanup(shutil.rmtree, d, True)
        os.makedirs(os.path.join(d, "slot-0"))
        with open(os.path.join(d, "slot-0", "owner"), "w") as f:
            f.write("pid=150963\nkind=win\nwinpid=11908\nstart=5\ncwd=/w\n"
                    "cmd=tools/micro-build.sh\ncontainer=outer\n"
                    "container=nested\n")
        e = supervisor_sys.RealSys(lock=d).lock_entries()[0]
        self.assertEqual((e["pid"], e["winpid"], e["container"]),
                         (150963, 11908, "nested"))


if __name__ == "__main__":
    unittest.main()
