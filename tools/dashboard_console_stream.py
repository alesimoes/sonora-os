"""Server-Sent Events for the console (owner, 2026-11-08: the answer must
appear the moment it exists, not on the next 3 s poll).

GET /api/console/stream keeps the connection open and writes one
`event: console` frame, whose data is the same JSON as GET /api/console,
whenever something the screen shows changed: chat.jsonl, actions.json, the
half-written answer, the broker log (agent replies) or the daemon snapshot.
A comment line every HEARTBEAT_S keeps proxies and the page's watchdog quiet.
The page falls back to polling when the stream drops.

Everything with I/O or time is injected, so the loop is tested without a
socket or a sleep.
"""
import json
import os
import time

HEARTBEAT_S = 15.0
POLL_S = 0.25
FILES = (("console", "chat.jsonl"), ("console", "actions.json"),
         ("console", "partial.json"), ("log", "messages.jsonl"))


def file_signature(hdir):
    """(mtime_ns, size) of each file the screen is built from; missing = None."""
    out = []
    for parts in FILES:
        try:
            st = os.stat(os.path.join(hdir, *parts))
            out.append((st.st_mtime_ns, st.st_size))
        except OSError:
            out.append(None)
    return tuple(out)


def frame(view):
    return b"event: console\ndata: " + json.dumps(view).encode() + b"\n\n"


def frames(view_fn, sig_fn, sleep=time.sleep, running=lambda: True,
           clock=time.monotonic, heartbeat_s=HEARTBEAT_S, poll_s=POLL_S):
    """Yields bytes: a console frame at once and after every change of
    sig_fn(), the same frame again as a heartbeat when quiet. Ends when running() is false."""
    last_sig, last_sent = object(), clock()
    while running():
        sig = sig_fn()
        if sig != last_sig:
            last_sig = sig
            last_sent = clock()
            yield frame(view_fn())
        elif clock() - last_sent >= heartbeat_s:
            # A full frame, not a comment: it also keeps the ages on screen
            # ("vivo 44 s", "26 min") moving while nothing else changes.
            last_sent = clock()
            yield frame(view_fn())
        sleep(poll_s)


def serve(wfile, gen):
    """Writes frames until the client goes away; True when it did."""
    try:
        for chunk in gen:
            wfile.write(chunk)
            wfile.flush()
    except (BrokenPipeError, ConnectionError, OSError):
        return True
    return False
