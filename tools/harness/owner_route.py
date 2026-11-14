"""Which "needs the owner" asks really need the owner (2026-11-13).

An agent's `needs_owner` (or a note the sweep reads as one, owner_infer)
put a task in waits_on_owner, and stall_check then left its feature alone
as "waits on the owner". On 2026-11-13 that list held 23 items and the owner
could act on six:
  * hardware runs (UMC1820, bare metal, KVM host, a physical stick): the
    owner ruled the same day that hardware-only validations are parked
    until the product is complete -> cause `hardware`, outside the list;
  * "restart Docker", "let the build queue drain", "re-queue the
    integrator": the daemon's and the coordinator's job -> `infra`;
  * "Not an owner decision", "(coordinator action)", and the daemon's own
    "third QA bounce" (testers' reasons, claims too narrow) -> `plan`.
Everything else stays the owner's. The stored `owner` text is kept for
blocks made before this rule: they are read through `cause`, never edited.
"""
import re

HARDWARE = "hardware"
LEGACY_BOUNCE = "third QA bounce, no recommended default"
LABEL = {HARDWARE: "Parked hardware run", "infra": "Harness/host action",
         "plan": "Plan action"}

_HW = re.compile(r"\bhardware\b|\bUMC ?1820\b|\bbare[- ]metal\b|\bKVM\b"
                 r"|\bphysical (?:USB )?stick\b|\bappliance\b|\bpower[- ]pull\b",
                 re.I)
# A hardware ask that also carries a design or licence question is still
# the owner's: only that question can unblock it.
_ALSO_DECIDE = re.compile(r"\b(?:and|also)\s+decide\b|\bdecide\s+whether\b"
                          r"|\blicen[cs]e\b", re.I)
_INFRA = re.compile(r"\brestart(?:ing)?\s+(?:the\s+)?docker\b"
                    r"|\bdocker\s+(?:desktop|engine)\b|\bqueue\s+(?:to\s+)?drain"
                    r"|\bre-?queue\s+the\s+integrator\b", re.I)
_PLAN = re.compile(r"\bnot\s+an?\s+owner\s+decision\b|\bcoordinator\s+action\b"
                   r"|^\s*coordinator\b", re.I)


def cause(ask, note=""):
    """None when `ask` names a real owner ruling (or is empty); else the
    cause it really is: hardware | infra | plan."""
    text = (ask or "").strip()
    if not text:
        return None
    if text == LEGACY_BOUNCE:
        return "plan"
    if _HW.search(text) and not _ALSO_DECIDE.search(text):
        return HARDWARE
    if _INFRA.search(text):
        return "infra"
    if _PLAN.search(text):
        return "plan"
    return None


def real_ask(ask, note=""):
    """`ask` when it is the owner's, else None."""
    return ask if ask and cause(ask, note) is None else None


def route(ask, why):
    """(owner, cause, note) for a block whose agent asked `ask`: a real ask
    stays the owner's; any other keeps its words in the note, where the
    agent that picks it up reads them."""
    c = cause(ask, why)
    if c is None:
        return (ask or None), None, why
    return None, c, f"{why}\n{LABEL[c]}: {ask}"
