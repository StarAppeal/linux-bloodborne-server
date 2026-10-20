# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""How long the rig's runs may take, and which runs make up each tier (Kaniel 2026-10-15: "add
tests for the recent bugs, keep scenarios short, and use lanes to run rigs in parallel").

- A scenario's timeout is at most SCENARIO_MAX_S, a suite chapter's budget at most CHAPTER_MAX_S,
  plus BOOT_S for each time a game in it starts again (a restart's boot, title and join: rig
  2026-10-15, restart chapters took 120-145 s). The game gives up a chapter still going after its
  budget (chapter <name> <seconds>), so one stuck chapter costs its budget, not the suite.
- No fixed wait over WAIT_MAX_S: a sync, or a wait on a condition (wait-members, wait-vital,
  wait-flag, wait-map...) instead. A wait is how long something might take, not how long it
  usually does.
- Whatever cannot fit is in LONG, the tier run before a local release, with its reason.

Tiers (run_scenario.py --tier <name> [more names]):
- quick: smoke, plus the suites of the code being changed (named after it), on every change.
- full: every suite, before a PR merges into server-coop.
- long: LONG, before a local release.
"""
import re

SCENARIO_MAX_S = 300
CHAPTER_MAX_S = 120
WAIT_MAX_S = 30
# What a game's start costs before its script's own steps count: the boot, the title, the team
# and the join (the rig's join scenarios pass in 72-99 s, join3 once in 135 s). A game started
# again (a stage) pays it again.
BOOT_S = 100
# A suite's games boot and join once (its join chapter's budget is its own); this is the rest.
SUITE_BOOT_S = 150

# Single scenarios that do not fit the budget (their own boot plus fixed waits over 30 s, or
# runs measured near or over 300 s), each kept for the long tier until it becomes a suite
# chapter. Most are older versions of a suite chapter (named after the arrow).
LONG = {
    "story-hostless": "the 10-20 case #286: a team join and a trip",
    "key-items-kept": "the 10-19 playtest case #267: a join, two trips",
    "team-world-other-block": "the 10-19 playtest case #266: two team sessions",
    "npc-progress-after-trip": "the 10-19 playtest case #269: a join, two trips",
    "wipe-one-map": "the 10-19 playtest case #270: three games, a death and a wake",
    "boss-hp-after-wipe": "the 10-19 playtest case #272: a wipe and a wake",
    "kill-reaches-arrival": "the 10-19 playtest case #278: three games, two trips",
    "soak": "a long session by design",
    "chalice-team": "two rituals and two dungeon loads, measured 313 s",
    "chalice-altar-host": "a ritual and two dungeon loads, measured 315 s",
    "chalice-altar-team": "a ritual and two dungeon loads, measured 315 s",
    "chalice-altar-no-chalice": "a ritual and two dungeon loads, measured 353 s",
    "chalice-root-team": "a root ritual and two dungeon loads, waits for the ritual",
    "guest-drops": "a 70 s wait for the drops (Kaniel's playtest 2026-10-16)",
    "boss-guest-runs": "50 s and 90 s waits by the boss (Kaniel's playtest 2026-10-16)",
    "boss-whole-fight": "a lamp trip, then 70 s of fixed waits in the fight (#292)",
    "cutscene-skip": "two stages (two boots), the opening cutscene played in between",
    "dead-in-host-save": "two stages (two boots): the host's game starts again after its kill",
    "team-load-beside-host": "two stages (two boots), a 45 s wait for the guest to leave the Dream",
    "team-world-pickups": "two stages (two boots): the guest's save takes the team world again",
    "ward-door": "a 420 s wait for the Ward door",
    "save-backups": "waits out the save backups' 150 s timer",
    "hit3": "measured 277-304 s; suite chapter: smoke/hit3, party3/hit3",
    "trip3": "measured 235-278 s; suite chapter: party3/trip3",
    "guest-trip3": "a 150 s wait; suite chapter: party3/guest-trip3",
    "hp-together": "measured 226-261 s; suite chapter: together/hp-together",
    "guest-hit-once": "a 120 s wait; suite chapter: trips/guest-hit-once",
    "guest-lamp-rest": "a 200 s wait for the rest",
    "guest-talk-gilbert": "a 230 s wait for the talk",
    "guest-talk-by-the-lamp": "a 140 s wait; suite chapter: together/talk-by-the-lamp",
    "away-unseen3": "60 s waits, three games",
    "iosefka-gift": "a 90 s wait for her gift",
    "iosefka-look": "a 60 s wait for pictures",
    "guest-alone-mobs": "a 75 s wait for the mob",
    "guest-far-positions": "a 75 s wait for the mob",
    "guest-alone-hits": "a 75 s wait for the mob",
    "guests-pair-mobs": "an 80 s wait for the mob",
    "guest-mobs-host-comes": "40 s waits for the mob",
    "guest-hits-host-comes": "a 50 s wait for the mob",
    "guest-mobs-host-near": "40 s waits for the mob",
    "guest-acts-for-two": "a 70 s wait for the mob",
    "guest-talk-at-lamp": "an 80 s wait for the talk",
    "early-trip": "a 60 s wait",
    "ward-npc": "a 40 s wait; suite chapter: trips/ward-npc",
    "clinic": "a 40 s wait",
    "guest-flags-stay": "a 45 s wait; suite chapter: together/guest-flags-stay",
    "hp-apart": "a 55 s wait; suite chapter: trips/hp-apart",
    "boss-kill": "a 90 s wait; suite chapter: together/boss-kill",
    "boss-refight": "a 90 s wait; suite chapter: together/boss-refight",
    "gascoigne-together": "a 40 s wait",
    "boss-putdown": "a 60 s wait; suite chapter: trips/boss-putdown",
    "prey-replay": "60 s waits",
    "join-apart": "a 60 s wait; suite chapter: joins/join-apart",
    "lone-guest": "a 60 s wait; suite chapter: trips/lone-guest",
    "echoes-apart": "an 80 s wait",
    "host-death": "a 60 s wait",
    "guest-death": "a 60 s wait",
    "resume": "a 40 s wait; suite chapter: crashes/resume",
    "crash-apart": "a 40 s wait; suite chapter: crashes/crash-apart",
    "host-crash-apart": "a 60 s wait; suite chapter: crashes/host-crash-apart",
    "team-world": "a 60 s wait, measured 472 s",
    "team-world-lamps": "two team loads for the guest, a trip and a 20 s wait",
    "kill-apart": "a 45 s wait; suite chapter: smoke/kill-apart, trips/kill-apart",
    "dog": "a 45 s wait; suite chapter: trips/dog",
    "trade-menus": "a 40 s wait",
    "pickup-after-crash": "a 40 s wait",
    "pickup-after-trip": "a 90 s wait; suite chapter: trips/pickup-after-trip",
    "pickup-after-reload": "an 80 s wait",
    "hp-after-host-trip": "a 45 s wait",
    "hp-after-guest-trip": "a 60 s wait",
    "dead-before-session": "a 60 s wait",
    "hit-after-host-trip": "a 90 s wait; suite chapter: trips/hit-after-host-trip",
    "crash-rejoin-hp": "a 40 s wait",
    "border-fight": "a 60 s wait",
    "guest-death-seen": "a 60 s wait",
    "echoes-after-crash": "a 90 s wait",
    "host-death-boss": "a 60 s wait",
    "give-up": "a 60 s wait; suite chapter: party-death/give-up",
    "down-alone": "a 120 s wait; suite chapter: party-death/down-alone",
    "down-revive3": "a 60 s wait; suite chapter: party3/down-revive3",
    "crash3": "two boots of three games; suite chapter: party3/crash3",
    "join3-uncap": "measured 546-564 s when the third game is not summoned",
    "lamp-after-crash": "two boots and a lamp",
    "owner-crash": "two boots, measured 200-209 s",
}

WAIT = re.compile(r"wait (\d+(?:\.\d+)?)$")


def fixed_waits(steps) -> list[float]:
    """The seconds of every fixed `wait <s>` in a script (or a list of stages)."""
    found = []
    for step in steps or []:
        if isinstance(step, list):
            found += fixed_waits(step)
        elif match := WAIT.match(step):
            found.append(float(match.group(1)))
    return found


def stages(script) -> list:
    return script if script and isinstance(script[0], list) else [script]


def scenario_need(scenario: dict) -> float:
    """The least time a scenario's slowest game needs: a boot per stage and its fixed waits."""
    return max(BOOT_S * len(stages(script)) + sum(fixed_waits(script))
               for script in scenario["roles"].values())


