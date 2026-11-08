"""Action cards: propose, confirm, cancel, retry (owner, 2026-11-08).

A card is born `pending` from a console proposal that passed the whitelist
(console_verbs.VERBS). Only a `confirm` request, posted by the owner's tap,
runs it, once: `pending -> done | failed`. `cancelled` and `expired` never
run. Everything that happens to a card is in audit.jsonl and in the
daemon's events. Single writer: these functions run on the daemon's tick.
"""
from . import console_store as S
from . import protocol as P
from .console_verbs import VERBS

MAX_ACTIONS = 3


def check(verb, args):
    """(clean args, Card) or ValueError: the one gate every proposal passes."""
    v = VERBS.get(verb)
    if v is None:
        raise ValueError(f"'{verb}' is not an allowed action")
    if not isinstance(args, dict):
        raise ValueError("args must be an object")
    clean = v.clean(args)
    return clean, v.describe(clean)


def propose(hdir, verb, args, now, msg_id=None):
    """Stores a pending card; an older pending card on the same target is
    cancelled. Returns the record; ValueError when the whitelist refuses."""
    clean, card = check(verb, args)
    v = VERBS[verb]
    acts = S.load_actions(hdir)
    target = f"{verb.split('.')[0]}:{v.target(clean)}"
    for a in acts.values():
        if a["state"] == "pending" and a["target"] == target:
            _set(hdir, acts, a, "cancelled", "replaced by a newer card", now)
    rec = {"id": S.new_id("a"), "verb": verb, "args": clean, "title": card.title,
           "steps": list(card.steps), "effect": card.effect, "risk": v.risk,
           "target": target, "state": "pending", "created": P.now_iso(),
           "created_ts": now, "msg": msg_id, "result": ""}
    acts[rec["id"]] = rec
    S.save_actions(hdir, acts)
    S.audit(hdir, "proposed", rec["id"], verb=verb, args=clean)
    return rec


def _set(hdir, acts, rec, state, result, now):
    rec.update(state=state, result=result, updated=now)
    S.save_actions(hdir, acts)
    S.audit(hdir, state, rec["id"], verb=rec["verb"], result=result[:200])


def _refuse(orch, action_id, why):
    S.audit(orch.hdir, "refused", action_id, why=why)
    orch.store.event(None, "console_refused", f"{action_id}: {why}"[:200])
    S.add_message(orch.hdir, "system", why)


def confirm(orch, action_id):
    """Runs the card if, and only if, it is pending and fresh."""
    hdir, now = orch.hdir, orch.clock()
    acts = S.load_actions(hdir)
    rec = acts.get(action_id)
    if rec is None:
        return _refuse(orch, action_id, "ação desconhecida")
    if S.effective_state(rec, now) == "expired" and rec["state"] == "pending":
        _set(hdir, acts, rec, "expired", "not confirmed in 15 min", now)
        return _refuse(orch, action_id, "cartão expirado: peça de novo")
    if rec["state"] != "pending":
        return _refuse(orch, action_id, f"o cartão já está {rec['state']}")
    S.audit(hdir, "confirmed", action_id, verb=rec["verb"])
    try:
        result = VERBS[rec["verb"]].run(orch, rec["args"])
    except Exception as e:   # a failed action is a card state, not a crash
        _set(hdir, acts, rec, "failed", str(e), now)
        orch.store.event(None, "console_action_failed", f"{rec['verb']}: {e}"[:200])
        S.add_message(hdir, "system", f"FALHOU: {rec['title']}: {e}")
        return
    _set(hdir, acts, rec, "done", result, now)
    orch.store.event(None, "console_action", f"{rec['verb']}: {result}"[:200])
    S.add_message(hdir, "system", f"EXECUTADO: {rec['title']}")


def cancel(orch, action_id):
    acts = S.load_actions(orch.hdir)
    rec = acts.get(action_id)
    if rec is None or rec["state"] != "pending":
        return _refuse(orch, action_id, "só um cartão pendente pode ser cancelado")
    _set(orch.hdir, acts, rec, "cancelled", "owner", orch.clock())


def retry(orch, action_id):
    """A failed card becomes a new pending one; it still needs a confirm."""
    rec = S.load_actions(orch.hdir).get(action_id)
    if rec is None or rec["state"] != "failed":
        return _refuse(orch, action_id, "só um cartão que falhou pode ser repetido")
    new = propose(orch.hdir, rec["verb"], rec["args"], orch.clock(), rec.get("msg"))
    S.add_message(orch.hdir, "orch", "Novo cartão, aguardando o seu Confirmar.",
                  cards=[{"type": "action", "id": new["id"]}])


def sweep(orch):
    """Pending cards past their time become expired (so the log says so)."""
    acts, now = S.load_actions(orch.hdir), orch.clock()
    for rec in acts.values():
        if rec["state"] == "pending" and S.effective_state(rec, now) == "expired":
            _set(orch.hdir, acts, rec, "expired", "not confirmed in 15 min", now)
