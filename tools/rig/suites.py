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
import budget
from scenarios import (AWAY_FROM_LAMP, BLOODSTAIN_DROPPED, BLOODSTAIN_STOLEN,  # noqa: F401
                       CATHEDRAL_WARD, CATHEDRAL_WARD_LAMP, CENTRAL_YHARNAM, CENTRAL_YHARNAM_LAMP,
                       CLERIC_BEAST, CLERIC_BEAST_DEFEATED, CLINIC, CRASH_CLEANUP, DOG, ENEMY,
                       FORBIDDEN_WOODS_LAMP_LIT, GASCOIGNE, GASCOIGNE_DEFEATED, GIVE_UP,
                       HUNTERS_DREAM, LETHAL_FALL, LEVEL_SYNC, OTHER_MAP_FLAG, PARTY_DEATH,
                       REVIVE_HITS, SESSION_HOLDS, HOST_JOINS, GUEST_JOINS, START,
                       STORY_HIDDEN, THIS_MAP_FLAG, WARD_ENEMY, WARD_ENEMY_2, WARD_NPC,
                       GUEST_ALONE_SPOT,
                       IOSEFKA_WAIT, HP_LOWERED, KNEEL, YHARNAM_TO_WARD, attack, only_kill)

DREAM = "0x15000000"  # the Hunter's Dream's map (HUNTERS_DREAM is its lamp)
# The Cleric Beast's event (12411700) gives the Sword Hunter Badge (item lot 50000010) unless flag
# 6645 ("holds the badge", set in the Dream) is on. The host holds it; the guest has none but the
# host's 6645 in its save, as the team's world put it there before the flag was each player's own.
SWORD_BADGE = "goods:4114"
NO_SWORD_BADGE = [f"trade-stock {SWORD_BADGE} 0", "set-flag 6645 0"]
HOLDS_SWORD_BADGE = [f"trade-stock {SWORD_BADGE} 1", "set-flag 6645 1"]
HOSTS_SWORD_BADGE_FLAG = [f"trade-stock {SWORD_BADGE} 0", "set-flag 6645 1"]
# The party list's icons came (Kaniel 2026-10-12: no team icons; the log said "icons none").
PARTY_LIST_ICONS = r"party list is the HUD's own \(\d+ tags, icons (?!none)"
# A Dream gift messenger the guest's game switched off as the host has it (Ada 2026-10-12: no
# gifts after his first death; the host had taken its own, its messengers gone).
HOSTS_MESSENGERS = (r"characters switched like the host has them on map 0x15000000 \(off, \+on\):"
                    r".* 0x1000000[0-3]\b")

RESTART = "<restart>"
# Steps a game runs on its title screen, before it is in the world (the lobby's, the title's).
TITLE_STEPS = ("team-", "title-")
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

DOG_MAX_HP = 364
# An enemy no chapter before these hurts, awake by the first lamp (179 HP, 30 m away). The dog is
# not one for real hits: in the rig's saves the game keeps it hidden (+0x28, no backread) at 35 m,
# so no swing lands (rig 2026-10-13), and after hp-apart its HP is the host's shared damage.
FRESH_ENEMY = "0x1001402e"
ENEMY_MAX_HP = 484
GUESTS = ("guest", "guest2")
# Phase 10's bug, HP bars that lag: a chapter's other checks still fail it (known_problems).
HP_LAG_BUG = {"known_bug": "phase 10: HP bars lag behind the hit (0.17-1.2 s measured, the limit "
                           "0.15 s)",
              "known_problems": [r"reached the other games' bars in", r"'s hits reached \S+ bar"]}

# The rig refresh's tests of recent bugs (Kaniel 2026-10-15). A test of a bug whose fix has not
# landed carries known_bug (what is wrong, and the phase that fixes it): it shows as KNOWN, and the
# runner says when it passes. A log line such a test expects and no build writes yet is the
# fix's to write: the test is its contract.
# Where a player stands at the first lamp (its census), to come back to without a trip.
AT_THE_LAMP = "-199.7 -50.7 179.0"
# Phase 7's goal for a join: from the guest's load's end to its session, at most this long.
JOIN_AFTER_LOAD_S = 10
# The team's load screen (or its bar) shown and never gone after (Lisara 2026-10-14: after
# Continue the team bars stayed on top).
LOAD_VIEW_STAYS = r"load view is up \(\d+ rows\)(?![\s\S]*load view is gone)"


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
                   expect={"host": [HOST_JOINS],
                           **{guest: [GUEST_JOINS] for guest in roles[1:]}},
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


# Open sky, on the lower ground west of the first lamp (rig 2026-10-18: the old spots were
# under a roof 9 m up, so every rise there landed on it, out of bounds, and the revives' swings
# went wide; Kaniel). East of the lamp the open strip ends in a drop past x -194.
WIPE_SPOT = {"host": "-212.0 -54.5 170.0", "guest": "-209.0 -54.5 170.0",
             "guest2": "-209.0 -54.5 174.0"}


# A guest's lethal fall with the host by it: from its own spot, or it lands on the host's body
# and takes no damage (rig 2026-10-13: party-death's guest never went down after the join, both
# at the lamp; the scenario version waited 15 s and the host had moved on).
GUEST_FALL = [f"teleport {WIPE_SPOT['guest']}", "wait 2", *LETHAL_FALL]


# Every game of a party wipe runs the wipe: kills, enemy HP and the world log start over
# (Kaniel's playtest 2026-10-18: the last member to die took the party's wipe for its own, the
# first one already up again by its respawn, and the enemies kept their HP).
WIPE_RAN = r"Co-op lifecycle: wipe, session epoch \d+ \(reset: .*enemy hp"
WIPE_TAKEN_AS_OWN = r"this death is no party wipe"


def wipe(roles, name: str = "wipe", guests_point: int | None = None,
         hurt_first: bool = False) -> dict:
    """Party death's wipe, last in its suite (the map reloads): every guest goes down, then the
    host falls with nobody standing; all die, wake at their lamps with the map reset (Kaniel
    2026-10-11: after the wipe 16 enemies stayed hidden and the mob showed three HPs), each with
    its own bloodstain and no enemy taking it.

    `hurt_first`: the host hurts ENEMY first and every game sees it hurt; after the wipe it
    is whole again in every game (Kaniel's playtest 2026-10-18: enemies kept the HP they had
    when the party wiped)."""
    players = len(roles)
    # Each falls on its own spot a few metres from the lamp, so nobody lands on another member's
    # body (rig 2026-10-12: a body under the fall took its damage and guest2 never went down; the
    # same spread as wipe3).
    spread = {role: [f"teleport {WIPE_SPOT[role]}", "wait 2"] if role in WIPE_SPOT else []
              for role in roles}
    steps = {"host": ["godmode off", f"sync {name}-guests-down", *spread["host"], *LETHAL_FALL,
                      "wait-vital dead 30", "wait-vital up 150"]}
    if hurt_first:
        # Godmode until the guests are down: the hurt enemy comes for the host.
        steps["host"] = [f"hit {ENEMY} 30", "wait 3", f"sync {name}-hurt", "wait 5",
                         "census 40", f"expect-hp {ENEMY} < {ENEMY_MAX_HP}",
                         f"sync {name}-guests-down", "godmode off", *steps["host"][2:]]
    for guest in roles[1:]:
        # `guests_point`: the guests rest elsewhere first (a return point that stood 10 s,
        # ReturnPointSettleMs), so the wipe shows where they wake: where the host does.
        own_point = [f"return-point {guests_point}", "wait 12"] if guests_point else []
        steps[guest] = ["godmode off", *own_point, *spread[guest], *LETHAL_FALL,
                        "wait-vital down 30", f"sync {name}-guests-down", "wait-vital dead 90",
                        "wait-vital up 150"]
        if hurt_first:
            steps[guest] = [f"sync {name}-hurt", "wait 5", "census 40",
                            f"expect-hp {ENEMY} < {ENEMY_MAX_HP}", *steps[guest]]
    for role in roles:
        steps[role] += [f"wait-members {players} 300", f"sync {name}-woken", "wait 15",
                        "census 60", f"expect-hp {ENEMY} == {ENEMY_MAX_HP}",
                        f"expect-hidden {ENEMY} 0", f"expect-hp {DOG} == {DOG_MAX_HP}",
                        f"expect-session {players}"]
        if guests_point:
            steps[role].append(f"expect-others-near {players - 1} 30")
    # The dog's +0x28 reads 1 at full HP in every game after a wipe (rig 2026-10-12, smoke): the
    # map's own state, not a kill hiding it (its HP is checked).
    return chapter(
        name, "a party wipe resets the map for everyone, the same in every game", steps,
        expect={"host": [r"party death: died for real on map 0x18010000 \(reset=true\)",
                         r"travelling there \(reset=true\)", BLOODSTAIN_DROPPED, WIPE_RAN],
                **{guest: [r"party death: wipe \d+ ends down \d+; dies \(reset=true",
                           r"travelling there \(reset=true\)", BLOODSTAIN_DROPPED, WIPE_RAN,
                           *([rf"respawn at the host's point {CENTRAL_YHARNAM_LAMP} "
                              rf"\(own {guests_point}\)"] if guests_point else [])]
                   for guest in roles[1:]}},
        forbid={role: SESSION_HOLDS + [BLOODSTAIN_STOLEN, WIPE_TAKEN_AS_OWN] for role in roles})


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


