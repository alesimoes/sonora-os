"""Tiny threaded stand-in for the real broker, enough to exercise the clients."""
import json
import socket
import threading

TOKEN = "tok"


class StubBroker:
    def __init__(self):
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(8)
        self.port = self.srv.getsockname()[1]
        self.claims = {}       # path -> owner
        self.mail = {}         # name -> list of msgs
        self.acked = {}
        self.hook_seen = {}
        self.registers = []    # (name, last_acked_seq or None)
        self.sent = []
        self.conns = []
        self._stop = False
        threading.Thread(target=self._accept, daemon=True).start()

    def put(self, name, seq, type="bug", body="x", frm="tester-1"):
        self.mail.setdefault(name, []).append(
            {"id": f"id{seq}", "seq": seq, "type": type, "from": frm, "body": body})

    def stop(self):
        self._stop = True
        # On Linux close() alone leaves the port listening while another
        # thread is blocked in accept(); shutdown wakes it and ends the listen.
        try:
            self.srv.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.srv.close()
        for c in self.conns:
            try:
                c.close()
            except OSError:
                pass

    def _accept(self):
        while not self._stop:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            self.conns.append(c)
            threading.Thread(target=self._serve, args=(c,), daemon=True).start()

    def _serve(self, c):
        name = None
        out = lambda d: c.sendall((json.dumps(d) + "\n").encode())  # noqa: E731
        try:
            for line in c.makefile("rb"):
                r = json.loads(line)
                op, rid = r["op"], r["rid"]
                if name is None and op in ("check", "peek"):
                    if r.get("token") != TOKEN:
                        out({"rid": rid, "ok": False, "error": "bad token"})
                        return
                    name = r["name"]
                if op == "register":
                    if r.get("token") != TOKEN:
                        out({"rid": rid, "ok": False, "error": "bad token"})
                        return
                    name = r["name"]
                    self.registers.append((name, r.get("last_acked_seq")))
                    since = r.get("last_acked_seq")
                    if since is None:
                        since = self.acked.get(name, 0)
                    pend = [m for m in self.mail.get(name, []) if m["seq"] > since]
                    out({"rid": rid, "ok": True, "feature": "f1", "pending": pend})
                elif op == "send":
                    self.sent.append((name, r))
                    out({"rid": rid, "ok": True, "id": "new"})
                elif op == "ack":
                    self.acked[name] = max(self.acked.get(name, 0), r["seq"])
                    out({"rid": rid, "ok": True})
                elif op == "claim":
                    taken = [p for p in r["paths"] if self.claims.get(p, name) != name]
                    if taken:
                        out({"rid": rid, "ok": False, "error": f"{taken[0]} claimed by {self.claims[taken[0]]}"})
                    else:
                        for p in r["paths"]:
                            self.claims[p] = name
                        out({"rid": rid, "ok": True})
                elif op == "release":
                    for p in r["paths"]:
                        if self.claims.get(p) == name:
                            del self.claims[p]
                    out({"rid": rid, "ok": True})
                elif op == "check":
                    owner = self.claims.get(r["path"])
                    if owner and owner != name:
                        out({"rid": rid, "ok": True, "allowed": False, "reason": f"claimed by {owner}"})
                    else:
                        out({"rid": rid, "ok": True, "allowed": True})
                elif op == "peek":
                    seen = self.hook_seen.get(name, self.acked.get(name, 0))
                    ms = [m for m in self.mail.get(name, []) if m["seq"] > seen]
                    if ms:
                        self.hook_seen[name] = ms[-1]["seq"]
                    out({"rid": rid, "ok": True, "messages": ms})
                elif op == "ping":
                    out({"rid": rid, "ok": True})
        except (OSError, ValueError):
            pass
