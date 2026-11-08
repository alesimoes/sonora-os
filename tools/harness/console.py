"""The owner's console on the daemon (owner, 2026-11-08).

The dashboard posts requests (console_door); the daemon, which alone writes
.harness/console/, does the rest on its tick:
- an owner message for an agent is an ownerchat ask (the agent answers in
  the conversation, see console_chat);
- an owner message for the orchestrator is answered by a short `console`
  run (.claude/agents/console.md, a wake task like the lead's) that reads
  the exported state and replies with `send(to=daemon, type=console_reply)`;
- a reply carries text, an optional list and at most MAX_ACTIONS proposed
  actions. The daemon checks each against the whitelist and turns it into a
  card whose wording it writes itself; nothing runs until the owner taps
  Confirmar (console_actions.confirm).
"""
from . import console_actions as A
from . import console_session as session
from . import console_store as S
from . import ownerchat
from . import protocol as P

KIND = "console_request"
MAX_LAUNCHES = 2          # runs per owner message before giving up
MAX_LIST_ROWS = 30
BRIEF = ("Answer the owner's unanswered console messages: run "
         "`python tools/console_read.py inbox`, read what you need with the "
         "other console_read.py commands, and send one console_reply per "
         "message (see .claude/agents/console.md). Then stay alive: wait for "
         "the next owner_ask with recv and answer it the same way, until one "
         "says end (then write result.json and stop). Never answer one "
         "req twice. For a slow answer you may first send console_reply "
         "{req, text, partial: true}; the screen shows it until the final "
         "reply.")


# -- requests from the dashboard (called by ownerchat.tick) -------------
def handle_request(orch, req):
    kind = req.get("kind")
    if kind == "console":
        _owner_message(orch, req)
    elif kind == "confirm":
        A.confirm(orch, str(req.get("action_id")))
    elif kind == "cancel":
        A.cancel(orch, str(req.get("action_id")))
    elif kind == "retry":
        A.retry(orch, str(req.get("action_id")))


def _owner_message(orch, req):
    to, text = req.get("to"), str(req.get("text") or "")
    msg = S.add_message(orch.hdir, "user", text, to=to)
    if not to:
        return    # tick() starts the console run
    run = orch.store.last_run_of_name(to) if P.valid_name(str(to)) else None
    if run is None:
        S.add_message(orch.hdir, "system", f"{to} não é um agente deste orquestrador.",
                      req=msg["id"])
        return
    ownerchat.ask(orch, run, text)


# -- the run that answers ----------------------------------------------
def _busy(orch):
    if any(r["name"] == "console" for r in orch.store.active_runs()):
        return True
    return any(w["agent"] == "console" and w["state"] == "queued"
               for w in orch.store.tasks(kind="wake"))


def tick(orch, now):
    A.sweep(orch)
    pending = S.unanswered(orch.hdir)
    if session.is_active(orch):
        session.feed(orch, pending, now)
        return
    if not pending or _busy(orch):
        return
    first = pending[0]
    key = f"console_tries:{first['id']}"
    tries = int(orch.store.get_meta(key, "0") or 0)
    if tries >= MAX_LAUNCHES:
        S.add_message(orch.hdir, "system", "Não consegui responder a esta mensagem; "
                      "tente de novo ou pergunte a um agente.", req=first["id"])
        return
    orch.store.set_meta(key, str(tries + 1))
    session.start(orch, pending, now)
    n =orch.store.next_counter("wake")
    wid = f"wake-console-{KIND}-{n}"
    orch.store.add_task(wid, None, "console", kind="wake", brief=BRIEF)
    orch.store.set_task(wid, event=KIND)
    orch.store.event(wid, "console_run", f"for {first['id']}")


# -- the run's reply ----------------------------------------------------
def _list_card(raw):
    if not isinstance(raw, dict) or not isinstance(raw.get("rows"), list):
        return None
    rows = []
    for r in raw["rows"][:MAX_LIST_ROWS]:
        if isinstance(r, str):
            r = {"text": r}
        if isinstance(r, dict) and r.get("text"):
            rows.append({"text": str(r["text"])[:200],
                         **({"id": str(r["id"])[:64]} if r.get("id") else {})})
    return {"type": "list", "title": str(raw.get("title") or "")[:80], "rows": rows}


def _action_cards(orch, raw, req_id):
    cards, refused = [], []
    for a in (raw if isinstance(raw, list) else [])[:A.MAX_ACTIONS]:
        verb = a.get("verb") if isinstance(a, dict) else None
        try:
            rec = A.propose(orch.hdir, str(verb), a.get("args") or {},
                            orch.clock(), req_id)
        except ValueError as e:
            refused.append(f"{verb}: {e}")
            S.audit(orch.hdir, "refused", "-", verb=str(verb), why=str(e)[:200])
            continue
        cards.append({"type": "action", "id": rec["id"]})
    return cards, refused


def _seen(orch, src):
    return any(m.get("src") == src for m in S.history(orch.hdir))


def on_reply(orch, msg):
    """Called from the daemon for every console_reply message it receives."""
    body = msg.get("body")
    if not isinstance(body, dict) or _seen(orch, msg.get("id")):
        return
    open_ids = [m["id"] for m in S.unanswered(orch.hdir)]
    req_id = str(body.get("req") or "")
    if req_id not in open_ids and open_ids:
        req_id = open_ids[0]    # a wrong id must not leave the message open
    if body.get("partial"):
        S.set_partial(orch.hdir, req_id, str(body.get("text") or ""))
        return
    S.clear_partial(orch.hdir)
    cards = []
    lst = _list_card(body.get("list"))
    if lst:
        cards.append(lst)
    acts, refused = _action_cards(orch, body.get("actions"), req_id)
    text = str(body.get("text") or "").strip() or "(sem texto)"
    if refused:
        text += "\n\nRecusei: " + "; ".join(refused)
    S.add_message(orch.hdir, "orch", text, req=req_id or None,
                  cards=(cards + acts) or None, src=msg.get("id"))