def wait_problems(where: str, steps) -> list[str]:
    return [f"{where}: a fixed wait of {seconds:g} s (at most {WAIT_MAX_S} s: sync or wait on "
            "a condition)" for seconds in fixed_waits(steps) if seconds > WAIT_MAX_S]


def scenario_problems(name: str, scenario: dict) -> list[str]:
    """What breaks the budget in a single scenario (a LONG one may)."""
    if name in LONG:
        return []
    problems = []
    timeout = scenario.get("timeout", 600)
    if timeout > SCENARIO_MAX_S:
        problems.append(f"{name}: timeout {timeout} s (at most {SCENARIO_MAX_S} s, or LONG)")
    if (need := scenario_need(scenario)) > timeout:
        problems.append(f"{name}: needs {need:g} s (boots and fixed waits), timeout {timeout} s")
    for role, script in scenario["roles"].items():
        problems += wait_problems(f"{name} {role}", script)
    return problems


def restarts(steps: list[str]) -> int:
    return sum(1 for step in steps if step.startswith("<restart"))


def chapter_budget(chapter: dict) -> int:
    """A chapter's time: its own budget, and a boot for each restart of its games (one game's
    restarts follow each other; the others wait for them). "boots_in": the boots of a restart
    that ended the chapter before, which go on in this one."""
    boots = sum(restarts(steps) for steps in chapter["steps"].values()) + chapter.get("boots_in", 0)
    return chapter.get("budget", CHAPTER_MAX_S) + BOOT_S * boots


