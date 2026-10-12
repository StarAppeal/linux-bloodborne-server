# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""The rig's suites: many checks in one session from one boot (run_scenario.py runs them by
name, as scenarios). Kaniel 2026-10-12: "consolidate a lot of them to spend less time on opening
game and connecting, and add more".

A suite's games start once, join once and then go through its chapters in turn. compile_suite
turns it into one script per game:

  START, then for each chapter: chapter <name>, keep-going <on|off>, sync <name>, its steps

- chapter <name> marks where each game's log section for the chapter begins: the runner judges
  every chapter on its own lines (expect, forbid, same_hp, hp_sums_hits, max_hp_lag, allow, as
  a scenario's).
- sync <name> waits until every game in it got there; the runner writes the go file then
  (instead of the scenarios' fixed waits, a guess of how long the other game takes). Every
  chapter begins with one for all games; a chapter's steps may add their own (names unique in
  the suite, each in at least two games).
- keep-going on: a failed step gives up its chapter only; the game goes on at the next one (the
  runner lets the others past that chapter's syncs). A "fatal" chapter (the join: nothing after
  it means anything without it) runs with keep-going off and ends the suite when it fails.
- RESTART in a game's steps stops that game there (as a crash would) and starts it again with
  the steps after it (a scenario's stages).

run_scenario.py <suite> --chapter <name> runs only those chapters (and the "setup" ones they
need, the join).
"""
from scenarios import (AWAY_FROM_LAMP, BLOODSTAIN_DROPPED, BLOODSTAIN_STOLEN,  # noqa: F401
                       CATHEDRAL_WARD, CATHEDRAL_WARD_LAMP, CENTRAL_YHARNAM, CENTRAL_YHARNAM_LAMP,
                       CLERIC_BEAST, CLERIC_BEAST_DEFEATED, CLINIC, CRASH_CLEANUP, DOG, ENEMY,
                       FORBIDDEN_WOODS_LAMP_LIT, GASCOIGNE, GASCOIGNE_DEFEATED, HUNTERS_DREAM,
                       LETHAL_FALL, LEVEL_SYNC, OTHER_MAP_FLAG, PARTY_DEATH, REVIVE_HITS,
                       SESSION_HOLDS, START, STORY_HIDDEN, THIS_MAP_FLAG, WARD_ENEMY, WARD_NPC,
                       attack, only_kill)

RESTART = "<restart>"
RESTART_WITH = "<restart with "


def restart_with(saves: str) -> str:
    """RESTART with other saves put in first (rig/saves/<saves>, as a scenario's "saves"): a
    brand-new character, as a player who starts one."""
    return f"{RESTART_WITH}{saves}>"

# How players play (the robustness review, 2026-10-12): party death on. At 30 FPS: at the kit's
# uncapped frame rate the third player is never summoned (rig 2026-10-12, 3 runs of 3; scenario
# join3-uncap keeps that case), and Kaniel chose 30 FPS for the suites until it is fixed.
AS_PLAYERS = {**PARTY_DEATH, "BB_FPS": "30"}
# Without party death: a death is the game's own (a guest gets up at its lamp in the session).
NO_PARTY_DEATH = {"SHADPS4_BLOODBORNE_PARTY_DEATH": "0", "BB_FPS": "30"}

DOG_MAX_HP = 364
ENEMY_MAX_HP = 484
GUESTS = ("guest", "guest2")


def chapter(name: str, about: str, steps: dict[str, list[str]], **checks) -> dict:
    """One chapter: each game's steps (a game with none only syncs at its start) and its checks:
    expect/forbid {role: [patterns]}, allow [patterns], same_hp, same_hp_census, hp_sums_hits,
    max_hp_lag, and fatal/setup (see the module's notes)."""
    return {"name": name, "about": about, "steps": steps, **checks}


def everyone(roles, *steps: str) -> dict[str, list[str]]:
    return {role: list(steps) for role in roles}


def holds(roles) -> dict[str, list[str]]:
    """The session must hold in this chapter for every game."""
    return {role: SESSION_HOLDS for role in roles}


def join(roles, about: str = "every game joins the host by itself (the bells ring on their "
                              "own); the guests keep their full HP and their own level") -> dict:
    players = len(roles)
    steps = {role: ["godmode", f"wait-session {players} 480", "wait 10",
                    f"expect-session {players}"] for role in roles}
    for guest in roles[1:]:
        steps[guest] = ["note-max-hp", *steps[guest], "expect-max-hp self == noted",
                        f"expect-effect {LEVEL_SYNC} 0"]
    return chapter("join", about, steps, fatal=True, setup=True,
                   expect={"host": [r"auto-rejoin started search effect=9000"],
                           **{guest: [r"auto-rejoin started search effect=9005"]
                              for guest in roles[1:]}},
                   forbid=holds(roles))


def map_reset(roles, name: str = "everyone-leaves") -> dict:
    """Every game goes to the Dream and back: kills held while one stayed come back once
    everyone left (Kaniel 2026-10-11: half the location reset after a trip), and the next
    chapters find the map's enemies whole again."""
    players = len(roles)
    steps = {role: [f"travel {HUNTERS_DREAM}", f"sync {name}-all-away", "wait 5",
                    f"travel {CENTRAL_YHARNAM_LAMP}", f"wait-members {players} 300",
                    f"sync {name}-all-back", "wait 15", "census 60",
                    f"expect-hp {ENEMY} == {ENEMY_MAX_HP}", f"expect-hidden {ENEMY} 0",
                    f"expect-hp {DOG} == {DOG_MAX_HP}", f"expect-session {players}"]
             for role in roles}
    return chapter(name, "everyone leaves the map and comes back: every enemy is back, whole",
                   steps, forbid=holds(roles))


def wipe(roles, name: str = "wipe") -> dict:
    """Party death's wipe, last in its suite (the map reloads): every guest goes down, then the
    host falls with nobody standing; all die, wake at their lamps with the map reset (Kaniel
    2026-10-11: after the wipe 16 enemies stayed hidden and the mob showed three HPs), each with
    its own bloodstain and no enemy taking it."""
    players = len(roles)
    steps = {"host": ["godmode off", f"sync {name}-guests-down", *LETHAL_FALL,
                      "wait-vital dead 30", "wait-vital up 150"]}
    for guest in roles[1:]:
        steps[guest] = ["godmode off", *LETHAL_FALL, "wait-vital down 30",
                        f"sync {name}-guests-down", "wait-vital dead 90", "wait-vital up 150"]
    for role in roles:
        steps[role] += [f"wait-members {players} 300", f"sync {name}-woken", "wait 15",
                        "census 60", f"expect-hp {ENEMY} == {ENEMY_MAX_HP}",
                        f"expect-hidden {ENEMY} 0", f"expect-hp {DOG} == {DOG_MAX_HP}",
                        f"expect-session {players}"]
    # The dog's +0x28 reads 1 at full HP in every game after a wipe (rig 2026-10-12, smoke): the
    # map's own state, not a kill hiding it (its HP is checked).
    return chapter(
        name, "a party wipe resets the map for everyone, the same in every game", steps,
        expect={"host": [r"party death: died for real on map 0x18010000 \(reset=true\)",
                         r"travelling there \(reset=true\)", BLOODSTAIN_DROPPED],
                **{guest: [r"party death: wipe \d+ ends down \d+; dies \(reset=true",
                           r"travelling there \(reset=true\)", BLOODSTAIN_DROPPED]
                   for guest in roles[1:]}},
        forbid={role: SESSION_HOLDS + [BLOODSTAIN_STOLEN] for role in roles})


def hits_in_turn(roles, target: str, prefix: str, ready: str | None = None) -> dict[str, list]:
    """Each game hits `target` once, in turn (a sync between hits), then all take a census."""
    steps = {role: [] for role in roles}
    if ready:
        for role in roles:
            steps[role].append(f"sync {ready}")
    for turn, hitter in enumerate(roles, 1):
        for role in roles:
            if role == hitter:
                steps[role].append(f"hit {target} 30")
            steps[role].append(f"sync {prefix}-{turn}")
    for role in roles:
        steps[role] += ["wait 8", "census 40"]
    return steps


def trip_back(role: str, players: int, back: str) -> list[str]:
    """To the Dream and back to the first lamp, until every member is on the map again."""
    return [f"travel {HUNTERS_DREAM}", "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}",
            f"wait-members {players} 300", f"sync {back}"]


PARTY3 = ("host", "guest", "guest2")
PARTY2 = ("host", "guest")

# Borders between two maps the rig crosses on foot (Kaniel 2026-10-12: the hardest bugs come where
# two maps meet, crossed back and forth). Each comes from a player's log: its "Bloodborne map
# change <from> -> <to> at (...); trail: x y z ..." line (LogMapChanges) is the path a player
# walked over it, which glide replays. "lamp" is the warp point nearest the trail's start (the
# players get there by lamp, then the short teleport to the trail's start). Empty until such a
# line is in a log: no border chapter then.
BORDERS: dict[str, dict] = {}
BORDER_CROSSINGS = 3


def border_crossings(name: str, border: dict) -> dict:
    """Both players cross the border together and back, BORDER_CROSSINGS times; after each way
    both are on the same map, and at the end the session holds and each sees the other."""
    points = border["trail"].split()
    back = " ".join(" ".join(points[at:at + 3]) for at in range(len(points) - 3, -1, -3))
    start = " ".join(points[:3])
    steps = [f"travel {border['lamp']}", "wait-members 2 300", f"sync {name}-there",
             f"teleport {start}", "wait 3"]
    for crossing in range(1, BORDER_CROSSINGS + 1):
        steps += [f"glide {border['trail']}", f"sync {name}-{crossing}-over", "wait 5",
                  f"expect-map {border['to']}", f"glide {back}", f"sync {name}-{crossing}-back",
                  "wait 5", f"expect-map {border['from']}"]
    steps += ["wait 10", "census 60", "expect-session 2", "expect-others-near 1 20"]
    return chapter(f"border-{name}", f"both cross the {name} border {BORDER_CROSSINGS} times and "
                                     "back: one map each time, nobody lost",
                   {role: list(steps) for role in PARTY2}, forbid=holds(PARTY2))

SUITES = {
    # Before every kit (the robustness review: 12 of the night's bugs came from untested kits).
    "smoke": {
        "about": "a party of three as players play: join, trip, hits from three worlds, a kill "
                 "apart, pickups, a guest's crash and rejoin, a wipe",
        "roles": PARTY3,
        "settings": AS_PLAYERS,
        "join_within": 240,
        "timeout": 1800,
        "chapters": [
            join(PARTY3),
            chapter("trip", "the host goes to the Dream and back; the session holds and it "
                            "re-creates both guests; everyone sees both others",
                    {"host": [*trip_back("host", 3, "trip-back"), "wait 20", "snapshot",
                              "expect-session 3", "expect-others-near 2 15"],
                     **{guest: ["sync trip-back", "wait 20", "snapshot", "expect-session 3",
                                "expect-others-near 2 15"] for guest in GUESTS}},
                    expect={"host": [r"re-creating (?:\S+ )*rig_guest (?:\S+ )*on map 0x18010000",
                                     r"re-creating (?:\S+ )*rig_guest2 (?:\S+ )*on map 0x18010000"]},
                    forbid=holds(PARTY3)),
            chapter("hit3", "each guest back from its own trip (three worlds apart), all three "
                            "hit one enemy in turn: one HP in every game, each hit counted once, "
                            "on the other bars within 0.15 s",
                    {role: ([] if role == "host" else
                            [f"travel {HUNTERS_DREAM}", "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}",
                             "wait-members 3 300"]) + steps
                     for role, steps in hits_in_turn(PARTY3, ENEMY, "hit3",
                                                     ready="hit3-apart").items()},
                    same_hp=ENEMY, hp_sums_hits=ENEMY, max_hp_lag=0.15, forbid=holds(PARTY3)),
            chapter("kill-apart", "the host kills an enemy while the second guest is in the "
                                  "Dream; that guest's copy is gone when it comes back",
                    {"host": ["sync ka-away", *attack(ENEMY), "wait 5", "sync ka-killed",
                              "wait-members 3 300", "sync ka-back", "wait 20", "snapshot"],
                     "guest": ["sync ka-away", "sync ka-killed", "sync ka-back"],
                     "guest2": [f"travel {HUNTERS_DREAM}", "sync ka-away", "sync ka-killed",
                                f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 3 300",
                                "sync ka-back", "wait 20", "snapshot"]},
                    expect={"host": [r"kill board gets 1 kills"],
                            "guest2": [r"(?:hid 1 enemies a member killed on map 0x18010000|"
                                       r"0x18010000, 1 near died)"]},
                    forbid={"host": only_kill(ENEMY)}),
            chapter("pickups", "after the trips each game still picks up the item by the lamp "
                               "with X (Kaniel 2026-10-09: no pickups after a reload)",
                    {"host": ["pickup 20", "wait 3", "snapshot", "sync pickups-host"],
                     "guest": ["sync pickups-host", "pickup 20", "wait 3", "snapshot",
                               "sync pickups-guest"],
                     "guest2": ["sync pickups-host", "sync pickups-guest", "pickup 20", "wait 3",
                                "snapshot"]},
                    forbid=holds(PARTY3)),
            chapter("rejoin", "the guest's game dies and comes back: it joins again, the party is "
                              "three again, and its kept pickups survive the restart (Kaniel "
                              "2026-10-11: krolebord's pickups wiped by a restart)",
                    {"guest": [RESTART, START, "godmode", "wait-session 3 480", "wait 10",
                               "sync rejoin-back", "expect-session 3"],
                     "host": ["sync rejoin-back", "wait 10", "expect-session 3"],
                     "guest2": ["sync rejoin-back", "wait 10", "expect-session 3"]},
                    # PR #37: the others note the member gone, and the guest counts as standing
                    # only once it is in the session's world again (a wipe waited for it).
                    expect={"host": [r"Co-op lifecycle: member left"],
                            "guest2": [r"Co-op lifecycle: member left"],
                            "guest": [r"Bloodborne party: in the session's world"]},
                    forbid={"guest": [r"new character in save slot"]},
                    allow=CRASH_CLEANUP),
            wipe(PARTY3),
        ],
    },
    "together": {
        "about": "two players in one world (after the join, before any trip): what each sees, "
                 "shares and picks up, talks by the lamp, deaths at once, and the bosses",
        "roles": PARTY2,
        "settings": AS_PLAYERS,
        "join_within": 180,
        "timeout": 1800,
        "chapters": [
            join(PARTY2),
            chapter("story-hidden", "a character the host's story hides (Iosefka) stays hidden "
                                    "on the guest: it hides and shows characters as the host",
                    {"host": ["census 80", f"expect-hidden {STORY_HIDDEN} 1"],
                     "guest": ["census 80", f"expect-hidden {STORY_HIDDEN} 1 30"]},
                    expect={"guest": [rf"characters as the host has them on map 0x18010000: "
                                      rf"hid \d+, showed \d+:.* {STORY_HIDDEN}"]},
                    forbid=holds(PARTY2)),
            chapter("phantom-look", "how a guest is drawn: each dumps itself, with a picture",
                    {"host": ["dump self", "shot host-self"],
                     "guest": ["dump self", "shot guest-self", "dump nearest"]},
                    forbid=holds(PARTY2)),
            chapter("guest-flags-stay", "flags the guest's own world sets stay in it",
                    {"host": [f"expect-flag {OTHER_MAP_FLAG} 0", f"expect-flag {THIS_MAP_FLAG} 0",
                              "sync flags-set", "wait 20", f"expect-flag {OTHER_MAP_FLAG} 0",
                              f"expect-flag {THIS_MAP_FLAG} 0"],
                     "guest": [f"set-flag {OTHER_MAP_FLAG} 1", f"set-flag {THIS_MAP_FLAG} 1",
                               "sync flags-set", "wait 20", f"expect-flag {OTHER_MAP_FLAG} 1"]},
                    forbid=holds(PARTY2)),
            chapter("lamp-share", "a lamp the guest lights is lit in the host's world too",
                    {"host": [f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 0", "sync lamp-lit",
                              f"wait-flag {FORBIDDEN_WOODS_LAMP_LIT} 1 90"],
                     "guest": [f"set-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", "sync lamp-lit"]},
                    forbid=holds(PARTY2)),
            chapter("hp-together", "both hit one enemy in one world: the same HP in both games",
                    hits_in_turn(PARTY2, ENEMY, "hpt"), same_hp=ENEMY,
                    expect=everyone(PARTY2, r"enemy HP shared over the HP board"),
                    forbid=holds(PARTY2)),
            chapter("near-kill", "both strike one enemy to death at once: it dies once in each "
                                 "world, body and loot there, never hidden (Kaniel 2026-10-11: the "
                                 "hunter died twice and vanished with its loot)",
                    {role: ["sync near-kill-go", *attack(CLINIC), "wait 10", "census 40",
                            f"expect-hp {CLINIC} == 0", f"expect-hidden {CLINIC} 0"]
                     for role in PARTY2},
                    forbid={role: [r"hid [1-9]\d* enemies a member killed on map 0x18010000"]
                            for role in PARTY2}),
            chapter("pickup", "each picks up the map item by the lamp with X in the session",
                    {"host": ["pickup 60", "wait 5", "snapshot", "sync pickup-host"],
                     "guest": ["sync pickup-host", "pickup 60", "wait 5", "snapshot"]},
                    forbid=holds(PARTY2)),
            chapter("talk-by-the-lamp", "the guest presses X at the first lamp and beside "
                                        "Gilbert's window: it never takes the lamp to the Dream",
                    {"guest": ["press cross 200", "wait 8", "press circle 200", "wait 4",
                               f"expect-map {CENTRAL_YHARNAM}",
                               *[step for dx, dz, heading in ((0, 1.2, 0), (0, -1.2, 3.14),
                                                              (1.2, 0, 1.57), (-1.2, 0, -1.57))
                                 for step in (f"approach 0x100140a0 {dx} {dz} {heading}", "wait 3",
                                              "press cross 200", "wait 6", "press circle 200",
                                              "wait 4", f"expect-map {CENTRAL_YHARNAM}")],
                               "expect-session 2"]},
                    forbid=holds(PARTY2)),
            chapter("boss-refight", "the host fights Father Gascoigne again (its world puts him "
                                    "back by a reload) with the guest there: the guest's old kill "
                                    "does not hide him, he dies in the host's world",
                    {"host": [f"set-flag {GASCOIGNE_DEFEATED} 0",
                              f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                              "sync refight-back", "wait 15", f"expect-hp {GASCOIGNE} > 0",
                              f"kill {GASCOIGNE}", f"wait-flag {GASCOIGNE_DEFEATED} 1 60",
                              "wait 10", "expect-session 2"],
                     "guest": ["sync refight-back", "wait 30", "expect-session 2"]},
                    forbid={"host": SESSION_HOLDS + [
                        r"(?s)hid \d+ enemies a member killed.*autoplay: kill 0x1001409c"],
                        "guest": SESSION_HOLDS}),
            chapter("boss-kill", "the Cleric Beast dies in the host's world with the guest there; "
                                 "the session holds",
                    {"host": [f"kill {CLERIC_BEAST}", f"wait-flag {CLERIC_BEAST_DEFEATED} 1 60",
                              "sync boss-dead", "wait 30", "expect-session 2"],
                     "guest": ["sync boss-dead", "wait 30", "expect-session 2"]},
                    expect={"host": [r"flags in session: set [^|]*\b4000\b"]},
                    forbid=holds(PARTY2)),
        ],
    },
    "trips": {
        "about": "two players whose worlds go apart by trips to the Dream: who sees whom, HP and "
                 "kills across worlds, pickups after trips, a guest alone on a map, boss put-downs",
        "roles": PARTY2,
        "settings": AS_PLAYERS,
        "join_within": 180,
        "timeout": 2400,
        "chapters": [
            join(PARTY2),
            chapter("trip", "the host goes to the Dream and back; the session holds and the guest "
                            "is re-created",
                    {"host": [*trip_back("host", 2, "trip-back"), "wait 15", "snapshot",
                              "expect-session 2", "expect-others-near 1 15"],
                     "guest": ["sync trip-back", "wait 15", "snapshot", "expect-session 2",
                               "expect-others-near 1 15"]},
                    expect={"host": [r"party trip started",
                                     r"re-creating rig_guest on map 0x18010000"]},
                    forbid=holds(PARTY2)),
            chapter("hit-after-host-trip", "only the host went to the Dream: the guest's hits "
                                           "still reach the host's copy, each once",
                    {**hits_in_turn(("guest", "host"), ENEMY, "haht")},
                    same_hp=ENEMY, hp_sums_hits=ENEMY, forbid=holds(PARTY2)),
            chapter("guest-hit-once", "after the guest's trip too: its real hits count once in "
                                      "both games (Kaniel's friend 2026-10-11: each hit twice)",
                    {role: (trip_back("guest", 2, "gho-back") + ["wait 15"]
                            if role == "guest" else ["sync gho-back", "wait 15"]) + steps
                     for role, steps in hits_in_turn(("guest", "host"), DOG, "gho").items()},
                    same_hp=DOG, hp_sums_hits=DOG,
                    expect={"guest": [rf"hit on {DOG} by this player",
                                      r"enemy HP shared over the HP board"]},
                    forbid=holds(PARTY2)),
            chapter("hp-apart", "worlds apart, the host hurts an enemy: the guest's copy follows",
                    {"host": [f"hurt {DOG} 200", "sync hp-apart-hurt", "wait 12",
                              f"expect-hp {DOG} <= 200"],
                     "guest": ["sync hp-apart-hurt", "wait 12", f"expect-hp {DOG} <= 200"]},
                    expect={"guest": [r"lowered \d+ enemies by the other members' damage"]},
                    forbid=holds(PARTY2)),
            chapter("kill-apart", "the host kills an enemy while the guest is in the Dream; the "
                                  "guest's copy is gone when it comes back",
                    {"host": ["sync ka-away", *attack(ENEMY), "wait 5", "sync ka-killed",
                              "wait-members 2 240", "sync ka-back", "wait 20", "snapshot"],
                     "guest": [f"travel {HUNTERS_DREAM}", "sync ka-away", "sync ka-killed",
                               f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                               "sync ka-back", "wait 20", "snapshot"]},
                    expect={"host": [r"kill board gets 1 kills"],
                            "guest": [r"(?:hid 1 enemies a member killed on map 0x18010000|"
                                      r"0x18010000, 1 near died)"]},
                    forbid={"host": only_kill(ENEMY)}),
            chapter("dog", "Kaniel's dog: the guest kills an enemy while the host is in the "
                           "Dream; the host finds it dead, not alive again",
                    {"guest": ["sync dog-away", *attack(DOG), "wait 5", "sync dog-killed",
                               "wait-members 2 240", "sync dog-back", "wait 20", "snapshot"],
                     "host": [f"travel {HUNTERS_DREAM}", "sync dog-away", "sync dog-killed",
                              f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                              "sync dog-back", "wait 20", "snapshot"]},
                    expect={"guest": [r"kill board gets 1 kills"],
                            "host": [r"(?:hid 1 enemies a member killed on map 0x18010000|"
                                     r"0x18010000, 1 near died)"]},
                    forbid={"guest": only_kill(DOG)}),
            chapter("kills-held", "kills stay dead while one member stays on the map: the guest "
                                  "goes to the Dream and back, the host stays (Kaniel 2026-10-11: "
                                  "half the location reset after a trip)",
                    {"guest": trip_back("guest", 2, "held-back") + [
                        "wait 15", "census 60", f"expect-hidden {ENEMY} 1 30",
                        f"expect-hidden {DOG} 1 30"],
                     "host": ["sync held-back", "wait 15", "census 60"]},
                    forbid=holds(PARTY2)),
            map_reset(PARTY2),
            chapter("pickup-after-trip", "after trips both still pick up the item by the lamp",
                    {"host": ["pickup 20", "wait 5", "snapshot", "sync pat-host"],
                     "guest": ["sync pat-host", "pickup 20", "wait 5", "snapshot"]},
                    forbid=holds(PARTY2)),
            chapter("boss-putdown", "the guest has not beaten Father Gascoigne, the host has: after "
                                    "the host's trip its world puts him down again, and the "
                                    "guest's own boss event does not take that for its kill",
                    {"guest": [f"set-flag {GASCOIGNE_DEFEATED} 0",
                               f"travel {CENTRAL_YHARNAM_LAMP}", f"expect-hp {GASCOIGNE} > 0",
                               "sync putdown-ready", "sync putdown-back", "wait 30",
                               f"dump {GASCOIGNE}", "expect-session 2"],
                     "host": ["sync putdown-ready", *trip_back("host", 2, "putdown-back"),
                              "wait 30", f"expect-flag {GASCOIGNE_DEFEATED} 1",
                              "expect-session 2"]},
                    forbid={"host": SESSION_HOLDS,
                            "guest": SESSION_HOLDS + [r"sharing trophy 14 ",
                                                      r"Bloodborne echoes: \+3200 "]}),
            chapter("dream-shops", "a guest in the Dream during the session: its messengers and "
                                   "doors open as in its own game (Kaniel 2026-10-11: no insight "
                                   "shop, no Hunter Chief Emblem for guests) and the shop's badge "
                                   "check holds (every weapon opened by mistake). Flag 9401 "
                                   "(workshop doors) is to be confirmed on the first run",
                    {"guest": [f"travel {HUNTERS_DREAM}", "wait 20", "expect-flag 9401 1",
                               f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                               "sync shops-back"],
                     "host": ["sync shops-back"]}),
            chapter("lone-guest", "a guest alone on a map runs its enemies and hands them back "
                                  "when the host comes",
                    {"guest": [f"travel {CATHEDRAL_WARD_LAMP}", "sync lone-there", "wait 15",
                               *attack(WARD_ENEMY), "wait 10", "sync lone-killed",
                               "wait-members 2 300", "wait 20", "snapshot"],
                     "host": ["sync lone-there", "sync lone-killed", "wait 10",
                              f"travel {CATHEDRAL_WARD_LAMP}", "wait-members 2 240", "wait 20",
                              "snapshot"]},
                    expect={"guest": [r"guest alone on map 0x18000000 took over \d+ enemies",
                                      r"guest no longer alone; handed back",
                                      r"Bloodborne echoes: \+\d+ \(rates 1\) from 0x18fc99b"],
                            "host": [r"added \d+ echoes from a kill another member made apart"]}),
            chapter("ward-npc", "both by the Oedon Chapel lamp: the host hurts the NPC there, the "
                                "guest hits it; one HP in both games (Kaniel 2026-10-10)",
                    {"host": [f"hurt {WARD_NPC} 600", "sync npc-hurt", "sync npc-hit", "wait 20",
                              "census 60"],
                     "guest": ["sync npc-hurt", "wait 25", f"expect-hp {WARD_NPC} <= 600",
                               f"hit {WARD_NPC} 20", "sync npc-hit", "wait 20", "census 60"]},
                    same_hp=WARD_NPC, forbid=holds(PARTY2)),
        ],
    },
    "party-death": {
        "about": "party death: downs and revives, giving up, a death alone, a guest's bloodstain "
                 "after its rejoin, and the wipe",
        "roles": PARTY2,
        "settings": AS_PLAYERS,
        "join_within": 180,
        "timeout": 1800,
        "chapters": [
            join(PARTY2),
            chapter("down-revive", "the guest falls with 1 HP and goes down; the host strikes it "
                                   "up again",
                    {"host": ["wait-member-vital down 120", "shot sees-down", "approach-downed",
                              "wait 2", *REVIVE_HITS, "wait-member-vital up 30",
                              "shot after-revive"],
                     "guest": ["godmode off", *LETHAL_FALL, "wait-vital down 30", "shot down",
                               "expect-hp self == 1", "wait-vital up 90", "expect-hp self > 1",
                               "godmode"]},
                    expect={"host": [r"party death: (real )?revive hit on"],
                            "guest": [r"party death: down \d+ on map 0x18010000",
                                      r"party death: revived from down"]},
                    forbid={"host": SESSION_HOLDS,
                            "guest": SESSION_HOLDS + [r"party guest died"]}),
            chapter("down-by-enemy", "the guest with 1 HP next to an enemy goes down, the enemy "
                                     "leaves it alone, and the host revives it",
                    {"host": ["wait-member-vital down 150", "wait 22", "approach-downed",
                              "wait 2", *REVIVE_HITS],
                     "guest": ["godmode off", "hurt self 1", f"approach {ENEMY}",
                               "wait-vital down 90", "wait 20", "expect-hp self == 1",
                               "wait-vital up 90", "expect-hp self > 1", "godmode"]},
                    expect={"host": [r"party death: (real )?revive hit on"],
                            "guest": [r"party death: enemies forgot \d+ target\(s\) on downed "
                                      r"players \(self down=true", r"party death: revived from down"]},
                    forbid={"host": SESSION_HOLDS,
                            "guest": SESSION_HOLDS + [r"party death: killed while down"]}),
            chapter("give-up", "the downed guest holds Circle and dies; it gets up at its lamp "
                               "without a reload and the session holds",
                    {"guest": ["godmode off", *LETHAL_FALL, "wait-vital down 30",
                               "press circle 3600", "wait-vital dead 15", "wait-vital up 90",
                               "wait-members 2 240", "godmode"],
                     "host": ["wait-member-vital down 120"]},
                    expect={"guest": [r"party death: giving up ends down \d+; dies \(reset=false",
                                      r"party guest died; revived"]},
                    forbid={"host": SESSION_HOLDS,
                            "guest": SESSION_HOLDS + [r"reset=true"]}),
            chapter("stain-after-rejoin", "the guest's game dies after it died for real (give-up "
                                          "left its bloodstain); back in the host's world it "
                                          "sees its own stain with its echoes (Kaniel "
                                          "2026-10-11: only the host found a bloodstain)",
                    {"guest": [RESTART, START, "godmode", "wait-session 2 480", "wait 20",
                               "sync stain-back"],
                     "host": ["sync stain-back"]},
                    expect={"guest": [r"showed the guest's own bloodstain \([1-9]\d* echoes\)"]},
                    allow=CRASH_CLEANUP),
            chapter("down-alone", "with the host in the Dream nobody else is on the guest's map: "
                                  "its lethal fall is a real death that reloads the map",
                    {"host": [f"travel {HUNTERS_DREAM}", "sync alone-away", "sync alone-woken",
                              f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240"],
                     "guest": ["sync alone-away", "godmode off", *LETHAL_FALL,
                               "wait-vital dead 30", "wait-vital up 120", "godmode",
                               "sync alone-woken", "wait-members 2 300"]},
                    expect={"guest": [r"party death: died for real on map 0x18010000 "
                                      r"\(reset=true\)", r"travelling there \(reset=true\)"]},
                    forbid={"host": SESSION_HOLDS,
                            "guest": SESSION_HOLDS + [r"party death: down \d+"]}),
            wipe(PARTY2),
            chapter("stain-after-wipe", "after the wipe the guest's game dies and comes back: "
                                        "in the host's world it sees its own bloodstain from "
                                        "the wipe, with its echoes",
                    {"guest": [RESTART, START, "godmode", "wait-session 2 480", "wait 20",
                               "sync saw-back"],
                     "host": ["sync saw-back"]},
                    expect={"guest": [r"showed the guest's own bloodstain \([1-9]\d* echoes\)"]},
                    allow=CRASH_CLEANUP),
        ],
    },
    "edges": {
        "about": "where two maps meet and games are mid-load (Kaniel 2026-10-12: fog borders "
                 "crossed back and forth, teleports and cutscenes make the hardest bugs): every "
                 "game agrees on the map, the kills and the enemy HP afterwards",
        "roles": PARTY2,
        "settings": AS_PLAYERS,
        "join_within": 180,
        "timeout": 2400,
        "chapters": [
            join(PARTY2),
            chapter("travel-mid-load", "the host hurts an enemy, then goes to the Dream and back "
                                       "while the guest's map reloads (a warp to its lamp): one "
                                       "map, one HP, each sees the other",
                    {"host": [f"hit {ENEMY} 30", "sync tml-go", f"travel {HUNTERS_DREAM}",
                              "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                              "sync tml-back", "wait 15", "census 40",
                              f"expect-map {CENTRAL_YHARNAM}", "expect-others-near 1 15",
                              "expect-session 2"],
                     "guest": ["sync tml-go", f"travel {CENTRAL_YHARNAM_LAMP}",
                               "wait-members 2 300", "sync tml-back", "wait 15", "census 40",
                               f"expect-map {CENTRAL_YHARNAM}", "expect-others-near 1 15",
                               "expect-session 2"]},
                    same_hp=ENEMY, forbid=holds(PARTY2)),
            chapter("kill-mid-load", "the host kills an enemy and teleports away while the guest "
                                     "is loading: the kill reaches the guest's world",
                    {"host": ["sync kml-go", "wait 2", *attack(ENEMY), f"teleport {AWAY_FROM_LAMP}",
                              "wait-members 2 240", "sync kml-back", "wait 15", "census 60"],
                     "guest": ["sync kml-go", f"travel {CENTRAL_YHARNAM_LAMP}",
                               "wait-members 2 300", "sync kml-back", "wait 15", "census 60",
                               f"expect-hp {ENEMY} == 0"]},
                    expect={"host": [r"kill board gets 1 kills"]},
                    forbid={"host": only_kill(ENEMY)}),
            chapter("both-travel", "both go to the Dream at the same moment and come back at the "
                                   "same moment: the session holds and each sees the other",
                    {role: ["sync bt-go", f"travel {HUNTERS_DREAM}", "sync bt-dream",
                            f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 300",
                            "sync bt-back", "wait 20", "expect-session 2",
                            "expect-others-near 1 15"] for role in PARTY2},
                    forbid=holds(PARTY2)),
            chapter("outage", "the servers are out of reach for 20 s (players' links dropped for "
                              "15-31 s, 2026-10-11): the session holds and the boards come back",
                    {role: ["sync outage-go", "wait 30", "wait-members 2 120",
                            "wait-member-vital up 60", "expect-session 2"] for role in PARTY2},
                    outages={"outage-go": 20}),
            chapter("cutscene", "Father Gascoigne dies in the host's world with the guest there: "
                                "his death's time-of-day cutscene reloads the host's world (a "
                                "trip), and both are back together after it. Whether this kill "
                                "plays the cutscene again is to be confirmed on the first run",
                    {"host": [f"set-flag {GASCOIGNE_DEFEATED} 0",
                              f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                              "sync cs-ready", f"kill {GASCOIGNE}",
                              f"wait-flag {GASCOIGNE_DEFEATED} 1 60", "wait 60",
                              "wait-members 2 300", "sync cs-after", "wait 15",
                              "expect-session 2", "expect-others-near 1 15"],
                     "guest": ["sync cs-ready", "wait 60", "wait-members 2 300", "sync cs-after",
                               "wait 15", "expect-session 2", "expect-others-near 1 15"]},
                    expect={"host": [r"a world reload \(time of day\) is a trip"]},
                    forbid=holds(PARTY2)),
            *[border_crossings(name, border) for name, border in BORDERS.items()],
        ],
    },
    "crashes": {
        "about": "games that die and come back (one at a time, the other stays up), and deaths "
                 "without party death",
        "roles": PARTY2,
        "settings": NO_PARTY_DEATH,
        "join_within": 180,
        "timeout": 3000,
        "chapters": [
            join(PARTY2),
            chapter("deaths", "without party death: the host dies and gets up without a reload, "
                              "then the guest dies away from the lamp, gets up at it and the host "
                              "sees it there",
                    {"host": ["godmode off", "kill self", "wait 30", "godmode",
                              "sync host-dead-done", "wait-members 2 120", "sync guest-up",
                              "wait 10", "expect-others-near 1 12", "expect-session 2"],
                     "guest": ["sync host-dead-done", f"teleport {AWAY_FROM_LAMP}", "wait 5",
                               "godmode off", "kill self", "wait 40", "godmode",
                               "wait-members 2 120", "sync guest-up", "wait 10",
                               "expect-others-near 1 12", "expect-session 2"]},
                    expect={"host": [r"live player died in a party session"],
                            "guest": [r"party guest died; revived"]},
                    forbid=holds(PARTY2)),
            chapter("lamp", "the guest lights a lamp in the session (both worlds lit)",
                    {"host": [f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 0", "sync lamp-lit",
                              f"wait-flag {FORBIDDEN_WOODS_LAMP_LIT} 1 90"],
                     "guest": [f"set-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", "sync lamp-lit",
                               "wait 20"]},
                    forbid=holds(PARTY2)),
            chapter("resume", "the guest's game dies away from the lamp in the host's world: it "
                              "comes back where it was and joins again; then the lamp it lit is "
                              "still lit in both worlds, the host's hit reaches its copy, and "
                              "both pick up items",
                    {"guest": [f"teleport {AWAY_FROM_LAMP}", "wait 15", RESTART, "godmode",
                               "wait-session 2 420", "wait 10", "expect-session 2",
                               f"expect-map {CENTRAL_YHARNAM}",
                               f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", "sync resume-back",
                               "sync resume-hurt", "wait 30", f"expect-hp {ENEMY} <= 300",
                               "sync resume-host-picked", "pickup 20", "wait 5"],
                     "host": ["sync resume-back", f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 1",
                              f"hurt {ENEMY} 300", "sync resume-hurt", "pickup 20", "wait 5",
                              "sync resume-host-picked"]},
                    expect={"guest": [r"guest resumed at \(-21\d(\.\d+)?, -5\d(\.\d+)?, "
                                      r"17\d(\.\d+)?\) on map 0x18010000"]},
                    allow=CRASH_CLEANUP),
            chapter("crash-apart", "the guest's game dies while it is apart (in the Ward); the "
                                   "host stays in its world, the guest comes back in the Ward and "
                                   "they join again; a kill it makes alone there pays the host",
                    {"guest": [f"travel {CATHEDRAL_WARD_LAMP}", "wait 15", RESTART, "godmode",
                               f"wait-map {CATHEDRAL_WARD} 180", "wait-session 2 420", "wait 10",
                               "expect-session 2", f"expect-map {CATHEDRAL_WARD}",
                               *attack(WARD_ENEMY), "wait 15", "sync apart-back"],
                     "host": ["wait-solo 180", "wait 10", f"expect-map {CENTRAL_YHARNAM}",
                              "sync apart-back", "expect-session 2"]},
                    expect={"guest": [r"kill board gets 1 kills"],
                            "host": [r"added \d+ echoes from a kill another member made apart"]},
                    allow=CRASH_CLEANUP),
            chapter("host-crash-apart", "the host's game dies while the guest is apart (in the "
                                        "Ward); the guest stays and rings from there, and they "
                                        "join again without meeting first",
                    {"host": ["sync hca-ready", RESTART, START, "godmode", "wait-session 2 420",
                              "wait 10", "expect-session 2", f"expect-map {CENTRAL_YHARNAM}",
                              "sync hca-back"],
                     "guest": ["sync hca-ready", "wait-solo 180", "wait 10",
                               f"expect-map {CATHEDRAL_WARD}", "sync hca-back", "wait 10",
                               "expect-session 2", f"expect-map {CATHEDRAL_WARD}"]},
                    allow=CRASH_CLEANUP),
            chapter("host-crash", "with both back together, the host's game dies; the guest "
                                  "stays where it is and they join again once the host is back",
                    {"guest": [f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                               "sync hc-ready", "wait-solo 180", "wait 10",
                               f"expect-map {CENTRAL_YHARNAM}", "sync hc-back", "wait 10",
                               "expect-session 2"],
                     "host": ["wait-members 2 240", "sync hc-ready", RESTART, START, "godmode",
                              "wait-session 2 420", "wait 10", "expect-session 2",
                              "sync hc-back"]},
                    allow=CRASH_CLEANUP),
            chapter("leave", "the guest's game goes away and the host, alone now, picks up a map "
                             "item before anyone joins again (Kaniel 2026-10-11: no pickups "
                             "after a friend left)",
                    {"guest": ["sync leave-ready", RESTART, "wait 1"],
                     "host": ["sync leave-ready", "wait-solo 120", "wait 10", "pickup 20",
                              "wait 3", "snapshot"]},
                    expect={"host": [r"lost a member; staying where it is"]},
                    allow=CRASH_CLEANUP),
        ],
    },
    "joins": {
        "about": "joins from different places: the host stays up, the guest's game starts again "
                 "for each (its own world first)",
        "roles": PARTY2,
        "settings": AS_PLAYERS,
        "join_within": 240,
        "timeout": 2400,
        "chapters": [
            chapter("hurt-before-join", "an enemy hurt in the guest's own world before it joins "
                                        "stays whole in the host's; the session forming kills "
                                        "nothing",
                    {"host": ["godmode", "wait 20", f"expect-hp {DOG} == {DOG_MAX_HP}",
                              "wait-session 2 300", "wait 30", "census 60",
                              f"expect-hp {DOG} == {DOG_MAX_HP}", "expect-session 2"],
                     "guest": ["godmode", f"hurt {DOG} 5", "wait-session 2 300", "wait 30",
                               "census 60", f"expect-hp {DOG} > 300", "expect-session 2"]},
                    fatal=True, setup=True,
                    forbid={"host": SESSION_HOLDS + [r"Bloodborne echoes: \+\d+"],
                            "guest": SESSION_HOLDS}),
            chapter("join-after-trip", "a guest that travelled on its own before any session (to "
                                       "the Ward) still rings and joins the host on another map",
                    {"guest": [RESTART, START,
                               f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                               "wait-session 2 420", "wait 20", "expect-session 2",
                               f"expect-map {CATHEDRAL_WARD}", "sync jat-joined"],
                     "host": ["sync jat-joined", "expect-session 2",
                              f"expect-map {CENTRAL_YHARNAM}"]},
                    allow=CRASH_CLEANUP),
            chapter("hp-across-blocks", "the guest joined in the Ward, the host in Central "
                                        "Yharnam: the host's hit reaches the guest's copy once it "
                                        "comes over",
                    {"host": [f"hurt {ENEMY} 200", "sync hab-hurt"],
                     "guest": ["sync hab-hurt", f"travel {CENTRAL_YHARNAM_LAMP}",
                               "wait-members 2 240", "wait 20", f"expect-hp {ENEMY} <= 200"]},
                    expect={"guest": [r"lowered \d+ enemies by the other members' damage"]},
                    forbid=holds(PARTY2)),
            chapter("join-ward", "joining in Cathedral Ward, whose co-op the guest's own world has "
                                 "not opened",
                    {"host": [f"travel {CATHEDRAL_WARD_LAMP}", "sync jw-host-there",
                              "sync jw-joined", "expect-session 2"],
                     "guest": ["sync jw-host-there", RESTART, START,
                               f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                               "wait-session 2 420", "wait 10", "expect-session 2",
                               "sync jw-joined"]},
                    expect={"host": [r"join-anywhere patch"]},
                    allow=CRASH_CLEANUP),
            chapter("join-apart", "joining across maps: the host in the Ward, the guest in "
                                  "Central Yharnam; the guest joins where it is (no kick)",
                    {"host": ["sync ja-restarted", "sync ja-joined", "wait 30",
                              "expect-session 2", f"expect-map {CATHEDRAL_WARD}"],
                     "guest": [RESTART, START, "godmode", "sync ja-restarted",
                               "wait-session 2 420", "sync ja-joined", "wait 30",
                               "expect-session 2", f"expect-map {CENTRAL_YHARNAM}"]},
                    allow=CRASH_CLEANUP),
            chapter("restart-keeps", "the guest's game starts again in the session: it is the "
                                     "same character, so its kept pickups stay and nothing is "
                                     "backed up (Kaniel 2026-10-11: krolebord's pickups wiped by "
                                     "a restart; the review: never wipe on a bad read)",
                    {"guest": [RESTART, START, "godmode", "wait-session 2 420", "wait 10",
                               "sync rk-back", "expect-session 2"],
                     "host": ["sync rk-back", "expect-session 2"]},
                    forbid={"guest": [r"new character in save slot"]},
                    files_absent={"guest": ["bloodborne_party_*.bak-*"]},
                    allow=CRASH_CLEANUP),
            chapter("new-character", "a guest on a brand-new character (rig/saves/new-character) "
                                     "joins where it stands: its kept lists start clean once, "
                                     "the old ones backed up first",
                    {"guest": [restart_with("new-character"), "godmode", "wait-session 2 420",
                               "wait 20", "expect-session 2", "sync nc-back"],
                     "host": ["sync nc-back", "expect-session 2"]},
                    expect={"guest": [r"new character in save slot"]},
                    files_present={"guest": ["bloodborne_party_*.bak-*"]},
                    allow=CRASH_CLEANUP),
        ],
    },
    "party3": {
        "about": "parties of three: trips in every order, revives by the other guest, a guest's "
                 "crash and leave, and the wipe",
        "roles": PARTY3,
        "settings": AS_PLAYERS,
        "join_within": 240,
        "timeout": 2700,
        "chapters": [
            join(PARTY3),
            chapter("guest-trip3", "one guest goes to the Dream and back; afterwards everyone sees "
                                   "both others (Kaniel 2026-10-09)",
                    {"guest": trip_back("guest", 3, "gt3-back") + [
                        "wait 25", "expect-session 3", "expect-others-near 2 15"],
                     "host": ["sync gt3-back", "wait 25", "expect-others-near 2 15"],
                     "guest2": ["sync gt3-back", "wait 25", "expect-others-near 2 15"]},
                    forbid=holds(PARTY3)),
            chapter("trip3", "the second guest waits in the Dream while the host goes there and "
                             "back: the host re-creates the guest on its map, then the second "
                             "guest when it comes back",
                    {"guest2": [f"travel {HUNTERS_DREAM}", "sync t3-away", "sync t3-host-back",
                                f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 3 300",
                                "sync t3-all-back", "wait 25", "expect-others-near 2 15"],
                     "host": ["sync t3-away", f"travel {HUNTERS_DREAM}", "wait 5",
                              f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 300",
                              "sync t3-host-back", "sync t3-all-back", "wait 25",
                              "expect-session 3", "expect-others-near 2 15"],
                     "guest": ["sync t3-away", "sync t3-host-back", "sync t3-all-back",
                               "wait 25", "expect-others-near 2 15"]},
                    expect={"host": [r"re-creating rig_guest on map 0x18010000 \(1 of 2 members\)",
                                     r"re-creating (?:\S+ )*rig_guest2 (?:\S+ )*on map 0x18010000"]},
                    forbid=holds(PARTY3)),
            chapter("two-away", "the host and one guest go to the Dream while the second guest "
                                "stays; back together, every game sees both others (Kaniel "
                                "2026-10-11: guest 1 no longer saw guest 2)",
                    {"host": trip_back("host", 3, "ta-back") + ["wait 25",
                                                                "expect-others-near 2 15"],
                     "guest": trip_back("guest", 3, "ta-back") + ["wait 25",
                                                                  "expect-others-near 2 15"],
                     "guest2": ["sync ta-back", "wait 25", "expect-others-near 2 15"]},
                    forbid=holds(PARTY3)),
            chapter("hit3", "all three hit one enemy in turn: one HP everywhere, each hit once",
                    hits_in_turn(PARTY3, ENEMY, "p3hit"),
                    same_hp=ENEMY, hp_sums_hits=ENEMY, max_hp_lag=0.15, forbid=holds(PARTY3)),
            chapter("down-revive3", "the guest goes down and the second guest, not the host, "
                                    "revives it",
                    {"guest": ["godmode off", *LETHAL_FALL, "wait-vital down 30",
                               "expect-hp self == 1", "wait-vital up 90", "expect-hp self > 1",
                               "godmode"],
                     "guest2": ["wait-member-vital down 180", "approach-downed", "wait 2",
                                *REVIVE_HITS],
                     "host": ["wait-member-vital down 180"]},
                    expect={"guest": [r"party death: down \d+ on map 0x18010000",
                                      r"party death: revived from down"],
                            "guest2": [r"party death: (real )?revive hit on"]},
                    forbid={"host": SESSION_HOLDS + [r"party death: (real )?revive hit on"],
                            "guest": SESSION_HOLDS, "guest2": SESSION_HOLDS}),
            chapter("crash3", "one guest's game dies; host and the other guest stay together and "
                              "both still pick up items, and the party is three again once it is "
                              "back",
                    {"guest2": ["sync c3-ready", RESTART, START, "godmode", "wait-session 3 480",
                                "wait 10", "sync c3-back", "expect-session 3"],
                     "host": ["sync c3-ready", "wait 20", "pickup 20", "wait 3",
                              "sync c3-host-picked", "sync c3-back", "wait 10",
                              "expect-session 3"],
                     "guest": ["sync c3-ready", "sync c3-host-picked", "pickup 20", "wait 3",
                               "sync c3-back", "wait 10", "expect-session 3"]},
                    expect={role: [r"lost a member; staying where it is"]
                            for role in ("host", "guest")},
                    allow=CRASH_CLEANUP),
            wipe(PARTY3),
        ],
    },
}


def compile_suite(suite: dict, only: list[str] | None = None) -> dict:
    """A suite as the runner runs it (a scenario with chapters): each game's script as stages,
    the chapters' checks, and every sync with its chapter and games. `only` keeps those chapters
    and the setup ones."""
    roles = list(suite["roles"])
    chapters = [c for c in suite["chapters"]
                if only is None or c["name"] in only or c.get("setup")]
    stages: dict[str, list[list[str]]] = {role: [[START]] for role in roles}
    stage_saves: dict[str, dict[int, str]] = {}
    syncs: dict[str, dict] = {}
    outages: dict[str, float] = {}
    for c in chapters:
        keep_going = f"keep-going {'off' if c.get('fatal') else 'on'}"
        syncs[c["name"]] = {"chapter": c["name"], "roles": list(roles)}
        outages.update(c.get("outages", {}))
        for role in roles:
            stages[role][-1] += [f"chapter {c['name']}", keep_going, f"sync {c['name']}"]
            for step in c["steps"].get(role, []):
                if step == RESTART or step.startswith(RESTART_WITH):
                    # Started again: the same chapter, from the title screen (with other saves
                    # put in first for restart_with).
                    stages[role].append([f"chapter {c['name']}", keep_going])
                    if step != RESTART:
                        saves = step[len(RESTART_WITH):-1]
                        stage_saves.setdefault(role, {})[len(stages[role]) - 1] = saves
                    continue
                stages[role][-1].append(step)
                if step.startswith("sync "):
                    name = step.split()[1]
                    syncs.setdefault(name, {"chapter": c["name"], "roles": []})
                    syncs[name]["roles"].append(role)
    checks = ("about", "expect", "forbid", "allow", "same_hp", "same_hp_census", "hp_sums_hits",
              "max_hp_lag", "files_present", "files_absent")
    return {
        "about": suite["about"],
        "roles": {role: script if len(script) > 1 else script[0]
                  for role, script in stages.items()},
        "settings": suite.get("settings", {}),
        "timeout": suite.get("timeout", 1800),
        "join_within": suite.get("join_within"),
        "chapters": [{"name": c["name"], "roles": roles,
                      **{key: c[key] for key in checks if key in c}} for c in chapters],
        "fatal": [c["name"] for c in chapters if c.get("fatal")],
        "syncs": syncs,
        "outages": outages,
        "stage_saves": stage_saves,
        **({"saves": suite["saves"]} if "saves" in suite else {}),
    }


SUITE_SCENARIOS = {name: compile_suite(suite) for name, suite in SUITES.items()}