# Back at the first lamp with everyone, for the chapters after one that ends in the Dream.
BACK_TO_WARD = [f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 3 300"]

PARTY3 = ("host", "guest", "guest2")
PARTY2 = ("host", "guest")

# Borders between two maps the rig crosses on foot (Kaniel 2026-10-12: the hardest bugs come where
# two maps meet, crossed back and forth). Each comes from a player's log: its "Bloodborne map
# change <from> -> <to> at (...); trail: x y z ..." line (LogMapChanges) is the path a player
# walked over it, which glide replays. "lamp" is the warp point nearest the trail's start (the
# players get there by lamp, then the short teleport to the trail's start).
BORDERS: dict[str, dict] = {
    # Kaniel 2026-10-13 00:12 (kaniel-20261013b:9830), after Father Gascoigne died: the map
    # flipped back and forth 3 times on this walk, each flip a "time of day" world reload taken
    # for a trip; the last trip never ended, and Ada's game left the session from 0x193c68e.
    "tomb": {"from": CENTRAL_YHARNAM, "to": CATHEDRAL_WARD, "lamp": 2412953,
             "trail": "20.2 -17.3 103.1 23.3 -17.4 102.0 26.4 -17.3 102.2 29.6 -17.4 103.4 "
                      "32.7 -17.3 102.9 33.6 -17.4 102.6 35.1 -17.4 102.0 38.0 -16.1 100.4 "
                      "40.2 -14.6 98.0 39.4 -13.2 94.9 37.2 -11.8 92.7 34.2 -10.4 93.3 "
                      "31.3 -9.1 94.9"},
}
# A border crossed must not leave the session or keep a trip going (Kaniel 2026-10-13).
BORDER_FORBID = [r"LeaveSession from the game", r"member leave .* trip=true"]
BORDER_CROSSINGS = 3


def border_crossings(name: str, border: dict, *, name_as: str | None = None,
                     crossings: int = BORDER_CROSSINGS, about: str | None = None) -> dict:
    """Both players cross the border together and back, `crossings` times; after each way both
    are on the same map, and at the end the session holds and each sees the other. The chapter
    is border-<name> unless `name_as` names it."""
    points = border["trail"].split()
    back = " ".join(" ".join(points[at:at + 3]) for at in range(len(points) - 3, -1, -3))
    start = " ".join(points[:3])
    steps = [f"travel {border['lamp']}", "wait-members 2 300", f"sync {name}-there",
             f"teleport {start}", "wait 3"]
    for crossing in range(1, crossings + 1):
        steps += [f"glide {border['trail']}", f"sync {name}-{crossing}-over", "wait 5",
                  f"expect-map {border['to']}", f"glide {back}", f"sync {name}-{crossing}-back",
                  "wait 5", f"expect-map {border['from']}"]
    steps += ["wait 10", "census 60", "expect-session 2", "expect-others-near 1 20"]
    return chapter(name_as or f"border-{name}",
                   about or f"both cross the {name} border {crossings} times and back: one map "
                            "each time, nobody lost",
                   {role: list(steps) for role in PARTY2},
                   forbid={role: SESSION_HOLDS + BORDER_FORBID for role in PARTY2})

def ward_kill_held() -> list[dict]:
    """Kaniel 2026-10-15 (Lisara's log, friend38): both walked from Central Yharnam into the
    Cathedral Ward and killed there; Kaniel went to the Dream and back while Lisara stood in the
    Ward, and the kills were alive again. A walk keeps the visit, and the kills were filed under
    the map it began on, where nobody was. Two chapters: the walk and the kill, then the trip."""
    border = BORDERS["tomb"]
    start = " ".join(border["trail"].split()[:3])
    # The glide alone sometimes leaves the map on Central Yharnam (border-after-gascoigne's known
    # bug), so each then steps on, by the Ward's lamp: still a walk, nothing loads.
    walk = [f"travel {border['lamp']}", "wait-members 2 300", "sync wkh-there",
            f"teleport {start}", "wait 3", f"glide {border['trail']}",
            f"glide {YHARNAM_TO_WARD}", "sync wkh-over", "wait 5", f"expect-map {CATHEDRAL_WARD}"]
    return [
        chapter("ward-walk-kill", "both walk from Central Yharnam into the Cathedral Ward; the "
                                  "host kills an enemy there",
                {"host": [*walk, *attack(WARD_ENEMY), "wait 5", "sync wkh-killed"],
                 "guest": [*walk, "sync wkh-killed"]},
                expect={"host": [r"kill board gets 1 kills"]},
                forbid={"host": only_kill(WARD_ENEMY), "guest": SESSION_HOLDS}),
        chapter("ward-kill-held", "the host goes to the Dream and back to the Ward's lamp while "
                                  "the guest stays in the Ward: the kill stands in both games",
                {"host": [f"travel {HUNTERS_DREAM}", "wait 5", f"travel {CATHEDRAL_WARD_LAMP}",
                          "wait-members 2 300", "sync wkh-back", "wait 15", "census 60",
                          f"expect-hp {WARD_ENEMY} == 0"],
                 "guest": ["sync wkh-back", "wait 15", "census 60",
                           f"expect-hp {WARD_ENEMY} == 0"]},
                forbid={"host": SESSION_HOLDS + [rf"kill board gets \d+ kills, [1-9]\d* back "
                                                 rf".*back {WARD_ENEMY}"],
                        "guest": SESSION_HOLDS}),
    ]


SUITES = {
    # Before every kit (the robustness review: 12 of the night's bugs came from untested kits).
    "smoke": {
        "about": "a party of three as players play: join, trip, hits from three worlds, a kill "
                 "apart, pickups, a guest's crash and rejoin, a wipe",
        "roles": PARTY3,
        "settings": AS_PLAYERS,
        "join_within": 240,
        "chapters": [
            join(PARTY3),
            chapter("trip", "the host goes to the Dream and back; the session holds and it "
                            "re-creates both guests; everyone sees both others",
                    {"host": [*trip_back("host", 3, "trip-back"), "wait 20", "snapshot",
                              "expect-session 3", "expect-others-near 2 15"],
                     **{guest: ["sync trip-back", "wait 20", "snapshot", "expect-session 3",
                                "expect-others-near 2 15"] for guest in GUESTS}},
                    # Phase 11: the nameplates over the others show their characters' names
                    # (2026-10-19: the game itself names them so, never by their online ids).
                    expect={"host": [r"re-creating (?:\S+ )*rig_guest (?:\S+ )*on map 0x18010000",
                                     r"re-creating (?:\S+ )*rig_guest2 (?:\S+ )*on map 0x18010000",
                                     r'nameplate \d is "(?!rig_)[^"]+"'],
                            **{guest: [r'nameplate \d is "(?!rig_)[^"]+"'] for guest in GUESTS}},
                    forbid=holds(PARTY3),
                    known_bug="after the host's trip the guests sometimes lose its character "
                              "(backlog, docs/HANDOFF.md; rig 2026-10-17, 1 of 3 smoke runs)",
                    known_problems=[r"expect-others-near 2 15 FAILED",
                                    r"nameplate"]),
            chapter("hit3", "each guest back from its own trip (three worlds apart), all three "
                            "hit one enemy in turn: one HP in every game, each hit counted once, "
                            "on the other bars within 0.15 s",
                    {role: ([] if role == "host" else
                            [f"travel {HUNTERS_DREAM}", "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}",
                             "wait-members 3 300"]) + steps
                     for role, steps in hits_in_turn(PARTY3, ENEMY, "hit3",
                                                     ready="hit3-apart").items()},
                    same_hp=ENEMY, hp_sums_hits=ENEMY, max_hp_lag=0.15, forbid=holds(PARTY3),
                    **HP_LAG_BUG),
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
                              "2026-10-11: Brom's pickups wiped by a restart)",
                    {"guest": [RESTART, START, "godmode", "wait-session 3 480", "wait 10",
                               "sync rejoin-back", "expect-session 3"],
                     "host": ["sync rejoin-back", "wait 10", "expect-session 3"],
                     "guest2": ["sync rejoin-back", "wait 10", "expect-session 3"]},
                    # PR #37: the others note the member gone, and the guest counts as standing
                    # only once it is in the session's world again (a wipe waited for it).
                    expect={"host": [r"Co-op lifecycle: member left"],
                            "guest2": [r"Co-op lifecycle: member left"],
                            "guest": [GUEST_JOINS]},
                    forbid={"guest": [r"new character in save slot"]},
                    allow=CRASH_CLEANUP),
            chapter("overlay-textures", "each game opens the port's menu and goes through its "
                                        "pages: the overlay's textures fit its descriptor pool "
                                        "(0.2.4: icons and runes garbled past 16 textures)",
                    everyone(PARTY3, "keys toggle r1 r1 r1 r1 r1 back", "wait 5",
                             "expect-session 3"),
                    expect={role: [r"menu keys toggle r1"] for role in PARTY3},
                    overlay_textures=list(PARTY3), forbid=holds(PARTY3)),
            chapter("boss-badge", "the Cleric Beast dies in the host's world with both guests "
                                  "there; the second guest's save has a host's \"holds the "
                                  "badge\" flag (as a team world put it there): each player has "
                                  "one Sword Hunter Badge after (Kaniel 2026-10-18: of three, "
                                  "the second guest got none)",
                    {"host": [*NO_SWORD_BADGE, "sync badge3-set", "wait 5",
                              f"kill {CLERIC_BEAST}", f"wait-flag {CLERIC_BEAST_DEFEATED} 1 60",
                              "sync badge3-dead", "wait 30", "expect-session 3",
                              f"expect-held {SWORD_BADGE} 1 20"],
                     "guest": [*NO_SWORD_BADGE, "sync badge3-set", "sync badge3-dead", "wait 30",
                               "expect-session 3", f"expect-held {SWORD_BADGE} 1 20"],
                     "guest2": [*HOSTS_SWORD_BADGE_FLAG, "sync badge3-set", "sync badge3-dead",
                                "wait 30", "expect-session 3",
                                f"expect-held {SWORD_BADGE} 1 20"]},
                    forbid=holds(PARTY3)),
            # After the rejoin chapter's restart the guest's lethal fall is a death alone, not a
            # down (rig: smoke's wipe failed in 10 of 12 runs; party-death's, without a restart
            # just before, passes).
            {**wipe(PARTY3), "known_bug": "intermittent: after a guest's restart and rejoin, its "
                                          "lethal fall is a real death, not a down, and the wipe "
                                          "goes wrong (rig 2026-10-15: 2 of 3)"},
        ],
    },
    "together": {
        "about": "two players in one world (after the join, before any trip): what each sees, "
                 "shares and picks up, talks by the lamp, deaths at once, and the bosses",
        "roles": PARTY2,
        "settings": AS_PLAYERS,
        "join_within": 180,
        "chapters": [
            join(PARTY2),
            chapter("story-hidden", "a character the host's story hides (Iosefka) stays hidden "
                                    "on the guest: it hides and shows characters as the host",
                    {"host": ["census 80", f"expect-hidden {STORY_HIDDEN} 1"],
                     "guest": ["census 80", f"expect-hidden {STORY_HIDDEN} 1 30"]},
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
                    # Same HP is the check: the "one world" line comes once, at the join.
                    forbid=holds(PARTY2)),
            chapter("hp-whole", "together, the host takes the dog to 300 HP: the guest's copy "
                                "shows 300, not its low byte (Kaniel 2026-10-12: the world log "
                                "sent the dog's 438 HP as 182)",
                    {"host": [f"hurt {DOG} 300", "sync hpw-hurt", "wait 10",
                              f"expect-hp {DOG} == 300"],
                     "guest": ["sync hpw-hurt", "wait 10", f"expect-hp {DOG} == 300"]},
                    forbid=holds(PARTY2)),
            # Phase 8 (whole-map owner): enemies by a teammate far from the map's owner run and
            # stay on the ground: the owner wakes the groups around every teammate and runs them.
            chapter("apart-fight", "the host goes far up by Iosefka's clinic, the guest walks up "
                                   "to an enemy by the lamp: it comes for the guest and the "
                                   "guest's hit lands",
                    {"host": ["sync af-go", f"teleport {IOSEFKA_WAIT}", "sync af-apart",
                              "sync af-done", f"teleport {AT_THE_LAMP}", "wait 3"],
                     "guest": ["sync af-go", "sync af-apart", "wait 3", "census 40",
                               f"teleport {GUEST_ALONE_SPOT}", "wait 3", "census 40", "wait 20",
                               "census 40", f"hit {ENEMY} 30", "wait 3", "sync af-done",
                               f"teleport {AT_THE_LAMP}", "wait 3"]},
                    moves={"guest": ENEMY},
                    expect={"guest": [rf"hit on {ENEMY} by this player"],
                            "host": [r"runs \d+ awake enemies the game let go"]},
                    forbid=holds(PARTY2)),
            chapter("apart-collision", "with the host far away again, the enemy by the guest stays "
                                       "on the ground (no fall through a map whose collision only "
                                       "the far owner loaded)",
                    {"host": ["sync ac-go", f"teleport {IOSEFKA_WAIT}", "sync ac-apart",
                              "sync ac-done", f"teleport {AT_THE_LAMP}", "wait 3"],
                     "guest": ["sync ac-go", "sync ac-apart", f"teleport {GUEST_ALONE_SPOT}",
                               "wait 3", "census 40", "wait 10", "census 40", "wait 10",
                               "census 40", "wait 10", "census 40", "sync ac-done",
                               f"teleport {AT_THE_LAMP}", "wait 3"]},
                    grounded={"guest": ENEMY}, forbid=holds(PARTY2)),
            chapter("apart-hurt", "with the host far away, the enemy by the guest hurts it (its "
                                  "hits on a guest from the owner's game land)",
                    {"host": ["sync ah-go", f"teleport {IOSEFKA_WAIT}", "sync ah-apart",
                              "sync ah-done", f"teleport {AT_THE_LAMP}", "wait 3"],
                     "guest": ["sync ah-go", "sync ah-apart", "godmode off",
                               f"teleport {GUEST_ALONE_SPOT}", "wait 25",
                               "expect-hp self < 594", "godmode", "sync ah-done",
                               f"teleport {AT_THE_LAMP}", "wait 3"]},
                    forbid=holds(PARTY2)),
            chapter("near-kill", "both strike one enemy to death at once: it dies once in each "
                                 "world, body and loot there, never hidden (Kaniel 2026-10-11: the "
                                 "hunter died twice and vanished with its loot)",
                    {role: ["sync near-kill-go", *attack(ENEMY), "wait 10", "census 40",
                            f"expect-hp {ENEMY} == 0", f"expect-hidden {ENEMY} 0"]
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
                              "sync refight-back"],
                     "guest": ["sync refight-back"]},
                    forbid=holds(PARTY2)),
            chapter("boss-refight-kill", "back in the reloaded world Gascoigne stands, is not "
                                         "hidden by the guest's old kill, and dies in the host's "
                                         "world",
                    {"host": ["wait 15", f"expect-hp {GASCOIGNE} > 0",
                              f"expect-hidden {GASCOIGNE} 0",
                              f"kill {GASCOIGNE}", f"wait-flag {GASCOIGNE_DEFEATED} 1 60",
                              "wait 10", "expect-session 2", "sync refight-done"],
                     "guest": ["sync refight-done", "expect-session 2"]},
                    # Not "a member's kill hid something" before his: after the host's reload
                    # near-kill's enemy dies again as held kills do (rig 2026-10-13); his HP and
                    # +0x28 are the check.
                    forbid=holds(PARTY2)),
            chapter("boss-kill", "the Cleric Beast dies in the host's world with the guest there; "
                                 "the session holds. The host holds the Sword Hunter Badge "
                                 "already and the guest's save has the host's \"holds it\" flag "
                                 "(as a team world put it there): the guest gets its own badge, "
                                 "the host no second one (Kaniel 2026-10-18: a guest never got "
                                 "it)",
                    {"host": [*HOLDS_SWORD_BADGE, "sync badge-set", "wait 5",
                              f"kill {CLERIC_BEAST}", f"wait-flag {CLERIC_BEAST_DEFEATED} 1 60",
                              "sync boss-dead", "wait 30", "expect-session 2",
                              f"expect-held {SWORD_BADGE} 1", "expect-flag 6645 1"],
                     "guest": [*HOSTS_SWORD_BADGE_FLAG, "sync badge-set", "sync boss-dead",
                               "wait 30", "expect-session 2", f"expect-held {SWORD_BADGE} 1 20",
                               "expect-flag 6645 1"]},
                    expect={"host": [r"flags in session: .*\b4000\b"]},
                    forbid=holds(PARTY2)),
            chapter("boss-badge-kept", "the guest got the badge in the join's state (a phantom, "
                                       "before any trip): it still holds it after its first trip "
                                       "to the Dream (Kaniel 2026-10-18: krolebord's went at his "
                                       "first trip after the Cleric Beast)",
                    {"host": ["sync badge-kept"],
                     "guest": [f"travel {HUNTERS_DREAM}", f"wait-map {DREAM} 90", "wait 10",
                               f"expect-held {SWORD_BADGE} 1", "sync badge-kept"]},
                    # Since #267 a phantom's list holds the guest's own key items, so the badge
                    # stays without the give-back (rig 2026-10-20: held, no "gave back" line).
                    forbid=holds(PARTY2)),
        ],
    },
    "trips": {
        "about": "two players whose worlds go apart by trips to the Dream: who sees whom, HP and "
                 "kills across worlds, pickups after trips, a guest alone on a map, boss put-downs",
        "roles": PARTY2,
        "settings": AS_PLAYERS,
        "join_within": 180,
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
                    known_bug="intermittent: after the host's trip the guest is sometimes not "
                              "near it (rig 2026-10-15, 1 of 2 full passes)",
                    known_problems=[r"expect-others-near 1 15 FAILED"],
                    forbid=holds(PARTY2)),
            chapter("story-after-trip", "the guest goes to the Dream and back: Iosefka, whom "
                                        "the host's story hides, stays hidden while its world "
                                        "loads (Kaniel 2026-10-12: she stood in the guest's "
                                        "Central Yharnam for about 6 s after a trip)",
                    {"guest": [*trip_back("guest", 2, "sat-back"), "wait 1",
                               f"expect-hidden {STORY_HIDDEN} 1", "wait 5",
                               f"expect-hidden {STORY_HIDDEN} 1"],
                     "host": ["sync sat-back"]},
                    forbid=holds(PARTY2)),
            chapter("hit-after-host-trip", "only the host went to the Dream: the guest's hits "
                                           "still reach the host's copy, each once",
                    {**hits_in_turn(("guest", "host"), ENEMY, "haht")},
                    same_hp=ENEMY, hp_sums_hits=ENEMY, forbid=holds(PARTY2),
                    known_bug="intermittent: after the host's trip the guest's hit on the enemy "
                              "sometimes never lands (rig 2026-10-15, 1 of 2 full passes)",
                    known_problems=[r"no hit landed in", r"no census of 0x1001400b",
                                    r"no hit on 0x1001400b", r"took \d+ of 0x1001400b"]),
            chapter("guest-hit-once", "after the guest's trip too: its real hits count once in "
                                      "both games (Kaniel's friend 2026-10-11: each hit twice)",
                    {role: (trip_back("guest", 2, "gho-back") + ["wait 15"]
                            if role == "guest" else ["sync gho-back", "wait 15"]) + steps
                     for role, steps in hits_in_turn(("guest", "host"), FRESH_ENEMY,
                                                     "gho").items()},
                    same_hp=FRESH_ENEMY, hp_sums_hits=FRESH_ENEMY,
                    # The HP board line comes once, when the worlds go apart (the trip before).
                    expect={"guest": [rf"hit on {FRESH_ENEMY} by this player"]},
                    known_bug="intermittent: after the guest's trip the host's copy takes only "
                              "some of its hits (rig 2026-10-15: 6 of 10, 1 of 2 full passes)",
                    known_problems=[r"took \d+ of 0x1001402e, the hits \d+"],
                    forbid=holds(PARTY2)),
            chapter("hp-apart", "worlds apart, the host takes the dog to 300 HP: the guest's copy "
                                "follows, all of it (more than a byte: Kaniel 2026-10-12, the "
                                "dog's 438 reached the guest as 182), and it is still 300 after "
                                "the guest's own trip (the dog showed full HP after a return)",
                    {"host": [f"hurt {DOG} 300", "sync hp-apart-hurt", "wait 12",
                              f"expect-hp {DOG} == 300", "sync hp-apart-back"],
                     "guest": ["sync hp-apart-hurt", "wait 12", f"expect-hp {DOG} == 300",
                               *trip_back("guest", 2, "hp-apart-back"), "wait 15", "census 60",
                               f"expect-hp {DOG} == 300"]},
                    expect={"guest": [HP_LOWERED]},
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
                    {"guest": ["sync dog-away", *attack(FRESH_ENEMY), "wait 5", "sync dog-killed",
                               "wait-members 2 240", "sync dog-back", "wait 20", "snapshot"],
                     "host": [f"travel {HUNTERS_DREAM}", "sync dog-away", "sync dog-killed",
                              f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                              "sync dog-back", "wait 20", "snapshot", "census 60",
                              f"expect-hp {FRESH_ENEMY} == 0"]},
                    known_bug="a kill the guest makes while the host is in the Dream comes back "
                              "to life once the host returns (rig 2026-10-15: full HP in both "
                              "games; backlog, World and progress)",
                    expect={"guest": [rf"(?:kill board gets 1 kills|{FRESH_ENEMY} died in "
                                      r"attack cycle)"],
                            "host": [r"(?:hid 1 enemies a member killed on map 0x18010000|"
                                     r"0x18010000, [1-9]\d* near died)"]},
                    forbid={"guest": only_kill(FRESH_ENEMY)}),
            chapter("kills-held", "kills stay dead while one member stays on the map: the guest "
                                  "goes to the Dream and back, the host stays (Kaniel 2026-10-11: "
                                  "half the location reset after a trip)",
                    {"guest": trip_back("guest", 2, "held-back") + [
                        "wait 15", "census 60", f"expect-hp {ENEMY} == 0"],
                     "host": ["sync held-back", "wait 15", "census 60"]},
                    forbid=holds(PARTY2)),
            *ward_kill_held(),
            map_reset(PARTY2),
            chapter("kill-seen", "together, the host kills an enemy: the guest's copy is dead with "
                                 "the zero hit, not at the kill reward (Kaniel 2026-10-12: kills "
                                 "stood at their last HP for 1-2.4 s)",
                    {"host": ["sync ks-go", *attack(ENEMY), "sync ks-killed", "wait 10",
                              "census 40"],
                     "guest": ["sync ks-go", "sync ks-killed", "wait 1", f"expect-hp {ENEMY} == 0",
                               "wait 10", "census 40"]},
                    forbid=holds(PARTY2)),
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
                     "host": ["sync shops-back"]},
                    # Its gift messengers stay as its own game has them (PR #63).
                    forbid={"guest": [HOSTS_MESSENGERS]}),
            chapter("lone-guest", "a guest alone on a map runs its enemies and hands them back "
                                  "when the host comes",
                    {"guest": [f"travel {CATHEDRAL_WARD_LAMP}", "sync lone-there", "wait 15",
                               *attack(WARD_ENEMY_2), "wait 10", "sync lone-killed",
                               "wait-members 2 300", "wait 20", "snapshot"],
                     "host": ["sync lone-there", "sync lone-killed", "wait 10",
                              f"travel {CATHEDRAL_WARD_LAMP}", "wait-members 2 240", "wait 20",
                              "snapshot"]},
                    expect={"guest": [r"(?:guest alone on map 0x18000000 took over \d+ enemies|"
                                      r"enemy owner: took over \d+ enemies on map 0x18000000)",
                                      r"(?:guest no longer alone; handed back|"
                                      r"enemy owner: this game follows \w+ on map 0x18000000)",
                                      r"Bloodborne echoes: \+\d+ \(rates 1\) from 0x18fc99b"],
                            "host": [r"added \d+ echoes from a kill another member made apart"]}),
            chapter("ward-npc", "both by the Oedon Chapel lamp: the host hurts the NPC there, the "
                                "guest hits it; one HP in both games (Kaniel 2026-10-10)",
                    {"host": [f"hurt {WARD_NPC} 600", "sync npc-hurt", "sync npc-hit", "wait 20",
                              "census 60"],
                     "guest": ["sync npc-hurt", "wait 25", f"expect-hp {WARD_NPC} <= 600",
                               f"hit {WARD_NPC} 20", "sync npc-hit", "wait 20", "census 60"]},
                    same_hp=WARD_NPC, forbid=holds(PARTY2),
                    known_bug="guests can't hurt the Oedon Chapel NPC (backlog; rig 2026-10-15: "
                              "no hit landed in 20 s)"),
            chapter("gascoigne-down", "Father Gascoigne dies in the host's world with the guest "
                                      "there (his cutscene reloads the world); both are back on "
                                      "the map",
                    {"host": [f"set-flag {GASCOIGNE_DEFEATED} 0",
                              f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                              "sync gd-ready", f"kill {GASCOIGNE}",
                              f"wait-flag {GASCOIGNE_DEFEATED} 1 60", "wait 20",
                              "wait-members 2 300", "sync gd-after", "expect-session 2"],
                     "guest": [f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                               "sync gd-ready", "wait 20", "wait-members 2 300", "sync gd-after",
                               "expect-session 2"]},
                    known_bug="after Gascoigne's cutscene the guest's game sometimes leaves the "
                              "session (Ada 2026-10-13; rig 2026-10-15, 1 of 2 full passes)",
                    known_problems=[r"sceNpMatching2LeaveRoom", r"expect-session 2 FAILED"],
                    forbid=holds(PARTY2)),
            {**border_crossings("tomb", BORDERS["tomb"], name_as="border-after-gascoigne",
                             crossings=1,
                             about="after Gascoigne's death both cross the Central Yharnam to "
                                   "Cathedral Ward border and back: no fake trip (Kaniel "
                                   "2026-10-13: the map flipped back and forth)"),
                       "known_bug": "fake trips at the border after Gascoigne (backlog; rig "
                                    "2026-10-15: the host stayed on Central Yharnam, 1 of 2)",
                       "known_problems": [r"expect-map 0x180[01]0000 FAILED", r"LeaveSession",
                                          r"member leave .* trip=true", r"expect-session 2 FAILED",
                                          r"expect-others-near 1 20 FAILED"]},
        ],
    },
    "party-death": {
        "about": "party death: downs and revives, giving up, a death alone, a guest's bloodstain "
                 "after its rejoin, and the wipe",
        "roles": PARTY2,
        "settings": AS_PLAYERS,
        "join_within": 180,
        "chapters": [
            join(PARTY2),
            chapter("down-revive", "the guest falls with 1 HP and goes down; the host strikes it "
                                   "up again",
                    {"host": ["wait-member-vital down 120", "shot sees-down", "approach-downed",
                              "wait 2", *REVIVE_HITS, "wait-member-vital up 30",
                              "shot after-revive"],
                     "guest": ["godmode off", *GUEST_FALL, "wait-vital down 30", "shot down",
                               "press options 200", "wait 1", "press r3 200",
                               "expect-hp self == 1", "wait-vital up 90", "expect-hp self > 1",
                               "godmode"]},
                    # PR #69: Options and R3 do nothing while down; PR #71: R1 hits count as light
                    # ones. No kneel (Kaniel 2026-10-15).
                    expect={"host": [r"party death: ((real )?revive hit on .*, light\)|"
                                     r"revive swing at)"],
                            "guest": [r"party death: down \d+ on map 0x18010000",
                                      r"party death: down, the pad dropped buttons",
                                      r"party death: revived from down"]},
                    forbid={"host": SESSION_HOLDS,
                            "guest": SESSION_HOLDS + [r"party guest died", KNEEL]}),
            chapter("down-revive-heavy", "the host revives the downed guest with R2: each heavy "
                                         "hit counts for more than an R1 (Kaniel 2026-10-12: R2 "
                                         "revived no faster)",
                    {"host": ["wait-member-vital down 120", "approach-downed", "wait 2",
                              f"{REVIVE_HITS[0]} r2", "wait-member-vital up 30"],
                     "guest": ["godmode off", *GUEST_FALL, "wait-vital down 30",
                               "wait-vital up 120", "godmode"]},
                    # A swing that misses the body counts 25 for R1, more for R2 (rig 2026-10-15:
                    # 37).
                    expect={"host": [r"party death: ((real )?revive hit on .*, heavy\)|revive "
                                     r"swing at .* counted for (?:2[6-9]|[3-9]\d|\d{3,}) damage)"],
                            "guest": [r"party death: revived from down"]},
                    forbid=holds(PARTY2)),
            # After the two revives: each down costs more than the one before (twice, then four
            # times the max HP, DownedState), and this restart starts the guest's count again.
            # First (rig 2026-10-18, once this down worked), it made down-revive-heavy the third
            # down: 64 R2 swings, over its budget.
            chapter("death-right-after-join", "the guest's game starts again and joins; its "
                                              "lethal fall 2 s after the join puts it down, and "
                                              "the host revives it",
                    {"guest": [RESTART, START, "wait-session 2 420",
                               f"teleport {WIPE_SPOT['guest']}", "wait 2", *LETHAL_FALL,
                               "wait-vital down 30", "sync drj-down", "wait-vital up 120",
                               "godmode"],
                     "host": ["sync drj-down", "approach-downed", "wait 2", *REVIVE_HITS,
                              "wait-member-vital up 30"]},
                    expect={"guest": [r"party death: down \d+ on map 0x18010000",
                                      r"party death: revived from down"]},
                    forbid={"guest": [r"party guest died"]},
                    allow=CRASH_CLEANUP),
            chapter("down-no-kneel", "the guest goes down: neither its game nor the host's copy "
                                     "of it kneels (Kaniel 2026-10-15)",
                    {"host": ["wait-member-vital down 120", "wait 5",
                              "wait-member-vital up 120"],
                     "guest": ["godmode off", *GUEST_FALL, "wait-vital down 30", "wait 5",
                               "press circle 3600", "wait-vital dead 15", "wait-vital up 90",
                               "godmode"]},
                    expect={"guest": [r"party death: down \d+ on map"]},
                    forbid={role: SESSION_HOLDS + [KNEEL] for role in PARTY2}),
            chapter("down-by-enemy", "the guest with 1 HP next to an enemy, locked on to it, goes "
                                     "down: the lock-on is let go (PR #69), the enemy leaves it "
                                     "alone, and the host revives it. Whether the rig's R3 locks "
                                     "on there is to be confirmed on the first run",
                    # Its end is a give-up, not a revive: a revive takes 50-70 s of swings (rig
                    # 2026-10-15), revives have chapters of their own.
                    {"host": ["wait-member-vital down 150", "wait-member-vital up 150"],
                     "guest": ["godmode off", "hurt self 1", f"approach {ENEMY}", "press r3 200",
                               "wait-vital down 90", "wait 20", "expect-hp self == 1",
                               "press circle 3600", "wait-vital dead 15", "wait-vital up 90",
                               "godmode"]},
                    # Phase 8: the map's owner runs the enemy, so its game forgets the target.
                    expect={"host": [r"party death: enemies forgot \d+ target\(s\) on downed "
                                     r"players \(self down=false, others down=1"],
                            "guest": [r"party death: giving up ends down"]},
                    known_bug="phase 8: with 1 HP (hurt self 1) the owner-run enemy beside the "
                              "guest never strikes; at full HP it strikes and downs the guest (rig "
                              "2026-10-15)",
                    forbid={"host": SESSION_HOLDS,
                            "guest": SESSION_HOLDS + [r"party death: killed while down"]}),
            chapter("give-up", "the downed guest holds Circle and dies; it gets up at its lamp "
                               "without a reload and the session holds",
                    {"guest": ["godmode off", *GUEST_FALL, "wait-vital down 30",
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
                    # Its game made a new HUD after the restart: the party list has its icons.
                    expect={"guest": [r"showed the guest's own bloodstain \([1-9]\d* echoes\)",
                                      PARTY_LIST_ICONS]},
                    allow=CRASH_CLEANUP),
            chapter("down-alone", "with the host in the Dream nobody else is on the guest's map: "
                                  "its lethal fall is a real death that reloads the map",
                    {"host": [f"travel {HUNTERS_DREAM}", "sync alone-away", "sync alone-woken",
                              f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240"],
                     # The host's trip first: with it still on the map the fall is a down.
                     "guest": ["sync alone-away", "wait 15", "godmode off", *LETHAL_FALL,
                               "wait-vital dead 30", "wait-vital up 120", "godmode",
                               "sync alone-woken", "wait-members 2 300"]},
                    expect={"guest": [r"party death: died for real on map 0x18010000 "
                                      r"\(reset=true\)", r"travelling there \(reset=true\)"]},
                    # Right after stain-after-rejoin's restart (as smoke's wipe after rejoin).
                    known_bug="after a guest's restart and rejoin its lethal fall is neither a "
                              "down nor a death alone (rig 2026-10-15, twice)",
                    forbid={"host": SESSION_HOLDS,
                            "guest": SESSION_HOLDS + [r"party death: down \d+"]}),
            chapter("down-session-lost", "the guest is down when the host's game dies: its down "
                                         "ends in a death a few seconds later, not a kneel with no "
                                         "banner (the review, PR #65), and they join again",
                    {"guest": ["godmode off", *GUEST_FALL, "wait-vital down 30", "sync dsl-down",
                               "wait-vital dead 60", "wait-vital up 120", "godmode",
                               "wait-session 2 480", "sync dsl-back", "expect-session 2"],
                     "host": ["wait-member-vital down 120", "sync dsl-down", RESTART, START,
                              "godmode", "wait-session 2 480", "sync dsl-back",
                              "expect-session 2"]},
                    # Phase 11: the host's dead game no longer ends the session at once (its
                    # TCP connection was what ended it; the link waits for it to come back): the
                    # guest is down with nobody standing, a wipe, and dies all the same.
                    expect={"guest": [r"party death: (?:the party session ending|wipe \d+) ends "
                                      r"down \d+; dies"]},
                    allow=CRASH_CLEANUP),
            # Kaniel 2026-10-13: after a wipe everyone wakes where the host does. Kaniel
            # 2026-10-18: the enemy the host hurt before it is whole again.
            wipe(PARTY2, guests_point=2412951, hurt_first=True),
            chapter("stain-after-wipe", "after the wipe the guest's game dies and comes back: "
                                        "in the host's world it sees its own bloodstain from "
                                        "the wipe, with its echoes",
                    {"guest": [RESTART, START, "godmode", "wait-session 2 480", "wait 20",
                               "sync saw-back"],
                     "host": ["sync saw-back"]},
                    expect={"guest": [r"showed the guest's own bloodstain \([1-9]\d* echoes\)"]},
                    known_bug="after a wipe and a restart the guest is not shown its own "
                              "bloodstain (rig 2026-10-15; after a give-up it is)",
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
        "chapters": [
            join(PARTY2),
            chapter("travel-mid-load", "the host goes to the Dream and back while the guest's map "
                                       "reloads (a warp to its lamp), then hurts an enemy: one "
                                       "map, one HP, each sees the other",
                    {"host": ["sync tml-go", f"travel {HUNTERS_DREAM}",
                              "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                              "sync tml-back", f"hit {ENEMY} 30", "wait 15", "census 40",
                              f"expect-map {CENTRAL_YHARNAM}", "expect-others-near 1 30",
                              "expect-session 2"],
                     "guest": ["sync tml-go", f"travel {CENTRAL_YHARNAM_LAMP}",
                               "wait-members 2 300", "sync tml-back", "wait 15", "census 40",
                               f"expect-map {CENTRAL_YHARNAM}", "expect-others-near 1 30",
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
            chapter("link-cut", "phase 7: the guest's connection drops for 3 s, then for 10 s "
                                "(a Wi-Fi blip): both ride it out in the same session, no "
                                "crash and no rejoin, and see each other again",
                    {"host": ["sync cut-go", "sync cut-short", "sync cut-long",
                              "wait-members 2 60", "wait-member-vital up 60", "expect-session 2",
                              "expect-others-near 1 30"],
                     "guest": ["sync cut-go", "cut-link 3", "wait 8", "expect-session 2",
                               "sync cut-short", "cut-link 10", "wait 15", "sync cut-long",
                               "wait-members 2 60", "wait-member-vital up 60",
                               "expect-session 2", "expect-others-near 1 30"]},
                    forbid=holds(PARTY2)),
            chapter("server-restart", "phase 11: shadNet stops and starts again while both play "
                                      "(the host's PC hiccups): the session holds, both games "
                                      "sign in again by themselves through the link and see "
                                      "each other",
                    {role: ["sync restart-go", "wait 20", "wait-members 2 120",
                            "wait-member-vital up 60", "expect-session 2",
                            "expect-others-near 1 30"] for role in PARTY2},
                    server_restarts=["restart-go"],
                    expect={role: [r"Rpc bridge: shadNet ended the connection",
                                   r"the game stays signed in while it comes back",
                                   r"Co-op server world: shadNet started over"]
                            for role in PARTY2},
                    forbid=holds(PARTY2)),
            chapter("cutscene", "Father Gascoigne dies in the host's world with the guest there: "
                                "his death's time-of-day cutscene reloads the host's world (a "
                                "trip), and both are back together after it. Whether this kill "
                                "plays the cutscene again is to be confirmed on the first run",
                    # The cutscene's reload starts within seconds of the kill; then both wait for
                    # the other on the map (no fixed minute: the rig's budget, 2026-10-15).
                    {"host": [f"set-flag {GASCOIGNE_DEFEATED} 0",
                              f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                              "sync cs-ready", f"kill {GASCOIGNE}",
                              f"wait-flag {GASCOIGNE_DEFEATED} 1 60", "wait 20",
                              "wait-members 2 300", "sync cs-after", "wait 10",
                              "expect-session 2", "expect-others-near 1 15"],
                     "guest": ["sync cs-ready", "wait 20", "wait-members 2 300", "sync cs-after",
                               "wait 10", "expect-session 2", "expect-others-near 1 15"]},
                    forbid=holds(PARTY2)),
            *[border_crossings(name, border) for name, border in BORDERS.items()],
            chapter("guest-hits-npc", "both by the Oedon Chapel lamp, the guest hits the NPC "
                                      "there: its hit lands and both games show it hurt",
                    {"host": [f"travel {CATHEDRAL_WARD_LAMP}", "wait-members 2 240",
                              "sync ghn-there", "sync ghn-hit", "wait 5", "census 40",
                              f"expect-hp {WARD_NPC} < 999"],
                     "guest": [f"travel {CATHEDRAL_WARD_LAMP}", "wait-members 2 240",
                               "sync ghn-there", f"hit {WARD_NPC} 20", "sync ghn-hit", "wait 5",
                               "census 40", f"expect-hp {WARD_NPC} < 999"]},
                    known_bug="guests can't hurt some NPCs, the Oedon Chapel one and the hag "
                              "(backlog; Kaniel wants guests to)",
                    forbid=holds(PARTY2)),
        ],
    },
    "crashes": {
        "about": "games that die and come back (one at a time, the other stays up), and deaths "
                 "by giving up",
        "roles": PARTY2,
        "settings": AS_PLAYERS,
        "join_within": 180,
        "chapters": [
            join(PARTY2),
            chapter("deaths", "the host goes down and gives up and gets up without a reload, then "
                              "the guest does away from the lamp, gets up at it and the host sees "
                              "it there",
                    {"host": ["godmode off", *GIVE_UP, "wait-vital up 60", "godmode",
                              "sync host-dead-done", "wait-member-vital down 120",
                              "wait-members 2 120", "sync guest-up", "wait 10",
                              "expect-others-near 1 12", "expect-session 2"],
                     "guest": ["sync host-dead-done", f"teleport {AWAY_FROM_LAMP}", "wait 5",
                               "godmode off", *GIVE_UP, "wait-vital up 60", "godmode",
                               "wait-members 2 120", "sync guest-up", "wait 10",
                               "expect-others-near 1 12", "expect-session 2"]},
                    expect={"host": [r"party death: giving up ends down \d+; dies \(reset=false",
                                     r"live player died in a party session"],
                            "guest": [r"party death: giving up ends down \d+; dies \(reset=false",
                                      r"party guest died; revived"]},
                    forbid=holds(PARTY2)),
            chapter("lamp", "the guest lights a lamp in the session (both worlds lit)",
                    {"host": [f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 0", "sync lamp-lit",
                              f"wait-flag {FORBIDDEN_WOODS_LAMP_LIT} 1 90"],
                     "guest": [f"set-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", "sync lamp-lit",
                               "wait 20"]},
                    forbid=holds(PARTY2)),
            # The return point settles in 10 s (ReturnPointSettleMs): 12 s away before the crash.
            chapter("resume", "the guest's game dies away from the lamp in the host's world: it "
                              "comes back where it was and joins again",
                    {"guest": [f"teleport {AWAY_FROM_LAMP}", "wait 12", RESTART, "godmode",
                               "wait-session 2 420", "wait 5", "expect-session 2",
                               f"expect-map {CENTRAL_YHARNAM}", "sync resume-back"],
                     "host": ["sync resume-back"]},
                    expect={"guest": [r"guest resumed at \(-21\d(\.\d+)?, -5\d(\.\d+)?, "
                                      r"17\d(\.\d+)?\) on map 0x18010000"]},
                    known_bug="phase 7: a restarted guest joins its team at the lamp, it does not "
                              "resume where it was (rig 2026-10-15: no resume line)",
                    known_problems=[r"expected, not seen: guest resumed at"],
                    allow=CRASH_CLEANUP),
            chapter("after-resume", "after its resume the lamp the guest lit is still lit in both "
                                    "worlds, the host's hit reaches its copy, and both pick up "
                                    "items",
                    {"guest": [f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", "sync resume-hurt",
                               "wait 10", f"expect-hp {ENEMY} <= 300", "sync resume-host-picked",
                               "pickup 20", "wait 5"],
                     "host": [f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", f"hurt {ENEMY} 300",
                              "sync resume-hurt", "pickup 20", "wait 5",
                              "sync resume-host-picked"]},
                    known_bug="the lamp the guest lit is out in its world after its restart (rig "
                              "2026-10-15; backlog: changes lost across a load)",
                    known_problems=[r"expect-flag 12707810 1 FAILED"],
                    forbid=holds(PARTY2)),
            chapter("rejoin-enemies", "the guest's game dies beside an enemy the host woke and "
                                      "comes back there (Krolebord 0.2.5: a crash in the game's "
                                      "character update after a rejoin)",
                    {"guest": [f"teleport {GUEST_ALONE_SPOT}", "wait 12", RESTART, "godmode",
                               "wait-session 2 420", "wait 5", "expect-session 2",
                               "sync re-back"],
                     "host": ["sync re-back"]},
                    allow=CRASH_CLEANUP),
            chapter("rejoin-enemies-hold", "a minute beside those enemies after the rejoin (it "
                                           "goes back to them: phase 7 resumes it there): no "
                                           "crash, the session holds",
                    {"guest": [f"teleport {GUEST_ALONE_SPOT}", "wait 30", "census 40", "wait 30",
                               "census 40", "expect-session 2"],
                     "host": ["wait 30", "wait 30", "expect-session 2"]},
                    forbid=holds(PARTY2)),
            chapter("kills-after-crash", "the guest kills an enemy and its game dies: back in the "
                                         "session the enemy is still dead in both games",
                    {"guest": [*attack(FRESH_ENEMY), "wait 5", RESTART, "godmode",
                               "wait-session 2 420", "wait 5", "census 60",
                               f"expect-hp {FRESH_ENEMY} == 0", "sync kac-back"],
                     "host": ["sync kac-back", "wait 5", "census 60",
                              f"expect-hp {FRESH_ENEMY} == 0"]},
                    expect={"guest": [rf"(?:kill board gets 1 kills|{FRESH_ENEMY} died in "
                                      r"attack cycle)"]},
                    known_bug="a crashed guest's kill comes back to life after its rejoin (rig "
                              "2026-10-15: 175 HP in both games; backlog, World and progress)",
                    allow=CRASH_CLEANUP),
            chapter("rejoin-continue", "the guest quits and takes Continue: it joins again and the "
                                       "team's load bars are gone (Lisara 2026-10-14: Continue "
                                       "never rejoined, the bars stayed on top)",
                    # The load view's bar stays a little after the join: 25 s is "stayed".
                    {"guest": [RESTART, START, "godmode", "wait-session 2 420", "wait 25",
                               "expect-session 2", "sync rc-back"],
                     "host": ["sync rc-back"]},
                    expect={"guest": [r"load view is gone"]},
                    forbid={"guest": [LOAD_VIEW_STAYS]},
                    allow=CRASH_CLEANUP),
            # Since #225/#228 a guest's team load goes beside the host (Kaniel 2026-10-20:
            # change the tests to that).
            chapter("crash-apart", "the guest's game dies while it is apart (in the Ward); the "
                                   "host stays in its world, the guest comes back beside the host "
                                   "and they join again",
                    {"guest": [f"travel {CATHEDRAL_WARD_LAMP}", "wait 12", RESTART, "godmode",
                               f"wait-map {CENTRAL_YHARNAM} 180", "wait-session 2 420", "wait 5",
                               "expect-session 2", f"expect-map {CENTRAL_YHARNAM}",
                               "sync apart-back"],
                     "host": ["wait-solo 180", "wait 5", f"expect-map {CENTRAL_YHARNAM}",
                              "sync apart-back", "expect-session 2"]},
                    allow=CRASH_CLEANUP),
            chapter("kill-apart-pays", "back in the session, the guest goes apart (to the Ward): "
                                       "its kill there pays the host",
                    {"guest": [f"travel {CATHEDRAL_WARD_LAMP}", "wait 12", *attack(WARD_ENEMY), "wait 15", "sync kap-done"],
                     "host": ["sync kap-done", "wait 5"]},
                    expect={"guest": [rf"(?:kill board gets 1 kills|{WARD_ENEMY} died in "
                                      r"attack cycle)"],
                            "host": [r"added \d+ echoes from a kill another member made apart"]},
                    forbid=holds(PARTY2)),
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
                     # The item by the lamp went in an earlier chapter (rig 2026-10-13: "no
                     # usable map item near" within 20 m): the nearest within 45 m.
                     "host": ["sync leave-ready", "wait-solo 120", "wait 10", "pickup 45",
                              "wait 3", "snapshot"]},
                    # Since phase 6 the host notes the member gone and reopens its session.
                    expect={"host": [r"(?:lost a member; staying where it is|member left, session "
                                     r"epoch)"]},
                    allow=CRASH_CLEANUP),
            # Last: the guest's game crashes in it (a crash ends the suite).
            chapter("host-crash-apart", "the host's game dies while the guest is apart (in the "
                                        "Ward); the guest stays and rings from there, and they "
                                        "join again without meeting first",
                    {"host": ["sync hca-ready", RESTART, START, "godmode", "wait-session 2 420",
                              "wait 10", "expect-session 2", f"expect-map {CENTRAL_YHARNAM}",
                              "sync hca-back"],
                     "guest": [f"travel {CATHEDRAL_WARD_LAMP}", "wait-members 1 60",
                               "sync hca-ready", "wait-solo 180", "wait 10",
                               f"expect-map {CATHEDRAL_WARD}", "sync hca-back", "wait 10",
                               "expect-session 2", f"expect-map {CATHEDRAL_WARD}"]},
                    allow=CRASH_CLEANUP),
            # Phase 8: the map's owner dies mid-fight; the other game takes the map over with the
            # enemy as hurt as it was, where it was, and it keeps fighting.
            chapter("owner-crash-fight", "the host (the map's owner) hurts the enemy by the guest "
                                         "and its game dies: the guest's game runs it on, as "
                                         "hurt as it was, and it keeps coming for the guest",
                    {"host": ["sync ocf-go", f"teleport {GUEST_ALONE_SPOT}", "wait 3",
                              f"hurt {ENEMY} 200", "wait 3", "sync ocf-hurt", RESTART, START,
                              "godmode", "wait-session 2 420", "wait 5", "expect-session 2",
                              "sync ocf-back"],
                     "guest": ["sync ocf-go", f"teleport {GUEST_ALONE_SPOT}", "sync ocf-hurt",
                               "wait-solo 180", "wait 3", f"expect-hp {ENEMY} <= 200",
                               "census 40", "wait 10", "census 40", f"expect-hp {ENEMY} <= 200",
                               "sync ocf-back"]},
                    moves={"guest": ENEMY}, allow=CRASH_CLEANUP,
                    known_bug="the guest's game crashes (eboot+0x13cffba) once the dead owner's "
                              "game comes back, as in host-crash-apart (backlog: Krolebord's crash "
                              "after a rejoin; rig 2026-10-15)"),
        ],
    },
    "joins": {
        "about": "joins from different places: the host stays up, the guest's game starts again "
                 "for each (its own world first)",
        "roles": PARTY2,
        "settings": AS_PLAYERS,
        "join_within": 240,
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
                    # Its expect-hp is the check: the lowering's line comes only when the guest's
                    # copy had the old HP when it came over (rig 2026-10-15: 1 run of 2).
                    forbid=holds(PARTY2)),
            chapter("join-ward", "joining in Cathedral Ward, whose co-op the guest's own world has "
                                 "not opened",
                    {"host": [f"travel {CATHEDRAL_WARD_LAMP}", "sync jw-host-there",
                              "sync jw-joined", "expect-session 2"],
                     "guest": ["sync jw-host-there", RESTART, START,
                               f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                               "wait-session 2 420", "wait 10", "expect-session 2",
                               "sync jw-joined"]},
                    # The join-anywhere patch line is the boot's, before this chapter (rig
                    # 2026-10-13): the session is the check.
                    allow=CRASH_CLEANUP),
            chapter("join-apart", "joining across maps: the host in the Ward, the guest in "
                                  "Central Yharnam; the guest's join takes it beside the host",
                    {"host": ["sync ja-restarted", "sync ja-joined", "wait 20",
                              "expect-session 2", f"expect-map {CATHEDRAL_WARD}"],
                     # Phase 7: a restarted guest comes back where it last stood in the team's
                     # world (the server's point), so it goes to Central Yharnam first.
                     "guest": [f"travel {CENTRAL_YHARNAM_LAMP}", "wait 12", RESTART, START,
                               "godmode", "sync ja-restarted",
                               "wait-session 2 420", "sync ja-joined", "wait 20",
                               "expect-session 2", f"expect-map {CATHEDRAL_WARD}"]},
                    allow=CRASH_CLEANUP),
            chapter("restart-keeps", "the guest's game starts again in the session: it is the "
                                     "same character, so its kept pickups stay and nothing is "
                                     "backed up (Kaniel 2026-10-11: Brom's pickups wiped by "
                                     "a restart; the review: never wipe on a bad read)",
                    {"guest": [RESTART, START, "godmode", "wait-session 2 420", "wait 10",
                               "sync rk-back", "expect-session 2"],
                     "host": ["sync rk-back", "expect-session 2"]},
                    forbid={"guest": [r"new character in save slot"]},
                    files_absent={"guest": ["bloodborne_party_*.bak-*"]},
                    allow=CRASH_CLEANUP),
            chapter("insert-while-loading", "the guest's game starts again and Insert is pressed "
                                            "on and off while the team's load screen is up: "
                                            "no crash, it joins (0.2.3: two games crashed)",
                    {"guest": [RESTART, "team-ready", "title-keys toggle", "title-wait 3",
                               "title-keys toggle", "title-wait 3", "title-keys toggle",
                               "title-wait 3", "title-keys toggle", START, "keys back",
                               "godmode", "wait-session 2 420", "wait 5", "expect-session 2",
                               "sync iwl-back"],
                     "host": ["sync iwl-back", "expect-session 2"]},
                    expect={"guest": [r"menu keys toggle"]},
                    allow=CRASH_CLEANUP),
            chapter("guest-save-menu", "the guest opens the port's menu (Insert), edits its "
                                       "character on the Save page and closes it: the session "
                                       "holds (Lisara 2026-10-14: dropped after Insert, then save)",
                    {"guest": ["keys toggle", "wait 2", "keys r1 r1 r1 r1 r1", "wait 2",
                               "keys right", "wait 2", "keys back", "wait 20",
                               "expect-session 2", "sync gsm-done"],
                     "host": ["sync gsm-done", "expect-session 2"]},
                    expect={"guest": [r"menu keys right"]},
                    known_bug="after the Save page's edit (echoes +10,000) the guest's game "
                              "tick stood still for 111 s while its frames went on (rig "
                              "2026-10-15; Lisara dropped after Insert, then save)",
                    forbid=holds(PARTY2)),
            chapter("join-time", "the guest's game starts again: from its load's end to its "
                                 "join takes no more than the budget (phase 7: a join costs only "
                                 "the guest's own load)",
                    {"guest": [RESTART, START, "godmode", "wait-session 2 420",
                               "sync jt-back", "expect-session 2"],
                     "host": ["sync jt-back", "expect-session 2"]},
                    max_join_s={"guest": JOIN_AFTER_LOAD_S},
                    allow=CRASH_CLEANUP),
            chapter("new-character", "a guest on a brand-new character (rig/saves/new-character) "
                                     "joins where it stands: its kept lists start clean once, "
                                     "the old ones backed up first",
                    {"guest": [restart_with("new-character"), "godmode", "wait-session 2 420",
                               "wait 20", "expect-session 2", "expect-flag 9401 0",
                               "sync nc-back"],
                     "host": ["sync nc-back", "expect-session 2"]},
                    # PR #63: not at the old character's resume point. PR #68: the Dream's
                    # first-visit flag stays off until its own visit (the clinic lamp showed early).
                    expect={"guest": [r"new character in save slot"]},
                    forbid={"guest": [r"guest resumed at", r"guest resumes on map"]},
                    files_present={"guest": ["bloodborne_party_*.bak-*"]},
                    known_bug="the rig's new-character chapter fails (backlog): the restarted "
                              "guest plays its old character (rig 2026-10-15: resumed, flag 9401 "
                              "on, no new-character line)",
                    allow=CRASH_CLEANUP),
            chapter("first-death-dream", "the new character dies for the first time in the "
                                         "session: it wakes in the Hunter's Dream (its own first "
                                         "death, PR #68), then takes the headstone back and the "
                                         "session holds (Ada 2026-10-12: the headstone after a "
                                         "death trip ended it; still open)",
                    {"guest": ["godmode off", *GIVE_UP,
                               f"wait-map {DREAM} 180", "wait 10",
                               f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 300", "godmode",
                               "sync fdd-back", "wait 10", "expect-session 2"],
                     "host": ["sync fdd-back", "expect-session 2"]},
                    expect={"guest": [r"guest travels by headstone"]},
                    known_bug="needs new-character's brand-new character (backlog; rig "
                              "2026-10-15: the old one's fall put it nowhere near down)",
                    forbid=holds(PARTY2)),
        ],
    },
    "party3": {
        "about": "parties of three: trips in every order, revives by the other guest, a guest's "
                 "crash and leave, and the wipe",
        "roles": PARTY3,
        "settings": AS_PLAYERS,
        "join_within": 240,
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
            # Kaniel's playtest 2026-10-21 (v1.1.2): Lisara's link went quiet while the host and
            # krolebord arrived in Yahar'gul, so she fell off their map boards; their re-create
            # took her for the host and ended. She arrived later, saw both, and neither saw her.
            chapter("late-quiet3", "the host and one guest go to the Dream; the second guest's "
                                   "link drops for 10 s while they are there, then it follows "
                                   "them: every game sees both others",
                    {"host": [f"travel {HUNTERS_DREAM}", "sync lq-there", "sync lq-cut",
                              "wait-members 3 300", "sync lq-all", "wait 25",
                              "expect-session 3", "expect-others-near 2 15", *BACK_TO_WARD],
                     "guest": ["wait 5", f"travel {HUNTERS_DREAM}", "sync lq-there", "sync lq-cut",
                               "wait-members 3 300", "sync lq-all", "wait 25",
                               "expect-session 3", "expect-others-near 2 15", *BACK_TO_WARD],
                     "guest2": ["sync lq-there", "wait 10", "cut-link 10", "wait 15",
                                "sync lq-cut", f"travel {HUNTERS_DREAM}", "wait-members 3 300",
                                "sync lq-all", "wait 25", "expect-session 3",
                                "expect-others-near 2 15", *BACK_TO_WARD]},
                    expect={role: [r"re-creating (?:\S+ )*rig_guest2 (?:\S+ )*on map 0x15000000"]
                            for role in ("host", "guest")},
                    forbid=holds(PARTY3)),
            # guest-to-guest-hp (Kaniel 2026-10-14: lag between guests too): every direction on its
            # own, guest to guest and guest to host as well as host to guest (the plan runs it at
            # --ping 100). One enemy only: the rig's characters kill a fresh one in one hit.
            chapter("hit3", "all three hit one enemy in turn: one HP everywhere, each hit once, "
                            "each game's hits on each other game's bar within 0.15 s",
                    hits_in_turn(PARTY3, ENEMY, "p3hit"),
                    same_hp=ENEMY, hp_sums_hits=ENEMY, max_hp_lag=0.15, max_hp_lag_each=0.15,
                    forbid=holds(PARTY3)),
            chapter("kill-seen3", "the host kills that enemy: it dies on every screen within "
                                  "0.2 s of the killing hit",
                    {"host": ["sync ks3-go", *attack(ENEMY), "sync ks3-killed", "wait 5",
                              "census 40"],
                     **{guest: ["sync ks3-go", "sync ks3-killed", "wait 5", "census 40"]
                        for guest in GUESTS}},
                    killed=ENEMY, max_kill_lag=0.2, forbid=holds(PARTY3)),
            chapter("down-revive3", "the guest goes down and the second guest, not the host, "
                                    "revives it",
                    {"guest": ["godmode off", *GUEST_FALL, "wait-vital down 30",
                               "expect-hp self == 1", "wait-vital up 90", "expect-hp self > 1",
                               "godmode"],
                     "guest2": ["wait-member-vital down 180", "approach-downed", "wait 2",
                                *REVIVE_HITS],
                     "host": ["wait-member-vital down 180"]},
                    expect={"guest": [r"party death: down \d+ on map 0x18010000",
                                      r"party death: revived from down"],
                            "guest2": [r"party death: ((real )?revive hit on|revive swing at)"]},
                    forbid={"host": SESSION_HOLDS + [r"party death: (real )?revive hit on"],
                            "guest": SESSION_HOLDS, "guest2": SESSION_HOLDS}),
            # Before crash3: after a guest's restart its lethal fall is a real death (smoke's wipe).
            wipe(PARTY3),
            chapter("crash3", "one guest's game dies; host and the other guest stay together and "
                              "both still pick up items, and the party is three again once it is "
                              "back",
                    {"guest2": ["sync c3-ready", RESTART, START, "godmode", "wait-session 3 480",
                                "wait 10", "sync c3-back", "expect-session 3"],
                     # hit3 woke the enemy by the item; awake, it hits the host on the item and
                     # no X goes through (rig 2026-10-13): kill-seen3 kills it first.
                     "host": ["sync c3-ready", "wait 20", "pickup 20", "wait 3",
                              "sync c3-host-picked", "sync c3-back", "wait 10",
                              "expect-session 3"],
                     "guest": ["sync c3-ready", "sync c3-host-picked", "pickup 20", "wait 3",
                               "sync c3-back", "wait 10", "expect-session 3"]},
                    # Phase 7: the host's game may take the member's going without its leave-limit
                    # flow (its idle time-out is off); its pickup above is the check there.
                    expect={"guest": [r"lost a member; staying where it is"]},
                    allow=CRASH_CLEANUP),
            # The host's game goes for good in this chapter, back in the next.
            chapter("host-crash3", "phase 7, host migration: the host's game dies; the two guests "
                                   "stay together in their session, one of them hosting it now, "
                                   "nobody leaving or joining again",
                    {"host": ["sync hc3-ready", RESTART, "wait 1"],
                     # Where each stands after crash3's restart varies (its resume): the session
                     # is the check, not who is near.
                     **{guest: ["sync hc3-ready", "wait 20", "expect-session 2"]
                        for guest in ("guest", "guest2")}},
                    expect={guest: [r"session host moved to"] for guest in ("guest", "guest2")},
                    forbid={guest: [r"sceNpMatching2LeaveRoom: called"]
                            for guest in ("guest", "guest2")},
                    allow=CRASH_CLEANUP),
            # Last: the returning host's game crashes in it (a crash ends the suite).
            chapter("leader-returns3", "the old host's game comes back and joins the session the "
                                       "guest hosts now, on the slot that guest left: three "
                                       "again",
                    {"host": [START, "godmode", "wait-session 3 300", "wait 10", "sync lr3-back",
                              "expect-session 3"],
                     **{guest: ["sync lr3-back", "wait 10", "expect-session 3"]
                        for guest in ("guest", "guest2")}},
                    # The old host boots here, from host-crash3's restart.
                    boots_in=1,
                    # Fixed 2026-10-17: Lua's OnMatchingCheck ended its session every few seconds
                    # (patch site "matching check"); the crash at eboot+0x13cffba went with the
                    # flag block guard.
                    forbid={"host": [r"StopRequest from the game"]},
                    allow=CRASH_CLEANUP),
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
    server_restarts: list[str] = []
    for c in chapters:
        keep_going = f"keep-going {'off' if c.get('fatal') else 'on'}"
        mark = f"chapter {c['name']} {budget.chapter_budget(c)}"
        syncs[c["name"]] = {"chapter": c["name"], "roles": list(roles)}
        outages.update(c.get("outages", {}))
        server_restarts += c.get("server_restarts", [])
        for role in roles:
            stages[role][-1] += [mark, keep_going, f"sync {c['name']}"]
            marks_due = False  # a restarted game's chapter mark, after its title's steps
            for step in c["steps"].get(role, []):
                if step == RESTART or step.startswith(RESTART_WITH):
                    # Started again: the same chapter, from the title screen (with other saves
                    # put in first for restart_with).
                    stages[role].append([])
                    marks_due = True
                    if step != RESTART:
                        saves = step[len(RESTART_WITH):-1]
                        stage_saves.setdefault(role, {})[len(stages[role]) - 1] = saves
                    continue
                if marks_due and not step.startswith(TITLE_STEPS):
                    # The title runs only its own steps (team-*, title-*): they go first.
                    stages[role][-1] += [mark, keep_going]
                    marks_due = False
                stages[role][-1].append(step)
                if step.startswith("sync "):
                    name = step.split()[1]
                    syncs.setdefault(name, {"chapter": c["name"], "roles": []})
                    syncs[name]["roles"].append(role)
            if marks_due:
                stages[role][-1] += [mark, keep_going]
    checks = ("about", "expect", "forbid", "allow", "same_hp", "same_hp_census", "hp_sums_hits",
              "max_hp_lag", "max_hp_lag_each", "killed", "max_kill_lag", "max_join_s", "moves",
              "grounded", "overlay_textures", "console_expect", "known_bug", "known_problems",
              "files_present",
              "files_absent")
    return {
        "about": suite["about"],
        "roles": {role: script if len(script) > 1 else script[0]
                  for role, script in stages.items()},
        "settings": suite.get("settings", {}),
        "timeout": budget.suite_timeout({"chapters": chapters}),
        "join_within": suite.get("join_within"),
        "chapters": [{"name": c["name"], "roles": roles,
                      **{key: c[key] for key in checks if key in c}} for c in chapters],
        "fatal": [c["name"] for c in chapters if c.get("fatal")],
        "syncs": syncs,
        "outages": outages,
        "server_restarts": server_restarts,
        "stage_saves": stage_saves,
        **({"saves": suite["saves"]} if "saves" in suite else {}),
    }


SUITE_SCENARIOS = {name: compile_suite(suite) for name, suite in SUITES.items()}