def chapter_problems(suite: str, chapter: dict) -> list[str]:
    """What breaks the budget in a suite chapter: its budget, a fixed wait over the limit, or a
    game whose restarts and fixed waits alone take longer than its budget."""
    where = f"{suite}/{chapter['name']}"
    budget = chapter.get("budget", CHAPTER_MAX_S)
    problems = []
    if budget > CHAPTER_MAX_S:
        problems.append(f"{where}: budget {budget} s (at most {CHAPTER_MAX_S} s)")
    for role, steps in chapter["steps"].items():
        problems += wait_problems(f"{where} {role}", steps)
        if (need := sum(fixed_waits(steps))) > budget:
            problems.append(f"{where} {role}: needs {need:g} s of fixed waits, budget {budget} s")
    return problems


def suite_timeout(suite: dict) -> int:
    """A suite's whole time: its games' boot and every chapter's budget."""
    return SUITE_BOOT_S + sum(chapter_budget(chapter) for chapter in suite["chapters"])


def tiers(suites: dict, scenarios: dict) -> dict[str, list[str]]:
    """Each tier's runs, the longest first (lanes take them in turn)."""
    full = sorted(suites, key=lambda name: -suite_timeout(suites[name]))
    return {
        "quick": ["smoke"],
        "full": full,
        "long": [name for name in LONG if name in scenarios],
    }


def describe(name: str, scenario: dict) -> str:
    """--list: a scenario's timeout and scripted waits."""
    waits = [sum(fixed_waits(script)) for script in scenario["roles"].values()]
    tier = " LONG" if name in LONG else ""
    return f"timeout {scenario.get('timeout', 600)} s, waits {max(waits):g} s{tier}"
