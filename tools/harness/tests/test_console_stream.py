"""The console's Server-Sent Events stream (tools/dashboard_console_stream.py)."""
import http.client
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer

import support  # noqa: F401  (sets sys.path)

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
import dashboard_console_stream as stream  # noqa: E402
from harness import console_store as S  # noqa: E402


class Loop(unittest.TestCase):
    def run_loop(self, sigs, **kw):
        """Frames for a sequence of signatures, one per poll, then stop."""
        it = iter(sigs)
        cur = {"v": None, "left": len(sigs)}

        def sig():
            cur["v"] = next(it)
            return cur["v"]

        def running():
            cur["left"] -= 1
            return cur["left"] >= 0

        views = []

        def view():
            views.append(cur["v"])
            return {"sig": cur["v"]}

        out = list(stream.frames(view, sig, sleep=lambda s: None, running=running, **kw))
        return out, views

    def test_a_frame_at_once_and_one_per_change_only(self):
        out, views = self.run_loop(["a", "a", "b", "b", "b", "c"], heartbeat_s=1e9)
        self.assertEqual(views, ["a", "b", "c"])
        self.assertEqual(out[0], b'event: console\ndata: {"sig": "a"}\n\n')

    def test_a_quiet_stream_resends_the_view_on_the_heartbeat_only(self):
        t = iter(range(0, 1000, 10))
        out, views = self.run_loop(["a"] * 5, heartbeat_s=15, clock=lambda: next(t))
        # the clock steps 10 s and the beat is 15 s: fewer frames than ticks
        self.assertEqual(len(out), len(views))
        self.assertTrue(1 < len(out) < 5)
        self.assertTrue(all(o.startswith(b"event: console") for o in out))

    def test_the_signature_follows_the_chat_files(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        before = stream.file_signature(tmp)
        S.add_message(tmp, "user", "oi")
        after = stream.file_signature(tmp)
        self.assertNotEqual(before, after)
        S.add_message(tmp, "orch", "olá")
        self.assertNotEqual(after, stream.file_signature(tmp))
        S.set_partial(tmp, "m1", "meio")
        self.assertEqual(stream.file_signature(tmp)[2][1] > 0, True)

    def test_a_client_that_left_ends_the_stream_without_an_error(self):
        class Gone:
            def write(self, b):
                raise BrokenPipeError()

            def flush(self):
                pass
        self.assertTrue(stream.serve(Gone(), iter([b"x"])))

    def test_serve_writes_every_frame_and_flushes(self):
        class W:
            def __init__(self):
                self.data, self.flushes = b"", 0

            def write(self, b):
                self.data += b

            def flush(self):
                self.flushes += 1
        w = W()
        self.assertFalse(stream.serve(w, iter([b"a", b"b"])))
        self.assertEqual((w.data, w.flushes), (b"ab", 2))


class OverHttp(unittest.TestCase):
    """The real handler on a loopback socket: headers and the first frame."""

    def test_the_endpoint_streams_the_view_as_event_stream(self):
        import dashboard
        import dashboard_console_http as chttp
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        S.add_message(tmp, "user", "ping")
        old = (chttp.harness_dir, chttp.daemon_alive)
        chttp.harness_dir = lambda: tmp
        chttp.daemon_alive = lambda: True
        self.addCleanup(lambda: (setattr(chttp, "harness_dir", old[0]),
                                 setattr(chttp, "daemon_alive", old[1])))
        stop = threading.Event()
        orig = chttp.frames
        chttp.frames = lambda **kw: orig(running=lambda: not stop.is_set(), **kw)
        self.addCleanup(setattr, chttp, "frames", orig)
        srv = ThreadingHTTPServer(("127.0.0.1", 0), dashboard.Handler)
        srv.daemon_threads = True
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        self.addCleanup(stop.set)
        conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=10)
        conn.request("GET", "/api/console/stream")
        r = conn.getresponse()
        self.assertEqual((r.status, r.getheader("Content-Type")), (200, "text/event-stream"))
        self.assertEqual(r.readline(), b"event: console\n")
        data = json.loads(r.readline().decode().split("data: ", 1)[1])
        self.assertEqual(data["chat"]["messages"][-1]["text"], "ping")
        conn.close()


if __name__ == "__main__":
    unittest.main()
