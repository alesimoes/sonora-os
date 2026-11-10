"""Model tiers (owner, 2026-11-06): the model follows the task's grade, not
the agent. junior=haiku, pleno=sonnet, senior=opus, principal=fable.

A plan slice carries `tier` (a name) and/or `grade` ("S/low"); routing.toml
[tiers] maps grades to models and may rename the tier models. Pure functions;
the launcher is the only place that turns the answer into `--model`.
"""
DEFAULT_MODEL = "sonnet"
ALLOWED_MODELS = ("haiku", "sonnet", "opus", "fable")
TIER_MODELS = {"junior": "haiku", "pleno": "sonnet", "senior": "opus",
               "principal": "fable"}
ORDER = ("haiku", "sonnet", "opus", "fable")
SENIOR = TIER_MODELS["senior"]


def model_of_tier(tier, routing=None):
    """Model for a tier name or an "S/low" grade; None when unknown."""
    if not isinstance(tier, str):
        return None
    tier = tier.strip()
    table = routing.tiers() if routing else {}
    for key in (tier, tier.lower()):
        m = table.get(key)
        if m in ALLOWED_MODELS:
            return m
    return TIER_MODELS.get(tier.lower())


def task_model(routing, tier=None, grade=None):
    """The grade wins over a tier name; no tier or grade means sonnet.
    Owner 2026-11-09: the model follows the risk, read from the grade
    through routing.toml; 40 slices carried a "senior" tier picked by size
    under the old table and ran on opus (5 of 6 agents at once)."""
    return (model_of_tier(grade, routing) or model_of_tier(tier, routing)
            or DEFAULT_MODEL)


def at_least(model, floor):
    return model if ORDER.index(model) >= ORDER.index(floor) else floor


def run_model(routing, kind, tier=None, grade=None, senior_round=False):
    """Model for one run. Reviews and QA never run below `review_min`; the
    senior round of an escalation lifts a fixer to opus at least. The
    principal run is not decided here (launcher: always fable)."""
    model = task_model(routing, tier, grade)
    floor = (routing.tiers().get("review_min") if routing else None) \
        or DEFAULT_MODEL
    if kind == "integrator":
        # Merging a conflict is mechanical whatever the slice's risk: an
        # opus integrator on a contract slice cost several dollars a run
        # (owner 2026-11-09). A stuck one climbs through the stall
        # resolver's pin, not through the grade.
        return floor if floor in ORDER else DEFAULT_MODEL
    if kind in ("tester", "planner"):
        return at_least(model, floor if floor in ORDER else DEFAULT_MODEL)
    if senior_round:
        return at_least(model, SENIOR)
    return model
