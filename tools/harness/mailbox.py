"""Durable log and per-recipient mailbox under .harness/ (ADR-0047 4b).

Only the broker writes here. Every file appears atomically: written to a
".tmp.*" sibling, fsynced, then os.replace()d, so a crash leaves either the
old state or the new one, never a partial file. Readers ignore ".tmp.*".
"""
import json
import os
import secrets
import time


def atomic_write(path, data):
    d = os.path.dirname(path)
    os.makedirs(d, exist_ok=True)
    tmp = os.path.join(d, f".tmp.{os.getpid()}.{secrets.token_hex(4)}")
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    _fsync_dir(d)


def _fsync_dir(d):
    # Not supported on Windows; the rename is still atomic there.
    try:
        fd = os.open(d, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)


class Mailbox:
    def __init__(self, root):
        self.root = root
        self.mail = os.path.join(root, "mail")
        self.log_path = os.path.join(root, "log", "messages.jsonl")
        os.makedirs(self.mail, exist_ok=True)
        os.makedirs(os.path.dirname(self.log_path), exist_ok=True)

    def sweep_tmp(self, older_than_s=3600):
        cutoff = time.time() - older_than_s
        for dp, _, files in os.walk(self.root):
            for f in files:
                p = os.path.join(dp, f)
                if f.startswith(".tmp.") and os.path.getmtime(p) < cutoff:
                    os.remove(p)

    def _dir(self, name):
        return os.path.join(self.mail, name)

    def put(self, name, msg):
        fn = f"{msg['seq']:010d}-{msg['type']}-{msg['id']}.json"
        atomic_write(os.path.join(self._dir(name), fn),
                     json.dumps(msg, sort_keys=True).encode())

    def append_log(self, msg):
        with open(self.log_path, "ab") as f:
            f.write(json.dumps(msg, sort_keys=True).encode() + b"\n")
            f.flush()
            os.fsync(f.fileno())

    def names(self):
        return sorted(n for n in os.listdir(self.mail)
                      if os.path.isdir(self._dir(n)))

    def _files(self, name):
        d = self._dir(name)
        if not os.path.isdir(d):
            return []
        return sorted(f for f in os.listdir(d)
                      if f.endswith(".json") and not f.startswith("."))

    def max_seq(self, name):
        fs = self._files(name)
        return int(fs[-1].split("-", 1)[0]) if fs else 0

    def get_acked(self, name):
        try:
            with open(os.path.join(self._dir(name), ".acked")) as f:
                return int(f.read().strip() or 0)
        except (OSError, ValueError):
            return 0

    def set_acked(self, name, seq):
        if seq > self.get_acked(name):
            atomic_write(os.path.join(self._dir(name), ".acked"),
                         str(seq).encode())

    def unacked(self, name, after):
        """Messages with seq > after, in seq order."""
        out = []
        for f in self._files(name):
            if int(f.split("-", 1)[0]) > after:
                with open(os.path.join(self._dir(name), f), "rb") as fh:
                    out.append(json.loads(fh.read()))
        return out

    def save_state(self, fname, obj):
        atomic_write(os.path.join(self.root, fname),
                     json.dumps(obj, sort_keys=True, indent=1).encode())

    def load_state(self, fname, default):
        try:
            with open(os.path.join(self.root, fname), "rb") as f:
                return json.loads(f.read())
        except (OSError, ValueError):
            return default
