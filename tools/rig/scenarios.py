# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""The rig's test scenarios (run_scenario.py): each role's autoplay script, the lines its log
must show ("expect"), the lines it must not show ("forbid") and the known problems it may show
("allow").

Every run starts from the baseline saves (run_scenario.py --save-baseline): every game at the first
Central Yharnam lamp. Every script starts with START (travel there unless already there).

Commands (see AutoplayVerbs in bloodborne_autoplay.cpp): wait <s>, wait-session <n> [<timeout>],
wait-members <n> [<timeout>] (party members on this map), wait-map <map> [<timeout>],
travel <warp point> [<timeout>], ensure-map <map> <warp point> [<timeout>] (travel unless
there already), hurt <nearest|handle> <hp>, kill <nearest|self|handle>,
teleport <x> <y> <z> [<heading>], approach <target> [<dx> [<dz> [<heading>]]], mark <target> (later "marked"),
attack <target> [<timeout>] (1 HP left, then light attacks from each side until it dies),
hit <target> [<timeout>] (light attacks until one lands),
press <button> [<ms>], cutscene-options <s> (Options once the next cutscene has played that
long: the script stands still under one), stick <x> <y> [<ms>], pickup [<meters>] [<timeout>] (onto the
nearest usable map item, then X until it is gone), snapshot, log <text>,
expect-hp <handle|self> <op> <value>, wait-hp <handle|self> <op> <value> [<timeout>] (until it
holds), expect-session <n>, expect-map <map>,
set-flag <flag> <0|1>, expect-flag <flag> <0|1>, wait-flag <flag> <0|1> [<timeout>],
wait-solo [<timeout>] (until the session is gone), note-max-hp (the player's, for
expect-max-hp <target> <op> <value|noted>), expect-effect <SpEffect> <0|1> [<target>] (the
player's, or that character's), watch-effects <target> <seconds> (from now on, the character's
SpEffects logged each second as "effects of <handle> hp <hp>: ...", and each change as "effects of
<handle> changed: +id -id"), wait-bullets <bullet> <count> [<timeout>] (until this game made
that many bullets of that BulletParam id since the step began); the rig's games log every
bullet they make ("bullet: <id> from <owner>"),
expect-others-near <n> <meters> (n other players alive that close), expect-compass <bearing> <metres>
[<degrees> [<metres>]] [down|dead] (within 10 s a marker of the teammate compass at that bearing,
degrees right of straight ahead, and distance; 20 degrees and 1.5 m either way by default; down or
dead: the marker shows that), expect-others-shown <n> (exactly n other players
shown, not hidden for another map), watch-unseen <handle> <seconds> (from now on, every frame the
character is drawn logs "watched <handle> drawn"; forbid that), godmode [off] (the player's HP kept at max), done. Targets: nearest, self or a handle in hex; kill
uses the game's own kill (as an event's ForceCharacterDeath), so bosses die in one step.
Party death (the scenario's "settings" switch it on): rise <metres> (straight up, to fall),
approach-downed (beside the nearest downed member on the map), revive-downed [<timeout>]
[r1|r2] (R1, or R2, on the downed member until nobody on the map is down), wait-vital <up|down|dead>
[<timeout>] (this player's state), wait-member-vital <up|down|dead> [<timeout>] (another
member's, on the vitals board). shot <name>: the port's picture of its next frame, overlays and
all (the party HUD), kept in the run's folder as <role>-<name>.png; the emulator skips it.
"""

import os

HUNTERS_DREAM = 2102962

# The Doll (0x10000001, team 26: a friendly NPC), 6.8 m from the Dream's lamp. From her east side,
# facing her (a heading h faces (-sin h, cos h)), X opens her talk; more X go on through her
# words, take her first choice (Level Up) and stay in its menu: then the menu's talk command 68
# runs every frame (the guest's talk answers), as in krolebord's log (playtest 2026-10-18).
DOLL = "0x10000001"
# 13 m from her, where the host's Doll is put: in krolebord's teammates' worlds she stood 16 m from
# where she stood in his (a guest's copy that follows the host's game goes there too).
DOLL_ELSEWHERE = "-18.3 -5.4 -16.9"
LEVEL_UP_AT_THE_DOLL = [f"approach {DOLL} 1.0 0 1.5708", "wait 2", "press cross 200", "wait 5",
                        *[step for _ in range(4) for step in ("press cross 200", "wait 3")],
                        "wait 2"]


CENTRAL_YHARNAM = "0x18010000"
CENTRAL_YHARNAM_LAMP = 2412950  # the first lamp, where the rig's characters start
CENTRAL_YHARNAM_LAMP_2 = 2412951  # talk 241001, 3 m from talk 241319 (Gilbert's window?)
CATHEDRAL_WARD = "0x18000000"
CATHEDRAL_WARD_LAMP = 2402950
CATHEDRAL_WARD_LAMP_LIT = 12407810  # its active flag (bloodborne_map.h's LampActiveFlag)
# Cathedral Ward's Wandering Nightmares (0x10010027..2c): event 12400200's slot flags, then each
# one's drop flag (m24_00_00_00.emevd, its init lines).
NIGHTMARE_SLOT_FLAGS = range(12400202, 12400208)
NIGHTMARE_FLAGS = ("12400202 12400203 12400204 12400205 12400206 12400207 "
                   "52400980 52400960 52400990 52400970 52400950 52400940")
ENEMY = "0x1001400b"  # 484 HP, 17 m from the lamp (farther ones are not awake)
BY_THE_LAMP = "0x100140a0"  # 999 HP, 1.6 m from the first lamp: by Gilbert's window
AWAY_FROM_LAMP = "-215.0 -54.5 178.0"  # beside that enemy, 17 m from the lamp
WARD_ENEMY = "0x10010019"  # 211 HP, 7 m from the Cathedral Ward lamp (the nearest, 0x10010075,
# has 999 HP and dies of nothing: an object, as Central Yharnam's 0x100140a0)
# Another one, 22 m from that lamp (123 HP): lone-guest's, as WARD_ENEMY's kill stands from
# ward-walk-kill on (Central Yharnam and the Ward load together, so it never comes back).
WARD_ENEMY_2 = "0x10010021"
# It spawns at (6.8, -11.0, 122.5) (rig census 2026-10-18): host-arrives-hurt-enemy puts it 4 m off
# that, the guest beside it.
HURT_ENEMY_AT = "6.8 -11.0 126.5"
HURT_ENEMY_SPOT = "8.3 -11.0 127.5 0.0"
# Then 20 m from it toward the lamp: inside the guest's jump watch (25 m), too far for the host's
# game to hand it to the guest, so the host's copy runs it (from its spawn on the red build).
HURT_ENEMY_AWAY = "15.0 -9.5 108.3 3.1"
CLERIC_BEAST = "0x1001409f"  # entity 2410800; its death runs event 12411700
CLERIC_BEAST_DEFEATED = 2411  # set by event 12411700 after Handle Boss Defeat
GASCOIGNE = "0x1001409c"  # entity 2410810 (his beast form: 0x1001409e, 2410811)
GASCOIGNE_BEAST = "0x1001409e"
AWAY_FROM_GASCOIGNE = "0x10014003"  # stands about 60 m from where he waits
BIG_BY_THE_LAMP = "0x10014001"  # boss-sized (max HP 1917), near the first lamp
# Event 12411800's own flag: on, the map's load puts Gascoigne down (Force Character Death);
# off, he waits for the fight. Both rig saves have him beaten.
GASCOIGNE_DEFEATED = 12411800
# The opening cutscene's event in Central Yharnam (m24_01): off, the next load plays it again.
OPENING_EVENT = 12410000
# Hidden by the host's story (event disable +0x28) and shown on the guest after its join until
# guests followed the host (Kaniel 2026-10-10: Iosefka at her clinic door, play-down-revive).
STORY_HIDDEN = "0x10014005"
IOSEFKA_AT = (-200.0, -40.5, 209.8)  # her census position on the rig
# 3 m from her on each side, facing her (a heading h faces (-sin h, cos h)).
IOSEFKA_VIEWS = [("south", -200.0, 206.8, 0.0), ("north", -200.0, 212.8, 3.1416),
                 ("west", -203.0, 209.8, -1.5708), ("east", -197.0, 209.8, 1.5708)]
# Up the hallway north of her: out of the other player's pictures.
IOSEFKA_WAIT = "-200.0 -40.5 218.0 0.0"
# The compass scenario's spots: the guest up that hallway, the host 5.2 m south of it.
COMPASS_GUEST = "-200.0 -40.5 218.0"
COMPASS_HOST = "-200.0 -40.5 212.8"
# Kaniel 2026-10-24 (team pings): a spot on the map in the Cathedral Ward, and its x and z.
PING_MAP_SPOT = "-150.0 -40.0 300.0"
PING_MAP_XZ = "-150.0 300.0"
# 7 m from ENEMY (-216.7, 178.1), on the lamp's side: close enough for it to notice a player.
GUEST_ALONE_SPOT = "-210.0 -54.5 181.0"
GUEST_PAIR_SPOT = "-213.0 -54.5 184.0"  # 3-4 m from GUEST_ALONE_SPOT


# At her clinic's door, facing it (south), where Kaniel talked to her (2026-10-13): Cross goes
# through her talk (the first gives her blood vials, item lot 23000).
IOSEFKA_DOOR = "-200.0 -40.5 208.4 3.1416"
TALK_THROUGH = [step for _ in range(12) for step in ("press cross 200", "wait 2")]


# The Cathedral Ward lamp (Oedon Chapel): its talk character (0x10010075) stands at
# (16.15, -8.90, 103.47) turned to 30 degrees in m24_00's MSB ("warp chair, start point"), 1.5 m
# from where a trip there puts a player (17.58 -9.5 102.65, bloodborne_lamp_points.h). Neither
# player got a prompt from that side (rig 2026-10-14), so the use tries its front: 0.7 and 1 m
# straight ahead of it and 0.8 m at 45 degrees either side, facing it (a talk that opens is
# closed again).
WARD_LAMP_SPOTS = ("16.50 -9.5 104.08 -2.62", "16.65 -9.5 104.34 -2.62", "16.92 -9.5 103.68 -1.83",
                   "15.94 -9.5 104.24 2.88")


# Vicar Amelia (m24_00, 2400800, at 68.4 35.7 340.9): her entrance (event 12401802, cutscene 24000060)
# plays once the host stands in region 2402805; her skull's event is 12401803 (Kaniel's playtest
# 2026-10-17).
ALTAR = "68.4 36.0 343.0 0.52"  # beside her
AMELIA_ARENA = "57.7 35.0 322.6 0.52"  # region 2402805


# The way from the Ward lamp through the gate to Central Yharnam (a guest's trail, playtest
# 2026-10-13: the maps meet at about 31 -9 95).
WARD_TO_YHARNAM = ("21.1 -9.5 100.6 23.1 -9.5 101.5 24.0 -9.5 99.7 25.6 -9.1 98.4 27.3 -9.1 96.9 "
                   "30.7 -8.9 94.9 32.6 -9.7 94.1 34.4 -10.6 92.7 36.0 -11.2 92.6")
YHARNAM_TO_WARD = ("34.4 -10.6 92.7 32.6 -9.7 94.1 30.7 -8.9 94.9 27.3 -9.1 96.9 25.6 -9.1 98.4 "
                   "24.0 -9.5 99.7 23.1 -9.5 101.5 21.1 -9.5 100.6 18.8 -9.5 102.3")


def USE_WARD_LAMP(role: str) -> list[str]:
    steps = []
    for index, spot in enumerate(WARD_LAMP_SPOTS):
        steps += [f"teleport {spot}", "wait 2", f"shot {role}-lamp-{index}",
                  "press cross 200", "wait 5", f"shot {role}-pressed-{index}", "press circle 200",
                  "wait 3", "press circle 200", "wait 3"]
    return steps


def IOSEFKA_LOOK(role: str) -> list[str]:
    """3 m south of Iosefka, a little to the side (the player's back hides what is straight
    ahead), a picture facing her."""
    return [f"teleport -199.2 {IOSEFKA_AT[1]} 206.8 0.0", "wait 3", f"shot {role}-iosefka"]
# Event flags neither rig character has: one of another map (the Forbidden Woods) and one of
# Central Yharnam's range. A guest's own world setting them must not reach the host's world (its
# save's progress, re-set after a reload, killed the clinic's beast in a new host's world).
# The dog 35 m from the first lamp (364 HP). Kaniel 2026-10-10: it died in a new host's world as
# the session formed, its loot on the ground: the guest's own world had posted it hurt and the
# host's was lowered to that before the join.
DOG = "0x10014009"
WARD_NPC = "0x10010075"  # the NPC sitting by the Oedon Chapel lamp (999 HP)
# Another ordinary enemy near the first lamp (suites.FRESH_ENEMY: real hits land on it).
SECOND_ENEMY = "0x1001402e"
CLINIC = "0x10014000"  # 573 HP, by Iosefka's door 32 m from the first lamp, 10 m up
# Gilbert (entity 2410781), 5.6 m from the second lamp: the map's story removes him (2004:29,
# the hide byte +0x28) seconds after every load, so he stood by the lamp until then (Kaniel's
# and Lisara's playtest 2026-10-15). The first lamp is 110 m away.
GILBERT = "0x1001400c"
CLINIC_DOG = "0x10014009"  # entity 2410019, by the clinic; the rig's characters have it hidden
GILBERT_HERE = 1246  # his step "at the window" (m24_01 event 12410760 shows him only with it)
GILBERT_SEEN = r"watched 0x1001400c drawn"
OTHER_MAP_FLAG = 12700999  # no door's (DoorFlags are shared with the party)
OLD_YHARNAM_GATE = 12800201  # the object act flag of Old Yharnam's gate (m28 event 12800480)
OLD_YHARNAM_GATE_DONE = 12800480  # that gate's event done: a load sets the gate open
YAHARGUL_LAMP = 2802950  # Yahar'gul, Unseen Village (257.4 -51.41 69.9)
YAHARGUL_LAMP_LIT = 12807810
# The first bell (m28_00 event 12805030 slot 0): a player inside area 2802500 (a box 14 by 12, 23
# high, at 260.2 -39.2 118.2) sets 12805030 within a second, and the summons waiting for it
# (12804500: 2800302 at 256.9 -39.1 115.3 and 2800303 at 267.4 -38.9 115.4) rise (animation 6200)
# and their generators (2803002, 2803003) start; each one's death plays SpEffect 5560 (12804000) a
# second later.
YAHARGUL_BELL_RUNG = 12805030
# The blood moon: without it Yahar'gul's story (12800400 on 9800/9801/9802) hides the crowd,
# the summons too (2004:29 on 2800302).
BLOOD_MOON = 9802
YAHARGUL_BELL_AREA = "260.2 -38.5 118.2"
YAHARGUL_BELL_GUEST = "258.5 -38.5 116.5"  # (at 256.0 -38.5 112.0 a guest died, godmode on)
# Open ground 30 m from the bell's area, on the street from the lamp (where crowd 2800301 walks):
# a rise from 10.7 m off its middle (254.8 -38.5 127.4) was still inside it (it is 23 m high).
YAHARGUL_BY_THE_BELL = "268.0 -46.5 89.0"
THIS_MAP_FLAG = 12410999
FORBIDDEN_WOODS_LAMP_LIT = 12707810  # a lamp neither rig character has lit (see LampOfActiveFlag)
START = f"ensure-map {CENTRAL_YHARNAM} {CENTRAL_YHARNAM_LAMP}"
# An enemy by a player moved farther in one frame than any runs (a hand-over that jumped).
ENEMY_JUMPED = r"Bloodborne party: enemy [0-9a-f]+ jumped"
# Phase 5b: a world flag of Central Yharnam's block no event uses (the rig's saves have it off).
TEAM_WORLD_FLAG = 12410999
BLOOD_MOON = 9802  # the time of day the maps' events pick (9800 evening, 9801 night)
# The Blood Gem Workshop Tool's chest (its item lot's flag; neither rig character has opened it).
WORKSHOP_TOOL_CHEST = 52411005
LEVEL_SYNC = 9200  # the SpEffect that scales a guest down to the host's level
# The "Impurity" rune (the League oath, GemeffectParam 2127001) puts 6142 on the player; common
# event 9035 adds 180040 (max HP x1.02) to a white phantom wearing it, KeepImpurityBonus to
# anyone in a party session (the host too).
IMPURITY_RUNE = 6142
IMPURITY_HP = 180040
# How a game joins: the port's team opens and joins the host's session itself (phase 6, no bell
# and no signs); shadPS4 rings the bells.
HOST_JOINS = (r"(?:auto-rejoin started search effect=9000|"
              r"Co-op team join: this host's session \d+ to the server)")
GUEST_JOINS = (r"(?:auto-rejoin started search effect=9005|"
               r"Bloodborne team join: joining the host's session .* joined=true)")
# The session must hold: a game that leaves the room (LeaveRoom) broke it.
SESSION_HOLDS = [r"sceNpMatching2LeaveRoom: called"]
# The party boards' member ids of the rig's accounts (their names in hex).
RIG_HOST_ID = "rig_host".encode().hex()
RIG_GUEST_ID = "rig_guest".encode().hex()
# A downed player's kneel, on its own game or on another's copy of it (Kaniel 2026-10-15: none).
KNEEL = r"party death: (pose [1-9]\d* asked|.*'s copy kneels)"
# A member's game died: the others' games close the room it was in (and open a new one for its
# return), which check_party_log reports as leaving the session outside a trip.
CRASH_CLEANUP = [r"left the session outside a trip"]
# Kaniel 2026-10-25: a member that leaves the team's session (back at the title, or its game gone)
# is out of the team; the guest whose game hosts the session now leads it (it took the room, or it
# stood alone and opened the session itself), and the leaver comes back as a guest.
LEADS_NOW = (r"Co-op team play: this guest leads the team now: it hosts|Co-op team join: the "
             r"session host moved to this game")
PLAYS_AS_GUEST = r"Co-op team lobby: the save plays in team \w+ as its guest"
# Party death (SHADPS4_BLOODBORNE_PARTY_DEATH): a lethal hit puts a member down while another
# stands on its map. A lethal fall is the hit the rig can deal: 1 HP left, then 18 m up. Under
# open sky (WIPE_SPOT; rig 2026-10-18: 12 m did nothing, 18 m about 60% of max HP, 20 m and more
# a death outright, not a hit); under a roof the player lands on it (the old spots, a roof 9 m up).
PARTY_DEATH = {"SHADPS4_BLOODBORNE_PARTY_DEATH": "1"}
# After a real death each player has its own bloodstain where it fell (GameDataMan's record on
# this map) and no enemy takes it (no echo theft): the bloodstain state lines say so.
BLOODSTAIN_DROPPED = (r"bloodstain state map=0x18010000 has_mark=1 record_valid=\d+ "
                      r"record_30=-?\d+ record_38=\d+ record_block=0x18010000")
BLOODSTAIN_STOLEN = r"bloodstain state .* stain_hidden=1"
LETHAL_FALL = ["hurt self 1", "rise 18"]
# Enemy HP kept in step: over the HP board (shadPS4's path) or by the area's owner (the port,
# phase 4: one game runs each area's enemies, the others follow it).
HP_SHARED = r"enemy HP (?:shared over the HP board|from the area's owner)"
# A copy lowered to the others' HP: the board's damage (old path) or the owner's HP (phase 4).
HP_LOWERED = (r"(?:lowered \d+ enemies by the other members' damage|"
              r"this follower's copies took the owner's HP from the log)")
OWNER_RUNS = r"enemy owner: this game runs map 0x18010000's enemies"
OWNER_FOLLOWS = r"enemy owner: this game follows \w+ on map 0x18010000"
# A real death while a partner stands on the map, as a player has one (party death is always on in
# the port, Kaniel 2026-10-13): the lethal fall puts it down and it gives up (Circle held 3 s).
GIVE_UP = [*LETHAL_FALL, "wait-vital down 30", "wait 2", "press circle 3600", "wait-vital dead 15"]
# wipe3 spreads the three a few metres from the lamp before each falls, so nobody lands on another
# member's body (the UI thread, 2026-10-12: a body under the fall took its damage).
# Open sky, on the lower ground west of the first lamp (rig 2026-10-18: the old spots were
# under a roof 9 m up, so every rise there landed on it, out of bounds, and the revives' swings
# went wide; Kaniel). East of the lamp the open strip ends in a drop past x -194.
WIPE3_SPOT = {"host": "-212.0 -54.5 170.0", "guest": "-209.0 -54.5 170.0",
              "guest2": "-209.0 -54.5 174.0"}
# A first down takes three hits (R1 beside the downed member), at least 0.4 s apart. Each R1
# lunges the hitter on, out of reach (2.5 m) after one, so it steps back beside it before each.
# Revive hits until the downed member is up: how many depends on the striker's weapon (party death
# takes its attack rating in damage).
# The status page (its face is the player's portrait): Circle closes what the start shows (an item's
# text), Options opens the menu on Inventory, Right goes to Status, Cross opens it.
OPEN_STATUS = ["wait 5", "press circle 200", "wait 2", "press options 200", "wait 3",
               "press right 200", "wait 2", "press cross 200", "wait 4"]
CLOSE_MENU = ["press circle 200", "wait 2", "press circle 200", "wait 3"]
REVIVE_HITS = ["revive-downed 150"]  # real hits: an unarmed rig hunter needs about 24 landed punches


def only_kill(handle: str, *beside: str) -> list[str]:
    """No kill posted but this one: a false kill hides a living enemy from the others. `beside`:
    enemies standing so close that the same swing may kill them too (a real kill, posted with it)."""
    if not beside:
        return [rf"kill board gets (?!1 kills, 0 back \(visit [^)]*\): {handle} )"]
    ours = "|".join((handle, *beside))
    return [rf"(?m)kill board gets (?!\d+ kills, 0 back \(visit [^)]*\):"
            rf"(?: (?:{ours}) \(dead [^)]*\))+\s*$)"]


def attack(target: str) -> list[str]:
    """A real kill of an ordinary enemy (the kill reward and the death run as in play; the
    game's own kill leaves one standing in a party)."""
    return [f"attack {target} 40"]


SCENARIOS = {
    "setup": {
        "about": "every rig game at the first Central Yharnam lamp, saved there (then "
                 "--save-baseline)",
        "roles": {
            "host": [START, "wait 20", "snapshot"],
            "guest": [START, "wait 20", "snapshot"],
        },
        "expect": {"host": [r"snapshot: map 0x18010000"], "guest": [r"snapshot: map 0x18010000"]},
        "timeout": 300,
        "no_baseline": True,
    },
    "boot": {
        "about": "a game gets from the title screen into the world by itself",
        "roles": {
            "host": [START, "snapshot", "wait 3", "snapshot"],
        },
        "expect": {"host": [r"Bloodborne autoplay snapshot: map 0x"]},
        "timeout": 300,
    },
    "join": {
        "about": "host and guest join each other by themselves (the port's team without a bell, "
                 "shadPS4's bells on their own); the guest keeps its full HP and its own level",
        "roles": {
            "host": [START, "wait-session 2 300", "wait 10", "snapshot", "expect-session 2"],
            "guest": [START, "note-max-hp", "wait-session 2 300", "wait 10", "snapshot",
                      "expect-session 2", "expect-max-hp self == noted",
                      f"expect-effect {LEVEL_SYNC} 0"],
        },
        "expect": {
            "host": [HOST_JOINS],
            "guest": [GUEST_JOINS],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "fps-pace": {
        "about": "Kaniel 2026-10-22, the unlocked frame rate online: host and guest together for 40 "
                 "s with the native packet trace (SHADPS4_BLOODBORNE_PACKET_TRACE=1), the host "
                 "walking a little; tools/re/packet_summary.py then counts each game's packets "
                 "(run it with BB_RIG_FPS=60, and uncap + BB_ONLINE_UNLOCKED_FPS=1)",
        "roles": {
            "host": [START, "wait-session 2 240", "wait 10", "sync counting", "stick 128 0 1500",
                     "stick 128 255 1500", "wait 17", "wait 20", "sync counted", "snapshot",
                     "expect-session 2"],
            "guest": [START, "wait-session 2 240", "wait 10", "sync counting", "wait 20",
                      "wait 20", "sync counted", "snapshot", "expect-session 2"],
        },
        "syncs": {"counting": {"chapter": "fps-pace", "roles": ["host", "guest"]},
                  "counted": {"chapter": "fps-pace", "roles": ["host", "guest"]}},
        "expect": {"host": [HOST_JOINS], "guest": [GUEST_JOINS]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "impurity-rune": {
        "about": "player report 2026-10-22: the League's \"Impurity\" rune (more HP while "
                 "cooperating) did nothing in co-op: the host and a guest wearing it get its HP "
                 "bonus, the guest also after a trip (no longer a phantom then)",
        "roles": {
            "host": [START, "wait-session 2 300", "wait 10", "note-max-hp",
                     f"add-effect {IMPURITY_RUNE}", "sync rune-on", "wait 5", "peek self 78",
                     f"expect-effect {IMPURITY_HP} 1", "expect-max-hp self > noted",
                     "sync guest-tripped", f"expect-effect {IMPURITY_HP} 1", "snapshot",
                     "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 10", "note-max-hp",
                      f"add-effect {IMPURITY_RUNE}", "sync rune-on", "wait 5", "peek self 78",
                      f"expect-effect {IMPURITY_HP} 1", "expect-max-hp self > noted",
                      f"travel {HUNTERS_DREAM}", "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}",
                      "wait 5", f"add-effect {IMPURITY_RUNE}", "wait 5", "peek self 78",
                      f"expect-effect {IMPURITY_HP} 1", "sync guest-tripped", "snapshot",
                      "expect-session 2"],
        },
        "syncs": {"rune-on": {"chapter": "impurity-rune", "roles": ["host", "guest"]},
                  "guest-tripped": {"chapter": "impurity-rune", "roles": ["host", "guest"]}},
        "expect": {"host": [HOST_JOINS], "guest": [GUEST_JOINS]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "doll-talk-arrival": {
        "about": "playtest 2026-10-18: a guest levelling up at the Doll while the host arrives in "
                 "the Dream (its characters change hands) and moves its own Doll away: the guest's "
                 "Doll stays its game's and where she is, the guest closes the menu and walks away "
                 "(krolebord stayed kneeling for good)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "sync doll-menu",
                     f"travel {HUNTERS_DREAM}", "wait-members 2 120", "wait 3",
                     f"place {DOLL} {DOLL_ELSEWHERE}", "wait 3", "sync host-in-dream",
                     "sync guest-walked", "snapshot", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 5", f"travel {HUNTERS_DREAM}",
                      "wait 8", "census 30", *LEVEL_UP_AT_THE_DOLL, "sync doll-menu",
                      "sync host-in-dream", "wait 15", "press circle 200", "wait 2",
                      "press circle 200", "wait 2", "press circle 200", "wait 2",
                      "press circle 200", "wait 4", "expect-walks 2 10", "sync guest-walked",
                      "snapshot", "expect-session 2"],
        },
        "syncs": {"doll-menu": {"chapter": "doll-talk-arrival", "roles": ["host", "guest"]},
                  "host-in-dream": {"chapter": "doll-talk-arrival", "roles": ["host", "guest"]},
                  "guest-walked": {"chapter": "doll-talk-arrival", "roles": ["host", "guest"]}},
        # The level-up menu open (its command every frame), the guest running the Dream's
        # characters until the host comes, then following the host's game.
        "expect": {"host": [r"autoplay: placed 0x10000001"],
                   "guest": [r"talk answers: .*f53=1x\d+ .*c68x\d\d",
                             r"this game runs map 0x15000000",
                             r"enemy owner: this game follows \S+ on map 0x15000000",
                             r"autoplay: walked"]},
        # The Doll never goes to another game's hands (the red build: "changes hands: follows" on
        # both), nor on the guest where the host's copy stands (the host's own move shows there).
        "forbid": {"host": [r"enemy 10000001 \(1\) changes hands: follows"],
                   "guest": [r"enemy 10000001 \(1\) changes hands: follows",
                             r"enemy 10000001 jumped"]},
        "timeout": 300,
    },
    "trip": {
        "about": "the host goes to the Hunter's Dream and back; the session holds and the players "
                 "are re-created by name",
        "roles": {
            "host": [START, "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}", "wait 5",
                     f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 120", "wait 15", "snapshot",
                     "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 30", "wait-members 2 240", "wait 25",
                      "snapshot", "expect-session 2"],
        },
        "expect": {
            "host": [r"party trip started", r"re-creating rig_guest on map 0x18010000"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "kill-apart": {
        "about": "the host kills an enemy while the guest is in the Dream; the guest's copy of it "
                 "is hidden when it comes back",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 45", *attack(ENEMY),
                     "wait 5", "wait-members 2 240", "wait 20", "snapshot"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}",
                      "wait 40", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240", "wait 25",
                      "snapshot"],
        },
        "expect": {
            "host": [r"kill board gets 1 kills"],
            "guest": [r"(?:hid 1 enemies a member killed on map 0x18010000|0x18010000, 1 near died)"],
        },
        "forbid": {"host": only_kill(ENEMY)},
        "timeout": 720,
    },
    "trade": {
        "about": "trading on the server (the port with the server world): "
                 "the host gives the guest 4 Blood Vials, swaps 3000 Blood Echoes for 5 Quicksilver "
                 "Bullets, then offers 2 vials the guest declines (they come back)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10",
                     "trade-stock vials 10", "trade-stock bullets 0", "trade-stock echoes 5000",
                     "wait 5",
                     "trade-offer vials 4 120", "expect-held vials 6", "wait-trade accepted 120",
                     "trade-offer echoes 3000 for bullets 5 60", "expect-held echoes 2000",
                     "expect-held bullets 5 60",
                     "trade-offer vials 2 60", "expect-held vials 4", "wait-trade declined 120",
                     "expect-held vials 6 30", "wait-trade returned 10"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10",
                      "trade-stock vials 0", "trade-stock bullets 10", "trade-stock echoes 0",
                      "trade-answer yes 180", "expect-held vials 4 30", "wait-trade received 10",
                      "trade-answer yes 120", "expect-held echoes 3000 30", "expect-held bullets 5",
                      "trade-answer no 120", "wait 10", "expect-held vials 4"],
        },
        "expect": {
            "host": [r"Bloodborne trade: offered .* 4 Blood Vials",
                     r"Bloodborne trade: added 5 Quicksilver Bullets",
                     r"Bloodborne trade: added 2 Blood Vials"],
            "guest": [r"Bloodborne trade: added 4 Blood Vials",
                      r"Bloodborne trade: added 3,000 Blood Echoes",
                      r"Bloodborne trade: declined offer"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "trade-any": {
        "about": "trading any item (Kaniel 2026-10-15): the host gives the guest a key item (Oedon "
                 "Tomb Key), a weapon with a blood gem in it (Hunter Axe +5) and armor (Charred "
                 "Hunter Garb) in one offer; the guest gets each, the weapon with its gem",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10",
                     "trade-stock goods:4000 1", "trade-stock weapon:5000500 1",
                     "trade-socket weapon:5000500 gem:1000000 13101",
                     "trade-stock armor:11000 1", "wait 5",
                     "trade-offer goods:4000 1 weapon:5000500 1 armor:11000 1 120",
                     "expect-held goods:4000 0", "expect-held weapon:5000500 0",
                     "expect-held armor:11000 0", "wait-trade accepted 120"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10",
                      "trade-stock goods:4000 0", "trade-stock weapon:5000500 0",
                      "trade-stock armor:11000 0",
                      "trade-answer yes 180", "expect-held goods:4000 1 30",
                      "expect-held weapon:5000500 1 30",
                      "expect-socketed weapon:5000500 gem:1000000 13101",
                      "expect-held armor:11000 1", "wait-trade received 10"],
        },
        "expect": {
            "host": [r"Bloodborne trade: offered .* Oedon Tomb Key, Hunter Axe \+5, Charred "
                     r"Hunter Garb"],
            "guest": [r"Bloodborne trade: added Oedon Tomb Key, Hunter Axe \+5, Charred Hunter "
                      r"Garb",
                      # A trade's grant keeps the item-received popup (key-items-silent's are
                      # silent).
                      r"item-get popup goods 4000 x 1 queued"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "trade-menus": {
        "about": "trading's menus (drawn): the guest answers the "
                 "host's offer in the game's yes/no box and the host sees the game's box saying it "
                 "was accepted; then the host gives 2 vials through the inventory's Give row and the "
                 "game's quantity picker, and 2,000 Blood Echoes through Give Blood Echoes; the "
                 "guest accepts both",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10",
                     "trade-stock vials 10", "trade-stock bullets 10", "trade-stock echoes 2000",
                     "wait 5", "trade-offer vials 4 120", "wait-trade accepted 120", "wait 3",
                     "shot host-notice", "press cross", "wait 2",
                     # Blood Vial (the first row) -> Give -> 2 in the picker.
                     "press options", "wait 2", "press cross", "wait 2", "shot host-inventory",
                     "press cross", "wait 2", "press down", "wait 1", "press down", "wait 1",
                     "shot host-commands-give", "press cross", "wait 2", "shot host-quantity",
                     "press up", "wait 1", "press left", "wait 1", "shot host-quantity-2",
                     "press cross", "wait 2",
                     "expect-held vials 4", "shot host-offered",
                     # Blood Vial -> Give Blood Echoes -> the picker's top (all of them).
                     "press cross", "wait 2", "press down", "wait 1", "press down", "wait 1",
                     "press down", "wait 1", "press cross", "wait 2", "shot host-echoes",
                     "press down", "wait 1", "press left", "wait 1", "shot host-echoes-max",
                     "press cross", "wait 2",
                     "expect-held echoes 0",
                     "press circle", "wait 1", "press circle", "wait 1", "press circle",
                     "wait 1", "press circle", "wait 40", "shot host-end"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10",
                      "trade-stock vials 0", "trade-stock echoes 0", "wait 40",
                      "shot guest-ask", "press left", "wait 1", "shot guest-ask-yes", "press cross",
                      "wait 3", "shot guest-received", "press cross", "expect-held vials 4 30",
                      "wait 30", "shot guest-ask-2", "press left", "wait 1", "press cross",
                      "wait 3", "shot guest-received-2", "expect-held vials 6 30",
                      "wait 10", "shot guest-ask-3", "press left", "wait 1", "press cross",
                      "wait 3", "shot guest-received-3", "expect-held echoes 2000 30"],
        },
        "expect": {
            "host": [r"Bloodborne trade: offered .* 4 Blood Vials",
                     r"Bloodborne trade: offered .* 2 Blood Vials",
                     r"Bloodborne trade: offered .* 2,000 Blood Echoes"],
            "guest": [r"Bloodborne trade: accepted offer", r"Bloodborne trade: added 4 Blood Vials",
                      r"Bloodborne trade: added 2,000 Blood Echoes"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 900,
    },
    "trade-echoes": {
        "about": "giving Blood Echoes through the menus (Kaniel 2026-10-16: a clearer soul trade): "
                 "the host holding 54,321 opens Give Blood Echoes (the last row of any item's "
                 "commands); the picker opens on 1,000 and steps by 1,000, so Up twice is 3,000; "
                 "then Down from 1,000 wraps to all the rest; the guest accepts both",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10",
                     "trade-stock vials 10", "trade-stock bullets 0", "trade-stock echoes 54321",
                     "wait 5", "press options", "wait 2", "press cross", "wait 2",
                     # The first row's commands -> Up wraps to Give Blood Echoes -> 3,000 -> Yes.
                     "press cross", "wait 2", "press up", "wait 1", "press cross", "wait 2",
                     "press up", "wait 1", "press up", "wait 1", "press left", "wait 1",
                     "press cross", "wait 3", "expect-held echoes 51321",
                     # The menu closes after an offer; the game's OK box says it was accepted.
                     "wait-trade accepted 60", "wait 2", "press cross", "wait 2",
                     # Again: Down from the first step is all of them.
                     "press options", "wait 2", "press cross", "wait 2",
                     "press cross", "wait 2", "press up", "wait 1", "press cross", "wait 2",
                     "press down", "wait 1", "press left", "wait 1", "press cross", "wait 3",
                     "expect-held echoes 0", "wait 5"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10",
                      "trade-stock echoes 0", "trade-answer yes 180",
                      "expect-held echoes 3000 30", "trade-answer yes 120",
                      "expect-held echoes 54321 30", "wait-trade received 10"],
        },
        "expect": {
            "host": [r"Bloodborne trade: offered .* 3,000 Blood Echoes",
                     r"Bloodborne trade: offered .* 51,321 Blood Echoes"],
            "guest": [r"Bloodborne trade: added 3,000 Blood Echoes",
                      r"Bloodborne trade: added 51,321 Blood Echoes"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "server-restart": {
        "about": "phase 3, the party's world on the server (the port): the host kills an enemy while the guest is in the Dream, shadNet restarts "
                 "mid-session, the host kills another; the guest coming back finds both dead (a "
                 "server that lost the world would start a new one, and the first kill with it)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 25", *attack(ENEMY),
                     "wait 5", "sync killed", "wait 20", *attack(SECOND_ENEMY), "wait 5",
                     "sync killed2", "wait-members 2 240", "wait 20", "snapshot"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}",
                      "sync killed", "sync killed2", "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}",
                      "wait-members 2 240", "wait 25", "snapshot"],
        },
        "syncs": {"killed": {"chapter": "server-restart", "roles": ["host", "guest"]},
                  "killed2": {"chapter": "server-restart", "roles": ["host", "guest"]}},
        "server_restarts": ["killed"],
        "expect": {
            "host": [r"Co-op server world: applied .*kill 0x1001400b",
                     r"Co-op server world: shadNet started over",
                     rf"Co-op server world: applied .*kill {SECOND_ENEMY}"],
            "guest": [r"Co-op server world: shadNet started over",
                      rf"Co-op server world: applied .*kill {SECOND_ENEMY}",
                      r"(?:hid 2 enemies a member killed on map 0x18010000, 0 near died|"
                      r"hid 1 enemies a member killed on map 0x18010000, 1 near died|"
                      r"hid 0 enemies a member killed on map 0x18010000, 2 near died)"],
        },
        "forbid": {"guest": [r"Co-op server world: following the party's log 0x[0-9a-f]+0{7}3\b",
                             r"Co-op server world: shadNet refused"]},
        "timeout": 300,
    },
    "story-npc-trips": {
        "about": "Gilbert, whom the map's story removes seconds after each load, is not drawn "
                 "by the second Central Yharnam lamp after a trip there (Kaniel and Lisara "
                 "2026-10-15: he stood there a moment, then vanished): first the guest alone (the "
                 "host 110 m away at the first lamp), then the host with the guest there",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 30", "sync guest-back",
                     f"watch-unseen {GILBERT} 30", f"travel {HUNTERS_DREAM}", "wait 5",
                     f"travel {CENTRAL_YHARNAM_LAMP_2}", "wait 15", "snapshot",
                     "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 30",
                      f"watch-unseen {GILBERT} 30", f"travel {HUNTERS_DREAM}", "wait 5",
                      f"travel {CENTRAL_YHARNAM_LAMP_2}", "wait 15", "sync guest-back",
                      "wait 30", "snapshot", "expect-session 2"],
        },
        "syncs": {"guest-back": {"chapter": "story-npc-trips", "roles": ["host", "guest"]}},
        "forbid": {"host": [GILBERT_SEEN] + SESSION_HOLDS, "guest": [GILBERT_SEEN] + SESSION_HOLDS},
        "timeout": 300,
    },
    "dog": {
        "about": "Kaniel's dog: the guest kills an enemy while the host is in the Dream; the host "
                 "coming back finds it dead, not alive again",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}",
                     "wait 40", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240", "wait 25",
                     "snapshot"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 45", *attack(ENEMY),
                      "wait 5", "wait-members 2 240", "wait 20", "snapshot"],
        },
        "expect": {
            "guest": [r"kill board gets 1 kills"],
            "host": [r"(?:hid 1 enemies a member killed on map 0x18010000|0x18010000, 1 near died)"],
        },
        "forbid": {"guest": only_kill(ENEMY)},
        "timeout": 720,
    },
    "story-hidden": {
        "about": "a character the host's story hides (Iosefka) stays hidden on the guest after "
                 "its join: the guest hides and shows every character as the host does",
        "roles": {
            "host": [START, "godmode", "census 80", "wait-session 2 300", "wait 20", "census 80",
                     f"expect-hidden {STORY_HIDDEN} 1"],
            "guest": [START, "godmode", "census 80", "wait-session 2 300", "wait 20", "census 80",
                      f"expect-hidden {STORY_HIDDEN} 1 30", "wait 10",
                      f"expect-hidden {STORY_HIDDEN} 1"],
        },
        "expect": {"guest": [rf"characters as the host has them on map 0x18010000: hid \d+, "
                             rf"showed \d+:.* {STORY_HIDDEN}"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "story-switched-on": {
        "about": "an enemy the guest's own story has switched off while the host's has it on "
                 "(Kaniel's friend 2026-10-15: the Forbidden Woods' enemy out of the red mist "
                 "never showed for her): the guest switches its copy on as the host has it",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 20",
                     f"expect-disabled {ENEMY} 0", "wait 30",
                     f"expect-disabled {ENEMY} 0"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 20",
                      f"expect-disabled {ENEMY} 0", f"set-disabled {ENEMY} 1",
                      f"expect-disabled {ENEMY} 0 30", "wait 10", f"expect-disabled {ENEMY} 0"],
        },
        "expect": {"guest": [rf"autoplay set-disabled: {ENEMY} -> 1",
                             rf"characters switched like the host has them on map 0x18010000 "
                             rf"\(off, \+on\):.* \+{ENEMY}"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "iosefka-new": {
        "about": "iosefka-look on a new character (Kaniel's, rig/saves/new-character): every "
                 "character nearby in both worlds, then Iosefka's dump and pictures (the guest "
                 "saw her on new characters, Kaniel 2026-10-10). The host's story removed her "
                 "before her model loaded; the guest switches its loaded copy off (2004:5), the "
                 "host's stays as its story left it",
        "saves": "new-character",
        "roles": {
            role: ["godmode", "census 300", "wait-session 2 300", "wait 20", "census 300",
                   f"expect-disabled {STORY_HIDDEN} {1 if role == 'guest' else 0} 30",
                   f"dump {STORY_HIDDEN}", f"dump {CLINIC}",
                   *[step for name, x, z, heading in IOSEFKA_VIEWS
                     for step in (f"teleport {x} {IOSEFKA_AT[1]} {z} {heading}", "wait 3",
                                  f"shot {role}-{name}")],
                   "census 300"]
            for role in ("host", "guest")
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "iosefka-mine": {
        "about": "iosefka-new on the characters each game has now (no saves put back): Kaniel's "
                 "own new characters (2026-10-10, copies in rig/saves/kaniel-1010-<role>)",
        "no_baseline": True,
        "roles": None,  # iosefka-new's, set below
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "iosefka-gift": {
        "about": "each player gets its own gift from an NPC (Kaniel 2026-10-13: the host got "
                 "Iosefka's blood vials, the guest who talked to her too got nothing): the host "
                 "talks to Iosefka at her door, then the guest does, and each is given the vials",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 15",
                     f"teleport {IOSEFKA_DOOR}", "wait 3", "shot host-door", *TALK_THROUGH, "wait 5", "snapshot", f"teleport {IOSEFKA_WAIT}", "wait 90",
                     "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", f"teleport {IOSEFKA_WAIT}",
                      "wait 75", f"teleport {IOSEFKA_DOOR}", "wait 3", "shot guest-door",
                      "press cross 200", "wait 2", "shot guest-talk-1", *TALK_THROUGH, "wait 5",
                      "snapshot", "expect-session 2"],
        },
        "expect": {"host": [r"item lot 23000 awarded from"],
                   "guest": [r"gave the guest its NPC gift, item lot 23000 \(trigger 72410310\)"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 600,
    },
    "iosefka-look": {
        "about": "Iosefka in the clinic's upstairs doorway (Kaniel's first screenshot: the guest "
                 "saw her there, the host did not): each player in turn looks at her from 3 m "
                 "south (the other waits up the hallway), then the guest tries the two flag "
                 "bits her character has in the host's world and not in its own (+0x1E1 0x80, "
                 "+0x1E4 1), a picture after each",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 20",
                     f"expect-hidden {STORY_HIDDEN} 1 30", f"dump {STORY_HIDDEN}",
                     *IOSEFKA_LOOK("host"), f"teleport {IOSEFKA_WAIT}", "wait 60"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 20",
                      f"expect-hidden {STORY_HIDDEN} 1 30", f"dump {STORY_HIDDEN}",
                      f"teleport {IOSEFKA_WAIT}", "wait 20", *IOSEFKA_LOOK("guest"),
                      f"poke {STORY_HIDDEN} 1e4 1 1", "wait 2", "shot guest-poke-1e4",
                      f"poke {STORY_HIDDEN} 1e1 84 1", "wait 2", "shot guest-poke-1e1",
                      f"dump {STORY_HIDDEN}"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 600,
    },
    "phantom-look": {
        "about": "how a guest is drawn (Kaniel 2026-10-11: no blue phantom tint on guests): "
                 "after the join each player dumps its own character and model and takes a "
                 "picture; the guest's own character shows the tint",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 20", "dump self",
                     "shot host-self", "wait 30"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 20", "dump self",
                      "shot guest-self", "dump nearest", "wait 10"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    # Kaniel 2026-10-14: with the host far away on the same map, the enemies beside a guest stood
    # still and did not attack (the host's game runs the map's enemies, and woke only its own).
    "guest-alone-mobs": {
        "about": "the host far away on the same map (by Iosefka's door), the guest walks up to an "
                 "enemy alone: it comes for the guest (moves) and hits it",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", f"teleport {IOSEFKA_WAIT}",
                     "wait 75", "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 15", f"census 40",
                      f"teleport {GUEST_ALONE_SPOT}", "wait 3", "census 40", "wait 25",
                      "census 40", "expect-session 2"],
        },
        "moves": {"guest": ENEMY},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 600,
    },
    # Kaniel 2026-10-14: the guest's game runs an enemy by it while the host is far; its hits
    # still meet the host's HP (the owner applies them and sends the HP back).
    "guest-alone-hits": {
        "about": "the host far away on the same map, the guest hits the enemy it runs twice: "
                 "both games end at max HP less the guest's hits",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", f"teleport {IOSEFKA_WAIT}",
                     "wait 75", "census 60", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 15",
                      f"teleport {GUEST_ALONE_SPOT}", "wait 8", f"hit {ENEMY} 30", "wait 1",
                      f"hit {ENEMY} 30", "wait 15", "census 40", "expect-session 2"],
        },
        "same_hp": ENEMY,
        "hp_sums_hits": ENEMY,
        "expect": {"guest": [r"hit on 0x1001400b by this player"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 600,
    },
    # Kaniel 2026-10-14: never two games running one enemy. Two guests by it, the host far: the
    # nearer guest runs it (the logs say who runs which, and when).
    "guests-pair-mobs": {
        "about": "the host far away on the same map, both guests by one enemy: it comes for them, "
                 "run by one guest's game at a time",
        "roles": {
            "host": [START, "godmode", "wait-session 3 300", f"teleport {IOSEFKA_WAIT}",
                     "wait 80", "expect-session 3"],
            "guest": [START, "wait-session 3 300", "wait 15", f"teleport {GUEST_ALONE_SPOT}",
                      "wait 3", "census 40", "wait 25", "census 40", "expect-session 3"],
            "guest2": [START, "wait-session 3 300", "wait 15", f"teleport {GUEST_PAIR_SPOT}",
                       "wait 3", "census 40", "wait 25", "census 40", "expect-session 3"],
        },
        "moves": {"guest": ENEMY},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS, "guest2": SESSION_HOLDS},
        "timeout": 720,
    },
    # The host comes back to the enemy the guest runs: its game takes it back, and it goes on.
    "guest-mobs-host-comes": {
        "about": "the guest runs an enemy with the host far away, then the host comes beside it: "
                 "the host's game takes it back and it keeps fighting",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", f"teleport {IOSEFKA_WAIT}",
                     "wait 40", f"teleport {GUEST_ALONE_SPOT}", "wait 40", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 15",
                      f"teleport {GUEST_ALONE_SPOT}", "wait 3", "census 40", "wait 25",
                      "census 40", "wait 30", "census 40", "expect-session 2"],
        },
        "moves": {"guest": ENEMY},
        "forbid": {"host": SESSION_HOLDS + [ENEMY_JUMPED],
                   "guest": SESSION_HOLDS + [ENEMY_JUMPED]},
        "timeout": 600,
    },
    # Kaniel 2026-10-15: the guest's hits on an enemy it ran came late, and the enemy went back to
    # full HP once the host came near. The guest's hits land at once; the host's copy takes them.
    "guest-hits-host-comes": {
        "about": "the guest hits the enemy it runs twice with the host far away, then the host "
                 "comes beside it: both games show the same HP, less the guest's hits",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", f"teleport {IOSEFKA_WAIT}",
                     "wait 50", f"teleport {GUEST_ALONE_SPOT}", "wait 25", "census 40",
                     "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 15",
                      f"teleport {GUEST_ALONE_SPOT}", "wait 8", f"hit {ENEMY} 30", "wait 1",
                      f"hit {ENEMY} 30", "wait 45", "census 40", "expect-session 2"],
        },
        "same_hp": ENEMY,
        "hp_sums_hits": ENEMY,
        "expect": {"guest": [r"hit on 0x1001400b by this player"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 600,
    },
    # Kaniel 2026-10-15: both on Central Yharnam, the guest a bit away from the host: every enemy
    # lagged (about 200 ms) and acted dumb, run by the host. As in vanilla, the host's game hands
    # the enemies only the guest is near to the guest's game, which runs them and sends their
    # positions (the host's copy takes them often, for a smooth hand-over back).
    "guest-far-positions": {
        "about": "the host far away on the same map, the enemy by the guest comes for it: the "
                 "host's game hands it to the guest's, which runs it; no enemy jumps",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", f"teleport {IOSEFKA_WAIT}",
                     "wait 75", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 15",
                      f"teleport {GUEST_ALONE_SPOT}", "wait 3", "census 40", "wait 40",
                      "census 40", "expect-session 2"],
        },
        "moves": {"guest": ENEMY},
        "expect": {"host": [r"enemy positions go out every 0\.10 s",
                            r"the game handed [1-9]\d* enemies to the others"],
                   "guest": [r"enemy follower runs [1-9]\d* enemies the owner handed it"]},
        "forbid": {"host": SESSION_HOLDS + [ENEMY_JUMPED], "guest": SESSION_HOLDS + [ENEMY_JUMPED]},
        "timeout": 600,
    },
    # Its control: the host beside the guest, the same enemy comes for them.
    "guest-mobs-host-near": {
        "about": "guest-alone-mobs with the host beside the guest: the enemy comes for them",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 18",
                     f"teleport {GUEST_ALONE_SPOT}", "wait 40", "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 15", f"census 40",
                      f"teleport {GUEST_ALONE_SPOT}", "wait 3", "census 40", "wait 25",
                      "census 40", "expect-session 2"],
        },
        "moves": {"guest": ENEMY},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 600,
    },
    "hp-together": {
        "about": "both players hit one enemy in one world (after the join), then the host goes "
                 "to the Dream and back, then the guest, and both hit it again from their own "
                 "worlds; each time both games show the same HP with both players' damage in it, "
                 "and at the end each sees the other (Kaniel 2026-10-10: HP bars apart after "
                 "trips; host and guest unseen after the host's early trip). The guest hits once "
                 "the second time: its weapon kills the enemy in two more",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 30", "census 40",
                     f"hit {ENEMY} 30", "wait 1", f"hit {ENEMY} 30", "wait 12", "census 40",
                     "wait 10", f"travel {HUNTERS_DREAM}", "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}",
                     "wait-members 2 240", "wait 70", "census 40", f"hit {ENEMY} 30", "wait 1",
                     f"hit {ENEMY} 30", "wait 8", "census 40", "wait 4", "census 40",
                     "expect-others-near 1 40", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 30", "census 40",
                      f"hit {ENEMY} 30", "wait 1", f"hit {ENEMY} 30", "wait 12", "census 40",
                      "wait 25", "wait-members 2 240", "wait 5", f"travel {HUNTERS_DREAM}",
                      "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240", "wait 15",
                      "census 40", f"hit {ENEMY} 30", "wait 46",
                      "census 40", "wait 4", "census 40", "expect-others-near 1 40",
                      "expect-session 2"],
        },
        # The second census of each (after the hits in one world), and the last.
        "same_hp": ENEMY,
        "same_hp_census": [1],
        "expect": {"host": [HP_SHARED],
                   "guest": [HP_SHARED]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 900,
    },
    "early-trip": {
        "about": "the host goes to the Dream and back while the guest is still loading in: back "
                 "on the map the host asks the guest for its character data and both see each "
                 "other; then the guest raises a stat (a level up) and the host gets the new data "
                 "(Kaniel 2026-10-10: guest unseen after the host's early trip; char info "
                 "updated for everybody on a level up)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 2", f"travel {HUNTERS_DREAM}",
                     "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240", "wait 40",
                     "census 40", "expect-others-near 1 40", "wait 30", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 60", "wait-members 2 240",
                      "wait 20", "census 40", "expect-others-near 1 40", "raise-stat 0 1",
                      "wait 15", "expect-session 2"],
        },
        "expect": {"host": [r"asked a member still joining for its data",
                            r"a member that was still joining sent its data",
                            r"member slot \d stats or face changed"],
                   "guest": [r"stats or face changed; sent its data"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 800,
    },
    "ward-npc": {
        "about": "both players by the Cathedral Ward lamp (Oedon Chapel): the host hurts the NPC "
                 "there and the guest's copy follows (the game sends no NPC HP), then the guest "
                 "hits it and the host's copy follows; one HP in both games (Kaniel 2026-10-10)",
        "roles": {
            "host": [START, f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                     "wait-session 2 420", "wait 15", f"hurt {WARD_NPC} 600", "wait 40", "census 60", "wait 10", "census 60",
                     "expect-session 2"],
            "guest": [START, f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                      "wait-session 2 420", "wait 25", "census 60",
                      f"expect-hp {WARD_NPC} <= 600", "peek self 88", f"peek {WARD_NPC} 88",
                      f"hit {WARD_NPC} 20", "wait 8",
                      "census 60", "wait 20", "census 60", "expect-session 2"],
        },
        "same_hp": WARD_NPC,
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 700,
    },
    "hurt-before-join": {
        "about": "an enemy hurt in the guest's own world before it joins stays whole in the "
                 "host's: no HP crosses worlds outside a session, and the session forming "
                 "kills nothing (the dog by the first lamp)",
        "roles": {
            "host": [START, "godmode", "wait 25", f"expect-hp {DOG} == 364",
                     "wait-session 2 300", "wait 30", "census 60", f"expect-hp {DOG} == 364",
                     "expect-session 2"],
            "guest": [START, "godmode", f"hurt {DOG} 5", "wait-session 2 300", "wait 30",
                      "census 60", f"expect-hp {DOG} > 300", "expect-session 2"],
        },
        "forbid": {"host": SESSION_HOLDS + [r"Bloodborne echoes: \+\d+"],
                   "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "clinic": {
        "about": "both players by Iosefka's Clinic in the session: what each world shows and "
                 "kills there (Kaniel 2026-10-10: the clinic's beast dead in a new host's world, "
                 "with its loot)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10", f"approach {CLINIC} 2 0",
                     "wait 5", "census 40", "wait 40", "census 40", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 15",
                      f"approach {CLINIC} -2 0", "wait 5", "census 40", "wait 40", "census 40",
                      "expect-session 2"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 600,
    },
    # Kaniel 2026-10-24 (player Fran's report): the Wandering Nightmares (c1180) are missing, some
    # only until a reload. Cathedral Ward's six (0x10010027..2c) run event 12400200: once its slot
    # flag (12400202..07) is on they are switched off at every load; the flag goes on when the drop's
    # flag (524009x0) is, or, in a CLIENT's game (multiplayer state 1), as soon as the player's
    # character type reads 0, no kill needed. This logs both flag sets and the player's type (+0x78)
    # in both games through the join, a trip there and a reload, and the Nightmares' switches. Before
# the fix (2026-10-24) all six slot flags went on in both games after the trip, no drop flag, and
# five Nightmares were switched off after the reload. The guest's rig save has the slot flags on
# before the join; the session's world puts them off.
    "nightmare-flags": {
        "about": "the Cathedral Ward Wandering Nightmares stay for host and guest through join, "
                 "trip and reload (their slot flags stay off with no kill)",
        "roles": {
            role: [START, "godmode", f"flags {NIGHTMARE_FLAGS}", "peek self 78",
                   "wait-session 2 300", f"flags {NIGHTMARE_FLAGS}", "peek self 78", "wait 10",
                   f"flags {NIGHTMARE_FLAGS}", f"travel {CATHEDRAL_WARD_LAMP}", "peek self 78",
                   f"flags {NIGHTMARE_FLAGS}", "wait 15", f"flags {NIGHTMARE_FLAGS}",
                   "census 400", "wait 30", f"flags {NIGHTMARE_FLAGS}",
                   f"travel {CATHEDRAL_WARD_LAMP}", "peek self 78", "wait 15",
                   f"flags {NIGHTMARE_FLAGS}", "census 400",
                   *[f"expect-flag {flag} 0" for flag in NIGHTMARE_SLOT_FLAGS],
                   "expect-disabled 0x1001002c 0", "done"]
            for role in ("host", "guest")
        },
        "timeout": 300,
    },
    "guest-flags-stay": {
        "about": "flags a guest's own world sets in the session stay in it: the host's world "
                 "does not get them (lamps and boss fogs go over the party's flag board)",
        "roles": {
            "host": [START, "godmode", f"expect-flag {OTHER_MAP_FLAG} 0",
                     f"expect-flag {THIS_MAP_FLAG} 0", "wait-session 2 300", "wait 45",
                     f"expect-flag {OTHER_MAP_FLAG} 0", f"expect-flag {THIS_MAP_FLAG} 0",
                     "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10",
                      f"set-flag {OTHER_MAP_FLAG} 1", f"set-flag {THIS_MAP_FLAG} 1", "wait 40",
                      f"expect-flag {OTHER_MAP_FLAG} 1", "expect-session 2"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 600,
    },
    "hp-apart": {
        "about": "after the guest's trip each runs its own copy of the map; the host hurts an "
                 "enemy and the guest's copy follows",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 55", "wait-members 2 240",
                     "wait 20", f"hurt {ENEMY} 200", "wait 12", "snapshot",
                     f"expect-hp {ENEMY} <= 200"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}",
                      "wait 10", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240", "wait 45",
                      "snapshot", f"expect-hp {ENEMY} <= 200"],
        },
        "expect": {"guest": [HP_LOWERED]},
        "timeout": 720,
    },
    "guest-drops": {
        "about": "Kaniel 2026-10-16: the enemies the host kills drop in the guest's world with a "
                 "handle each (krolebord found every kill's drop on the first wolf: one pickup "
                 "took them all)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 15", f"kill {ENEMY}",
                     "wait 5", f"kill {DOG}", "wait 5", f"kill {BY_THE_LAMP}", "wait 30",
                     "snapshot", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 70", "snapshot",
                      "expect-session 2"],
        },
        "forbid": {"host": SESSION_HOLDS,
                   "guest": SESSION_HOLDS + [r"0x38: [1-9]\d* items, \d+ usable, 0 with handle"]},
        "timeout": 600,
    },
    "boss-kill": {
        "about": "the Cleric Beast dies in the host's world with the guest there; the session "
                 "holds (no leave after the boss-clear script)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 15", f"kill {CLERIC_BEAST}",
                     f"wait-flag {CLERIC_BEAST_DEFEATED} 1 60", "wait 40", "snapshot",
                     "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 90", "snapshot",
                      "expect-session 2"],
        },
        "expect": {"host": [r"flags in session: set [^|]*\b4000\b"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 600,
    },
    "boss-refight": {
        "about": "the host fights Father Gascoigne, whom the guest beat long ago in its own "
                 "world: he stays in the host's world (the guest's old kill must not hide him) "
                 "and dies there with the guest in the session",
        "roles": {
            "host": [START, f"set-flag {GASCOIGNE_DEFEATED} 0", f"travel {CENTRAL_YHARNAM_LAMP}",
                     "godmode", "wait-session 2 300", "wait 20", f"expect-hp {GASCOIGNE} > 0",
                     f"kill {GASCOIGNE}", f"wait-flag {GASCOIGNE_DEFEATED} 1 60", "wait 20",
                     "snapshot", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 90", "snapshot",
                      "expect-session 2"],
        },
        # Before the fight nothing hides Gascoigne; once the host kills him the guest sees him die
        # in the session and posts that, which is fine.
        "forbid": {
            "host": SESSION_HOLDS + [r"(?s)hid \d+ enemies a member killed.*autoplay: kill "
                                     r"0x1001409c"],
            "guest": SESSION_HOLDS,
        },
        "timeout": 720,
    },
    "gascoigne-together": {
        "about": "neither player has beaten Father Gascoigne; the guest hurts him in the session and "
                 "the host kills his beast form: afterwards both worlds have him down and no fight "
                 "left (playtest 2026-10-13: a guest's world kept his first form alive at 1 HP, and "
                 "the lamp by his arena answered with his talk)",
        "roles": {
            "host": [START, f"set-flag {GASCOIGNE_DEFEATED} 0", f"travel {CENTRAL_YHARNAM_LAMP}",
                     "godmode", "wait-session 2 300", "wait 40", f"dump {GASCOIGNE}",
                     f"dump {GASCOIGNE_BEAST}", f"kill {GASCOIGNE_BEAST}",
                     f"wait-flag {GASCOIGNE_DEFEATED} 1 60", "wait 30", f"dump {GASCOIGNE}",
                     f"dump {GASCOIGNE_BEAST}", "snapshot", "expect-session 2"],
            "guest": [START, f"set-flag {GASCOIGNE_DEFEATED} 0", f"travel {CENTRAL_YHARNAM_LAMP}",
                      "godmode", "wait-session 2 300", "wait 20", f"hurt {GASCOIGNE} 3000",
                      f"hurt {GASCOIGNE_BEAST} 3000", f"wait-flag {GASCOIGNE_DEFEATED} 1 120",
                      "wait 30", f"dump {GASCOIGNE}", f"dump {GASCOIGNE_BEAST}", "census 80",
                      "snapshot", "expect-session 2"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 900,
    },
    "boss-guest-runs": {
        "about": "Kaniel 2026-10-16: a guest alone at a boss-sized enemy (max HP 1500 or more; "
                 "the big one by the first lamp) while the host stays back: it never jumps in the "
                 "guest's eyes. Since Kaniel's playtest 2026-10-19 (#285) only the game's bosses "
                 "(npcType 1) are handed out, so this giant stays with the map's own runner",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 90", "snapshot",
                     "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 20",
                      f"approach {BIG_BY_THE_LAMP} 4 0", "wait 10", f"hurt {BIG_BY_THE_LAMP} 300",
                      "wait 50", "snapshot", "expect-session 2"],
        },
        "forbid": {"host": SESSION_HOLDS + [r"boss 0x10014001 is run by 7269675f"],
                   "guest": SESSION_HOLDS + [r"enemy 10014001 jumped",
                                             r"boss 0x10014001 is run by 7269675f"]},
        "timeout": 600,
    },
    "boss-whole-fight": {
        "about": "Kaniel 2026-10-20 (#292): a real boss (npcType 1) stays with the member whose "
                 "game took it for the whole fight, at any distance: the guest starts Father "
                 "Gascoigne's fight while the host stays at the lamp, then runs about 60 m off on "
                 "the same map (past the old 45 m hand-back); he is still the guest's",
        "roles": {
            "host": [START, f"set-flag {GASCOIGNE_DEFEATED} 0", f"travel {CENTRAL_YHARNAM_LAMP}",
                     "godmode", "wait-session 2 300", "sync bwf-away", "census 80", "snapshot",
                     "expect-session 2"],
            "guest": [START, f"set-flag {GASCOIGNE_DEFEATED} 0", f"travel {CENTRAL_YHARNAM_LAMP}",
                      "godmode", "wait-session 2 300", "wait 20", f"approach {GASCOIGNE} 4 0",
                      "wait 5", f"hit {GASCOIGNE} 30", "wait 15", f"expect-hp {GASCOIGNE} < 2031",
                      f"approach {AWAY_FROM_GASCOIGNE} 1.5 0", "census 80", "wait 30",
                      "census 80", "sync bwf-away", "snapshot", "expect-session 2"],
        },
        "syncs": {"bwf-away": {"chapter": "boss-whole-fight", "roles": ["host", "guest"]}},
        # The census right after the move finds him 45-80 m off, the guest on the ground (60 m
        # off him in the air dropped the guest out of the map).
        "expect": {"guest": [rf"boss {GASCOIGNE} is run by {RIG_GUEST_ID}\b",
                             rf"census: {GASCOIGNE} hp .* at (4[5-9]|[5-7]\d)\.\d m"]},
        # Once the guest's game has him, no line may give him to the host or back to the game's
        # own runner, in either game.
        "forbid": {role: SESSION_HOLDS + [
            rf"boss {GASCOIGNE} is run by {RIG_HOST_ID}\b",
            rf"(?s)boss {GASCOIGNE} is run by {RIG_GUEST_ID}\b.*boss {GASCOIGNE} is run by "
            r"the game's own runner",
            rf"enemy {GASCOIGNE[2:]} jumped"] for role in ("host", "guest")},
        "timeout": 600,
    },
    "save-backups": {
        "about": "the saves are backed up by themselves (Kaniel 2026-10-11): with a backup every "
                 "minute, the first one lands a minute after the start, in user/save-backups",
        "roles": {"host": [START, "wait 150", "snapshot"]},
        "settings": {"SHADPS4_BLOODBORNE_SAVE_BACKUPS": "1,2"},
        "expect": {"host": [r"Bloodborne save backups: every 1 min", r"Bloodborne save backup: "]},
        "timeout": 300,
    },
    "boss-putdown": {
        "about": "the reverse of boss-refight (Kaniel 2026-10-11, Prey Slaughtered after a "
                 "teleport): the host beat Father Gascoigne long ago, the guest has not; after "
                 "the host's trip its world puts him down again, and the guest's own boss event "
                 "must not take that for its kill (no banner, echoes, trophy or defeated flag)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 20", f"travel {HUNTERS_DREAM}",
                     "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240", "wait 60",
                     f"expect-flag {GASCOIGNE_DEFEATED} 1", "expect-session 2"],
            "guest": [START, f"set-flag {GASCOIGNE_DEFEATED} 0", f"travel {CENTRAL_YHARNAM_LAMP}",
                      "godmode", f"expect-hp {GASCOIGNE} > 0", "wait-session 2 300", "wait 30",
                      "wait-members 2 240", "wait 60", f"dump {GASCOIGNE}", "expect-session 2"],
        },
        "forbid": {
            "host": SESSION_HOLDS,
            "guest": SESSION_HOLDS + [r"sharing trophy 14 ", r"Bloodborne echoes: \+3200 "],
        },
        "timeout": 900,
    },
    "prey-replay": {
        "about": "Kaniel 2026-10-11 (Prey Slaughtered), on the characters each game has now (no saves put back; copy the "
                 "ones to test into rig/port/<role> first): the host waits in the Dream while the "
                 "guest joins in place and runs its map alone, then comes back, and Father Gascoigne must stay alive",
        "no_baseline": True,
        "roles": {
            "host": [START, "godmode", f"travel {HUNTERS_DREAM}", "wait-session 2 300", "wait 60",
                     f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240", "wait 60",
                     f"expect-hp {GASCOIGNE} > 0", "expect-session 2"],
            "guest": [START, "godmode", f"expect-flag {GASCOIGNE_DEFEATED} 0",
                      "wait-session 2 300", "wait 60", "wait-members 2 240", "wait 60",
                      f"expect-hp {GASCOIGNE} > 0", "expect-session 2"],
        },
        "forbid": {
            "host": SESSION_HOLDS + [r"sharing trophy 14 "],
            "guest": SESSION_HOLDS + [r"sharing trophy 14 ", r"Bloodborne echoes: \+3200 "],
        },
        "timeout": 900,
    },
    "lamp-share": {
        "about": "a lamp the guest lights is lit in the host's world too",
        "roles": {
            "host": [START, f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 0", "wait-session 2 300",
                     f"wait-flag {FORBIDDEN_WOODS_LAMP_LIT} 1 90", "snapshot"],
            # The guest's world loads again 10-20 s after the join; a flag set in the same 3 s
            # reads as part of that load and is not shared, so the lamp is lit after it.
            "guest": [START, "wait-session 2 300", "wait 30",
                      f"set-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", "wait 30", "snapshot"],
        },
        "timeout": 300,
    },
    "door-share": {
        "about": "a door the guest opens on another map (Old Yharnam's gate) is open in the "
                 "host's world too",
        "roles": {
            "host": [START, f"expect-flag {OLD_YHARNAM_GATE} 0",
                     f"expect-flag {OLD_YHARNAM_GATE_DONE} 0", "wait-session 2 300",
                     f"wait-flag {OLD_YHARNAM_GATE} 1 90",
                     f"wait-flag {OLD_YHARNAM_GATE_DONE} 1 30", "snapshot"],
            # As in lamp-share: set after the guest's world loads again following the join.
            "guest": [START, "wait-session 2 300", "wait 30", f"set-flag {OLD_YHARNAM_GATE} 1",
                      f"set-flag {OLD_YHARNAM_GATE_DONE} 1", "wait 30", "snapshot"],
        },
        "timeout": 300,
    },
    "join-ward": {
        "about": "joining in Cathedral Ward, whose co-op the guest's own world has not opened",
        "roles": {
            "host": [START, f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                     "wait-session 2 360", "wait 10", "snapshot", "expect-session 2"],
            "guest": [START, f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                      "wait-session 2 360", "wait 10", "snapshot", "expect-session 2"],
        },
        "expect": {"host": [r"join-anywhere patch"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "join-apart": {
        "about": "joining across maps: the host in Cathedral Ward, the guest in Central Yharnam; "
                 "the guest joins where it is and the session holds (no kick)",
        "roles": {
            "host": [START, f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                     "wait-session 2 360", "wait 60", "snapshot", "expect-session 2",
                     f"expect-map {CATHEDRAL_WARD}"],
            "guest": [START, "godmode", "wait-session 2 360", "wait 60", "snapshot",
                      "expect-session 2", f"expect-map {CENTRAL_YHARNAM}"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 720,
    },
    "join-after-trip": {
        "about": "a guest that travelled on its own before any session (in its own world) still "
                 "rings and joins the host on another map",
        "roles": {
            "host": [START, "godmode", "wait-session 2 420", "wait 30", "snapshot",
                     "expect-session 2", f"expect-map {CENTRAL_YHARNAM}"],
            "guest": [START, f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                      "wait-session 2 420", "wait 30", "snapshot", "expect-session 2",
                      f"expect-map {CATHEDRAL_WARD}"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "lone-guest": {
        "about": "a guest alone on a map runs its enemies, and hands them back when the host comes",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 60",
                     f"travel {CATHEDRAL_WARD_LAMP}", "wait-members 2 240", "wait 20",
                     "snapshot"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10",
                      f"travel {CATHEDRAL_WARD_LAMP}", "wait-members 2 300", "wait 25",
                      "snapshot"],
        },
        "expect": {
            "guest": [r"(?:guest alone on map 0x18000000 took over \d+ enemies|"
                      r"enemy owner: took over \d+ enemies on map 0x18000000)",
                      r"(?:guest no longer alone; handed back|"
                      r"enemy owner: this game follows \w+ on map 0x18000000)"],
        },
        "timeout": 720,
    },
    "host-arrives-hurt-enemy": {
        "about": "Kaniel's play test 2026-10-18: the guest alone in Cathedral Ward moves an enemy "
                 "off its spawn and hurts it; the host comes to the Ward and the enemy stays where "
                 "and as hurt as it is on both games (krolebord's jumped 19.9 m back to its spawn "
                 "as the host's new copy took over)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "sync guest-hurt",
                     f"travel {CATHEDRAL_WARD_LAMP}", "wait-members 2 120", "wait 15",
                     f"expect-hp {WARD_ENEMY_2} < 123", "census 40", "sync host-checked",
                     "expect-session 2"],
            # Put off its spawn from the lamp, 26 m away: out of the guest's jump watch (25 m), so
            # only a jump once the guest stands beside it counts.
            "guest": [START, "godmode", "wait-session 2 300", "wait 5",
                      f"travel {CATHEDRAL_WARD_LAMP}", "wait 8",
                      f"place {WARD_ENEMY_2} {HURT_ENEMY_AT}", "wait 2",
                      f"teleport {HURT_ENEMY_SPOT}", "wait 2", f"hurt {WARD_ENEMY_2} 60",
                      "wait 3", "census 40", f"teleport {HURT_ENEMY_AWAY}", "wait 2",
                      "sync guest-hurt", "wait-members 2 180", "wait 15",
                      f"expect-hp {WARD_ENEMY_2} < 123", "census 40", "sync host-checked",
                      "expect-session 2"],
        },
        "syncs": {"guest-hurt": {"chapter": "host-arrives-hurt-enemy", "roles": ["host", "guest"]},
                  "host-checked": {"chapter": "host-arrives-hurt-enemy",
                                   "roles": ["host", "guest"]}},
        # The host follows the guest's game first (shadNet's HostTakeBackMs), then runs the Ward.
        "expect": {"host": [r"enemy owner: this game follows \w+ on map 0x18000000",
                            r"enemy owner: this game runs map 0x18000000's enemies"],
                   "guest": [r"enemy owner: this game runs map 0x18000000's enemies",
                             r"enemy owner: this game follows \w+ on map 0x18000000"]},
        # The host's copy where it spawned (the red build's census), the guest's jumping there.
        "forbid": {"host": [r"census: 0x10010021 .*\(6\.8, -11\.0, 122\.5\)", *SESSION_HOLDS],
                   "guest": [r"enemy 10010021 jumped", *SESSION_HOLDS]},
        "timeout": 300,
    },
    "dead-in-host-save": {
        "about": "Kaniel's play test 2026-10-18: the host kills an enemy and its game starts again "
                 "(the enemy loads dead, at 0 HP, with no death flags); back in a session, the "
                 "host's shown list says it is dead, so the guests' copies go too (krolebord saw "
                 "such enemies standing, Kaniel saw nothing there)",
        # Kaniel 2026-10-25: the guest's game goes first (out of the team, invited back), so the
        # host's is the last to go and the host still leads when both come back.
        "roles": {
            "host": [[START, "godmode", f"teleport {GUEST_ALONE_SPOT}", "wait 5",
                      *attack(ENEMY), "wait 20", "sync dihs-gone", "wait 10", "done"],
                     [START, "godmode", "wait-session 2 420", "wait 30", "census 40", "done"]],
            "guest": [[START, "godmode", "wait-session 2 300", "sync dihs-gone", "done"],
                      [START, "godmode", "wait-session 2 420", "wait 40", "done"]],
        },
        "syncs": {"dihs-gone": {"chapter": "dead-in-host-save", "roles": ["host", "guest"]}},
        # The shown list's bit 8: dead in the host's world (DeadInTheHostsWorld).
        "expect": {"host": [r"census: 0x1001400b hp 0/",
                            r"shown 0x1001400b on 0x18010000 bits (?:8|9|1[0-5])\b"]},
        "allow": CRASH_CLEANUP,
        "timeout": 900,
    },
    "guest-kill-pays-host": {
        "about": "the guest kills an enemy beside it that the host's game handed to the "
                 "guest's; the host 17 m away on the same map gets the kill's echoes too (the "
                 "party shares souls; Kaniel's play test 2026-10-18 checked this path)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "sync guest-killed", "wait 20",
                     "sync host-checked", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 5",
                      f"teleport {GUEST_ALONE_SPOT}", "wait 10", *attack(ENEMY), "wait 2",
                      "sync guest-killed", "sync host-checked", "expect-session 2"],
        },
        "syncs": {"guest-killed": {"chapter": "guest-kill-pays-host", "roles": ["host", "guest"]},
                  "host-checked": {"chapter": "guest-kill-pays-host",
                                   "roles": ["host", "guest"]}},
        # The host's own game pays it.
        "expect": {"host": [r"Bloodborne echoes: \+[1-9]\d* \(rates \d+\) from 0x18fc99b"],
                   "guest": [r"Bloodborne echoes: \+[1-9]\d* \(rates \d+\) from 0x18fc99b"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "echoes-apart": {
        "about": "the guest kills an enemy alone in Cathedral Ward; the host in Central Yharnam "
                 "gets its echoes",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 80", "snapshot"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10",
                      f"travel {CATHEDRAL_WARD_LAMP}", "wait 10", "snapshot",
                      *attack(WARD_ENEMY), "wait 15", "snapshot"],
        },
        "expect": {
            "guest": [r"Bloodborne echoes: \+\d+ \(rates 1\) from 0x18fc99b"],
            "host": [r"added \d+ echoes from a kill another member made apart"],
        },
        "timeout": 600,
    },
    "host-death": {
        "about": "the host dies (goes down and gives up); it gets up without a reload and the "
                 "session holds",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "wait-session 2 300", "wait 15", *GIVE_UP, "wait 40", "snapshot",
                     "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait-member-vital down 120",
                      "wait 60", "snapshot", "expect-session 2"],
        },
        "expect": {"host": [r"party death: giving up ends down \d+; dies \(reset=false",
                            r"live player died in a party session"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 600,
    },
    "guest-death": {
        "about": "the guest dies (goes down and gives up) in the host's world; it gets up there "
                 "and the session holds",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-member-vital down 120",
                     "wait 60", "snapshot", "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 15", *GIVE_UP, "wait 40", "snapshot",
                      "expect-session 2"],
        },
        "expect": {"guest": [r"party death: giving up ends down \d+; dies \(reset=false",
                             r"party guest died; revived"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 600,
    },
    "guest-death-target": {
        "about": "the guest dies for real (goes down and gives up) and gets up in the host's world; "
                 "its body on the host's game loses the death mark, so the enemies there take it "
                 "as a target again (Kaniel 2026-10-25: Logarius went for the host only)",
        "settings": PARTY_DEATH,
        "roles": {
            # The mark goes HoldMs (3 s) after the guest's body is up again on the host's game.
            "host": [START, "godmode", "wait-session 2 300", "wait-member-vital dead 180",
                     "wait-member-vital up 60", "wait 10", "snapshot", "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 15", *GIVE_UP, "wait-vital up 60",
                      "wait 15", "snapshot", "expect-session 2"],
        },
        "expect": {"host": [r"body 0x[0-9a-f]+ is up but kept its death mark; cleared"],
                   "guest": [r"party death: giving up ends down \d+; dies \(reset=false",
                             r"party guest died; revived"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "host-crash": {
        "about": "the host's game crashes (the GPU's device lost does that in play); the guest "
                 "stays where it is and leads the team now (it hosts), and the old host's game "
                 "comes back as its guest (Kaniel 2026-10-25)",
        "roles": {
            "host": [[START, "godmode", "wait-session 2 300", "wait 15", "done"],
                     [START, "godmode", "wait-session 2 420", "wait 10", "snapshot",
                      "expect-session 2"]],
            "guest": [START, "godmode", "wait-session 2 300", "wait-solo 120", "wait 10",
                      "snapshot", f"expect-map {CENTRAL_YHARNAM}", "wait-session 2 420", "wait 10",
                      "snapshot", "expect-session 2"],
        },
        "expect": {"guest": [LEADS_NOW], "host": [PLAYS_AS_GUEST]},
        "allow": CRASH_CLEANUP,
        "timeout": 300,
    },
    "guest-crash": {
        "about": "the guest's game crashes; the host plays on, and they join again once the guest "
                 "is back (a dead game's sign must not stall the host)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-solo 120", "wait 10",
                     "snapshot", f"expect-map {CENTRAL_YHARNAM}", "wait-session 2 420", "wait 10",
                     "snapshot", "expect-session 2"],
            "guest": [[START, "godmode", "wait-session 2 300", "wait 15", "done"],
                      [START, "godmode", "wait-session 2 420", "wait 10", "snapshot",
                       "expect-session 2"]],
        },
        "allow": CRASH_CLEANUP,
        "timeout": 300,
    },
    "resume": {
        "about": "a guest whose game dies in the host's world comes back where it was (its own "
                 "save still has it at the lamp it joined from) and joins the host again",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 40", "wait-solo 120",
                     "wait-session 2 420", "wait 20", "snapshot", "expect-session 2"],
            "guest": [[START, "godmode", "wait-session 2 300", "wait 10",
                       f"teleport {AWAY_FROM_LAMP}", "wait 15", "snapshot", "done"],
                      ["godmode", "wait-session 2 420", "wait 10", "snapshot",
                       "expect-session 2", f"expect-map {CENTRAL_YHARNAM}"]],
        },
        "expect": {"guest": [r"guest resumed at \(-21\d(\.\d+)?, -5\d(\.\d+)?, "
                             r"17\d(\.\d+)?\) on map 0x18010000"]},
        "allow": CRASH_CLEANUP,
        "timeout": 900,
    },
    "crash-apart": {
        "about": "a guest's game dies while it is apart from the host (in the Ward); the host "
                 "stays in its world (it once went to the title screen) and they join again",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 40", "wait-solo 120",
                     "wait 30", "snapshot", f"expect-map {CENTRAL_YHARNAM}",
                     "wait-session 2 420", "wait 20", "snapshot", "expect-session 2"],
            "guest": [[START, "godmode", "wait-session 2 300", "wait 10",
                       f"travel {CATHEDRAL_WARD_LAMP}", "wait 25", "snapshot", "done"],
                      ["godmode", f"wait-map {CATHEDRAL_WARD} 180", "wait-session 2 420",
                       "wait 10", "snapshot", "expect-session 2", f"expect-map {CATHEDRAL_WARD}"]],
        },
        "allow": CRASH_CLEANUP,
        "timeout": 900,
    },
    "host-crash-apart": {
        "about": "the host's game dies while the guest is apart from it (in the Ward); the guest "
                 "stays there and leads the team now (it hosts there), and the old host's game "
                 "comes back as its guest, beside it in the Ward (Kaniel 2026-10-25)",
        "roles": {
            "host": [[START, "godmode", "wait-session 2 300", "wait 60", "done"],
                     [START, "godmode", "wait-session 2 420", "wait 20", "snapshot",
                      "expect-session 2", f"expect-map {CATHEDRAL_WARD}"]],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10",
                      f"travel {CATHEDRAL_WARD_LAMP}", "wait 15", "snapshot",
                      f"expect-map {CATHEDRAL_WARD}", "wait-solo 180", "wait 10", "snapshot",
                      f"expect-map {CATHEDRAL_WARD}", "wait-session 2 420", "wait 20",
                      "snapshot", "expect-session 2", f"expect-map {CATHEDRAL_WARD}"],
        },
        "expect": {"guest": [LEADS_NOW], "host": [PLAYS_AS_GUEST]},
        "allow": CRASH_CLEANUP,
        "timeout": 1080,
    },
    # Phase 5: the team lobby. The party is no setting: on the title the host creates a team and
    # invites, the guests accept; each one's Continue is its Ready, the host's is the team's Start.
    "team-join": {
        "about": "a team from the lobby: the host creates it and invites the guest, both press "
                 "Continue and play in one session (no party setting)",
        "roles": {
            "host": ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                     "wait-session 2 300", "wait 10", "snapshot", "expect-session 2"],
            "guest": ["team-accept", "team-ready", START, "wait-session 2 300", "wait 10",
                      "snapshot", "expect-session 2"],
        },
        "expect": {
            "host": [r"Co-op team lobby: the save plays in team \w+ as its host",
                     r"team lobby: a save's load waits for the team's Start"],
            "guest": [r"Co-op team lobby: the save plays in team \w+ as its guest"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    # Kaniel 2026-10-17: a cutscene both see (Vicar Amelia's entrance, m24_00 12401802: the host
    # walks into her arena, 24000060 plays for both) is skipped only once both voted: the guest's
    # Options alone skips nothing (1 of 2), the host's then skips it for both.
    "skip-vote": {
        "about": "both see Amelia's entrance; the guest's Options is a vote (1 of 2), the "
                 "cutscene goes on; the host's Options makes it 2 of 2 and both skip it",
        "roles": {
            "host": [START, f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                     "wait-session 2 300", f"teleport {ALTAR}", "wait-members 2 60", "wait 5",
                     "cutscene-options 16", f"teleport {AMELIA_ARENA}", "wait 30",
                     "expect-session 2"],
            "guest": [START, f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                      "wait-session 2 300", f"teleport {ALTAR}", "cutscene-options 6", "wait 30",
                      "expect-session 2"],
        },
        "expect": {role: [r"cutscene player's step is PrologueWait[\s\S]*1 of 2 watching want to "
                          r"skip[\s\S]*2 of 2 watching want to skip[\s\S]*cutscene player's "
                          r"step is Epilog"] for role in ("host", "guest")},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    # Kaniel 2026-10-15: a cutscene under the team's loading screen skips by itself (Options
    # pressed for the game's own skip). The opening's event (12410000, m24_01's emevd) plays its
    # cutscene again at the next load once its flag is off: the host clears it, and its next team
    # load plays the opening under the screen.
    "cutscene-skip": {
        "about": "the opening cutscene plays under the team's loading screen and skips by itself "
                 "(Options pressed); the screen opens and both are in one session",
        "roles": {
            # Kaniel 2026-10-25: the guest's game goes first (out of the team, invited back), so
            # the host's is the last to go and it still leads.
            "host": [["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                      "wait-session 2 300", f"set-flag {OPENING_EVENT} 0", "wait 15",
                      "sync cs-gone", "wait 10", "done"],
                     # The guest back at the title and ready first: both load together.
                     ["title-wait 60", "team-start", START, "wait-session 2 300", "wait 30",
                      "expect-session 2"]],
            "guest": [["team-accept", "team-ready", START, "wait-session 2 300", "wait 30",
                       "sync cs-gone", "done"],
                      ["team-accept", "team-ready", START, "wait-session 2 300", "wait 15",
                       "expect-session 2"]],
        },
        "syncs": {"cs-gone": {"chapter": "cutscene-skip", "roles": ["host", "guest"]}},
        "expect": {"host": [r"cutscene player's step is Update",
                            r"Options pressed to skip it[\s\S]*cutscene player's step is Epilog"
                            r"[\s\S]*this game is ready, 1 of 1[\s\S]*load view is gone"]},
        # Unskipped, the opening held the screen ~115 s (21593 silent buffers): skipped, under
        # 10000 (about 53 s, the load and the join included).
        "console_expect": {"host": [r"Runtime: sound back after \d{1,4} silent buffers"]},
        "allow": CRASH_CLEANUP,
        "timeout": 900,
    },
    # Play test 2026-10-18: guests got the opening cutscene again after a lamp trip from the Dream.
    # The opening's flag (12410000) came on in the session (at the join's reload), never in the
    # guest's own save, and a trip's load gives the guest's game its own saved world back; with the
    # host away from Central Yharnam nothing set it again, and m24_01's event played the opening.
    # Kaniel's play test 2026-10-18: the Workshop Tool's chest (item lot flag 52411005) opened again
    # after the host's save took a team world without it. A map item pickup is each character's
    # own: here the host opens the chest (its flag reaches the team's world), the guest's game
    # starts again and takes the team's world as it loads, and its own chest stays closed.
    "team-world-pickups": {
        "about": "a chest the host opened stays closed for a guest whose save takes the team's "
                 "world as it loads (map item pickups are each character's own)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", f"set-flag {WORKSHOP_TOOL_CHEST} 1",
                     "wait 15", "wait-solo 200", "wait-session 2 420", "wait 30",
                     f"expect-flag {WORKSHOP_TOOL_CHEST} 1", "done"],
            "guest": [[START, "godmode", "wait-session 2 300",
                       f"expect-flag {WORKSHOP_TOOL_CHEST} 0", "wait 25", "done"],
                      [START, "godmode", "wait-session 2 420", "wait 10",
                       f"expect-flag {WORKSHOP_TOOL_CHEST} 0", "done"]],
        },
        "expect": {"guest": [r"team's world is in this save: \d+ blocks"]},
        "allow": CRASH_CLEANUP,
        "timeout": 900,
    },
    # Here the host sets TEAM_WORLD_FLAG (Central Yharnam's block, unused) while the guest is in the
    # Dream, then waits there; the guest goes back to Central Yharnam: the flag is on.
    "team-world-trip": {
        "about": "a world flag the host set while the guest was in the Dream is on when the guest "
                 "goes back to Central Yharnam, the host away in the Dream",
        "roles": {
            "host": [START, "wait-session 2 300", "wait 30", f"set-flag {TEAM_WORLD_FLAG} 1",
                     "wait 15", f"travel {HUNTERS_DREAM}", "wait 30", "wait 30", "wait 30",
                     "done"],
            "guest": [START, "wait-session 2 300", "wait 5", f"travel {HUNTERS_DREAM}", "wait 30",
                      "wait 30", f"expect-flag {TEAM_WORLD_FLAG} 0", f"travel {CENTRAL_YHARNAM_LAMP}",
                      "wait 20", f"expect-flag {TEAM_WORLD_FLAG} 1", "done"],
        },
        "expect": {"guest": [r"party trip: arrived map=0x18010000"]},
        "timeout": 300,
    },
    # Kaniel's playtest 2026-10-22: krolebord, alone in Old Yharnam before the host, saw its
    # blood-moon hunters (m28 event 12800400 switches 2800740-2800744 on by flag 9802): his own
    # save's time of day, not the team's. Here the guest's game has the blood moon, the team's
    # world none; the host waits in the Dream and the guest's lamp trip loads Central Yharnam alone.
    "team-time-of-day-trip": {
        "about": "a guest whose own game has the blood moon takes the team's time of day (none) "
                 "at a lamp trip alone; the host's stays as it was",
        "roles": {
            "host": [START, "wait-session 2 300", "wait 20", f"travel {HUNTERS_DREAM}", "wait 30",
                     "wait 30", "wait 20", f"expect-flag {BLOOD_MOON} 0", "done"],
            "guest": [START, "wait-session 2 300", "wait 5", f"set-flag {BLOOD_MOON} 1",
                      "wait 30", f"travel {CENTRAL_YHARNAM_LAMP}", "wait 20",
                      f"expect-flag {BLOOD_MOON} 0", "done"],
        },
        # The arrival alone set it back on main too, after the map's events had picked by it: the
        # trip's load itself (the session's changes put the blood moon back) must take the team's.
        "expect": {"guest": [r"session's world changes as a trip loads the guest's own world"
                             r"[\s\S]{0,400}team's time of day in this load \(-off\): -9802",
                             r"party trip: arrived map=0x18010000"]},
        "timeout": 300,
    },
    # Kaniel 2026-10-22: a guest alone on a map saw NPCs the host's world hides (Cathedral Ward:
    # once the host came, only the right ones). Each trip's load put the guest's own game's story
    # steps of the session over the team's world while the map placed its characters; the arrival
    # set the team's back only after. Gilbert stands at his window only with 1246 (m24_01 event
    # 12410760; with none of 1240-1246, as the rig's characters have, he is hidden): the guest's
    # game takes that step, the host's world has none; the guest's lamp trip alone loads Central
    # Yharnam with the team's step.
    "team-story-trip": {
        "about": "a story step only the guest's game took (Gilbert at his window) yields to the "
                 "team's world at its lamp trip alone",
        "roles": {
            "host": [START, "wait-session 2 300", f"expect-flag {GILBERT_HERE} 0", "wait 20",
                     f"travel {HUNTERS_DREAM}", "wait 30", "wait 30", "wait 30",
                     f"expect-flag {GILBERT_HERE} 0", "done"],
            "guest": [START, "wait-session 2 300", "wait 5", f"set-flag {GILBERT_HERE} 1",
                      "wait 30", f"travel {CENTRAL_YHARNAM_LAMP}", "wait 28",
                      f"expect-flag {GILBERT_HERE} 0", "done"],
        },
        # On main the arrival set 1246 back too, after the trip's load had put it in for the map's
        # events (rig 2026-10-22: "set 5 of the session's world changes as a trip loads", 1246
        # among them): the load itself must leave it to the team's world.
        "expect": {"guest": [r"story steps left to the team's world \(-off\):[^\n]* 1246",
                             r"party trip: arrived map=0x18010000"]},
        "timeout": 300,
    },
    # Kaniel's test 2026-10-23: in the clinic the dog (0x10014009, entity 2410019) is not there;
    # both go to the Dream, the guest comes back alone and sees it; the host comes back and it is
    # gone again. As the map loads it starts loading ([control +0x14] wanted, +0x10 applied 4),
    # then the story hides it (+0x28); the host's game lets it go (0/0), the lone guest's kept it
    # (wanted 0, applied 4) and drew it. The lone guest switches it off (LoneGuestSwitchesOff).
    "dog-alone": {
        "about": "the clinic's dog, hidden by the story, stays unseen for a guest that comes back "
                 "alone from the Dream",
        "roles": {
            "host": [START, "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}",
                     "wait 25", "wait 25", f"travel {CENTRAL_YHARNAM_LAMP}", "wait 20", "census 60",
                     "done"],
            "guest": [START, "wait-session 2 300", "wait 15", f"travel {HUNTERS_DREAM}",
                      "wait 15", f"travel {CENTRAL_YHARNAM_LAMP}", "wait 12", "census 60",
                      f"expect-disabled {CLINIC_DOG} 1", "wait 20", "census 60", "done"],
        },
        "expect": {"guest": [r"party trip: arrived map=0x18010000"]},
        "timeout": 300,
    },
    # Kaniel's playtest 2026-10-24: krolebord teleported to the Grand Cathedral where Lisara (a guest)
    # ran the map, and saw Eileen there until the host came. Each guest without the host on its map
    # switches off what its story hid, the one following the other guest's game too. Here both
    # guests come back from the Dream to the clinic, the host stays in the Dream.
    "dog-alone3": {
        "about": "the clinic's dog stays unseen for both guests that come back to it without the "
                 "host, the one that runs the map and the one that follows it",
        "roles": {
            "host": [START, "wait-session 3 300", "wait 10", f"travel {HUNTERS_DREAM}", "wait 30",
                     "wait 30", "wait 30", "done"],
            "guest": [START, "wait-session 3 300", "wait 15", f"travel {HUNTERS_DREAM}",
                      "wait 15", f"travel {CENTRAL_YHARNAM_LAMP}", "wait 15", "census 60",
                      f"expect-disabled {CLINIC_DOG} 1", "wait 20", "done"],
            "guest2": [START, "wait-session 3 300", "wait 15", f"travel {HUNTERS_DREAM}",
                       "wait 18", f"travel {CENTRAL_YHARNAM_LAMP}", "wait 15", "census 60",
                       f"expect-disabled {CLINIC_DOG} 1", "wait 20", "done"],
        },
        "timeout": 300,
    },
    # Kaniel's test 2026-10-23: a guest alone in Cathedral Ward killed the hag and opened its door;
    # the host who came later found her alive and the door shut. A lone guest's game runs its map's
    # story; its changes to the map's flags go to the team (LoneGuestFlagsToShare). Here the host
    # is in the Dream, the guest alone in Central Yharnam sets TEAM_WORLD_FLAG (its map's, unused)
    # 10 s after its lamp trip there (the hag died 9 s after the load; the first fix waited for
    # the guest's alone state to settle and dropped it); the host comes back to it on. The map's
    # load flags stay the guest's (LoneGuestShares).
    "lone-guest-progress": {
        "about": "a map flag a guest alone on its map set is on for the host that comes back",
        "roles": {
            "host": [START, "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}", "wait 30",
                     "wait 30", f"travel {CENTRAL_YHARNAM_LAMP}", "wait 15",
                     f"expect-flag {TEAM_WORLD_FLAG} 1", "done"],
            "guest": [START, "wait-session 2 300", "wait 20", f"travel {CENTRAL_YHARNAM_LAMP}",
                      "wait 5", f"set-flag {TEAM_WORLD_FLAG} 1", "wait 30", "wait 20",
                      f"expect-flag {TEAM_WORLD_FLAG} 1", "done"],
        },
        "expect": {"guest": [r"lone guest's map steps go to the team \(-off\): 12410999\n"]},
        # Not the map's load flags (Gascoigne's per-visit 12414802...).
        "forbid": {"guest": [r"go to the team \(-off\):[^\n]* 124148"]},
        "timeout": 300,
    },
    # The same with three: the host and the second guest in the Dream while the first guest, alone
    # in Central Yharnam, sets the flag; both come back to it on.
    # Kaniel's test 2026-10-23: the hag a lone guest killed 9 s after its trip to Cathedral Ward was
    # alive for the host who came later. Its alone state had just changed (settling: deaths then
    # are no kills) and the kill was never posted. Here the guest, alone in Central Yharnam right
    # after a lamp trip, kills the enemy by the lamp with its own hits and stays; the host comes
    # back from the Dream and finds it killed (by the host it stands: it dies there, HP 0). The rig
    # does not hit the settle window every time: on the build before the fix it passed too.
    "lone-guest-kill": {
        "about": "an enemy a guest killed right after its lone trip stays killed for the host "
                 "that comes back while the guest is still there",
        "roles": {
            "host": [START, "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}", "wait 30",
                     "wait 30", f"travel {CENTRAL_YHARNAM_LAMP}", "wait 15", "census 40",
                     f"expect-hp {ENEMY} == 0", "done"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 20",
                      f"travel {CENTRAL_YHARNAM_LAMP}", f"teleport {GUEST_ALONE_SPOT}",
                      f"attack {ENEMY} 30", "census 40", "wait 30", "wait 30", "done"],
        },
        "expect": {"guest": [r"kill board gets 1 kills, 0 back \(visit 0x1801[0-9a-f]+\): "
                             r"0x1001400b"]},
        "timeout": 300,
    },
    "lone-guest-progress3": {
        "about": "a map flag a guest alone on its map set is on for the host and the other guest "
                 "that come back",
        "roles": {
            "host": [START, "wait-session 3 300", "wait 10", f"travel {HUNTERS_DREAM}", "wait 30",
                     "wait 30", f"travel {CENTRAL_YHARNAM_LAMP}", "wait 15",
                     f"expect-flag {TEAM_WORLD_FLAG} 1", "done"],
            "guest": [START, "wait-session 3 300", "wait 30", "wait 5", f"set-flag {TEAM_WORLD_FLAG} 1",
                      "wait 30", "wait 30", f"expect-flag {TEAM_WORLD_FLAG} 1", "done"],
            "guest2": [START, "wait-session 3 300", "wait 10", f"travel {HUNTERS_DREAM}",
                       "wait 30", "wait 30", f"travel {CENTRAL_YHARNAM_LAMP}", "wait 15",
                       f"expect-flag {TEAM_WORLD_FLAG} 1", "done"],
        },
        "timeout": 300,
    },
    # The other way with three: the host alone in Central Yharnam sets the flag while both guests
    # are in the Dream; both come back to it on (the team's world, TakesTeamWorldBlock).
    "host-progress3": {
        "about": "a map flag the host set while both guests were in the Dream is on for both when "
                 "they come back",
        "roles": {
            "host": [START, "wait-session 3 300", "wait 30", "wait 5", f"set-flag {TEAM_WORLD_FLAG} 1",
                     "wait 30", "wait 30", f"expect-flag {TEAM_WORLD_FLAG} 1", "done"],
            "guest": [START, "wait-session 3 300", "wait 10", f"travel {HUNTERS_DREAM}",
                      "wait 30", "wait 30", f"travel {CENTRAL_YHARNAM_LAMP}", "wait 15",
                      f"expect-flag {TEAM_WORLD_FLAG} 1", "done"],
            "guest2": [START, "wait-session 3 300", "wait 10", f"travel {HUNTERS_DREAM}",
                       "wait 30", "wait 30", f"travel {CENTRAL_YHARNAM_LAMP}", "wait 15",
                       f"expect-flag {TEAM_WORLD_FLAG} 1", "done"],
        },
        "timeout": 300,
    },
    # Release thread 2026-10-16 (dog): the host travelled to the Dream before it saw the guest's
    # character; neither game said ready and the guest stayed covered (its input held) until the
    # host came back. A member on another map is not waited for: both screens open with the host
    # still in the Dream.
    "team-load-apart": {
        "about": "the host leaves for the Dream as soon as the team's session is up: neither "
                 "loading screen waits for it, both open with the host still away",
        "roles": {
            "host": ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                     "wait-session 2 300", f"travel {HUNTERS_DREAM}", "wait 25",
                     "log apart-host", "wait 5"],
            "guest": ["team-accept", "team-ready", START, "wait-session 2 300", "wait 30",
                      "log apart-guest", "wait 5"],
        },
        "expect": {"host": [r"load view is gone[\s\S]*autoplay log: apart-host"],
                   "guest": [r"load view is gone[\s\S]*autoplay log: apart-guest"]},
        "forbid": {role: [r"load view is gone[\s\S]*load view is up"]
                   for role in ("host", "guest")},
        "timeout": 300,
    },
    # Kaniel 2026-10-15: the team's loading screen stays over the whole screen, the sound held,
    # until every guest has fully joined the host (its character spawned), then opens for all.
    # Kaniel 2026-10-17: the teammate compass. The host stands 5.2 m south of the guest in the
    # hallway north of Iosefka's door and turns. After a teleport with heading h the camera looks
    # along (sin h, -cos h) (rig 2026-10-17: h 0 looks south, -z; a quarter turn west, -x; half a
    # turn north): the guest behind the host, then on its right, then ahead. The guest, at h 0,
    # looks south at the host.
    "compass": {
        "about": "the teammate compass: each game's marker for the other is at the bearing and "
                 "distance where it stands as the camera sees it; the host shoots the strip",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 5",
                     f"teleport {COMPASS_HOST} 0.0", "wait 12", "expect-compass 180 5.2",
                     "shot host-compass-behind", f"teleport {COMPASS_HOST} 1.5708", "wait 4",
                     "expect-compass 90 5.2", "shot host-compass-right",
                     f"teleport {COMPASS_HOST} 3.1416", "wait 4", "expect-compass 0 5.2",
                     "shot host-compass-ahead", "wait 10"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 8",
                      f"teleport {COMPASS_GUEST} 0.0", "wait 10", "expect-compass 0 5.2",
                      "wait 25"],
        },
        "expect": {role: [r"autoplay compass: camera heading"] for role in ("host", "guest")},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    # Kaniel 2026-10-17: a downed member's marker on the compass (the party list's downed badge,
    # its revive, over the others). Both at the lamp; the guest falls and stays down.
    "compass-down": {
        "about": "the teammate compass shows a downed member as down, with its revive; the host "
                 "shoots it",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-member-vital down 120",
                     "wait 2", "expect-compass 0 3 180 40 down", "shot host-compass-down",
                     "wait 5", "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 15", *LETHAL_FALL,
                      "wait-vital down 30", "wait 20"],
        },
        "expect": {"host": [r"autoplay compass: camera heading .* down\]"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    # Kaniel 2026-10-24: team pings. The host pings where its camera aims (looking at the guest,
    # 5 m north) and then a spot on the map in the Cathedral Ward (another area): each replaces the
    # one before. The guest's game shows each on the right map at the right spot, the aimed one
    # on its screen; the ping is still there 15 s on and gone by 20 s.
    "pings": {
        "about": "team pings: the host's aimed ping, then its map ping on another area replacing "
                 "it, reach the guest with their maps and spots; a ping lasts 20 s",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 5",
                     f"teleport {COMPASS_HOST} 3.1416", "wait 8", "ping-aim", "wait 2",
                     f"expect-pings 1 10 {CENTRAL_YHARNAM}", "sync ping-aimed", "wait 3",
                     f"ping-map {CATHEDRAL_WARD} {PING_MAP_SPOT}", "wait 2",
                     f"expect-pings 1 10 {CATHEDRAL_WARD} {PING_MAP_XZ} 0.5", "sync ping-mapped",
                     "expect-pings 0 30", "wait 5"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 5",
                      f"teleport {COMPASS_GUEST} 0.0", "sync ping-aimed",
                      f"expect-pings 1 10 {CENTRAL_YHARNAM}", "sync ping-mapped",
                      f"expect-pings 1 10 {CATHEDRAL_WARD} {PING_MAP_XZ} 0.5", "wait 15",
                      f"expect-pings 1 2 {CATHEDRAL_WARD}", "expect-pings 0 15", "wait 5"],
        },
        "syncs": {"ping-aimed": {"chapter": "pings", "roles": ["host", "guest"]},
                  "ping-mapped": {"chapter": "pings", "roles": ["host", "guest"]}},
        "expect": {"host": [r"Co-op ping: placed seq 1 on map 0x18010000 .*\(aimed\)",
                            r"Co-op ping: placed seq 2 on map 0x18000000 .*\(on the map\)",
                            r"Co-op ping: this player's ping seq 2 is gone"],
                   "guest": [r"Co-op ping: from \w+ \(.*\) seq 1 on map 0x18010000",
                             r"autoplay pings: 1: \[.* on map 0x18010000 .* here [\d.]+ m",
                             r"Co-op ping: from \w+ \(.*\) seq 2 on map 0x18000000 at "
                             r"\(-150\.00, -40\.00, 300\.00\) enemy 0x0 \(from the map\)",
                             r"Co-op ping: \w+'s ping seq 2 is gone"]},
        "forbid": {"host": SESSION_HOLDS + [r"Co-op ping: too soon"],
                   "guest": SESSION_HOLDS + [r"Co-op ping: placed"]},
        "timeout": 300,
    },
    # Kaniel 2026-10-20 ("bloodstains of ours will be displayed on compass and map"; after v1.0.2:
    # "you only see your bloodstain on compass and map"): the guest (given echoes) dies by the lamp
    # and wakes there; its own stain is on its compass and map, the host's show none while it lies
    # there; the guest picks it up, and it goes from its own.
    "bloodstain": {
        "about": "bloodstains: a player's own on its compass and map, never on a teammate's, gone "
                 "once picked up",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-member-vital dead 180",
                     "wait-member-vital up 60", "wait 1", "expect-stains 0 0 2",
                     "shot host-stain-compass", "wait 30"],
            # The stain is recorded as the guest gets up; it picks it up 6 s later (the host has
            # looked by then). A longer wait first missed the pickup twice (rig 2026-10-21).
            "guest": [START, "wait-session 2 300", "trade-give echoes 500", "wait 15", *GIVE_UP,
                      "wait-vital up 60", "wait 6", "expect-stains 1 1 30",
                      "shot guest-stain-compass", "pickup-stain 15", "wait 3",
                      "expect-stains 0 0 30", "wait 10"],
        },
        "expect": {
            "host": [r"autoplay stains: none"],
            "guest": [r"autoplay stains: \[compass own", r"bloodstain picked up",
                      r"autoplay stains: none"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    # Kaniel 2026-10-25, the host rules (each off by default; the rig's games are in no team, so each
# plays by its own SHADPS4_BLOODBORNE_HOST_RULES, the same for both).
# Rot: the guest's real death (it gives up) adds a penalty, on it as its effect; picking up its own
# bloodstain takes it away; a second death adds one again and a lamp takes that away.
"rot-death-lamp": {
        "about": "the Rot rule: a real death adds a penalty (its effect on), the own bloodstain and a "
                 "lamp take it away",
        "settings": {**PARTY_DEATH, "SHADPS4_BLOODBORNE_HOST_RULES": "rot"},
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-member-vital dead 180",
                     "wait-member-vital up 60", "wait-member-vital dead 120",
                     "wait-member-vital up 60", "wait 25", "expect-rot 0"],
            "guest": [START, "wait-session 2 300", "trade-give echoes 500", "wait 15", *GIVE_UP,
                      "wait-vital up 60", "expect-rot 1 20", "wait 6", "pickup-stain 15",
                      "expect-rot 0 10", "wait 5", *GIVE_UP, "wait-vital up 60",
                      "expect-rot 1 20", "use-lamp", "expect-rot 0 10", "wait 10"],
        },
        "expect": {"guest": [r"Co-op rot: died for real: .* \(1 of 5;",
                             r"Co-op rot: cured by its own bloodstain",
                             r"Co-op rot: cured by a lamp"]},
        "forbid": {"host": SESSION_HOLDS + [r"Co-op rot: died for real"],
                   "guest": [r"Co-op rot: SpEffect \d+ could not be written"]},
        "timeout": 300,
    },
    # Travel together: the host's lamp asks first; With the team takes the guest to the Hunter's
    # Dream too, in the same session.
    "travel-together": {
        "about": "the travel together rule: the host's lamp trip with the team takes the guest to the "
                 "Hunter's Dream as well",
        "settings": {"SHADPS4_BLOODBORNE_HOST_RULES": "travel"},
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10", "use-lamp",
                     "travel-answer yes 10", "wait-map 0x15000000 120", "wait 15",
                     "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait-map 0x15000000 180",
                      "wait 15", "expect-session 2"],
        },
        "expect": {"host": [r"Co-op team trip: asks this player about travelling to 2102962",
                            r"Co-op team trip 1: the team travels to 2102962"],
                   "guest": [r"Co-op team trip 1: \w+ takes the team to 2102962",
                             r"Co-op team trip: this player travels to 2102962"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    # With the vote, the guest's No stops the trip: nobody travels.
    "travel-vote-no": {
        "about": "the travel together vote: the guest says No to the host's team trip and nobody "
                 "travels",
        "settings": {"SHADPS4_BLOODBORNE_HOST_RULES": "travel,vote"},
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10", "use-lamp",
                     "travel-answer yes 10", "wait 20", f"expect-map {CENTRAL_YHARNAM}",
                     "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "travel-answer no 60", "wait 20",
                      f"expect-map {CENTRAL_YHARNAM}", "expect-session 2"],
        },
        "expect": {"host": [r"Co-op team trip 1: the team votes on 2102962 \(1 asked\)",
                            r"Co-op team trip 1: \w+ says no",
                            r"Co-op team trip 1: \w+ said no; nobody travels"],
                   "guest": [r"Co-op team trip 1: \w+ asks the team to travel to 2102962",
                             r"Co-op team trip 1: this player says no",
                             r"Co-op team trip 1: \w+'s trip is off"]},
        "forbid": {"host": [r"Co-op team trip: this player travels"],
                   "guest": [r"Co-op team trip: this player travels"]},
        "timeout": 300,
    },
    # Enemy scaling: two hunters on the map scale its enemies one step up from the normal game
    # (x1.67 HP), back to it once the guest leaves for the Dream.
    "scale-team-size": {
        "about": "the enemy scaling rule: a second hunter on the map raises the enemies, its leaving "
                 "brings back the normal game",
        "settings": {"SHADPS4_BLOODBORNE_HOST_RULES": "scale"},
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-members 2 120", "wait 10",
                     "wait 30", "wait 30", "wait 30"],
            "guest": [START, "godmode", "wait-session 2 300", "wait-members 2 120", "wait 10",
                      f"travel {HUNTERS_DREAM}", "wait-map 0x15000000 120", "wait 10"],
        },
        "expect": {"host": [r"Bloodborne enemy scaling: 2 hunter\(s\) on this map: x1\.67 HP, "
                            r"x1\.17 damage, x1\.67 poise",
                            r"Bloodborne harder enemies: \d+ NpcParam rows, x1\.67 HP",
                            r"Bloodborne enemy scaling: 1 hunter\(s\) on this map: the normal game",
                            r"Bloodborne harder enemies: \d+ NpcParam rows, the normal game"],
                   "guest": [r"Bloodborne enemy scaling: 2 hunter\(s\) on this map: x1\.67 HP"]},
        "forbid": {"host": SESSION_HOLDS},
        "timeout": 300,
    },
    # A player's report on v1.1.2: a guest's own stain may not show on its compass. The guest's
    # deaths other than "bloodstain"'s give-up by the lamp, each ending where its stain lies.
    # Alone on its map (the host gone to the Ward): the fall is a real death, no down, and the
    # guest wakes at the host's lamp (PartyDeathPoint).
    "bloodstain-alone": {
        "about": "a guest alone on its map dies by a fall (no down), wakes at the host's lamp in "
                 "the Ward and walks back; its own stain is on its compass there",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 5",
                     f"travel {CATHEDRAL_WARD_LAMP}", "wait-member-vital dead 180", "wait 30",
                     "wait 30", "wait 30"],
            "guest": [START, "wait-session 2 300", "trade-give echoes 500", "wait 30",
                      *LETHAL_FALL, "wait-vital dead 30", "wait-vital up 60", "wait 8",
                      f"expect-map {CATHEDRAL_WARD}", "expect-stains 0 1 20",
                      f"glide {WARD_TO_YHARNAM}", "wait 8", f"expect-map {CENTRAL_YHARNAM}",
                      "expect-stains 1 1 30", "shot guest-stain-alone", "wait 3"],
        },
        "expect": {"guest": [r"autoplay stains: \[compass own"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    # Its last lamp the Ward's (the trip there): it walks to Central Yharnam, gives up on the open
    # ground west of the first lamp (a fall at the gate dropped it through the world, rig
    # 2026-10-22) and wakes where PartyDeathPoint puts it (the host's lamp, the stain's map).
    "bloodstain-far-lamp": {
        "about": "a guest whose last lamp is the Ward's walks to Central Yharnam and gives up "
                 "there; its own stain is on its compass and map",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-member-vital dead 240",
                     "wait 30", "wait 30", "wait 30"],
            "guest": [START, "godmode", "wait-session 2 300", "trade-give echoes 500",
                      f"travel {CATHEDRAL_WARD_LAMP}", "wait 5", f"glide {WARD_TO_YHARNAM}",
                      "wait 8", f"expect-map {CENTRAL_YHARNAM}", f"teleport {GUEST_ALONE_SPOT}",
                      "wait 3", "godmode off", *GIVE_UP, "wait-vital up 60", "wait 8",
                      "log woke after the give-up", "expect-stains 1 1 30",
                      "shot guest-stain-far-lamp", "wait 3"],
        },
        "expect": {"guest": [r"autoplay stains: \[compass own"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    # A lamp trip away and back after the give-up.
    "bloodstain-trip": {
        "about": "a guest gives up by the lamp, travels to the Dream and back; its own stain is "
                 "still on its compass and map",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-member-vital dead 180",
                     "wait 30", "wait 30", "wait 30"],
            "guest": [START, "wait-session 2 300", "trade-give echoes 500", "wait 15", *GIVE_UP,
                      "wait-vital up 60", "wait 6", "expect-stains 1 1 30",
                      f"travel {HUNTERS_DREAM}", "wait 10", "expect-stains 0 1 20",
                      f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 120", "wait 10",
                      "expect-stains 1 1 30", "shot guest-stain-trip", "wait 3"],
        },
        "expect": {"guest": [r"autoplay stains: \[compass own[\s\S]*autoplay stains: \[map own"
                             r"[\s\S]*autoplay stains: \[compass own"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    # The guest's game quits after the give-up and joins again (the join reload).
    "bloodstain-rejoin": {
        "about": "a guest gives up by the lamp, its game quits and joins the host again; its own "
                 "stain is still on its compass and map",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-member-vital dead 180",
                     "wait-solo 120", "wait-session 2 420", "wait 30", "wait 20"],
            "guest": [[START, "wait-session 2 300", "trade-give echoes 500", "wait 15", *GIVE_UP,
                       "wait-vital up 60", "wait 6", "expect-stains 1 1 30", "wait 5", "done"],
                      [START, "wait-session 2 420", "wait 15", "expect-stains 1 1 30",
                       "shot guest-stain-rejoin", "wait 3"]],
        },
        "expect": {"guest": [r"autoplay stains: \[compass own"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "team-load-art": {
        "about": "the team loading screen's art (Kaniel 2026-10-17: the co-op art's night city, cards, "
                 "medallions and sigils): each game shoots its loading screen as it comes up",
        "roles": {
            "host": ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START, "shot host-loading",
                     "wait-session 2 300"],
            "guest": ["team-accept", "team-ready", START, "shot guest-loading", "wait-session 2 300"],
        },
        "expect": {role: [r"Co-op team load: the screen covers the game"] for role in ("host", "guest")},
        "timeout": 300,
    },
    "team-load-cover": {
        "about": "a team's load: each game's loading screen stays full screen with its sound "
                 "held until the other hunter's character stands in its world, then it opens "
                 "and the sound comes back",
        "roles": {
            "host": ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                     "wait-session 2 300", "wait 30", "expect-session 2"],
            "guest": ["team-accept", "team-ready", START, "wait-session 2 300", "wait 30",
                      "expect-session 2"],
        },
        "expect": {role: [r"Co-op team load: the screen covers the game, its sound held",
                          r"Co-op team load: this game is ready, 1 of 1 other hunters' characters "
                          r"in its world[\s\S]*load view is gone",
                          r"Co-op team load: the screen opens, the sound back"]
                   for role in ("host", "guest")},
        "forbid": {role: SESSION_HOLDS + [r"load view is gone[\s\S]*load view is up"]
                   for role in ("host", "guest")},
        "console_expect": {role: [r"Runtime: sound held under the team's loading screen",
                                  r"Runtime: sound back after [1-9]\d* silent buffers"]
                           for role in ("host", "guest")},
        "timeout": 300,
    },
    # Play test 2026-10-18 (v0.3.11-local): Krolebord had left the team's world in the Dream; at the
    # next team load his game resumed him there while Kaniel, the host, loaded in Central Yharnam,
    # and the team's screen opened with them apart. A guest's team load ends beside its host: here
    # the guest leaves from the Dream, and its next team load (no START: nothing of the rig's own
    # takes it anywhere) travels to the host and stands where it is before the screen opens.
    "team-load-beside-host": {
        "about": "a guest that left the team's world in the Dream loads with the team again and "
                 "stands beside the host in Central Yharnam when the team's screen opens",
        "roles": {
            # Kaniel 2026-10-25: the guest's game goes first (out of the team, invited back), so
            # the host's is the last to go and it still leads.
            "host": [["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                      "wait-session 2 300", "wait 45", "sync tlbh-gone", "wait 10", "done"],
                     # The team again (the server drops a team whose members all went offline,
                     # as both games do between the stages).
                     ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                      "wait-session 2 300", "wait 20", "expect-session 2",
                      f"expect-map {CENTRAL_YHARNAM}"]],
            "guest": [["team-accept", "team-ready", START, "wait-session 2 300", "wait 5",
                       f"travel {HUNTERS_DREAM}", "wait 30", "sync tlbh-gone", "done"],
                      ["team-accept", "team-ready", "wait-session 2 300", "wait 15",
                       "expect-session 2",
                       f"expect-map {CENTRAL_YHARNAM}", "expect-others-near 1 15"]],
        },
        "expect": {"guest": [r"team load: the host's place is map 0x18010000[\s\S]*guest resumes "
                             r"on map 0x18010000; travelling[\s\S]*guest resumed at[\s\S]*the "
                             r"guest's resume is done[\s\S]*this game is ready, 1 of 1 other "
                             r"hunters' characters in its world[\s\S]*load view is gone"]},
        # Both stages' logs are read as one: a screen that opens and covers again counts in the
        # second (the stage that travels).
        "forbid": {"guest": [r"travelling[\s\S]*load view is gone[\s\S]*load view is up"]},
        "syncs": {"tlbh-gone": {"chapter": "team-load-beside-host", "roles": ["host", "guest"]}},
        "allow": CRASH_CLEANUP,
        "timeout": 900,
    },
    # Phase 5b: the team's world. A world flag no save has (TEAM_WORLD_FLAG, Central Yharnam's
    # block, unused) set by the host in the team's first session is in the team's world; the
    # guest's save takes it at its next team load, and loses it again when it plays alone (its own
    # world back).
    "team-world": {
        "about": "the team's world: the host's save makes it, the guest's save takes it in the "
                 "next session, and the guest alone has its own world back",
        "roles": {
            # Kaniel 2026-10-25: the guest's game goes first (out of the team, invited back), so
            # the host's is the last to go and it still leads.
            "host": [["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                      "wait-session 2 300", f"set-flag {TEAM_WORLD_FLAG} 1", "wait 15",
                      "sync tw-gone", "wait 10", "done"],
                     ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                      "wait-session 2 300", f"expect-flag {TEAM_WORLD_FLAG} 1",
                      "wait 60",
                      "done"]],
            "guest": [["team-accept", "team-ready", START, "wait-session 2 300",
                       f"expect-flag {TEAM_WORLD_FLAG} 0", "wait 45", "sync tw-gone", "done"],
                      ["team-accept", "team-ready", START, "wait-session 2 300",
                       f"expect-flag {TEAM_WORLD_FLAG} 1",
                       "done"],
                      ["team-leave", "team-alone", START, "wait 10",
                       f"expect-flag {TEAM_WORLD_FLAG} 0"]],
        },
        "expect": {
            "host": [r"Co-op team world: this host's world to the server"],
            "guest": [r"Co-op team world: the team's world is in this save",
                      r"Co-op team world: slot \d+'s own world kept aside",
                      r"Co-op team world: slot \d+ plays alone: its own world is back"],
        },
        "syncs": {"tw-gone": {"chapter": "team-world", "roles": ["host", "guest"]}},
        "allow": CRASH_CLEANUP,
        "timeout": 900,
    },
    # Kaniel's play test 2026-10-18: krolebord joined the team and his lamps listed only Cathedral
    # Ward. A lamp stays lit only for a character whose lamp list holds it (the game clears its
    # active flag otherwise), and the team world's lamps went into the guest's flags at its load,
    # never into its list. Here the host lights a lamp (FORBIDDEN_WOODS_LAMP_LIT) once the guest's
    # game has gone; the guest's next team load takes it and keeps it lit, also after a trip.
    "team-world-lamps": {
        "about": "a lamp the host lit while the guest was away stays lit for the guest after its "
                 "next team load and a trip",
        "roles": {
            "host": [["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                      "wait-session 2 300", "sync lamps-joined", "wait 15",
                      f"set-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", "wait 20",
                      f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", "done"],
                     ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                      "wait-session 2 300", "sync lamps-back", "done"]],
            "guest": [["team-accept", "team-ready", START, "wait-session 2 300",
                       f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 0", "sync lamps-joined", "done"],
                      ["team-accept", "team-ready", START, "wait-session 2 300", "wait 20",
                       f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", f"travel {HUNTERS_DREAM}",
                       "wait 25", f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", "sync lamps-back",
                       "done"]],
        },
        "expect": {
            "guest": [r"Co-op team world: the team's world is in this save"],
        },
        "syncs": {"lamps-joined": {"chapter": "team-world-lamps", "roles": ["host", "guest"]},
                  "lamps-back": {"chapter": "team-world-lamps", "roles": ["host", "guest"]}},
        "allow": CRASH_CLEANUP,
        "timeout": 600,
    },
    # Kaniel 2026-10-17: the world list (as the game's Load Game). A team's first hunt makes a
    # world; back at the title the host leaves that team (its guest still hunts in it, so its
    # world cannot change), and in no team its screen starts on that world (the save's own,
    # picked before a team) and its list shows it, greyed: the guest's team plays it now.
    "team-worlds": {
        "about": "the world list: a team's hunt makes a world, the host's next screen starts on it "
                 "and lists it (headless: pictures of the list)",
        "roles": {
            "host": [["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                      "wait-session 2 300", "done"],
                     ["team-leave", "team-worlds", "title-wait 3", "title-shot worlds-list",
                      "team-alone", START, "done"]],
            "guest": [["team-accept", "team-ready", START, "wait-session 2 300", "wait 10",
                       "done"]],
        },
        "expect": {
            "host": [r"Co-op team lobby: the team screen starts on world \w+"],
        },
        "timeout": 300,
    },
    "team-screen": {
        "about": "the team screen after Continue (run drawn: pictures): a team of two forms, the "
                 "guest is ready by itself, the host begins",
        "roles": {
            "host": ["team-create", "team-invite rig_guest", "team-wait 2", "team-screen",
                     "title-wait 3", "title-shot screen-host", "title-wait 10",
                     "title-shot screen-host-ready", "team-start", START, "wait-session 2 300",
                     "snapshot"],
            "guest": ["team-accept", "team-screen", "title-wait 2", "title-shot screen-guest",
                      "team-ready", "title-wait 2", "title-shot screen-guest-ready", START,
                      "wait-session 2 300", "snapshot"],
        },
        "timeout": 300,
    },
    "team-join3": {
        "about": "a team of three from the lobby: the host invites both guests",
        "roles": {
            "host": ["team-create", "team-invite rig_guest", "team-invite rig_guest2",
                     "team-wait 3", "team-start", START, "godmode", "wait-session 3 480", "wait 10",
                     "snapshot", "expect-session 3"],
            "guest": ["team-accept", "team-ready", START, "godmode", "wait-session 3 480", "wait 10",
                      "expect-others-shown 2", "snapshot", "expect-session 3"],
            "guest2": ["team-accept", "team-ready", START, "godmode", "wait-session 3 480", "wait 10",
                       "expect-others-shown 2", "snapshot", "expect-session 3"],
        },
        "expect": {
            "host": [r"Co-op team lobby: the save plays in team \w+ as its host"],
            "guest": [r"Co-op team lobby: the save plays in team \w+ as its guest"],
            "guest2": [r"Co-op team lobby: the save plays in team \w+ as its guest"],
        },
        "timeout": 300,
    },
    # Kaniel 2026-10-16: the team menu in game (the System menu's Co-op Team page). The leader
    # removes a member while both play: it leaves the session where it stands and plays on alone
    # (no title, no reload); the host plays on with its session open.
    "team-menu-kick": {
        "about": "in game, the host removes the guest from the team: the guest leaves the session "
                 "where it stands and plays alone, the host plays on",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10", "menu-kick rig_guest 60",
                     "wait-solo 120", "wait 10", "snapshot", f"expect-map {CENTRAL_YHARNAM}"],
            "guest": [START, "godmode", "wait-session 2 300", "wait-solo 120", "wait 15",
                      "snapshot", f"expect-map {CENTRAL_YHARNAM}", "wait 15", "snapshot",
                      f"expect-map {CENTRAL_YHARNAM}"],
        },
        "expect": {
            # Amanises 2026-10-25 (#412): then loaded into its own world where it stood, not left
            # invisible beside the host's frozen copy.
            "guest": [r"Co-op team menu: out of the team \(world:\w+\): left its session",
                      r"Co-op team menu: out of the team: its own world loads where it stood"],
        },
        # The removed guest's own leave (MultiPlayStopRequest where it stands) is the point.
        "allow": [r"left the session outside a trip"],
        "timeout": 300,
    },
    # A save playing alone is invited from the leader's menu and joins the hunt from its own,
    # where it stands, without going back to the title.
    # A member leaves the team from the menu while playing: out of the session where it stands, it
    # plays on alone; the host plays on with its session open.
    # A save played alone while another game starts a team loads into its world (rig 2026-10-16:
    # with the team menu's first build its load hung 3 times in 7, the game's flag read run every
    # frame of it).
    "team-solo-load": {
        "about": "the host starts a team alone and the guest plays alone: the guest's load gets "
                 "into its world",
        "roles": {
            "host": ["team-create", "team-start", START, "wait 20", "snapshot"],
            "guest": ["team-alone", START, "wait 10", "snapshot"],
        },
        "timeout": 200,
    },
    "team-menu-leave": {
        "about": "in game, the guest leaves the team from the menu: it leaves the session where it "
                 "stands and plays alone, the host plays on",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-solo 120", "wait 10",
                     "snapshot", f"expect-map {CENTRAL_YHARNAM}"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10", "menu-leave 60",
                      "wait-solo 60", "wait 15", "snapshot", f"expect-map {CENTRAL_YHARNAM}"],
        },
        "expect": {
            # Amanises 2026-10-25 (#412): then loaded into its own world where it stood, not left
            # invisible beside the host's frozen copy.
            "guest": [r"Co-op team menu: out of the team \(world:\w+\): left its session",
                      r"Co-op team menu: out of the team: its own world loads where it stood"],
        },
        "allow": [r"left the session outside a trip"],
        "timeout": 300,
    },
    # Kaniel 2026-10-16: the team menu from the Insert menu too (its "Co-op Team" row, the last
    # but Quit): the team screen opens over the game and its Close goes back to the game. The
    # team's loading screen lingers a while after the join (the menu does not open under it).
    "team-menu-insert": {
        "about": "in game, the Insert menu's Co-op Team row opens the team screen; its Close "
                 "closes it again, the session holds",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 30", "wait 20", "snapshot",
                     "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 25", "keys toggle up up enter",
                      "wait 3", "shot team-menu", "keys enter", "wait 3", "snapshot", "expect-session 2"],
        },
        "expect": {
            "guest": [r"Co-op team menu: the team screen is open from the port's Insert menu",
                      r"Co-op team menu: the team screen is closed back to the port's Insert menu"],
        },
        "forbid": {"guest": SESSION_HOLDS},
        "timeout": 300,
    },
    # Kaniel 2026-10-16: a removed member invited back joins again from the menu where it stands
    # (its game's second join crashed at eboot+0x13cffba until the flag block guard).
    "team-menu-rejoin": {
        "about": "in game, the host removes the guest, then invites it back: the guest joins the "
                 "hunt again from the menu, no title, no crash",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10", "menu-kick rig_guest 60",
                     "wait-solo 120", "wait 10", "menu-invite rig_guest 120",
                     "wait-session 2 120", "wait 20", "snapshot", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait-solo 120", "wait 10",
                      "menu-join 120", "wait-session 2 120", "wait 20", "snapshot",
                      "expect-session 2", f"expect-map {CENTRAL_YHARNAM}"],
        },
        "expect": {
            "guest": [r"Co-op team menu: out of the team \(world:\w+\): left its session",
                      r"Co-op team menu: this save joins team \w+ where it stands, as its guest"],
        },
        "allow": [r"left the session outside a trip"],
        "timeout": 300,
    },
    # Player report 2026-10-23 (v1.1.9): a guest of a team sometimes came back in the Hunter's Dream,
    # not where it was, and the host never saw it until the host went there too. The team on
    # Cathedral Ward (both rig saves start in Central Yharnam); the guest's game starts again and
    # it joins the team's session where it was, seen by the host.
    "team-guest-restart": {
        "about": "a team on Cathedral Ward: the guest's game starts again and its team load puts "
                 "it back on the Ward beside the host, both seeing each other",
        "roles": {
            "host": ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                     "godmode", "wait-session 2 300", f"travel {CATHEDRAL_WARD_LAMP}",
                     "wait-members 2 120", "wait 10", "wait-solo 180", "wait-session 2 420",
                     "wait-members 2 120", "wait 20", "snapshot", f"expect-map {CATHEDRAL_WARD}",
                     "expect-others-shown 1", "expect-session 2"],
            "guest": [["team-accept", "team-ready", START, "godmode", "wait-session 2 300",
                       f"travel {CATHEDRAL_WARD_LAMP}", "wait-members 2 120", "wait 15",
                       "snapshot", "done"],
                      ["team-ready", "godmode", "wait-session 2 420", "wait-members 2 120",
                       "wait 20", "snapshot", f"expect-map {CATHEDRAL_WARD}",
                       "expect-others-shown 1", "expect-session 2"]],
        },
        "allow": CRASH_CLEANUP,
        "timeout": 900,
    },
    # The same report: the host's game went (in the player's log it quit to the title after a
    # wipe and opened its session again) while the guest stayed in its world on the team's map.
    "team-host-restart": {
        "about": "a team on Cathedral Ward: the host's game starts again; the guest stays on the "
                 "Ward and leads the team now (it hosts there), and the old host joins it there "
                 "as its guest, both seeing each other (Kaniel 2026-10-25)",
        "roles": {
            "host": [["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                      "godmode", "wait-session 2 300", f"travel {CATHEDRAL_WARD_LAMP}",
                      "wait-members 2 120", "wait 15", "snapshot", "done"],
                     ["team-accept", "team-ready", "godmode", "wait-session 2 420",
                      "wait-members 2 120", "wait 20", "snapshot", f"expect-map {CATHEDRAL_WARD}",
                      "expect-others-shown 1", "expect-session 2"]],
            "guest": ["team-accept", "team-ready", START, "godmode", "wait-session 2 300",
                      f"travel {CATHEDRAL_WARD_LAMP}", "wait-members 2 120", "wait-solo 180",
                      "wait 5", "snapshot", "wait-session 2 420", "wait-members 2 120", "wait 20",
                      "snapshot", f"expect-map {CATHEDRAL_WARD}", "expect-others-shown 1",
                      "expect-session 2"],
        },
        "expect": {"guest": [LEADS_NOW], "host": [PLAYS_AS_GUEST]},
        "allow": CRASH_CLEANUP,
        "timeout": 900,
    },
    # The player's log: each time, the host's game left 35-40 s after a party wipe (both woke at
    # the host's lamp) and opened its session again; the guest came back in the Dream.
    "team-wipe-host-restart": {
        "about": "a team on Cathedral Ward wipes and wakes at the lamp; the host's game then "
                 "starts again: the guest stays on the Ward and leads the team now, and the old "
                 "host joins it there as its guest, both seeing each other",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                      "wait-session 2 300", f"travel {CATHEDRAL_WARD_LAMP}", "wait-members 2 120",
                      "wait-member-vital down 120", "wait 3", *LETHAL_FALL, "wait-vital dead 30",
                      "wait-vital up 120", "wait-members 2 240", "wait 25", "snapshot", "done"],
                     ["team-accept", "team-ready", "godmode", "wait-session 2 420",
                      "wait-members 2 120", "wait 20", "snapshot", f"expect-map {CATHEDRAL_WARD}",
                      "expect-others-shown 1", "expect-session 2"]],
            "guest": ["team-accept", "team-ready", START, "wait-session 2 300",
                      f"travel {CATHEDRAL_WARD_LAMP}", "wait-members 2 120", "wait 10",
                      *LETHAL_FALL, "wait-vital down 30", "wait-vital dead 60",
                      "wait-vital up 120", "godmode", "wait-solo 240", "wait 5", "snapshot",
                      "wait-session 2 420", "wait-members 2 120", "wait 20", "snapshot",
                      f"expect-map {CATHEDRAL_WARD}", "expect-others-shown 1",
                      "expect-session 2"],
        },
        "expect": {"guest": [LEADS_NOW], "host": [PLAYS_AS_GUEST]},
        "allow": CRASH_CLEANUP,
        "timeout": 900,
    },
    # Kaniel 2026-10-25: anyone who leaves is out of the team at once, the host too. The host quits
    # to the title (the System menu's Exit Game) while the guest plays: the guest leads the team
    # now and hosts its session where it stands (its own enemies, the team's world from its
    # save); the old host takes the invite back and joins as a guest, in the same game. The old host
    # waits at the title until the new leader's flag is in the team's world on the server (set some
    # 20 s after the old host left, reported within TeamWorldReportMs): a game takes the team's
    # world as it loads, so a load before that report has the flag only from its next load (rig
    # 2026-10-27: the old host joined 10 s before it, both runs).
    "host-quits-title": {
        "about": "the host quits to the title; the guest leads the team now and hosts where it "
                 "stands (it runs the enemies, its world is the team's), and the old host comes "
                 "back as its guest from the title",
        "roles": {
            "host": ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                     "godmode", "wait-session 2 300", "wait 10", "sync hqt-in", "quit-title",
                     "title-wait 45", "team-accept", "team-ready", "godmode", "wait-session 2 420",
                     "wait-members 2 120", "wait 20", "snapshot", "expect-session 2",
                     f"expect-map {CENTRAL_YHARNAM}", f"expect-flag {TEAM_WORLD_FLAG} 1",
                     f"expect-hp {ENEMY} < 484", "sync hqt-back"],
            "guest": ["team-accept", "team-ready", START, "godmode", "wait-session 2 300",
                      "sync hqt-in", "wait-solo 120", "wait 15", "snapshot",
                      f"expect-map {CENTRAL_YHARNAM}", f"set-flag {TEAM_WORLD_FLAG} 1",
                      f"hit {ENEMY} 30", "wait 5", f"expect-hp {ENEMY} < 484",
                      "wait-session 2 420", "wait-members 2 120", "sync hqt-back",
                      "expect-session 2", f"expect-flag {TEAM_WORLD_FLAG} 1"],
        },
        "syncs": {name: {"chapter": "host-quits-title", "roles": ["host", "guest"]}
                  for name in ("hqt-in", "hqt-back")},
        "expect": {"host": [r"Bloodborne autoplay: back at the title", PLAYS_AS_GUEST,
                            r"Co-op team world: the team's world is in this save"],
                   "guest": [LEADS_NOW, r"Co-op team world: this host's world to the server",
                             OWNER_RUNS]},
        "allow": CRASH_CLEANUP,
        "timeout": 600,
    },
    # The same with three: the host quits to the title; the two guests stay together in their
    # session, the one whose game took the room hosting it and leading the team; the old host
    # comes back as the third. The old host stays at the title until the guests' check that the two
    # are together is over (rig 2026-10-28: back in 25 s, it was the third at the check).
    "host-quits-title3": {
        "about": "a team of three: the host quits to the title; the guests stay together, one "
                 "of them hosting and leading, and the old host comes back as a guest: three "
                 "again",
        "roles": {
            "host": ["team-create", "team-invite rig_guest", "team-invite rig_guest2",
                     "team-wait 3", "team-start", START, "godmode", "wait-session 3 480",
                     "wait 10", "sync hqt3-in", "quit-title", "title-wait 45", "team-accept",
                     "team-ready",
                     "godmode", "wait-session 3 480", "wait 20", "snapshot", "expect-session 3",
                     "sync hqt3-back"],
            **{guest: ["team-accept", "team-ready", START, "godmode", "wait-session 3 480",
                       "sync hqt3-in", "wait 30", "snapshot", "expect-session 2",
                       "wait-session 3 480", "wait 10", "sync hqt3-back", "expect-session 3"]
               for guest in ("guest", "guest2")},
        },
        "syncs": {name: {"chapter": "host-quits-title3", "roles": ["host", "guest", "guest2"]}
                  for name in ("hqt3-in", "hqt3-back")},
        "expect": {"host": [r"Bloodborne autoplay: back at the title", PLAYS_AS_GUEST]},
        "expect_any": [r"Co-op team join: the session host moved to this game"],
        "allow": CRASH_CLEANUP,
        "timeout": 900,
    },
    "team-menu-join": {
        "about": "in game, the host invites a guest playing alone; the guest joins the hunt from "
                 "the menu where it stands",
        "roles": {
            "host": ["team-create", "team-start", START, "godmode", "wait 15",
                     "menu-invite rig_guest 240", "wait-session 2 300", "wait 10", "snapshot",
                     "expect-session 2"],
            "guest": ["team-alone", START, "godmode", "wait 10", "menu-join 240",
                      "wait-session 2 300", "wait 10", "snapshot", "expect-session 2",
                      f"expect-map {CENTRAL_YHARNAM}"],
        },
        "expect": {
            "guest": [r"Co-op team menu: this save joins team \w+ where it stands, as its guest"],
        },
        "timeout": 300,
    },
    "join3": {
        "about": "a party of three: both guests join the host (one bell summons both)",
        "roles": {
            "host": [START, "godmode", "wait-session 3 480", "wait 10", "snapshot",
                     "expect-session 3"],
            "guest": [START, "godmode", "wait-session 3 480", "wait 10", "snapshot",
                      "expect-session 3"],
            "guest2": [START, "godmode", "wait-session 3 480", "wait 10", "snapshot",
                       "expect-session 3"],
        },
        "timeout": 300,
    },
    # Kaniel 2026-10-15: between two guests, one guest's actions played on the other guest's own
    # character (a pick-up's gesture, a backstab's teleport, levers). The second guest stands
    # still; its census shows where its own character is before and after the first one acts.
    "guest-acts-for-two": {
        "about": "a party of three: the first guest picks up an item and attacks an enemy while "
                 "the second guest stands still; the second guest's own character stays put",
        "roles": {
            "host": [START, "godmode", "wait-session 3 480", "wait 70", "expect-session 3"],
            "guest": [START, "godmode", "wait-session 3 480", "wait 15", "pickup 60", "wait 5",
                      *attack(ENEMY), "wait 10", "expect-session 3"],
            "guest2": [START, "godmode", "wait-session 3 480", "wait 10", "census 5", "wait 20",
                       "census 5", "wait 25", "census 5", "expect-session 3"],
        },
        "stays": {"guest2": 3.0},
        "timeout": 900,
    },
    # Kaniel 2026-10-15: the host and a guest went to the Hunter's Dream and the second guest still
    # saw them in Central Yharnam. It sees neither while they are away, both once they are back.
    "away-unseen3": {
        "about": "a party of three: the host and the first guest go to the Hunter's Dream; the "
                 "second guest shows neither of them meanwhile, and both again once they are back",
        "roles": {
            "host": [START, "godmode", "wait-session 3 480", "wait 20", f"travel {HUNTERS_DREAM}",
                     "wait 60", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 3 240", "wait 30",
                     "expect-session 3"],
            "guest": [START, "godmode", "wait-session 3 480", "wait 25",
                      f"travel {HUNTERS_DREAM}", "wait 55", f"travel {CENTRAL_YHARNAM_LAMP}",
                      "wait-members 3 240", "wait 30", "expect-session 3"],
            # Both away from about 40 s on, back after about 85 s.
            "guest2": [START, "godmode", "wait-session 3 480", "wait 15", "expect-others-shown 2",
                       "wait 40", "expect-others-shown 0", "wait-members 3 240", "wait 20",
                       "expect-others-shown 2", "expect-session 3"],
        },
        "timeout": 1000,
    },
    "join3-uncap": {
        "about": "join3 at the kit's frame rate (Uncap FPS++): rig 2026-10-12, the host saw the "
                 "second guest's sign and never claimed it (3 runs of 3; at 30 FPS it joined)",
        "roles": {
            "host": [START, "godmode", "wait-session 3 480", "wait 10", "snapshot",
                     "expect-session 3"],
            "guest": [START, "godmode", "wait-session 3 480", "wait 10", "snapshot",
                      "expect-session 3"],
            "guest2": [START, "godmode", "wait-session 3 480", "wait 10", "snapshot",
                       "expect-session 3"],
        },
        "settings": {"BB_FPS": "uncap"},
        "timeout": 900,
    },
    "trip3": {
        "about": "a party of three apart (everyone sees both others at the end): the second guest waits in the Dream while the host "
                 "goes there and back; the host re-creates only the guest on its map, then the "
                 "second guest when it comes back",
        "roles": {
            "host": [START, "godmode", "wait-session 3 480", "wait 40", f"travel {HUNTERS_DREAM}",
                     "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}", "wait 60", "wait-members 3 300",
                     "wait 20", "snapshot", "expect-session 3", "expect-others-near 2 15"],
            "guest": [START, "godmode", "wait-session 3 480", "wait 150", "wait-members 3 300",
                      "wait 25", "snapshot", "expect-session 3", "expect-others-near 2 15"],
            "guest2": [START, "godmode", "wait-session 3 480", "wait 10",
                       f"travel {HUNTERS_DREAM}", "wait 90", f"travel {CENTRAL_YHARNAM_LAMP}",
                       "wait-members 3 300", "wait 25", "snapshot", "expect-session 3",
                       "expect-others-near 2 15"],
        },
        "expect": {
            "host": [r"re-creating rig_guest on map 0x18010000 \(1 of 2 members\)",
                     # Since PR #41 both may be named in one line ("rig_guest rig_guest2").
                     r"re-creating (?:\S+ )*rig_guest2 (?:\S+ )*on map 0x18010000"],
        },
        "timeout": 1200,
    },
    "guest-trip3": {
        "about": "a party of three: one guest goes to the Dream and back; afterwards both guests "
                 "and the host see each other (Kaniel 2026-10-09: guests did not see each other "
                 "after travelling)",
        "roles": {
            "host": [START, "godmode", "wait-session 3 480", "wait 150", "wait-members 3 300",
                     "wait 25", "snapshot", "expect-session 3", "expect-others-near 2 15"],
            "guest": [START, "godmode", "wait-session 3 480", "wait 20",
                      f"travel {HUNTERS_DREAM}", "wait 20", f"travel {CENTRAL_YHARNAM_LAMP}",
                      "wait-members 3 300", "wait 25", "snapshot", "expect-session 3",
                      "expect-others-near 2 15"],
            "guest2": [START, "godmode", "wait-session 3 480", "wait 150", "wait-members 3 300",
                       "wait 25", "snapshot", "expect-session 3", "expect-others-near 2 15"],
        },
        "timeout": 1200,
    },
    "guest-talk-at-lamp": {
        "about": "a guest in the session presses X at the first Central Yharnam lamp, where "
                 "Gilbert's window is (Kaniel 2026-10-09: the 'Talk' prompt took the guest to "
                 "the Dream through the lamp); the talk log shows which script answered",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 80", "snapshot",
                     "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 15", "snapshot",
                      "press cross 200", "wait 8", "press cross 200", "wait 8",
                      "press circle 200", "wait 8", "snapshot", f"expect-map {CENTRAL_YHARNAM}",
                      "expect-session 2"],
        },
        "timeout": 600,
    },
    "guest-lamp-rest": {
        "about": "a guest in the session uses the Cathedral Ward lamp (Oedon Chapel) with X and rests "
                 "there with the party (playtest 2026-10-13: guests could not use lamps in the Ward "
                 "or after Father Gascoigne); the host first, as the control",
        "roles": {
            "host": [START, f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                     "wait-session 2 420", "wait 5", *USE_WARD_LAMP("host"), "wait 200", "snapshot",
                     "expect-session 2"],
            "guest": [START, f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                      "wait-session 2 420", "wait 160", *USE_WARD_LAMP("guest"), "snapshot",
                      "expect-session 2"],
        },
        # The lamp's script (m24_00 t240000, states 1-4) takes X only while ESD functions 14, 19,
        # 26, 27 and 31 answer 0; 26 answered 1 for a guest. The rig's X at the lamp brings no
        # prompt even for the host (2026-10-14, 18 aims from both sides), so the check is the
        # guest's answers: 26 answers 0 as the host's, never 1.
        "expect": {"guest": [r"guest talk answers: .*\bf26=0x", r"\bf14=0x.*\bf19=0x"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS + [r"\bf26=1x"]},
        "timeout": 1000,
    },
    "ward-door": {
        "about": "the guest walks out of Cathedral Ward to Central Yharnam and back three times "
                 "(playtest 2026-10-13: after going back and forth through that door the guests "
                 "could not use the Ward lamp or hurt the NPC by it), then uses the Ward lamp",
        "roles": {
            "host": [START, f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                     "wait-session 2 420", "wait 420", "census 60", "snapshot", "expect-session 2"],
            "guest": [START, f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                      "wait-session 2 420", "wait 15",
                      *[step for _ in range(3) for step in (
                          f"glide {WARD_TO_YHARNAM}", "wait 12", f"glide {YHARNAM_TO_WARD}",
                          "wait 12")],
                      f"expect-map {CATHEDRAL_WARD}", *USE_WARD_LAMP("guest"), "census 60",
                      "snapshot", "expect-session 2"],
        },
        "expect": {"guest": [r"map change 0x18000000 -> 0x18010000",
                             r"guest talk answers: .*\bf26=0x"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS + [r"\bf26=1x"]},
        "timeout": 1100,
    },
    "pickup": {
        "about": "host and guest each pick up a map item with X in a session (the host could not "
                 "pick up items in the Forbidden Woods, 2026-10-09)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10", "pickup 60", "wait 5",
                     "snapshot", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 30", "pickup 60", "wait 5",
                      "snapshot", "expect-session 2"],
        },
        "timeout": 300,
    },
    "pickup-after-crash": {
        "about": "after the guest's game crashes and it joins again, both still pick up map items "
                 "with X (friend run 2026-10-09: no pickups after a crash); the item by the lamp: "
                 "the next one is 21 m down, and a jump there drops the player through the floor "
                 "and crashes the game in 0x0191B6E0 (twice with the guest back, twice without)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-solo 120",
                     "wait-session 2 420", "wait 15", "pickup 20", "wait 5", "snapshot",
                     "expect-session 2"],
            "guest": [[START, "godmode", "wait-session 2 300", "wait 15", "done"],
                      [START, "godmode", "wait-session 2 420", "wait 40", "pickup 20", "wait 5",
                       "snapshot", "expect-session 2"]],
        },
        "allow": CRASH_CLEANUP,
        "timeout": 900,
    },
    "pickup-after-leave": {
        "about": "the guest's game goes away and the host, alone now, picks up a map item with X "
                 "before anyone joins again (the leave's 'load wait' stayed on: Kaniel "
                 "2026-10-11, no pickups or talks after a friend left)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-solo 120", "wait 10",
                     "pickup 20", "wait 3", "snapshot"],
            "guest": [[START, "godmode", "wait-session 2 300", "wait 15", "done"],
                      ["wait 1", "done"]],
        },
        "expect": {"host": [r"lost a member; staying where it is"]},
        "allow": CRASH_CLEANUP,
        "timeout": 300,
    },
    "pickup-after-leave3": {
        "about": "a party of three: one guest's game goes away; the host and the other guest "
                 "still pick up map items with X (Kaniel 2026-10-11: after a friend left neither "
                 "could pick up or talk, only fight)",
        "roles": {
            "host": [START, "godmode", "wait-session 3 480", "wait 20", "wait-session 2 120",
                     "wait 10", "pickup 20", "wait 3", "snapshot"],
            "guest": [START, "godmode", "wait-session 3 480", "wait 20", "wait-session 2 120",
                      "wait 10", "pickup 20", "wait 3", "snapshot"],
            "guest2": [[START, "godmode", "wait-session 3 480", "wait 10", "done"],
                       ["wait 1", "done"]],
        },
        "expect": {role: [r"lost a member; staying where it is"] for role in ("host", "guest")},
        "allow": CRASH_CLEANUP,
        "timeout": 300,
    },
    "guest-talk-by-the-lamp": {
        "about": "a guest in the session presses X on each side of the character beside the first "
                 "Central Yharnam lamp (Gilbert's window), turned towards it; it must not take the "
                 "lamp to the Dream (Kaniel 2026-10-09: 'Talk' there used the lamp)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 140", "snapshot",
                     "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10", "snapshot",
                      f"approach {BY_THE_LAMP} 0 1.2 0", "wait 3", "snapshot", "press cross 200", "wait 6", "press circle 200", "wait 4", "snapshot",
                      f"expect-map {CENTRAL_YHARNAM}",
                      f"approach {BY_THE_LAMP} 0 -1.2 3.14", "wait 3", "snapshot", "press cross 200", "wait 6", "press circle 200", "wait 4", "snapshot",
                      f"expect-map {CENTRAL_YHARNAM}",
                      f"approach {BY_THE_LAMP} 1.2 0 1.57", "wait 3", "snapshot", "press cross 200", "wait 6", "press circle 200", "wait 4", "snapshot",
                      f"expect-map {CENTRAL_YHARNAM}",
                      f"approach {BY_THE_LAMP} 1.2 0 -1.57", "wait 3", "snapshot", "press cross 200", "wait 6", "press circle 200", "wait 4", "snapshot",
                      f"expect-map {CENTRAL_YHARNAM}",
                      f"approach {BY_THE_LAMP} -1.2 0 -1.57", "wait 3", "snapshot", "press cross 200", "wait 6", "press circle 200", "wait 4", "snapshot",
                      f"expect-map {CENTRAL_YHARNAM}",
                      f"approach {BY_THE_LAMP} -1.2 0 1.57", "wait 3", "snapshot", "press cross 200", "wait 6", "press circle 200", "wait 4", "snapshot",
                      f"expect-map {CENTRAL_YHARNAM}",
                      "expect-session 2"],
        },
        "timeout": 700,
    },
    "pickup-after-trip": {
        "about": "after a trip that keeps the session (the host to the Dream and back, the guest "
                 "to the Dream and back) both still pick up the item by the lamp with X (Kaniel "
                 "2026-10-09: no pickups in the Forbidden Woods after the time-of-day reload)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}",
                     "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 120", "wait 15",
                     "pickup 20", "wait 5", "snapshot", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 90", f"travel {HUNTERS_DREAM}",
                      "wait 10", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                      "wait 15", "pickup 20", "wait 5", "snapshot", "expect-session 2"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 720,
    },
    "pickup-after-reload": {
        "about": "after each one's map reloads in place in the session (a warp to the lamp it "
                 "stands at, as the time-of-day reload Kaniel had before the Forbidden Woods "
                 "pickups stopped, 2026-10-09) both still pick up the item by the lamp with X",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10",
                     f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 120", "wait 15",
                     "pickup 20", "wait 5", "snapshot", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 80",
                      f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240", "wait 15",
                      "pickup 20", "wait 5", "snapshot", "expect-session 2"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 720,
    },
    "guest-talk-gilbert": {
        "about": "at the second Central Yharnam lamp (talk 241001) a guest presses X at the window "
                 "3 m from it (talk 241319, Gilbert's), turned towards it; it must not take the lamp "
                 "to the Dream (Kaniel 2026-10-09: 'Talk' there used the lamp)",
        "roles": {
            # The host stays: its warp to another lamp of its map made the guest's game drop the
            # room for a moment (rig 2026-10-10, as pickup-after-reload's in-place warp).
            "host": [START, "godmode", "wait-session 2 300", "wait 230", "snapshot",
                     "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 20",
                      f"travel {CENTRAL_YHARNAM_LAMP_2}", "wait 15",
                      "snapshot",
                      "teleport -198.45 -28.4 63.96 2.29", "wait 2", "teleport -198.45 -28.4 63.96 2.29", "wait 3", "snapshot",
                      "press cross 200", "wait 6", "snapshot", "press circle 200", "wait 4",
                      f"expect-map {CENTRAL_YHARNAM}",
                      "teleport -198.45 -28.4 63.96 2.29", "wait 2", "teleport -198.45 -28.4 63.96 2.29", "wait 3", "snapshot",
                      "press cross 200", "wait 6", "snapshot", "press circle 200", "wait 4",
                      f"expect-map {CENTRAL_YHARNAM}",
                      "teleport -197.6 -28.4 64.7 2.29", "wait 2", "teleport -197.6 -28.4 64.7 2.29", "wait 3", "snapshot",
                      "press cross 200", "wait 6", "snapshot", "press circle 200", "wait 4",
                      f"expect-map {CENTRAL_YHARNAM}",
                      "teleport -198.45 -28.4 63.96 2.6", "wait 2", "teleport -198.45 -28.4 63.96 2.6", "wait 3", "snapshot",
                      "press cross 200", "wait 6", "snapshot", "press circle 200", "wait 4",
                      f"expect-map {CENTRAL_YHARNAM}",
                      "teleport -198.45 -28.4 63.96 2.0", "wait 2", "teleport -198.45 -28.4 63.96 2.0", "wait 3", "snapshot",
                      "press cross 200", "wait 6", "snapshot", "press circle 200", "wait 4",
                      f"expect-map {CENTRAL_YHARNAM}",
                      "expect-session 2"],
        },
        "timeout": 900,
    },
    "hp-after-host-trip": {
        "about": "after the host's trip to the Dream and back, enemy HP still goes both ways: the "
                 "host's hit reaches the guest's copy and the guest's real hit the host's (Kaniel "
                 "2026-10-10: HP not shared after trips, he thinks)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}",
                     "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 120", "wait 20",
                     f"hurt {ENEMY} 300", "wait 40", "snapshot", f"expect-hp {ENEMY} < 300",
                     "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 45", "wait-members 2 240",
                      "wait 30", "snapshot", f"expect-hp {ENEMY} <= 300", f"hit {ENEMY}",
                      "wait 10", "snapshot", "expect-session 2"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 720,
    },
    "hp-after-guest-trip": {
        "about": "after the guest's trip to the Dream and back, enemy HP still goes both ways: the "
                 "host's hit reaches the guest's copy and the guest's real hit the host's",
        "roles": {
            # The host hits once the guest is back (its wait-members), the guest looks 14 s
            # later and then hits (the t= on step lines puts both logs on one clock).
            "host": [START, "godmode", "wait-session 2 300", "wait 60", "wait-members 2 240",
                     "wait 20", f"hurt {ENEMY} 300", "wait 60", "snapshot",
                     f"expect-hp {ENEMY} < 300", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}",
                      "wait 10", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                      "wait 60", "snapshot", f"expect-hp {ENEMY} <= 300", f"hit {ENEMY}",
                      "wait 10", "snapshot", "expect-session 2"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 720,
    },
    "dead-before-session": {
        "about": "an enemy the host killed before the guest joined stays dead for the guest after "
                 "its trip to the Dream and back, in its own world (Kaniel 2026-10-11: the "
                 "guests saw enemies dead in the host's world alive again)",
        "roles": {
            "host": [START, "godmode", f"kill {ENEMY}", "wait-session 2 300", "wait 60",
                     "wait-members 2 240", "wait 40", "snapshot", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}",
                      "wait 10", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                      "wait 10", f"expect-hidden {ENEMY} 1 30", "snapshot", "expect-session 2"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 720,
    },
    "guest-hit-once": {
        "about": "after both players' trips each runs its own copy of the map; the guest's real "
                 "hits reach the host's copy through the game and count once: the host does not "
                 "post them back, and both end at max HP less the guest's hits (Kaniel's friend "
                 "2026-10-11: each hit repeated a split second later)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}",
                     "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 120", "wait 120",
                     "census 40", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 45", "wait-members 2 240",
                      "wait 5", f"travel {HUNTERS_DREAM}", "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}",
                      "wait-members 2 240", "wait 20", f"hit {ENEMY} 30", "wait 1",
                      f"hit {ENEMY} 30", "wait 20", "census 40", "expect-session 2"],
        },
        "same_hp": ENEMY,
        "hp_sums_hits": ENEMY,
        "expect": {"host": [HP_SHARED],
                   "guest": [r"hit on 0x1001400b by this player",
                             HP_SHARED]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 720,
    },
    "hit-after-host-trip": {
        "about": "only the host went to the Dream and back: the guest's hits must still reach "
                 "the host's copy (rig 2026-10-11: the guest stayed in 'one world' mode, its "
                 "copy went back to the host's full HP and its hits were lost)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}",
                     "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 120", "wait 90",
                     "census 40", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 45", "wait-members 2 240",
                      "wait 30", f"hit {ENEMY} 30", "wait 1", f"hit {ENEMY} 30", "wait 20",
                      "census 40", "expect-session 2"],
        },
        "same_hp": ENEMY,
        "hp_sums_hits": ENEMY,
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 720,
    },
    "hit3": {
        "about": "a party of three, each back from its own trip (worlds apart): all three hit "
                 "one enemy in turn, and every game ends at its max HP less the three hits, each "
                 "counted once (Kaniel 2026-10-11)",
        "roles": {
            "host": [START, "godmode", "wait-session 3 480", "wait 10", f"travel {HUNTERS_DREAM}",
                     "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 3 300", "wait 100",
                     f"hit {ENEMY} 30", "wait 70", "census 40", "expect-session 3"],
            "guest": [START, "godmode", "wait-session 3 480", "wait 45", "wait-members 3 300",
                      "wait 5", f"travel {HUNTERS_DREAM}", "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}",
                      "wait-members 3 300", "wait 60", f"hit {ENEMY} 30", "wait 55",
                      "census 40", "expect-session 3"],
            "guest2": [START, "godmode", "wait-session 3 480", "wait 45", "wait-members 3 300",
                       "wait 25", f"travel {HUNTERS_DREAM}", "wait 5",
                       f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 3 300", "wait 75",
                       f"hit {ENEMY} 30", "wait 45", "census 40", "expect-session 3"],
        },
        "same_hp": ENEMY,
        "hp_sums_hits": ENEMY,
        # From a hit to the other games' bars, on average (0.47 s on a 200 ms board round, 0.30 s
        # posting on the hit with 100 ms rounds, then straight from game to game over the P2P
        # link; forest run 2026-10-11: the host's two-hit kills ended before the guests' bars
        # moved).
        "max_hp_lag": 0.15,
        "expect": {"host": [HP_SHARED]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS, "guest2": SESSION_HOLDS},
        "timeout": 1200,
    },
    "crash-rejoin-hp": {
        "about": "after the guest's game crashes and it joins again, the host's hit still reaches "
                 "the guest's copy of an enemy "
                 "(friend run 2026-10-09: HP bars apart after a crash)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-solo 120",
                     "wait-session 2 420", "wait 20", f"hurt {ENEMY} 300", "wait 40", "snapshot",
                     "expect-session 2"],
            "guest": [[START, "godmode", "wait-session 2 300", "wait 15", "done"],
                      [START, "godmode", "wait-session 2 420", "wait 35", "snapshot",
                       f"expect-hp {ENEMY} <= 300", "expect-session 2"]],
        },
        # In a session the host's game owns its enemies' HP and sends it to the guest itself (a
        # guest writing its copy's HP is put back within seconds), so only the host's hit is
        # checked here; the boards cover members apart (hp-apart).
        "allow": CRASH_CLEANUP,
        "timeout": 900,
    },
    "owner-leaves": {
        "about": "phase 4: the host runs Central Yharnam's enemies and hurts one; it goes to the "
                 "Dream and the guest takes the area over with the enemy as hurt as it was (no "
                 "reset, no full HP), kills it, and the host coming back finds it dead",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 20", f"hurt {ENEMY} 200",
                     "wait 10", "sync hurt", f"travel {HUNTERS_DREAM}", "sync killed", "wait 5",
                     f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240", "wait 20",
                     "snapshot"],
            "guest": [START, "godmode", "wait-session 2 300", "sync hurt", "wait 5",
                      f"expect-hp {ENEMY} <= 200", "wait 20", f"expect-hp {ENEMY} <= 200",
                      *attack(ENEMY), "wait 5", "sync killed", "wait-members 2 240", "wait 20",
                      "snapshot"],
        },
        "syncs": {"hurt": {"chapter": "owner-leaves", "roles": ["host", "guest"]},
                  "killed": {"chapter": "owner-leaves", "roles": ["host", "guest"]}},
        "expect": {"host": [OWNER_RUNS,
                            r"(?:hid 1 enemies a member killed on map 0x18010000|"
                            r"0x18010000, 1 near died)"],
                   "guest": [OWNER_FOLLOWS, OWNER_RUNS]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "owner-crash": {
        "about": "phase 4: the guest runs Central Yharnam's enemies (the host went to the Dream and "
                 "came back to follow it); the guest's game crashes and the host takes the area "
                 "over: its own hit lands",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}",
                     "wait 10", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                     "wait 20", "sync back", "wait 30", f"hit {ENEMY} 30", "wait 5",
                     f"expect-hp {ENEMY} < 484", "snapshot"],
            "guest": [[START, "godmode", "wait-session 2 300", "wait 20", "wait-members 2 240",
                       "wait 20", "sync back", "wait 2", "done"],
                      [START, "wait 20", "snapshot"]],
        },
        "syncs": {"back": {"chapter": "owner-crash", "roles": ["host", "guest"]}},
        "expect": {"host": [OWNER_FOLLOWS, OWNER_RUNS], "guest": [OWNER_RUNS]},
        "allow": CRASH_CLEANUP,
        "timeout": 900,
    },
    "border-fight": {
        "about": "phase 4: the guest joins in place in Cathedral Ward while the host is in Central "
                 "Yharnam, two blocks of one area: each runs its own block's enemies, then the "
                 "guest crosses to the host's block and follows it there; both hits on one enemy "
                 "count once in both games",
        "roles": {
            "host": [START, "godmode", "wait-session 2 420", "wait 60", f"hit {ENEMY} 30",
                     "wait 40", "census 40", "expect-session 2"],
            "guest": [START, f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                      "wait-session 2 420", "wait 20", f"travel {CENTRAL_YHARNAM_LAMP}",
                      "wait-members 2 240", "wait 10", f"hit {ENEMY} 30", "wait 30",
                      "census 40", "expect-session 2"],
        },
        "same_hp": ENEMY,
        "hp_sums_hits": ENEMY,
        "expect": {"host": [OWNER_RUNS],
                   "guest": [r"enemy owner: this game runs map 0x18000000's enemies",
                             OWNER_FOLLOWS]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 900,
    },
    "hp-across-blocks": {
        "about": "the guest joins in place in Cathedral Ward while the host is in Central "
                 "Yharnam (two blocks of one area); HP reports still count between them",
        "roles": {
            "host": [START, "godmode", "wait-session 2 420", "wait 30", f"hurt {ENEMY} 200",
                     "wait 30", "snapshot", "expect-session 2"],
            "guest": [START, f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                      "wait-session 2 420", "wait 20", f"travel {CENTRAL_YHARNAM_LAMP}",
                      "wait-members 2 240", "wait 20", "snapshot", f"expect-hp {ENEMY} <= 200",
                      "expect-session 2"],
        },
        "expect": {"guest": [HP_LOWERED]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "guest-death-seen": {
        "about": "the guest dies (goes down and gives up) away from the lamp in the host's world "
                 "and gets up at the lamp; the host sees it there alive (friend run 2026-10-09: "
                 "invisible after respawning)",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-member-vital down 120",
                     "wait 60", "wait-members 2 120", "wait 10", "snapshot",
                     "expect-others-near 1 12", "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 10", f"teleport {AWAY_FROM_LAMP}",
                      "wait 5", *GIVE_UP, "wait 50", "wait-members 2 120", "snapshot",
                      "expect-others-near 1 12", "expect-session 2"],
        },
        "expect": {"guest": [r"party guest died; revived"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 600,
    },
    "echoes-after-crash": {
        "about": "after the guest's crash and rejoin, a kill it makes alone in Cathedral Ward still "
                 "sends the host its echoes and reaches the kill board",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-solo 120",
                     "wait-session 2 420", "wait 90", "snapshot", "expect-session 2"],
            "guest": [[START, "godmode", "wait-session 2 300", "wait 15", "done"],
                      [START, "godmode", "wait-session 2 420", "wait 10",
                       f"travel {CATHEDRAL_WARD_LAMP}", "wait 10", *attack(WARD_ENEMY),
                       "wait 15", "snapshot", "expect-session 2"]],
        },
        "expect": {
            "guest": [r"kill board gets 1 kills"],
            "host": [r"added \d+ echoes from a kill another member made apart"],
        },
        "allow": CRASH_CLEANUP,
        "timeout": 1080,
    },
    "lamp-after-crash": {
        "about": "a lamp the guest lights in the session stays lit in both worlds after the "
                 "guest's game crashes and comes back",
        "roles": {
            "host": [START, f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 0", "wait-session 2 300",
                     f"wait-flag {FORBIDDEN_WOODS_LAMP_LIT} 1 90", "wait-solo 120",
                     "wait-session 2 420", "wait 20", "snapshot",
                     f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", "expect-session 2"],
            "guest": [[START, "wait-session 2 300", "wait 30",
                       f"set-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", "wait 20", "done"],
                      [START, "wait-session 2 420", "wait 20", "snapshot",
                       f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", "expect-session 2"]],
        },
        "allow": CRASH_CLEANUP,
        "timeout": 1080,
    },
    "crash3": {
        "about": "a party of three: one guest's game crashes; host and the other guest stay "
                 "together, and the party is three again once it is back",
        "roles": {
            "host": [START, "godmode", "wait-session 3 480", "wait 30", "wait-session 2 120",
                     "wait-session 3 480", "wait 20", "snapshot", "expect-session 3"],
            "guest": [START, "godmode", "wait-session 3 480", "wait 30", "wait-session 2 120",
                      "snapshot", "expect-session 2", "wait-session 3 480", "wait 20",
                      "snapshot", "expect-session 3"],
            "guest2": [[START, "godmode", "wait-session 3 480", "wait 20", "done"],
                       [START, "godmode", "wait-session 3 480", "wait 20", "snapshot",
                        "expect-session 3"]],
        },
        "allow": CRASH_CLEANUP,
        "timeout": 1500,
    },
    "host-death-boss": {
        "about": "the host dies (goes down and gives up) in the middle of the Cleric Beast fight; "
                 "it gets up without a reload, the session holds and the boss is still there",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "wait-session 2 300", "wait 15", f"hurt {CLERIC_BEAST} 1500",
                     "wait 5", *GIVE_UP, "wait 40", "snapshot", f"expect-hp {CLERIC_BEAST} > 0",
                     f"expect-flag {CLERIC_BEAST_DEFEATED} 0", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait-member-vital down 120",
                      "wait 60", "snapshot", "expect-session 2"],
        },
        "expect": {"host": [r"party death: giving up ends down \d+; dies \(reset=false",
                            r"live player died in a party session"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 600,
    },
    "down-revive": {
        "about": "party death: the guest falls with 1 HP and goes down instead of dying; the host "
                 "comes over and strikes it until its weapon has dealt the guest's max HP, and it gets up with half its HP",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-member-vital down 120",
                     "shot sees-down", "approach-downed", "wait 2", "shot revive-prompt",
                     *REVIVE_HITS, "wait-member-vital up 30", "wait 3", "shot after-revive",
                     "wait 2", "snapshot", "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 15", *LETHAL_FALL, "wait-vital down 30",
                      "expect-hp self == 1", "wait 2", "shot down", "wait-vital up 90",
                      "wait 3", "shot after-revive", "snapshot", "expect-hp self > 1",
                      "expect-session 2"],
        },
        "expect": {
            # The prompt names the guest's character (website shots 2026-10-19: its online id).
            "host": [r"party death: ((real )?revive hit on|revive swing at)",
                     r'party HUD: prompt "Strike to revive (?!rig_)[^"]+"'],
            "guest": [r"party death: down \d+ on map 0x18010000",
                      r"party death: revived from down"],
        },
        "forbid": {"host": SESSION_HOLDS + [r'party HUD: prompt "Strike to revive rig_'],
                   "guest": SESSION_HOLDS + [r"party guest died"]},
        "timeout": 300,
    },
    "down-wait": {
        "about": "party death: the guest goes down (no kneel, Kaniel 2026-10-15) while nobody "
                 "strikes it for 12 s "
                 "(playtest 2026-10-13: the others saw Lisara kneel and no strike landed, so he had "
                 "to give up); then the host strikes it and it gets up",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-member-vital down 120",
                     "wait 12", "shot sees-down", "approach-downed", "wait 2", *REVIVE_HITS,
                     "wait-member-vital up 30", "wait 3", "shot after-revive", "wait 2", "snapshot",
                     "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 15", *LETHAL_FALL, "wait-vital down 30",
                      "wait 2", "shot down", "wait-vital up 120", "wait 3", "snapshot",
                      "expect-hp self > 1", "expect-session 2"],
        },
        "expect": {
            "host": [r"party death: (real revive hit on|revive swing at)"],
            "guest": [r"party death: down \d+ on map 0x18010000",
                      r"party death: revived from down"],
        },
        "forbid": {"host": SESSION_HOLDS + [KNEEL],
                   "guest": SESSION_HOLDS + [r"party guest died", KNEEL]},
        "timeout": 300,
    },
    "portrait": {
        "about": "party portraits: each player opens its status page (its face is taken from it), "
                 "then they join; each party list shows the other's face with its icon as a badge (the "
                 "downed drop once the guest falls)",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, *OPEN_STATUS, "shot status", *CLOSE_MENU, "godmode",
                     "wait-session 2 300", "wait 15", "shot party", "wait-member-vital down 120",
                     "wait 2", "shot sees-down", "snapshot", "expect-session 2"],
            "guest": [START, *OPEN_STATUS, "shot status", *CLOSE_MENU, "wait-session 2 300",
                      "wait 15", "shot party", *LETHAL_FALL, "wait-vital down 30", "wait 2",
                      "shot down", "snapshot", "expect-session 2"],
        },
        "expect": {
            "host": [r"party portraits: took the character's portrait from the status page",
                     r"party portraits: 7269675f6775657374's portrait came",  # rig_guest
                     r"Party portraits: slot 0 texture 0x[0-9a-f]+ .* shows the member's picture"],
            "guest": [r"party portraits: took the character's portrait from the status page",
                      r"party portraits: 7269675f686f7374's portrait came",  # rig_host
                      r"Party portraits: slot 0 texture 0x[0-9a-f]+ .* shows the member's picture"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "portrait-auto": {
        "about": "each player's portrait without the status page (Kaniel 2026-10-13: 'ideally "
                 "yes'): no kept portraits, the players never open a menu; each game draws its "
                 "character's status model off-screen and the other's party list shows it",
        "no_portraits": True,
        "roles": {
            "host": [START, "wait-session 2 300", "wait 20", "shot party", "snapshot",
                     "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 20", "shot party", "snapshot",
                      "expect-session 2"],
        },
        "expect": {role: [r"party portraits: off-screen model drawn; reading it back",
                          r"party portraits: took the character's portrait",
                          r"Party portraits: slot 0 texture 0x[0-9a-f]+ .* shows the member's "
                          r"picture"]
                   for role in ("host", "guest")},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "portrait-dressed": {
        "about": "party portraits on a dressed character (Kaniel 2026-10-13: are clothes on the portrait?): "
                 "Kaniel's guest character, in the starting Black Hood and Foreign Garb, opens its "
                 "status page (Square shows its face and model) and its portrait is taken from the "
                 "model: head and shoulders, hood and collar",
        "saves": "kaniel-1011-guest",
        "roles": {"host": [START, "shot world-before", *OPEN_STATUS, "press square 200", "wait 3",
                           "shot status-model", *CLOSE_MENU, "wait 5", "shot world"]},
        "expect": {"host": [r"party portraits: took the character's portrait from the status page"]},
        "timeout": 300,
    },
    "give-up": {
        "about": "party death: the downed guest holds Circle and dies as a party member dies "
                 "today: it gets up at its lamp without a reload and the session holds",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-member-vital down 120",
                     "wait 60", "snapshot", "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 15", *LETHAL_FALL, "wait-vital down 30",
                      "wait 2", "press circle 3600", "wait-vital dead 15", "wait-vital up 90",
                      "wait 10", "snapshot", "expect-session 2"],
        },
        "expect": {
            "guest": [r"party death: giving up ends down \d+; dies \(reset=false",
                      r"party guest died; revived"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS + [r"reset=true"]},
        "timeout": 600,
    },
    "give-up-by-enemy": {
        "about": "party death: the guest goes down next to an enemy and gives up; it wakes at its "
                 "lamp standing, not downed (Kaniel 2026-10-25: the hook's catches on the dying "
                 "body waited for the first tick up and put it straight down at the lamp)",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-member-vital down 150",
                     "wait-member-vital dead 30", "wait-member-vital up 90", "wait 15",
                     "snapshot", "expect-session 2"],
            # Full HP: at 1 HP the owner-run enemy never strikes (suites.py, rig 2026-10-15).
            "guest": [START, "wait-session 2 300", "wait 15", f"approach {ENEMY}",
                      "wait-vital down 150", "wait 2", "press circle 3600", "wait-vital dead 15",
                      "wait-vital up 90", "wait 10", "shot woke", "snapshot",
                      "expect-hp self > 1", "expect-session 2"],
        },
        "expect": {
            "guest": [r"party death: down \d+ on map 0x18010000",
                      r"party death: giving up ends down \d+; dies \(reset=false",
                      r"party death: up again after a real death"],
        },
        "forbid": {"host": SESSION_HOLDS,
                   "guest": SESSION_HOLDS + [
                       r"party death: up again after a real death(?:\n.*){0,400}?\n"
                       r".*party death: down \d+"]},
        "timeout": 300,
    },
    "wipe": {
        "about": "party death: the guest goes down, then the host falls with nobody left "
                 "standing; both die (the guest by the server's wipe) and wake at their lamps "
                 "with the map reloaded, and the session holds",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "wait-session 2 300", "wait-member-vital down 120", "wait 3",
                     *LETHAL_FALL, "wait-vital dead 30", "wait-vital up 120",
                     "wait-members 2 240", "wait 15", "snapshot", "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 15", *LETHAL_FALL, "wait-vital down 30",
                      "wait-vital dead 60", "wait-vital up 120", "wait-members 2 240", "wait 15",
                      "snapshot", "expect-session 2"],
        },
        "expect": {
            "host": [r"party death: died for real on map 0x18010000 \(reset=true\)",
                     r"travelling there \(reset=true, void fall=false\)", BLOODSTAIN_DROPPED],
            "guest": [r"party death: wipe \d+ ends down \d+; dies \(reset=true",
                      r"travelling there \(reset=true, void fall=false\)", BLOODSTAIN_DROPPED],
        },
        "forbid": {"host": SESSION_HOLDS + [BLOODSTAIN_STOLEN],
                   "guest": SESSION_HOLDS + [BLOODSTAIN_STOLEN]},
        "timeout": 300,
    },
    "down-alone": {
        "about": "party death: with the host away in the Hunter's Dream nobody else stands on the "
                 "guest's map, so its lethal fall is a real death that reloads the map (a wipe "
                 "of one)",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 10", f"travel {HUNTERS_DREAM}",
                     "wait 120", f"travel {CENTRAL_YHARNAM_LAMP}", "wait-members 2 240",
                     "wait 10", "snapshot", "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 70", *LETHAL_FALL, "wait-vital dead 30",
                      "wait-vital up 120", "wait-members 2 300", "wait 10", "snapshot",
                      "expect-session 2"],
        },
        "expect": {
            "guest": [r"party death: died for real on map 0x18010000 \(reset=true\)",
                      r"travelling there \(reset=true, void fall=false\)"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS + [r"party death: down \d+"]},
        "timeout": 900,
    },
    "down-by-enemy": {
        "about": "party death: the guest with 1 HP stands next to an ordinary enemy until it "
                 "hits; it goes down, the enemy leaves it alone (still 1 HP after 20 s), and the "
                 "host revives it",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait-member-vital down 150",
                     "wait 22", "approach-downed", "wait 2", *REVIVE_HITS,
                     "wait 5", "snapshot", "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 15", "hurt self 1", f"approach {ENEMY}",
                      "wait-vital down 90", "wait 20", "expect-hp self == 1", "wait-vital up 90",
                      "wait 3", "snapshot", "expect-hp self > 1", "expect-session 2"],
        },
        "expect": {
            "host": [r"party death: ((real )?revive hit on|revive swing at)"],
            "guest": [r"party death: down \d+ on map 0x18010000",
                      r"party death: enemies forgot \d+ target\(s\) on downed players "
                      r"\(self down=true",
                      r"party death: revived from down"],
        },
        "forbid": {"host": SESSION_HOLDS,
                   "guest": SESSION_HOLDS + [r"party death: killed while down"]},
        "timeout": 300,
    },
    "down-revive3": {
        "about": "party death in a party of three: the guest goes down and the second guest, "
                 "not the host, revives it",
        "settings": PARTY_DEATH,
        "roles": {
            # The host only watches: with the downed body's swings counted the revive can be over
            # in seconds, before its board shows the down (rig 2026-10-14).
            "host": [START, "godmode", "wait-session 3 480", "wait 60", "snapshot",
                     "expect-session 3"],
            # Checked at once: a missed swing counts within 0.4 s now (Kaniel 2026-10-16), so the
            # second guest's first ones may already be in a few seconds later.
            "guest": [START, "wait-session 3 480", "wait 20", *LETHAL_FALL, "wait-vital down 30",
                      "expect-hp self == 1", "wait 2", "shot down", "wait-vital up 90",
                      "wait 3", "shot after-revive", "snapshot", "expect-hp self > 1",
                      "expect-session 3"],
            "guest2": [START, "godmode", "wait-session 3 480", "wait-member-vital down 180",
                       "shot sees-down", "approach-downed", "wait 2", "shot revive-prompt",
                       *REVIVE_HITS, "wait 5", "snapshot", "expect-session 3"],
        },
        "expect": {
            "guest": [r"party death: down \d+ on map 0x18010000",
                      r"party death: revived from down"],
            "guest2": [r"party death: ((real )?revive hit on|revive swing at)"],
        },
        "forbid": {"host": SESSION_HOLDS + [r"party death: ((real )?revive hit on|revive swing at)"],
                   "guest": SESSION_HOLDS, "guest2": SESSION_HOLDS},
        "timeout": 900,
    },
    "wipe3": {
        "about": "party death in a party of three: both guests go down, then the host falls with "
                 "nobody left standing; all three die and wake at their lamps with the map "
                 "reloaded, and the session holds",
        "settings": PARTY_DEATH,
        "roles": {
            "host": [START, "wait-session 3 480", "wait-member-vital down 180", "wait 25",
                     "shot sees-both-down", f"teleport {WIPE3_SPOT['host']}", "wait 2",
                     *LETHAL_FALL, "wait-vital dead 30", "wait-vital up 150",
                     "wait-members 3 300", "wait 15", "snapshot", "expect-session 3"],
            "guest": [START, "wait-session 3 480", "wait 20", f"teleport {WIPE3_SPOT['guest']}",
                      "wait 2", *LETHAL_FALL, "wait-vital down 30", "wait-vital dead 90",
                      "wait-vital up 150", "wait-members 3 300", "wait 15", "snapshot",
                      "expect-session 3"],
            "guest2": [START, "wait-session 3 480", "wait-member-vital down 180", "wait 5",
                       f"teleport {WIPE3_SPOT['guest2']}", "wait 2", *LETHAL_FALL,
                       "wait-vital down 30", "wait-vital dead 90",
                       "wait-vital up 150", "wait-members 3 300", "wait 15", "snapshot",
                       "expect-session 3"],
        },
        "expect": {
            "host": [r"party death: died for real on map 0x18010000 \(reset=true\)",
                     r"travelling there \(reset=true, void fall=false\)", BLOODSTAIN_DROPPED],
            "guest": [r"party death: wipe \d+ ends down \d+; dies \(reset=true",
                      r"travelling there \(reset=true, void fall=false\)", BLOODSTAIN_DROPPED],
            "guest2": [r"party death: down \d+ on map 0x18010000",
                       r"party death: wipe \d+ ends down \d+; dies \(reset=true",
                       r"travelling there \(reset=true, void fall=false\)", BLOODSTAIN_DROPPED],
        },
        "forbid": {"host": SESSION_HOLDS + [BLOODSTAIN_STOLEN],
                   "guest": SESSION_HOLDS + [BLOODSTAIN_STOLEN],
                   "guest2": SESSION_HOLDS + [BLOODSTAIN_STOLEN]},
        "timeout": 300,
    },
    # Rig refresh (2026-10-15): its own boot, a brand-new character's first load in a team (the
    # opening cutscene). 0.2.3: the cutscene ran under the team's load screen and could not be
    # skipped; the title's Cross skips it now and the load screen goes once the world is in.
    "new-char-cutscene": {
        "about": "both games on brand-new characters (rig/saves/new-character) load into their "
                 "team: the opening cutscene is skipped, the team's load screen goes away, they "
                 "are in one session",
        "saves": "new-character",
        "roles": {role: ["wait-session 2 240", "wait 25", "expect-session 2"]
                  for role in ("host", "guest")},
        "expect": {role: [r"load view is gone"] for role in ("host", "guest")},
        "forbid": {role: SESSION_HOLDS + [r"load view is up \(\d+ rows\)(?![\s\S]*load view "
                                          r"is gone)"] for role in ("host", "guest")},
        "timeout": 300,
    },
    # Kaniel 2026-10-16: the port's settings menu (Insert) in the style of the team screens and
    # the party list: the game's menu art and font. A picture of each page; an effect's row is
    # switched and back, and the menu saves the change when it closes; the menu in Russian (the
    # serif's letters behind the game's font); the quit box, cancelled.
    "button-icons": {
        "about": "Kaniel 2026-10-19: every co-op screen's buttons as the game's own key guide icons "
                 "(no 'L1' or 'Square' written out) and their text clear of the ornaments: "
                 "pictures of the map, the Insert menu's pages, the Co-op Team page, the downed "
                 "screen and the revive prompt",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 5", "keys map", "wait 12",
                     "shot map", "keys circle", "wait 2", "keys toggle", "wait 3", "shot menu-display",
                     "keys r1 r1 r1", "wait 2", "shot menu-controls", "keys toggle", "wait 2",
                     "wait-member-vital down 120", "approach-downed", "wait 2", "shot revive-prompt",
                     "revive-downed 150", "wait-member-vital up 30", "wait 3", "expect-session 2"],
            "guest": [START, "wait-session 2 300", "wait 25", "keys toggle up up up enter",
                      "wait 3", "shot team-menu", "keys enter", "wait 3",
                      "wait 27", "hurt self 1", "rise 18", "wait-vital down 30", "wait 2",
                      "shot down", "wait-vital up 120", "wait 3", "expect-session 2"],
        },
        "settings": {"SHADPS4_BLOODBORNE_PARTY_DEATH": "1"},
        "forbid": {"host": [r"sceNpMatching2LeaveRoom: called"],
                   "guest": [r"sceNpMatching2LeaveRoom: called"]},
        "timeout": 300,
    },
    "settings-menu": {
        "about": "the port's settings menu (Insert) drawn with the game's menu art and font: a "
                 "picture of every page, a row's value changed and back, saved on close, the "
                 "menu in Russian, the quit box cancelled",
        "roles": {
            "host": [START, "wait 5", "keys toggle", "wait 3", "shot menu-display",
                     *[step for name in ("upscaler", "effects", "controls", "advanced", "save")
                       for step in ("keys r1", "wait 2", f"shot menu-{name}")],
                     "keys r1 r1 r1 down", "wait 2", "shot menu-effect-row",
                     "keys right", "wait 2", "shot menu-effect-changed",
                     "keys left", "wait 2",
                     # The Display page's language row: fourteen down from its first (past the
                     # compass, bloodstain, team ping and map rows, the greyed FPS limit skipped).
                     # Auto (the game's language), English, Russian, ...: two right is Russian.
                     "keys l1 l1 down down down down down down down down down down down down down down",
                     "wait 2", "keys right right", "wait 2",
                     "shot menu-russian", "keys left left", "wait 2",
                     # Up from a page's first row is Quit (nothing waits for a restart): its box
                     # opens on Cancel, which Cross takes.
                     "keys l1 r1 up enter", "wait 2", "shot menu-quit", "keys enter", "wait 3",
                     "shot menu-closed"],
        },
        "expect": {"host": [r"menu keys right"]},
        "console_expect": {"host": [r"Overlay: the settings menu uses \d+ of \d+ pieces of the "
                                    r"game's menu art",
                                    r"Overlay: settings changed in the menu, saved"]},
        "timeout": 300,
    },
    "menu-languages": {
        "about": "the port's menus in the game's language (Simplified Chinese here, the game's "
                 "own font for it) and every other the game has, chosen in the Insert menu: "
                 "Japanese, German, Korean, Traditional Chinese, then back to the game's",
        "settings": {"BB_LANGUAGE": "11"},
        "roles": {
            # The Display page's language row is fourteen down from its first: Auto, English,
            # Russian, Japanese (3), French, Spanish, Spanish (Latin America), German (7),
            # Italian, Dutch, Portuguese, Korean (11), Traditional Chinese (12), ...
            "host": [START, "wait 5", "keys toggle", "wait 3", "shot menu-zh-cn",
                     "keys down down down down down down down down down down down down down down",
                     "wait 2",
                     "keys right right right", "wait 3", "shot menu-ja",
                     "keys right right right right", "wait 3", "shot menu-de",
                     "keys right right right right", "wait 3", "shot menu-ko",
                     "keys right", "wait 3", "shot menu-zh-tw",
                     "keys left left left left left left left left left left left left", "wait 3",
                     "shot menu-auto", "keys toggle", "wait 3", "shot menu-closed"],
        },
        "expect": {"host": [r"menu keys right"]},
        "console_expect": {"host": [r"Overlay: zh_CN text from the game's zhocn font",
                                    r"Overlay: ja text from the game's engus font",
                                    r"Overlay: ko text from the game's korkr font",
                                    r"Overlay: zh_TW text from the game's zhotw font"]},
        "timeout": 300,
    },
    # Kaniel 2026-10-17: the map screen (an Elden Ring style map). Cathedral Ward's lamp lit, the
    # map opens on Central Yharnam (its picture drawn from the game's map files the first time),
    # L1 goes to Cathedral Ward (the areas in order: the Dream, Cathedral Ward, Central Yharnam),
    # Cross asks to travel to its lamp and Cross again travels.
    "map-screen": {
        "about": "the map screen: the area's picture drawn from the game's map files with the "
                 "player's arrow and the lamps lit; L1 to another area; a trip from the map to its "
                 "lamp; pictures of each",
        "roles": {
            # The player's own area is drawn ahead while the screen is shut; another as it shows.
            "host": [START, "godmode", f"set-flag {CATHEDRAL_WARD_LAMP_LIT}", "wait 25", "keys map", "wait 10",
                     "shot map-central-yharnam", "keys start", "wait 2", "shot map-centred",
                     # Kaniel 2026-10-17 ("one big map"): Triangle, the whole waking world drawn so far.
                     "keys triangle", "wait 20", "shot map-world", "keys l1",
                     "wait 30", "shot map-cathedral-ward", "keys enter", "wait 2", "shot map-question",
                     "keys enter", f"wait-map {CATHEDRAL_WARD} 120", "wait 8", "keys map", "wait 5",
                     "shot map-arrived", "keys circle", "wait 2"],
        },
        "expect": {"host": [rf"Bloodborne map screen: travels to {CATHEDRAL_WARD_LAMP}",
                            rf"Bloodborne map screen: arrived at {CATHEDRAL_WARD_LAMP}"]},
        "console_expect": {"host": [r"Map: (drew|read) m24_01_00_00", r"Map: (drew|read) m24_00_00_00"]},
        "timeout": 300,
    },
    # Kaniel 2026-10-18 ("sometimes roofs, sometimes floors"): R3 shows the floors (under the
    # roofs, the floor one stands on, walls inked) and again the roofs.
    "map-floors": {
        "about": "the map screen's floors: R3 shows the floor under the roofs at the player's "
                 "height, R3 again the roofs; pictures of both",
        "roles": {
            "host": [START, "godmode", "wait 25", "keys map", "wait 12", "shot map-first", "keys r3", "wait 8",
                     "shot map-switched", "keys r3", "wait 8", "shot map-switched-back", "keys circle", "wait 2"],
        },
        "console_expect": {"host": [r"Map: shows the floors at -?\d+\.\d m", r"Map: shows the roofs"]},
        "timeout": 240,
    },
    # Kaniel 2026-10-19 ("every map should be just one picture", "3d map ... as an optional
    # feature"): every world on one picture (Triangle shows all of them), L3 the 3D view.
    "map-3d": {
        "about": "the map screen's one picture of every world and its 3D view: L3 turns the "
                 "landscape on, Triangle shows all of it, L3 again the flat map; pictures of each",
        "roles": {
            "host": [START, "godmode", "wait 25", "keys map", "wait 15", "shot map-flat", "keys l3", "wait 25",
                     "shot map-3d", "keys triangle", "wait 15", "shot map-3d-all", "keys l3", "wait 4",
                     "shot map-flat-all", "keys circle", "wait 2"],
        },
        "console_expect": {"host": [r"Map: 3D view", r"Map: flat view",
                                    r"Map: (drew|read) m24_01_00_00 view 1"]},
        "settings": {"BB_MAP_3D": "1"},  # the 3D map is opt-in (Insert, Display)
        "timeout": 240,
    },
    # Player report 2026-10-22 (v1.1.2): on Cathedral Ward (the host at about -138 -29.5 170) the
    # Forbidden Woods' multiplayer wall (2701999, at -147 -33.8 178 in m27's MSB) stood on the
    # stairs down to the woods: the player is still on the Ward there, and only the current map's
    # walls were turned off. At that spot in a session the woods' walls are off (on 1.1.2 they
    # stood: "object 2701999: enabled 1"). The ground there ends 10 m on, so no walk through.
    "walls-woods-stairs": {
        "about": "in a session, the host on Cathedral Ward at the stairs to the Forbidden Woods: "
                 "m27's multiplayer walls are off as well as the Ward's",
        "roles": {
            "host": [START, f"ensure-map {CATHEDRAL_WARD} {CATHEDRAL_WARD_LAMP}", "godmode",
                     "wait-session 2 300", "wait 10", "teleport -138.0 -29.5 170.0 0.84", "wait 5",
                     "object 2701999 2701998 2401996 2401997", "snapshot", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 25", "expect-session 2"],
        },
        "expect": {"host": [r"autoplay object 2701999: enabled 0 ",
                            r"autoplay object 2701998: enabled 0 ",
                            r"autoplay object 2401996: enabled 0 "]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    # Player report 2026-10-22 (v1.1.2): a fall off a Forbidden Woods cliff (into the void: a real
    # death) while the teammate stood on the map respawned at the lamp in place, and the player got
    # up invisible to itself and to the teammate until a trip. Both at the woods' lamp (2702950,
    # -190.1 -76.2 252.2); the guest steps off the ledge west of it where the player did (the
    # trace: dead about 16 m down, still falling). The void fades a character out (+0x20C, 1.0
    # while shown) and only a map load puts it back: once up again, the guest is faded in (on 1.1.2
    # it stayed 0.0) and shown to the host. Player report 2026-10-23 (v1.1.9): the host's copy of
    # the guest faded out in the void too and stayed so (expect-others-shown: "faded out fade
    # 0.00"): it fades back in once the guest is up again.
    "void-fall-respawn": {
        "about": "in a session the guest steps off a Forbidden Woods cliff into the void while the "
                 "host stands at the lamp; it wakes at the lamp shown to itself and to the host",
        "roles": {
            "host": [START, "godmode", f"set-flag {FORBIDDEN_WOODS_LAMP_LIT} 1",
                     "wait-session 2 300", "travel 2702950", "wait-members 2 120", "wait 10",
                     "expect-others-shown 1",
                     "wait-member-vital dead 60", "wait-member-vital up 60", "wait 15", "snapshot",
                     "expect-others-shown 1", "expect-others-near 1 40", "shot host-sees-guest",
                     "expect-session 2"],
            "guest": [START, f"set-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", "wait-session 2 300",
                      "travel 2702950", "wait-members 2 120", "wait 10",
                      "teleport -211.0 -76.5 250.4 1.5708", "wait 2", "stick 128 0 2500",
                      "wait-vital dead 45", "wait-vital up 60", "wait 15", "snapshot",
                      "peek self 20c", "expect-others-shown 1", "expect-others-near 1 40",
                      "expect-session 2"],
        },
        "expect": {"guest": [r"party death: the death was a fall into the void",
                             r"travelling there \(reset=false, void fall=true\)",
                             r"autoplay peek: 0x10044000 \+0x20c = 0x3f800000"],
                   "host": [r"presence: player 0x1004400\d faded out \(a fall into the void\) "
                            r"and its member \w+ is up again: faded back in"]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "settings": PARTY_DEATH,
        "timeout": 300,
    },
    # Provi's report (v1.2.0): in Yahar'gul a fall down the lift's shaft put a party member down at
    # its foot, and the lift's first-use event (m28_00 12800607: 3:2, the local player inside area
    # 2802100, a box at 243.71 -97.97 3.56) set 12800612 and unlocked its call lever. On the rig that
    # event set nothing even for an up host teleported into its area (2026-10-27), so the same down
    # is checked at an area the rig's characters do trigger with the same condition: under the
    # blood moon the first bell (12805030) rings within a second of a player entering area 2802500.
    # The guest goes down outside it and its body is put into it: no bell on either game. The host,
    # up, then steps in and the bell rings.
    "yahargul-downed-bell-area": {
        "about": "in a session in Yahar'gul under the blood moon the guest lies down inside the "
                 "first bell's area: the bell stays silent; the host standing there rings it",
        "roles": {
            "host": [START, "godmode", f"set-flag {YAHARGUL_LAMP_LIT} 1",
                     f"set-flag {BLOOD_MOON} 1", "wait-session 2 300",
                     f"travel {YAHARGUL_LAMP}", "wait-members 2 120", "wait 10",
                     f"expect-flag {YAHARGUL_BELL_RUNG} 0", "wait-member-vital down 120",
                     "wait 20", f"expect-flag {YAHARGUL_BELL_RUNG} 0",
                     f"teleport {YAHARGUL_BELL_AREA} 0", "wait 5", "snapshot",
                     f"expect-flag {YAHARGUL_BELL_RUNG} 1", "expect-session 2"],
            "guest": [START, f"set-flag {YAHARGUL_LAMP_LIT} 1", f"set-flag {BLOOD_MOON} 1",
                      "wait-session 2 300", f"travel {YAHARGUL_LAMP}", "wait-members 2 120",
                      "wait 10", f"expect-flag {YAHARGUL_BELL_RUNG} 0",
                      f"teleport {YAHARGUL_BY_THE_BELL} 0", "wait 3", "hurt self 1", "rise 15",
                      "wait-vital down 30", f"teleport {YAHARGUL_BELL_AREA} 0", "wait 2",
                      "snapshot", "wait 10", f"expect-flag {YAHARGUL_BELL_RUNG} 0", "wait 30",
                      "expect-session 2"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "settings": PARTY_DEATH,
        "timeout": 300,
    },
    # Provi's report (v1.2.0): in Yahar'gul the bell-ringing woman's summons made a really loud
    # blood sound when they died, and some appeared without rising. Under the blood moon the host
    # rings the first bell (YAHARGUL_BELL_RUNG, within a second in its area) with the guest beside
    # it, kills two summons near the guest, and the guest kills one; both games' census shows each
    # copy's HP, death flags and switch.
    "yahargul-bell-summons": {
        "about": "in a session in Yahar'gul the host rings the first bell; its summons rise for "
                 "both, the host kills two near the guest and the guest one",
        "roles": {
            "host": [START, "godmode", f"set-flag {YAHARGUL_LAMP_LIT} 1",
                     f"set-flag {BLOOD_MOON} 1", "wait-session 2 300",
                     f"travel {YAHARGUL_LAMP}", "wait-members 2 120", "wait 10",
                     f"teleport {YAHARGUL_BELL_AREA} 0", "census 40", "wait 25",
                     f"set-flag {YAHARGUL_BELL_RUNG} 1", "wait 2", "census 40", "wait 8",
                     "census 40", "attack nearest 40", "wait 3", "census 40", "wait 20",
                     "census 40", "attack nearest 40", "wait 3", "census 40", "wait 30",
                     "census 40", "expect-session 2"],
            "guest": [START, "godmode", f"set-flag {YAHARGUL_LAMP_LIT} 1",
                      f"set-flag {BLOOD_MOON} 1", "wait-session 2 300",
                      f"travel {YAHARGUL_LAMP}", "wait-members 2 120", "wait 12",
                      f"teleport {YAHARGUL_BELL_GUEST} 0", "census 40", "wait 30", "census 40",
                      "wait 10", "census 40", "wait 25", "census 40", "attack nearest 40",
                      "wait 3", "census 40", "wait 30", "census 40", "expect-session 2"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    # Kaniel 2026-10-17: the map screen in a session: the other hunter at its spot on the
    # picture, ringed in the compass's light gray, and listed with its area.
    "map-team": {
        "about": "the map screen with a teammate: its marker where it stands, its name and area "
                 "in the list; the host shoots it",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 5", f"teleport {COMPASS_HOST} 0.0",
                     "wait 10", "keys map", "wait 12", "shot map-team", "keys circle", "wait 3",
                     "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 8", f"teleport {COMPASS_GUEST} 0.0",
                      "wait 30", "expect-session 2"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 300,
    },
    "soak": {
        "about": "half an hour of trips back and forth with the session up (leaks, slow growth, "
                 "a trip that breaks after many)",
        "roles": {
            "host": [START, "godmode", "wait-session 2 300",
                     *[step for _ in range(12) for step in
                       (f"travel {HUNTERS_DREAM}", "wait 20", f"travel {CENTRAL_YHARNAM_LAMP}",
                        "wait-members 2 240", "wait 40")],
                     "snapshot", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 2100", "wait-members 2 240",
                      "snapshot", "expect-session 2"],
        },
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 2700,
    },
}

# On Kaniel's characters the host's story switched off the copy of Iosefka by her door: so does
# the guest.
SCENARIOS["iosefka-mine"]["roles"] = {
    role: [*steps[:6], f"expect-disabled {CLINIC} 1 30", *steps[6:]]
    for role, steps in SCENARIOS["iosefka-new"]["roles"].items()
}


# Chalice dungeons. The Dream's first ritual altar: talk t210060 on the talk dummy c9020_0005 at
# (-18.27, -5.43, -16.88) in m21_00's MSB, used from 1 m east of it facing west. Its talk sets
# 6708 on the first use (and shows a message first); its menu is Chalice Ritual (first),
# Chalice Dungeon Search, Search by Chalice Glyph; Chalice Ritual, then Conduct Ritual, asks
# "Conduct a ritual...?" (No first: left is Yes). After the ritual the altar's menu has its
# layers, Layer One first (the warp 72100300). Chalice dungeons are map 0x1d000000.
# A headless game draws no menus: BB_GAME_MENU_TRACE_IDS=262000-267999 makes the port print
# which of the altar's texts it shows ("Game menu: shows ...").
PTHUMERU_CHALICE = "goods:6109"
RITUAL_BLOOD_1 = "goods:7000"
ALTAR_SPOT = "-17.27 -5.23 -16.88 1.571"
CHALICE_DUNGEON = "0x1d000000"


def CHALICE_ENTER(role: str) -> list[str]:
    """The Pthumeru Chalice and its materials, the Dream, the altar (its first use's message),
    Chalice Ritual, Conduct Ritual, Yes, then the altar again and Layer One."""
    return [f"trade-stock {PTHUMERU_CHALICE} 1", f"trade-stock {RITUAL_BLOOD_1} 10",
            "trade-stock echoes 20000", f"travel {HUNTERS_DREAM}", "wait 5",
            f"teleport {ALTAR_SPOT}", "wait 2", "press cross 200", "wait 7",
            *[step for _ in range(4) for step in ("press cross 200", "wait 7")],
            "press left 200", "wait 1", "press cross 200", "wait 20", f"log {role} ritual done",
            *[step for _ in range(3) for step in ("press cross 200", "wait 10")],
            f"wait-map {CHALICE_DUNGEON} 180", "wait 10", "snapshot",
            f"log {role} in the dungeon"]


SCENARIOS["chalice-ritual"] = {
    "about": "a ritual with the Pthumeru Chalice at the Dream's altar, then its Layer One (the "
             "server's own dungeon for the chalice: none made the game return to the title)",
    "roles": {"host": [START, "godmode", *CHALICE_ENTER("host"), f"expect-map {CHALICE_DUNGEON}"]},
    "expect": {"host": [r"host in the dungeon"]},
    # The story chalice's own layout, from the record's rites byte 90: with rites 0 the game built
    # the root layout m29_10_00_00, whose boss drops the root loot (2026-10-21).
    "settings": {"BB_FILE_TRACE": "1"},
    "console_expect": {"host": [r"File trace: /app0/dvdroot_ps4/event/m29_10_90_00/"
                                r"m29_10_90_00\.emevd"]},
    "timeout": 300,
}


def ALTAR_RECORDS(when: str) -> list[str]:
    """Every ritual altar's record and the current altar's view, logged (altar-record; only a log
    line with BB_RIG_NO_ALTAR_RECORD=1, for a build from before the verb)."""
    if os.environ.get("BB_RIG_NO_ALTAR_RECORD") == "1":
        return [f"log altar records {when}"]
    return [f"log altar records {when}", *[f"altar-record {altar}" for altar in range(7)],
            "altar-record view"]


# Kaniel's playtest 2026-10-24: a team altar's record merged with a stale one named no dungeon and
# the game crashed loading it. The records of a clean Pthumeru story chalice ritual: before it,
# after it, in its Layer One and back in the Dream (what a standard record holds).
SCENARIOS["altar-record-capture"] = {
    "about": "a Pthumeru Chalice ritual alone, every altar record logged before, after, in the "
             "dungeon and back in the Dream",
    "roles": {"host": [START, "godmode", f"trade-stock {PTHUMERU_CHALICE} 1",
                       f"trade-stock {RITUAL_BLOOD_1} 10", "trade-stock echoes 20000",
                       f"travel {HUNTERS_DREAM}", "wait 5", *ALTAR_RECORDS("before"),
                       f"teleport {ALTAR_SPOT}", "wait 2", "press cross 200", "wait 7",
                       *[step for _ in range(4) for step in ("press cross 200", "wait 7")],
                       "press left 200", "wait 1", "press cross 200", "wait 20",
                       "log host ritual done", *ALTAR_RECORDS("after the ritual"),
                       *[step for _ in range(3) for step in ("press cross 200", "wait 10")],
                       f"wait-map {CHALICE_DUNGEON} 180", "wait 10", "log host in the dungeon",
                       *ALTAR_RECORDS("in the dungeon"), f"travel {HUNTERS_DREAM}", "wait 15",
                       "expect-map 0x15000000", *ALTAR_RECORDS("back in the Dream")]},
    "expect": {"host": [r"host ritual done", r"host in the dungeon"]},
    "settings": {"BB_FILE_TRACE": "1"},
    "timeout": 300,
}

# Each member of a team of two conducts the same chalice's ritual at its own altar; the server
# gives both the one dungeon it keeps for that chalice, and each enters Layer One alone: they
# meet in it.
SCENARIOS["chalice-team"] = {
    "about": "a team of two enters the same chalice dungeon, each from its own altar",
    "roles": {
        "host": ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                 "godmode", "wait-session 2 300", "wait 10", *CHALICE_ENTER("host"),
                 "wait-members 2 300", "wait 20", "expect-session 2", "snapshot"],
        "guest": ["team-accept", "team-ready", START, "godmode", "wait-session 2 300", "wait 30",
                  "wait 10",
                  *CHALICE_ENTER("guest"), "wait-members 2 300", "wait 20", "expect-session 2",
                  "snapshot"],
    },
    "expect": {"host": [r"host in the dungeon"], "guest": [r"guest in the dungeon"]},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "allow": CRASH_CLEANUP,
    "timeout": 1200,
}



def CHALICE_FROM_OWN_ALTAR(role: str, hold: bool = True) -> list[str]:
    """The Dream's altar and its Layer One: a teammate's ritual put the dungeon there (team
    altars). `hold`: with a chalice in the bag (without one the base game's altar shows no menu;
    the port opens it while an altar holds a dungeon)."""
    return [*([f"trade-stock {PTHUMERU_CHALICE} 1"] if hold else []), f"travel {HUNTERS_DREAM}", "wait 5",
            f"teleport {ALTAR_SPOT}", "wait 2",
            *[step for _ in range(3) for step in ("press cross 200", "wait 10")],
            f"wait-map {CHALICE_DUNGEON} 180", "wait 10", "snapshot", f"log {role} in the dungeon"]


# Team altars (Kaniel 2026-10-17/18): a ritual at one member's altar puts its dungeon on every
# member's altar (the altar's flags are the team's, see FlagTable; the other games load the
# record into their altar). The host conducts the ritual; the guest, holding a chalice but
# conducting nothing, picks Layer One at its own altar; they meet in it. The guest's warp is the
# Dream's event 12107100, patched to run for a client (party_boss_events.py).
SCENARIOS["chalice-altar-team"] = {
    "about": "the host's ritual puts the dungeon on the guest's own altar; the guest enters "
             "its Layer One with no ritual nor search, and they meet in it",
    "roles": {
        "host": ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                 "godmode", "wait-session 2 300", "wait 10", *CHALICE_ENTER("host"),
                 "wait-members 2 400", "wait 20", "expect-session 2", "snapshot"],
        "guest": ["team-accept", "team-ready", START, "godmode", "wait-session 2 300",
                  *["wait 30"] * 5, *CHALICE_FROM_OWN_ALTAR("guest"), "wait-members 2 300",
                  "wait 20", "expect-session 2", "snapshot"],
    },
    "expect": {"host": [r"host in the dungeon"], "guest": [r"guest in the dungeon"]},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "allow": CRASH_CLEANUP,
    "timeout": 1200,
}

# The other way round: the guest's ritual, the host's own altar.
SCENARIOS["chalice-altar-host"] = {
    "about": "the guest's ritual puts the dungeon on the host's own altar; the host enters its "
             "Layer One with no ritual nor search, and they meet in it",
    "roles": {
        "host": ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                 "godmode", "wait-session 2 300", *["wait 30"] * 5,
                 *CHALICE_FROM_OWN_ALTAR("host"), "wait-members 2 300", "wait 20",
                 "expect-session 2", "snapshot"],
        "guest": ["team-accept", "team-ready", START, "godmode", "wait-session 2 300", "wait 10",
                  *CHALICE_ENTER("guest"), "wait-members 2 400", "wait 20",
                  "expect-session 2", "snapshot"],
    },
    "expect": {"host": [r"host in the dungeon"], "guest": [r"guest in the dungeon"]},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "allow": CRASH_CLEANUP,
    "timeout": 1200,
}

# Kaniel 2026-10-18: a teammate holding no chalice enters the team's dungeon too.
SCENARIOS["chalice-altar-no-chalice"] = {
    "about": "the host's ritual; the guest, holding no chalice, opens its own altar and enters "
             "the dungeon's Layer One; they meet in it",
    "roles": {
        "host": SCENARIOS["chalice-altar-team"]["roles"]["host"],
        "guest": ["team-accept", "team-ready", START, "godmode", "wait-session 2 300",
                  *["wait 30"] * 5, *CHALICE_FROM_OWN_ALTAR("guest", hold=False),
                  "wait-members 2 300", "wait 20", "expect-session 2", "snapshot"],
    },
    "expect": {"host": [r"host in the dungeon"],
               "guest": [r"given its chalice", r"guest in the dungeon"]},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "allow": CRASH_CLEANUP,
    "timeout": 1200,
}

# Kaniel 2026-10-24: a save left inside a chalice dungeon crashed as it loaded again (guests for
# sure). The host's ritual, the guest enters from its own altar, they meet; then one game starts
# again (its save in the dungeon), loads into the dungeon beside the other, and every altar record
# stays the ritual's.
def CHALICE_INSIDE_FIRST(role: str) -> list[str]:
    head = (["team-create", "team-invite rig_guest", "team-wait 2", "team-start"] if role == "host"
            else ["team-accept", "team-ready"])
    enter = (CHALICE_ENTER("host") if role == "host"
             else ["wait 30", "wait 30", "wait 30", "wait 30", "wait 30",
                   *CHALICE_FROM_OWN_ALTAR("guest")])
    return [*head, START, "godmode", "wait-session 2 300", "wait 10", *enter,
            "wait-members 2 400", "wait 20", "expect-session 2", *ALTAR_RECORDS(f"{role} inside")]


def CHALICE_INSIDE_AGAIN(role: str) -> list[str]:
    head = ["title-wait 20", "team-start"] if role == "host" else ["team-ready"]
    return [*head, "godmode", "wait-session 2 420", "wait-members 2 180", "wait 20",
            f"expect-map {CHALICE_DUNGEON}", f"log {role} loaded inside again",
            *ALTAR_RECORDS(f"{role} loaded inside"), "expect-session 2"]


def CHALICE_INSIDE_STAY(role: str) -> list[str]:
    return [*CHALICE_INSIDE_FIRST(role), "wait-solo 240", "wait-session 2 420",
            "wait-members 2 180", "wait 20", f"expect-map {CHALICE_DUNGEON}",
            *ALTAR_RECORDS(f"{role} after the other's load"), "expect-session 2"]


# Kaniel 2026-10-24: the team's altar records stay the ritual's through every trip. The host's
# ritual; both enter; then twice both go back to the Dream and enter again from their own altars.
# Every game logs every altar record at each step (the rig's check compares them: all the same).
def CHALICE_REENTER(role: str, times: int) -> list[str]:
    steps = []
    for time in range(times):
        steps += [f"travel {HUNTERS_DREAM}", "wait 20", "expect-map 0x15000000",
                  *ALTAR_RECORDS(f"{role} back in the Dream {time + 1}"),
                  *CHALICE_FROM_OWN_ALTAR(role, hold=False), "wait-members 2 300", "wait 15",
                  *ALTAR_RECORDS(f"{role} inside again {time + 1}")]
    return steps


# The playtest's merged record (1D 0F FF FF) set on an altar (or the view) by flags.
def MERGE_RECORD(base: int) -> list[str]:
    return [f"set-flag {base + flag} 1" for flag in (13, 15, *range(16, 32))]


# Kaniel's playtest 2026-10-24: the merged record on the altar in the Dream right after the ritual
# (on main it crashed at eboot+0x155ac3f, m29_15_i5_00 not found, as the playtest did): it is
# repaired within a second, and the Layer One it then opens loads the ritual's dungeon.
SCENARIOS["chalice-enter-broken"] = {
    "about": "a ritual alone; its altar's record merged as the playtest's was; its Layer One "
             "loads without a crash",
    "roles": {"host": [START, "godmode", f"trade-stock {PTHUMERU_CHALICE} 1",
                       f"trade-stock {RITUAL_BLOOD_1} 10", "trade-stock echoes 20000",
                       f"travel {HUNTERS_DREAM}", "wait 5", f"teleport {ALTAR_SPOT}", "wait 2",
                       "press cross 200", "wait 7",
                       *[step for _ in range(4) for step in ("press cross 200", "wait 7")],
                       "press left 200", "wait 1", "press cross 200", "wait 20",
                       "log host ritual done", *MERGE_RECORD(92900000), *MERGE_RECORD(94100000),
                       *ALTAR_RECORDS("merged"),
                       *[step for _ in range(3) for step in ("press cross 200", "wait 10")],
                       f"wait-map {CHALICE_DUNGEON} 180", "wait 10", "log host in the dungeon",
                       *ALTAR_RECORDS("in the dungeon")]},
    "expect": {"host": [r"host in the dungeon", r"named no dungeon \(1D 0F FF FF"]},
    "settings": {"BB_FILE_TRACE": "1"},
    "console_expect": {"host": [r"m29_10_90_00"]},
    "timeout": 300,
}

SCENARIOS["chalice-altar-same"] = {
    "about": "the host's ritual, both enter, then twice both go back to the Dream and enter "
             "again; every altar record stays the ritual's in both games",
    "roles": {role: [*CHALICE_INSIDE_FIRST(role), *CHALICE_REENTER(role, 2), "expect-session 2"]
              for role in ("host", "guest")},
    "expect": {role: [rf"{role} in the dungeon", rf"altar records {role} inside again 2"]
               for role in ("host", "guest")},
    "forbid": {role: [r"named no dungeon"] for role in ("host", "guest")},
    "allow": CRASH_CLEANUP,
    "timeout": 1500,
}

for _restarts in ("guest", "host"):
    _stays = "host" if _restarts == "guest" else "guest"
    SCENARIOS[f"chalice-load-inside-{_restarts}"] = {
        "about": f"a team in a chalice dungeon from the host's ritual; the {_restarts}'s game "
                 "starts again with its save inside and loads back into the dungeon",
        "roles": {
            _restarts: [[*CHALICE_INSIDE_FIRST(_restarts), "wait 10", "snapshot", "done"],
                        CHALICE_INSIDE_AGAIN(_restarts)],
            _stays: CHALICE_INSIDE_STAY(_stays),
        },
        "expect": {_restarts: [rf"{_restarts} in the dungeon", rf"{_restarts} loaded inside again"],
                   _stays: [rf"{_stays} in the dungeon"]},
        "allow": CRASH_CLEANUP,
        "timeout": 1500,
    }

# Kaniel 2026-10-20: random (root) chalice dungeons with team altars, both players by the altar.
# The host conducts the Pthumeru Root Chalice's ritual (the game makes a random dungeon and uploads
# it) and enters its Layer One; the guest, standing 2 m away the whole time, is not taken along
# (each game's altar talk sets the warp only for its own talker), then enters the same dungeon
# from its own altar (the team's record) and they meet.
PTHUMERU_ROOT_CHALICE = "goods:6100"
ROOT_RITUAL_MATERIALS = [f"trade-stock goods:{item} 20" for item in (7000, 7001, 7012, 7020, 7050)]
HUNTERS_DREAM_MAP = "0x15000000"
BESIDE_ALTAR_SPOT = "-17.27 -5.23 -14.88 1.571"
SCENARIOS["chalice-root-team"] = {
    "about": "a root chalice's random dungeon with both players by the altar: the host's ritual "
             "and entry do not take the guest along; the guest then enters from its own altar "
             "and they meet in the same dungeon",
    "roles": {
        "host": ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                 "godmode", "wait-session 2 300", f"trade-stock {PTHUMERU_ROOT_CHALICE} 1",
                 *ROOT_RITUAL_MATERIALS, "trade-stock echoes 20000", f"travel {HUNTERS_DREAM}",
                 "wait 20", f"teleport {ALTAR_SPOT}", "wait 2", "press cross 200", "wait 7",
                 *[step for _ in range(4) for step in ("press cross 200", "wait 7")],
                 "press left 200", "wait 1", "press cross 200", "wait 25", "log host ritual done",
                 *[step for _ in range(3) for step in ("press cross 200", "wait 10")],
                 f"wait-map {CHALICE_DUNGEON} 180", "wait 10", "snapshot",
                 "log host in the dungeon", "wait-members 2 400", "wait 20", "expect-session 2",
                 "snapshot"],
        "guest": ["team-accept", "team-ready", START, "godmode", "wait-session 2 300",
                  f"travel {HUNTERS_DREAM}", "wait 5", f"teleport {BESIDE_ALTAR_SPOT}",
                  *["wait 30"] * 5, f"expect-map {HUNTERS_DREAM_MAP}",
                  "log guest still in the Dream", f"teleport {ALTAR_SPOT}", "wait 2",
                  *[step for _ in range(3) for step in ("press cross 200", "wait 10")],
                  f"wait-map {CHALICE_DUNGEON} 180", "wait 10", "snapshot",
                  "log guest in the dungeon", "wait-members 2 300", "wait 20",
                  "expect-session 2", "snapshot"],
    },
    "expect": {"host": [r"host ritual done", r"host in the dungeon"],
               "guest": [r"guest still in the Dream", r"guest in the dungeon"]},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "allow": CRASH_CLEANUP,
    "timeout": 1200,
}

# Kaniel 2026-10-20: a team of three at the Dream's altar. The host conducts the ritual while both
# guests stand by and watch (neither is taken along); then one by one, host, guest, guest2, each
# enters the dungeon from its own altar (the guests hold a chalice: one given at the altar shows
# a pickup box that takes the altar's first press in a visible game; chalice-altar-no-chalice
# covers that). In the dungeon the host (the map's owner) kills the nearest enemy (the game's own
# kill: the rig's attacks never landed on the first room's enemy); then they go back to the
# Dream in another order: guest2, the host, the guest. The shots are for the website (a visible
# run with BB_SHOT_TRIGGER; skipped headless). The Dream's layer warp (12107200) was
# kept in step: the host's entry took both watching guests along (party_boss_events.py).
WATCH_SPOTS = {"guest": BESIDE_ALTAR_SPOT, "guest2": "-15.27 -5.23 -16.88 1.571"}
CHALICE3_SYNCS = ("c3-dream", "c3-ritual", "c3-host-in", "c3-guest-in", "c3-all-in",
                  "c3-killed", "c3-guest2-out", "c3-host-out", "c3-guest-out")


def CHALICE3_LAYER_ONE(role: str) -> list[str]:
    """The altar's Layer One, from the altar's spot, and the dungeon."""
    return [f"teleport {ALTAR_SPOT}", "wait 3", f"shot {role}-altar-layers",
            *[step for _ in range(3) for step in ("press cross 200", "wait 10")],
            f"wait-map {CHALICE_DUNGEON} 180", "wait 15", "snapshot",
            f"log {role} in the dungeon", f"shot {role}-dungeon-arrived"]


def CHALICE3_BACK(role: str) -> list[str]:
    return [f"travel {HUNTERS_DREAM}", "wait 15", f"expect-map {HUNTERS_DREAM_MAP}",
            f"log {role} back in the Dream", f"shot {role}-back-in-dream"]


SCENARIOS["chalice-team3"] = {
    "about": "a team of three: the host's ritual with both guests watching (not taken along), "
             "each enters the dungeon in turn from its own altar, the host kills an enemy "
             "there, then they go back to the Dream in another order",
    "roles": {
        "host": ["team-create", "team-invite rig_guest", "team-invite rig_guest2", "team-wait 3",
                 "team-start", START, "godmode", "wait-session 3 480", "wait 10",
                 f"trade-stock {PTHUMERU_CHALICE} 1", f"trade-stock {RITUAL_BLOOD_1} 10",
                 "trade-stock echoes 20000", f"travel {HUNTERS_DREAM}", "wait 15",
                 "sync c3-dream", f"teleport {ALTAR_SPOT}", "wait 4", "shot host-altar",
                 "press cross 200", "wait 7", "shot host-altar-menu",
                 *[step for i in range(4) for step in ("press cross 200", "wait 7",
                                                         f"shot host-ritual-{i}")],
                 "press left 200", "wait 1", "shot host-ritual-yes", "press cross 200",
                 "wait 20", "log host ritual done", "shot host-ritual-done", "sync c3-ritual",
                 *CHALICE3_LAYER_ONE("host")[1:], "sync c3-host-in", "sync c3-guest-in",
                 "sync c3-all-in", "wait-members 3 400", "wait 10", "expect-session 3",
                 "shot host-dungeon-team", "kill nearest", "wait 5", "census 40",
                 "log host killed one", "shot host-after-kill", "sync c3-killed",
                 "sync c3-guest2-out", *CHALICE3_BACK("host"), "sync c3-host-out",
                 "sync c3-guest-out", "wait-members 3 300", "wait 10", "expect-session 3",
                 "shot host-all-back"],
        "guest": ["team-accept", "team-ready", START, "godmode", "wait-session 3 480",
                  f"trade-stock {PTHUMERU_CHALICE} 1", f"travel {HUNTERS_DREAM}", "wait 15",
                  f"teleport {WATCH_SPOTS['guest']}",
                  "wait 3", "sync c3-dream", "wait 25", "shot guest-watching", "sync c3-ritual",
                  "sync c3-host-in", f"expect-map {HUNTERS_DREAM_MAP}",
                  "log guest still in the Dream", *CHALICE3_LAYER_ONE("guest"),
                  "sync c3-guest-in", "sync c3-all-in", "wait-members 3 400", "wait 10",
                  "expect-session 3", "shot guest-dungeon-team", "sync c3-killed", "wait 5",
                  "census 40", "shot guest-after-kill",
                  "sync c3-guest2-out", "sync c3-host-out", *CHALICE3_BACK("guest"),
                  "sync c3-guest-out", "wait-members 3 300", "wait 10", "expect-session 3",
                  "shot guest-all-back"],
        "guest2": ["team-accept", "team-ready", START, "godmode", "wait-session 3 480",
                   f"trade-stock {PTHUMERU_CHALICE} 1", f"travel {HUNTERS_DREAM}", "wait 15",
                   f"teleport {WATCH_SPOTS['guest2']}",
                   "wait 3", "sync c3-dream", "wait 30", "shot guest2-watching",
                   "sync c3-ritual", "sync c3-host-in", "sync c3-guest-in",
                   f"expect-map {HUNTERS_DREAM_MAP}", "log guest2 still in the Dream",
                   *CHALICE3_LAYER_ONE("guest2"), "sync c3-all-in", "wait-members 3 400",
                   "wait 10", "expect-session 3", "shot guest2-dungeon-team", "sync c3-killed",
                   "wait 5", "shot guest2-after-kill", *CHALICE3_BACK("guest2"),
                   "sync c3-guest2-out", "sync c3-host-out", "sync c3-guest-out",
                   "wait-members 3 300", "wait 10", "expect-session 3", "shot guest2-all-back"],
    },
    "syncs": {name: {"chapter": "chalice-team3", "roles": ["host", "guest", "guest2"]}
              for name in CHALICE3_SYNCS},
    "expect": {"host": [r"host ritual done", r"host in the dungeon",
                        r"autoplay: kill 0x\w+ hp \d+ -> 0", r"host back in the Dream"],
               "guest": [r"guest still in the Dream", r"guest in the dungeon",
                         r"guest back in the Dream"],
               "guest2": [r"guest2 still in the Dream", r"guest2 in the dungeon",
                          r"guest2 back in the Dream"]},
    "forbid": {role: SESSION_HOLDS for role in ("host", "guest", "guest2")},
    "allow": CRASH_CLEANUP,
    "timeout": 2400,
}

# The 2026-10-19 playtest's cases (Kaniel 2026-10-19: a short scenario for each case that failed).
SAW_HUNTER_BADGE = "goods:4103"  # krolebord's badge (#267)
SCENARIOS["key-items-kept"] = {
    "about": "#267: a guest's own key item (a hunter badge) stays in its inventory after the "
             "join, while it is the host's phantom, and after a lamp trip and back",
    "roles": {
        "host": [START, "godmode", "wait-session 2 300", "wait 90", "expect-session 2"],
        "guest": [START, "godmode", f"trade-stock {SAW_HUNTER_BADGE} 1",
                  f"expect-held {SAW_HUNTER_BADGE} 1", "wait-session 2 300", "wait 15",
                  f"expect-held {SAW_HUNTER_BADGE} 1 20", f"travel {HUNTERS_DREAM}", "wait 15",
                  f"expect-held {SAW_HUNTER_BADGE} 1 20", f"travel {CENTRAL_YHARNAM_LAMP}",
                  "wait 15", f"expect-held {SAW_HUNTER_BADGE} 1 20", "expect-session 2"],
    },
    "forbid": {"host": SESSION_HOLDS,
               "guest": SESSION_HOLDS + [r"held-item flag \d+ cleared: this player has no "
                                         r"goods 4103"]},
    "timeout": 600,
}

# An outside player on v1.1.4 (2026-10-23): a guest never learned Eileen's and Alfred's
# gestures from their talks, the host did. A talk's gesture step skips while its flag is on
# (talk-gesture does that step); Alfred's Pray is gesture 9 by flag 6749, Eileen's Shake off
# Cape 4 by 6744.
PRAY = "9 6749"
SCENARIOS["gesture-own"] = {
    "about": "each player learns an NPC's gesture from its own talk and keeps it: the host "
             "learns Alfred's Pray, then the guest's own talk teaches the guest too, and the "
             "guest still has it after a trip to the Dream and back; a gesture flag the guest's "
             "save took from the host without the gesture (Shake off Cape) goes off",
    "roles": {
        "host": [START, "godmode", "expect-gesture 9 0", "wait-session 2 300",
                 f"talk-gesture {PRAY}", "expect-gesture 9 1", "expect-flag 6749 1", "wait 30",
                 "wait 30", "wait 30", "expect-gesture 9 1", "expect-session 2"],
        "guest": [START, "godmode", "expect-gesture 9 0", "expect-gesture 4 0", "set-flag 6744 1",
                  "wait-session 2 300", "wait 20", f"talk-gesture {PRAY}", "expect-gesture 9 1",
                  "expect-flag 6749 1", "wait-flag 6744 0 20", f"travel {HUNTERS_DREAM}",
                  "wait 15", "expect-gesture 9 1", f"travel {CENTRAL_YHARNAM_LAMP}", "wait 15",
                  "expect-gesture 9 1", "expect-flag 6749 1", "expect-flag 6744 0",
                  "expect-session 2"],
    },
    "expect": {"host": [r"talk gave gesture 9 \(flag 6749\)"],
               "guest": [r"talk gave gesture 9 \(flag 6749\)",
                         r"gesture flag 6744 cleared: this player has not learned gesture 4"]},
    "forbid": {"host": SESSION_HOLDS + [r"talk gives no gesture"],
               "guest": SESSION_HOLDS + [r"talk gives no gesture"]},
    "timeout": 600,
}

# Kaniel 2026-10-23: a joining guest saw the game's "item received" popup ("Oedon Tomb Key 1 /
# OK") for each of its own key items put in the phantom's list; with many it went on and on.
SWORD_HUNTER_BADGE = "goods:4114"
OEDON_TOMB_KEY = "goods:4000"
SILENT_KEY_ITEMS = (OEDON_TOMB_KEY, SAW_HUNTER_BADGE, SWORD_HUNTER_BADGE)


def held_key_items(timeout: int = 20) -> list[str]:
    return [f"expect-held {item} 1 {timeout}" for item in SILENT_KEY_ITEMS]


SCENARIOS["key-items-silent"] = {
    "about": "a joining guest's own key items (Oedon Tomb Key and two hunter badges) go in the "
             "phantom's key item list with no item-received popup, and it still holds each after "
             "the join and after a lamp trip and back; a vial give before the join still queues "
             "its popup (the detector works)",
    "roles": {
        "host": [START, "godmode", "wait-session 2 300", "wait 100", "expect-session 2"],
        "guest": [START, "godmode",
                  *[step for item in SILENT_KEY_ITEMS
                    for step in (f"trade-stock {item} 0", f"trade-stock {item} 1")],
                  *held_key_items(5), "trade-stock vials 0", "trade-stock vials 3", "wait 3",
                  "wait-session 2 300", "wait 15", *held_key_items(),
                  f"travel {HUNTERS_DREAM}", "wait 15", *held_key_items(),
                  f"travel {CENTRAL_YHARNAM_LAMP}", "wait 15", *held_key_items(),
                  "expect-session 2"],
    },
    "expect": {"guest": [
        # The detector: the give before the join queued its popup.
        r"item-get popup goods \d+ x 3 queued",
        # Each key item went in silently.
        *[rf"item-get popup goods {goods} x 1 not shown \(a silent grant\)[^\n]*\n"
          rf"(?:[^\n]*\n){{0,3}}[^\n]*1 x goods {goods} of the guest's own in the phantom's "
          rf"key items" for goods in (4000, 4103, 4114)],
    ]},
    "forbid": {"host": SESSION_HOLDS,
               "guest": SESSION_HOLDS + [
                   # Once the first went in the phantom's list, none of them shows its popup.
                   r"in the phantom's key items[\s\S]*item-get popup goods (?:4000|4103|4114) "
                   r"x \d+ queued",
                   r"held-item flag \d+ cleared: this player has no goods (?:4103|4114)"]},
    "timeout": 600,
}

# A world flag in Cathedral Ward's block (not the block of the map anyone stands on): the team's
# progress there (krolebord's Ward gate, #266) and NPC steps (#269) live in blocks like it.
WARD_BLOCK_FLAG = 12400999
SCENARIOS["team-world-other-block"] = {
    "about": "#266: the team's world reaches a joining guest in every block, not only the map it "
             "joins on: a Cathedral Ward flag the host's team world has is on in the guest's game "
             "after its join's reload, in the next session",
    "roles": {
        "host": [
            ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
             "wait-session 2 300", f"set-flag {WARD_BLOCK_FLAG} 1",
             f"expect-flag {WARD_BLOCK_FLAG} 1", "wait 15", "sync twob-gone", "wait 10", "done"],
            ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
             "wait-session 2 300", f"expect-flag {WARD_BLOCK_FLAG} 1", "wait 60", "done"],
        ],
        "guest": [
            # Kaniel 2026-10-25: the guest's game goes first (out of the team, invited back),
            # so the host's is the last to go and it still leads.
            ["team-accept", "team-ready", START, "wait-session 2 300", "wait 45",
             "sync twob-gone", "done"],
            ["team-accept", "team-ready", START, "wait-session 2 300", "wait 10",
             f"expect-flag {WARD_BLOCK_FLAG} 1", "wait 20", f"expect-flag {WARD_BLOCK_FLAG} 1",
             "done"],
        ],
    },
    "syncs": {"twob-gone": {"chapter": "team-world-other-block", "roles": ["host", "guest"]}},
    "allow": ["left the session outside a trip"],
    "timeout": 900,
}

SCENARIOS["npc-progress-after-trip"] = {
    "about": "#269: the team's progress the host made while the guest was in the Dream reaches "
             "the guest when it comes back, in every block (a Cathedral Ward flag, the guest back "
             "at Central Yharnam's lamp)",
    "roles": {
        "host": [START, "godmode", "wait-session 2 300", "sync npt-away",
                 f"set-flag {WARD_BLOCK_FLAG} 1", f"expect-flag {WARD_BLOCK_FLAG} 1", "wait 5",
                 "sync npt-set", "sync npt-back", "wait 10", "expect-session 2"],
        "guest": [START, "godmode", "wait-session 2 300", "wait 5", f"travel {HUNTERS_DREAM}",
                  "wait 10", "sync npt-away", "sync npt-set", f"expect-flag {WARD_BLOCK_FLAG} 0",
                  f"travel {CENTRAL_YHARNAM_LAMP}", "wait 20",
                  f"expect-flag {WARD_BLOCK_FLAG} 1", "sync npt-back", "expect-session 2"],
    },
    "syncs": {name: {"chapter": "npc-progress-after-trip", "roles": ["host", "guest"]}
              for name in ("npt-away", "npt-set", "npt-back")},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "timeout": 480,
}

SCENARIOS["wipe-one-map"] = {
    "about": "#270: host and guest die on Central Yharnam while guest2 stands in the Dream: that "
             "map starts over for both (the hurt enemy whole again), the session holds",
    "settings": PARTY_DEATH,
    "roles": {
        "host": [START, "godmode", "wait-session 3 480", "sync wom-away", f"hit {ENEMY} 30",
                 "wait 3", "census 40", "sync wom-hurt", "sync wom-guest-down", "godmode off",
                 f"teleport {WIPE3_SPOT['host']}", "wait 2", *LETHAL_FALL, "wait-vital dead 30",
                 "wait-vital up 150", "wait 20", "census 60", f"expect-hp {ENEMY} == 484",
                 "sync wom-checked", "expect-session 3"],
        "guest": [START, "godmode", "wait-session 3 480", "sync wom-away", "sync wom-hurt",
                  "wait 3", "census 40", "godmode off", f"teleport {WIPE3_SPOT['guest']}",
                  "wait 2", *LETHAL_FALL, "wait-vital down 30", "sync wom-guest-down",
                  "wait-vital dead 90", "wait-vital up 150", "wait 20", "census 60",
                  f"expect-hp {ENEMY} == 484", "sync wom-checked", "expect-session 3"],
        "guest2": [START, "godmode", "wait-session 3 480", f"travel {HUNTERS_DREAM}", "wait 10",
                   "sync wom-away", "sync wom-hurt", "sync wom-guest-down", "sync wom-checked",
                   "expect-session 3"],
    },
    "syncs": {name: {"chapter": "wipe-one-map", "roles": ["host", "guest", "guest2"]}
              for name in ("wom-away", "wom-hurt", "wom-guest-down", "wom-checked")},
    "forbid": {role: SESSION_HOLDS for role in ("host", "guest", "guest2")},
    "timeout": 900,
}

SCENARIOS["boss-hp-after-wipe"] = {
    "about": "#272: the host hurts the boss-sized enemy by the first lamp, then the party wipes: "
             "it is back at full HP in both games",
    "settings": PARTY_DEATH,
    "roles": {
        "host": [START, "godmode", "wait-session 2 300", f"approach {BIG_BY_THE_LAMP} 4 0",
                 "wait 3", f"hurt {BIG_BY_THE_LAMP} 900", "wait 5", "census 40", "sync bw-hurt",
                 "sync bw-guest-down", "godmode off", f"teleport {WIPE3_SPOT['host']}", "wait 2",
                 *LETHAL_FALL, "wait-vital dead 30", "wait-vital up 150", "wait-members 2 300",
                 "wait 20", "census 80", f"expect-hp {BIG_BY_THE_LAMP} == 1917",
                 "sync bw-checked", "expect-session 2"],
        "guest": [START, "godmode", "wait-session 2 300", "sync bw-hurt", "wait 3", "census 60",
                  "godmode off", f"teleport {WIPE3_SPOT['guest']}", "wait 2", *LETHAL_FALL,
                  "wait-vital down 30", "sync bw-guest-down", "wait-vital dead 90",
                  "wait-vital up 150", "wait-members 2 300", "wait 20", "census 80",
                  f"expect-hp {BIG_BY_THE_LAMP} == 1917", "sync bw-checked", "expect-session 2"],
    },
    "syncs": {name: {"chapter": "boss-hp-after-wipe", "roles": ["host", "guest"]}
              for name in ("bw-hurt", "bw-guest-down", "bw-checked")},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "timeout": 900,
}

# Kaniel's play test 2026-10-25 (Logarius): the party died one by one. Lisara fell first while the
# others stood, then krolebord and Kaniel, all before her respawn. shadNet took the later wipes for
# their own (she stood at the lamp again by then) and her game, which never saw the wipe, gave the
# boss's old HP back to everyone. Here the guest gives up first, then host and guest2 fall.
SCENARIOS["boss-hp-staggered-wipe"] = {
    "about": "the guest dies first while the others stand, then host and guest2 fall before its "
             "respawn: the hurt boss-sized enemy is back at full HP in all three games",
    "settings": PARTY_DEATH,
    "roles": {
        "host": [START, "godmode", "wait-session 3 480", f"approach {BIG_BY_THE_LAMP} 4 0",
                 "wait 3", f"hurt {BIG_BY_THE_LAMP} 900", "wait 5", "census 40", "sync sw-hurt",
                 "godmode off", f"teleport {WIPE3_SPOT['host']}", "wait 2", "hurt self 1",
                 "sync sw-guest-dead", "rise 18", "wait-vital dead 60", "wait-vital up 150",
                 "wait-members 3 300", "wait 20", "census 80",
                 f"expect-hp {BIG_BY_THE_LAMP} == 1917", "sync sw-checked", "expect-session 3"],
        "guest": [START, "godmode", "wait-session 3 480", "sync sw-hurt", "wait 3", "census 60",
                  "godmode off", f"teleport {WIPE3_SPOT['guest']}", "wait 2", *GIVE_UP,
                  "sync sw-guest-dead", "wait-vital up 60", "wait-members 3 300", "wait 20",
                  "census 80", f"expect-hp {BIG_BY_THE_LAMP} == 1917", "sync sw-checked",
                  "expect-session 3"],
        "guest2": [START, "godmode", "wait-session 3 480", "sync sw-hurt", "wait 3", "census 60",
                   "godmode off", f"teleport {WIPE3_SPOT['guest2']}", "wait 2", "hurt self 1",
                   "sync sw-guest-dead", "rise 18", "wait-vital dead 60", "wait-vital up 150",
                   "wait-members 3 300", "wait 20", "census 80",
                   f"expect-hp {BIG_BY_THE_LAMP} == 1917", "sync sw-checked", "expect-session 3"],
    },
    "expect": {"guest": [r"giving up ends down \d+; dies \(reset=false",
                         r"nobody else stands on map 0x18010000 before this member's respawn",
                         r"travelling there \(reset=true"]},
    "syncs": {name: {"chapter": "boss-hp-staggered-wipe", "roles": ["host", "guest", "guest2"]}
              for name in ("sw-hurt", "sw-guest-dead", "sw-checked")},
    "forbid": {role: SESSION_HOLDS for role in ("host", "guest", "guest2")},
    "timeout": 900,
}

SCENARIOS["kill-reaches-arrival"] = {
    "about": "#278: the guest lands the killing hit on an enemy the host's game runs; guest2, in "
             "the Dream then, comes to Central Yharnam later and has it dead too",
    "roles": {
        "host": [START, "godmode", "wait-session 3 480", "sync kra-away", "sync kra-killed",
                 "wait 60", "expect-session 3"],
        "guest": [START, "godmode", "wait-session 3 480", "sync kra-away", *attack(ENEMY),
                  "wait 5", "census 40", "sync kra-killed", "wait 55", "expect-session 3"],
        "guest2": [START, "godmode", "wait-session 3 480", f"travel {HUNTERS_DREAM}", "wait 10",
                   "sync kra-away", "sync kra-killed", "wait 5", f"travel {CENTRAL_YHARNAM_LAMP}",
                   "wait 25", "census 60", "expect-session 3"],
    },
    "syncs": {name: {"chapter": "kill-reaches-arrival", "roles": ["host", "guest", "guest2"]}
              for name in ("kra-away", "kra-killed")},
    "expect": {"guest2": [r"hid \d+ enemies a member killed on map 0x18010000"]},
    "forbid": {role: SESSION_HOLDS for role in ("host", "guest", "guest2")},
    "timeout": 600,
}

# #286 (Kaniel 2026-10-20: NPCs showed up for guests on maps without the host): a story step (a
# quest flag outside the maps' and the items' ranges, no row of its own) the host sets reaches a
# team guest standing on a map the host never visits, while it plays there.
STORY_FLAG = 1399
SCENARIOS["story-hostless"] = {
    "about": "#286: the host sets a story step while the team's guest stands in Cathedral Ward, "
             "a map the host never visits: the guest's game follows it there, without a load",
    "roles": {
        "host": ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                 "godmode", "wait-session 2 300", "sync sh-away", f"set-flag {STORY_FLAG} 1",
                 f"expect-flag {STORY_FLAG} 1", "sync sh-set", "sync sh-seen",
                 "expect-session 2"],
        "guest": ["team-accept", "team-ready", START, "godmode", "wait-session 2 300", "wait 5",
                  f"travel {CATHEDRAL_WARD_LAMP}", "wait 10", f"expect-flag {STORY_FLAG} 0",
                  "sync sh-away", "sync sh-set", f"wait-flag {STORY_FLAG} 1 60", "sync sh-seen",
                  f"expect-map {CATHEDRAL_WARD}", "expect-session 2"],
    },
    "syncs": {name: {"chapter": "story-hostless", "roles": ["host", "guest"]}
              for name in ("sh-away", "sh-set", "sh-seen")},
    "expect": {"guest": [r"the host's story steps followed here"]},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "timeout": 600,
}


# #289 (Kaniel 2026-10-20: summoned enemies, the chalice bell woman's skeletons and the Witch of
# Hemwick's Mad Ones): a kill is taken back when a map generator brings the same enemy back. The
# Witch's Abode lamp stands in her arena; the rig's saves have her alive once her flag is off.
# The Mad One walking at the start dies in the host's game and comes back about 30 s later (rig
# 2026-10-20): in the guest's game it is killed, then shown and alive again. The guest at the lamp
# is within DiesNearMetres of it, so its copy dies by the game's kill rather than being hidden
# (rig 2026-10-28: "1 near died" on release-1.3.1 too).
HEMWICK = "0x16000000"
WITCH_ABODE_LAMP = 2202951
WITCH_DEFEATED = 12201800
MAD_ONE = "0x1000801b"  # entity 2200810
HEMWICK_START = [START, f"set-flag {WITCH_DEFEATED} 0", f"travel {WITCH_ABODE_LAMP}", "godmode",
                 f"expect-map {HEMWICK}"]
SCENARIOS["mad-one-back"] = {
    "about": "#289: the host kills one of the Witch of Hemwick's Mad Ones; when her generator "
             "brings it back the guest's copy is shown and alive (not hidden as killed)",
    "roles": {
        "host": [*HEMWICK_START, "wait-session 2 300", "wait 10", "census 60", "sync mob-ready",
                 f"kill {MAD_ONE}", "wait 3", f"expect-hp {MAD_ONE} == 0", "sync mob-killed",
                 "wait 25", "wait 20", "census 60", f"expect-hp {MAD_ONE} > 0",
                 f"expect-hidden {MAD_ONE} 0", "sync mob-checked", "expect-session 2"],
        "guest": [*HEMWICK_START, "wait-session 2 300", "wait 10", "census 60", "sync mob-ready",
                  "sync mob-killed", "wait 3", "census 60", f"wait-hp {MAD_ONE} == 0 10",
                  "wait 27", "wait 15", "census 60", f"expect-hp {MAD_ONE} > 0",
                  f"expect-hidden {MAD_ONE} 0 15", f"expect-disabled {MAD_ONE} 0",
                  "sync mob-checked", "expect-session 2"],
    },
    "syncs": {name: {"chapter": "mad-one-back", "roles": ["host", "guest"]}
              for name in ("mob-ready", "mob-killed", "mob-checked")},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "timeout": 300,
}


# Kaniel's chalice playtest 2026-10-25 (v1.2.2): the bell-ringing woman's red skeletons stayed
# invisible to the guests. A guest near the enemy when the host kills it takes the game's kill
# (DiesInsteadOfHiding), and when the host's generator brings it back that copy stayed dead. Now
# the guest's bonfire respawn brings it back too (BroughtBackByTheHost). The host stands closer,
# so it keeps the enemy (nearest player) and its generator is the one that runs.
SCENARIOS["mad-one-back-near"] = {
    "about": "a guest next to a Mad One the host kills sees it die; when the Witch of Hemwick's "
             "generator brings it back in the host's game, it is alive in the guest's game too",
    "roles": {
        "host": [*HEMWICK_START, "wait-session 2 300", "wait 10", f"approach {MAD_ONE} 0 2",
                 "sync near-ready", f"kill {MAD_ONE}", "wait 3", f"expect-hp {MAD_ONE} == 0",
                 "sync near-killed", "wait 25", "wait 20", "census 60", f"expect-hp {MAD_ONE} > 0",
                 "sync near-checked", "expect-session 2"],
        "guest": [*HEMWICK_START, "wait-session 2 300", "wait 10", f"approach {MAD_ONE} 0 8",
                  "sync near-ready", "sync near-killed", "wait 5", f"expect-hp {MAD_ONE} == 0",
                  "wait 20", f"wait-hp {MAD_ONE} > 0 60", "census 60", f"expect-hp {MAD_ONE} > 0",
                  f"expect-hidden {MAD_ONE} 0", f"expect-disabled {MAD_ONE} 0",
                  "sync near-checked", "expect-session 2"],
    },
    "syncs": {name: {"chapter": "mad-one-back-near", "roles": ["host", "guest"]}
              for name in ("near-ready", "near-killed", "near-checked")},
    "expect": {"guest": [r"brought back are dead here on map 0x16000000; bringing them "
                         r"back:[^\n]*0x1000801b"]},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "timeout": 300,
}


# Kaniel's playtest 2026-10-20: only the host saw the Mad Ones the Witch of Hemwick summons. They
# wait switched off with their hits off (2004:39); her summon (m22_00 12204839, waiting on her AI)
# turns them on in the host's game only, and a guest's copy switched on like the host's kept its
# hits off. The guest now copies the host's hits switch: once she summons, the guest turns a Mad
# One's hits on (0x1000801c and 0x1000801d are the ones her summons bring, 0x1000801b the
# opening event's). Her fight starts with a player in area 2202805 (m22_00 12201802 sets
# 12204800); the lamp's warp point is outside it, so the host starts it, and her summons wait for
# her HP (12204839: hers at 30% or less, or her clone's at half or less).
WITCH = "0x10008024"  # entity 2200800
WITCH_CLONE = "0x10008025"  # entity 2200801, switched on as her fight starts
WITCH_FIGHT_STARTED = 12204800
SCENARIOS["witch-summons"] = {
    "about": "the Witch of Hemwick summons Mad Ones in the host's game; the guest's copies get "
             "their hits turned on like the host's",
    "roles": {
        "host": [*HEMWICK_START, "wait-session 2 300", "sync summon-ready",
                 f"set-flag {WITCH_FIGHT_STARTED} 1", "wait 5", f"hurt {WITCH} 700",
                 f"hurt {WITCH_CLONE} 1900", "wait 30", "wait 30", "wait 30", "census 60",
                 "sync summon-checked", "expect-session 2"],
        "guest": [*HEMWICK_START, "wait-session 2 300", "sync summon-ready", "wait 30", "wait 30",
                  "wait 30", "census 60", "shot witch-summons", "sync summon-checked",
                  "expect-session 2"],
    },
    "syncs": {name: {"chapter": "witch-summons", "roles": ["host", "guest"]}
              for name in ("summon-ready", "summon-checked")},
    # A Mad One she summoned, switched on with its hits on (bit 1 of the census' draw word, the
    # model flags 2004:39 writes) in both games.
    "expect": {role: [r"census: 0x1000801[cd] [^\n]* disabled 0 at [^\n]* "
                      r"draw 0x[0-9a-f]*[014589cd] team"] for role in ("host", "guest")},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "timeout": 300,
}

# Kaniel 2026-10-20: back in the Dream the guests' headstones said "no place to awaken". Each
# headstone's talk asks one flag, "a lamp of mine is lit" (12105030-12105034), which m21_00's
# 12105020-12105024 set in each player's own game from its own lamps (12105031: the Forbidden
# Woods lamp 12707810, Hemwick's and Byrgenwerth's; the rig characters have none of them). The
# host lights the woods lamp before the session; the party's lamps are shared (the server world
# gave the guest the woods lamp at the join, rig 2026-10-21), so both have headstones 1 and 2 on.
# As the host's world flags the other's values came in over the game's flag messages and flipped
# them about once a second (hundreds of "the other player sends flag 1210503x" a minute).
HEADSTONE_LIT = 12105030  # + n: headstone n+1's "a lamp of mine is lit"
HEADSTONE_SAMPLES = 8  # one look every 5 s: at a flip a second one of them catches it


def headstones_seen(lit: dict[int, int]) -> list[str]:
    """Each headstone flag as this player's own lamps have it: once the Dream's events set them
    (some seconds after the arrival: a rig run looked at 5 s and found them all still off), then
    every 5 s."""
    return [*(f"wait-flag {flag} 1 60" for flag, on in lit.items() if on),
            *[step for _ in range(HEADSTONE_SAMPLES)
              for step in ("wait 5", *(f"expect-flag {flag} {on}" for flag, on in lit.items()))]]


# The flag messages of a ping-pong (main before #301: 400 in a minute); a few at the arrival are
# each game telling the other its own once. Atomic: each repeat takes the next message only (a
# plain lazy repeat backtracks for ever on a log without 30).
HEADSTONE_FLIPS = (r"the other player sends flag (?:1210503[0-4]|12100122) "
                   r"(?=(?>[\s\S]*?the other player sends flag (?:1210503[0-4]|12100122) ){29})")
SCENARIOS["dream-headstones-lit"] = {
    "about": "#301: host and guest in the Hunter's Dream, the host with a lamp lit before the "
             "session (shared with the guest): headstones 1 and 2 lit for both and staying so, "
             "no flag ping-pong between them",
    "roles": {
        "host": [START, f"set-flag {FORBIDDEN_WOODS_LAMP_LIT} 1", "godmode", "wait-session 2 300",
                 f"travel {HUNTERS_DREAM}", "wait-members 2 240", "sync dream-both",
                 *headstones_seen({HEADSTONE_LIT: 1, HEADSTONE_LIT + 1: 1}),
                 "sync dream-seen", "expect-session 2"],
        "guest": [START, f"expect-flag {FORBIDDEN_WOODS_LAMP_LIT} 0", "godmode",
                  "wait-session 2 300", f"travel {HUNTERS_DREAM}", "wait-members 2 240",
                  "sync dream-both", *headstones_seen({HEADSTONE_LIT: 1, HEADSTONE_LIT + 1: 1}),
                  "sync dream-seen", "expect-session 2"],
    },
    "syncs": {name: {"chapter": "dream-headstones-lit", "roles": ["host", "guest"]}
              for name in ("dream-both", "dream-seen")},
    "forbid": {role: SESSION_HOLDS + [HEADSTONE_FLIPS] for role in ("host", "guest")},
    "timeout": 300,
}


# Kaniel's play test 2026-10-21 (v1.1.1, #301 in): both guests' headstones said "no place to
# awaken" again, the host's worked. Each guest came in by the team screen with its save in the
# Dream: its own lamps lit its headstones there (KeepHeadstoneFlagsOwn "set"), and right after the
# join's reload (still the Dream) every one read off ("cleared: this player's lamps"), for the
# whole session. dream-headstones-lit joins outside the team, in Central Yharnam.
SCENARIOS["dream-headstones-team"] = {
    "about": "a team from the lobby, both saves in the Hunter's Dream before the guest joins (as "
             "in Kaniel's play test): the guest's own lamps still light its headstone after the "
             "join's reload",
    "roles": {
        "host": ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                 "godmode", f"travel {HUNTERS_DREAM}", "wait-session 2 300", "wait-members 2 240",
                 "sync dream-both", *headstones_seen({HEADSTONE_LIT: 1}), "sync dream-seen",
                 "expect-session 2"],
        "guest": ["team-accept", "team-ready", START, "godmode", f"travel {HUNTERS_DREAM}",
                  f"wait-flag {HEADSTONE_LIT} 1 60", "wait-session 2 300", "wait-members 2 240",
                  "sync dream-both", *headstones_seen({HEADSTONE_LIT: 1}), "sync dream-seen",
                  "expect-session 2"],
    },
    "syncs": {name: {"chapter": "dream-headstones-team", "roles": ["host", "guest"]}
              for name in ("dream-both", "dream-seen")},
    "expect": {"guest": [r"Co-op team lobby: the save plays in team \w+ as its guest",
                         r"lamps lit again for this guest \(its own and the team's\):.* 12417810"]},
    "forbid": {role: SESSION_HOLDS + [HEADSTONE_FLIPS,
                                      r"headstone flag 12105030 cleared: this player's lamps"]
               for role in ("host", "guest")},
    "timeout": 300,
}


# Kaniel 2026-10-21: with several players in the Dream the shop messengers rose from the bath and
# played their tune again and again. m21_00 12105040 rises them (2100211-2100213) while the shop
# menu is open (12105041, set by their talk t210691), waits for it to close, sinks them and
# restarts. Kept in step, one player's end of it ended the other's too (its flag turned on with
# that player's shop still open; no "the other player ends event" line: not that message), and the
# restart rose them again. Here the guest keeps its shop open while the host opens and closes its
# own twice, all through the messengers' own talk (the rig's set-flag sends nothing to the others):
# the guest's event is still waiting for its own shop to close (12105040 off).
SHOP_OPEN = 12105041
SHOP_MESSENGERS = 12105040
# West of the bath, facing it: X opens the shop, Circle closes it (rig probe 2026-10-22: not yet
# right after the travel).
OPEN_THE_SHOP = ["approach 0x10000013 -1.6 0 -1.5708", "wait 2", "press cross 200", "wait 2",
                 "press cross 200",
                 f"wait-flag {SHOP_OPEN} 1 10", "wait 3"]
CLOSE_THE_SHOP = [*[step for _ in range(4) for step in ("press circle 200", "wait 1")],
                  f"wait-flag {SHOP_OPEN} 0 10", "wait 3"]
SHOP_MESSENGERS_IN_STEP = r"the other player ends event 12105040 "
SCENARIOS["dream-shop-messengers"] = {
    "about": "host and guest in the Hunter's Dream: the guest's shop open while the host opens "
             "and closes its own twice; the guest's messengers still wait for its own shop",
    "roles": {
        "host": [START, "godmode", "wait-session 2 300", f"travel {HUNTERS_DREAM}",
                 "wait-members 2 240", "wait 25", "sync shop-guest-open",
                 *[step for _ in range(2) for step in (*OPEN_THE_SHOP, *CLOSE_THE_SHOP)],
                 f"expect-flag {SHOP_MESSENGERS} 1", "sync shop-done", "expect-session 2"],
        "guest": [START, "godmode", "wait-session 2 300", f"travel {HUNTERS_DREAM}",
                  "wait-members 2 240", "wait 25", *OPEN_THE_SHOP, "sync shop-guest-open",
                  "sync shop-done", f"expect-flag {SHOP_OPEN} 1",
                  f"expect-flag {SHOP_MESSENGERS} 0", *CLOSE_THE_SHOP,
                  f"expect-flag {SHOP_MESSENGERS} 1", "expect-session 2"],
    },
    "syncs": {name: {"chapter": "dream-shop-messengers", "roles": ["host", "guest"]}
              for name in ("shop-guest-open", "shop-done")},
    "forbid": {role: SESSION_HOLDS + [SHOP_MESSENGERS_IN_STEP] for role in ("host", "guest")},
    "timeout": 300,
}


# #300 (rig test 2026-10-21): the enemies that rise out of the mist in Hemwick's woods (m22_00
# event 12204000, a slot each for 2200500-2200508). On each load event 12204010 arms them
# (MIST_ARMED) for a host with 15 Insight or more (3:26, "SAN値で判定"); unarmed, 12204000 switches
# them off for good and ends (rig 2026-10-21: the rig's characters have less, and the guest then
# showed all nine standing while the host had them off). Armed, it switches them off and waits
# until, at night (flag 9801 or 9802), the local player is within 20 m: then switches them on with
# their rising animation 6200. The wait also wanted the host's character alive (4:3 player type
# 0), so only the host's approach woke them; party_boss_events.py's AMBUSH_EVENTS drops that
# check. 2200507 stands at (-218.9, -21.1, 555.6) (m22_00_00_00.msb), 84 m from the Charnel Lane
# lamp and 210 m from the Witch's Abode lamp, where the host stays.
# Rig 2026-10-21 (port): the guest's approach wakes it in the guest's game with and without that
# check dropped. With both players in the session before Hemwick loads (as here), the server
# world's "shown" record and map flag 12204007 switch the host's copy on too. When the guest had
# loaded Hemwick before the session formed, the host's copy stayed off unless the slot flags
# 12204000-12204008 are shared (#314).
CHARNEL_LANE_LAMP = 2202950
NIGHT = 9801
MIST_ARMED = 12204011
INSIGHT_POINT = 4680  # the SpEffect common event 9350 gives Insight with, a point each
MIST_ENEMY = "0x10008022"  # entity 2200507
BY_MIST_ENEMY = "-216.0 -21.0 552.0 -0.67"  # 4.6 m off, facing it
# Insight and night before Hemwick loads: its events run with them. The trip there waits for the
# session (a travel as the join lands left it, rig 2026-10-21).
MIST_START = [START, f"set-flag {WITCH_DEFEATED} 0",
              *[step for _ in range(15) for step in (f"add-effect {INSIGHT_POINT}", "wait 1")],
              f"set-flag {NIGHT} 1", "wait-session 2 300", "wait 5", f"travel {WITCH_ABODE_LAMP}",
              "godmode", f"expect-map {HEMWICK}", f"wait-flag {MIST_ARMED} 1 10"]
SCENARIOS["mist-ambush-guest"] = {
    "about": "#300: a guest walks up to one of Hemwick's mist ambush enemies while the host stays "
             "at the Witch's Abode lamp: it rises, in both games",
    "roles": {
        "host": [*MIST_START, "wait 10", "census 300",
                 f"expect-disabled {MIST_ENEMY} 1", "sync mist-ready", "sync mist-near",
                 f"expect-disabled {MIST_ENEMY} 0 30", "census 300", "sync mist-checked",
                 "expect-session 2"],
        "guest": [*MIST_START, "wait 5", f"travel {CHARNEL_LANE_LAMP}",
                  "wait 5", "census 150", f"expect-disabled {MIST_ENEMY} 1", "sync mist-ready",
                  f"teleport {BY_MIST_ENEMY}", "wait 3", f"expect-disabled {MIST_ENEMY} 0 15",
                  "census 60", "sync mist-near", "sync mist-checked", "expect-session 2"],
    },
    "syncs": {name: {"chapter": "mist-ambush-guest", "roles": ["host", "guest"]}
              for name in ("mist-ready", "mist-near", "mist-checked")},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "timeout": 300,
}


# Rig 2026-10-21 (mist-ambush-guest's first version): a guest's lamp trip that began as their
# session formed: the join lands about 16.0 s after both games are in the world, and only one
# landing between the warp and the old world's teardown (a second or so) broke it; the mist runs'
# trip 15.8 s in hit it every time. The join landed during its load,
# and its world teardown, seeing no party trip, ended the session ("left the session outside a
# trip"). Joining must hold whenever it lands (Kaniel).
SCENARIOS["join-during-trip"] = {
    "about": "a guest's lamp trip is under way when the session forms: it arrives in the session",
    "roles": {
        "host": [START, "godmode", "wait-session 2 300", "sync trip-arrived", "wait 10",
                 "expect-session 2"],
        "guest": [START, "godmode", "wait 15.7", f"travel {WITCH_ABODE_LAMP}",
                  f"expect-map {HEMWICK}", "wait-session 2 300", "sync trip-arrived", "wait 10",
                  "expect-session 2"],
    },
    "syncs": {"trip-arrived": {"chapter": "join-during-trip", "roles": ["host", "guest"]}},
    "forbid": {"host": SESSION_HOLDS, "guest": [*SESSION_HOLDS, *CRASH_CLEANUP]},
    "timeout": 300,
}


# Kaniel's playtest 2026-10-20: the Witch of Hemwick flickered for krolebord (a guest). Her vanish
# (m22_00 event 12204812, waiting on her own AI) switches her off and back on in every game, and
# the guest switched its copies along with the host's board too, out of step with its own fight.
# The host starts her fight (its area 2202805 is away from the lamp: 12204800 sets 12201802) and
# keeps hitting her; the guest's own fight alone switches her and her clone.
HEMWICK_WITCH = "0x10008024"  # entity 2200800
HEMWICK_WITCH_CLONE = "0x10008025"  # entity 2200801
SCENARIOS["witch-flicker"] = {
    "about": "the host fights the Witch of Hemwick; a guest's copies of her and her clone are "
             "left to its own fight, never switched along with the host's board",
    "roles": {
        "host": [*HEMWICK_START, "wait-session 2 300", "sync flicker-ready", "set-flag 12204800 1",
                 "wait 5", *[step for _ in range(6) for step in (f"hit {HEMWICK_WITCH} 15",
                                                                 "wait 5")],
                 "census 60", "sync flicker-checked", "expect-session 2"],
        "guest": [*HEMWICK_START, "wait-session 2 300", "sync flicker-ready", "wait 30", "wait 30",
                  "census 60", "sync flicker-checked", "expect-session 2"],
    },
    "syncs": {name: {"chapter": "witch-flicker", "roles": ["host", "guest"]}
              for name in ("flicker-ready", "flicker-checked")},
    "forbid": {"host": SESSION_HOLDS,
               "guest": [*SESSION_HOLDS, r"characters switched like the host has them on map "
                                         r"0x16000000 \(off, \+on\):[^\n]* \+?0x1000802[45]\b"]},
    "timeout": 300,
}


# Unarmed (the rig's characters have under 15 Insight): the host's ambush event switches all nine
# off for good and ends, its slot done flags on. The guest's copies stayed on (rig 2026-10-21: the
# guest saw nine enemies the host did not), since the host's switch is not followed far from the
# host; with the slot done it is (AmbushSlotFlag).
SCENARIOS["mist-ambush-unarmed"] = {
    "about": "#300: with the ambush unarmed (host under 15 Insight) the guest by Hemwick's woods "
             "sees its enemies off, as the host has them",
    "roles": {
        "host": [START, f"set-flag {WITCH_DEFEATED} 0", f"set-flag {NIGHT} 1",
                 "wait-session 2 300", "wait 5", f"travel {WITCH_ABODE_LAMP}", "godmode",
                 f"expect-map {HEMWICK}", f"expect-flag {MIST_ARMED} 0", "wait 10", "census 300",
                 f"expect-disabled {MIST_ENEMY} 1", "sync unarmed-checked", "expect-session 2"],
        "guest": [START, f"set-flag {WITCH_DEFEATED} 0", f"set-flag {NIGHT} 1",
                  "wait-session 2 300", "wait 5", f"travel {WITCH_ABODE_LAMP}", "godmode",
                  f"expect-map {HEMWICK}", "wait 5", f"travel {CHARNEL_LANE_LAMP}", "wait 5",
                  "census 150", f"expect-disabled {MIST_ENEMY} 1 20", "census 150",
                  "sync unarmed-checked", "expect-session 2"],
    },
    "syncs": {"unarmed-checked": {"chapter": "mist-ambush-unarmed", "roles": ["host", "guest"]}},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "timeout": 300,
}


# Kaniel's playtest 2026-10-20: the Witch's Abode lamp broke for the whole party. Its "light it"
# request (ABODE_LAMP_REQUEST, common event 7100: lights the lamp once, and ends at once on a load
# with the request on) was set while she lived and lit nothing; it reached the host, and the
# host's reached the guests as a story step. Here the guest's world starts that way (request on,
# lamp unlit): loading Hemwick clears it. The host then lights the lamp after her kill: the guest
# never takes the host's request, and the lamp reaches its lamp list.
ABODE_LAMP_REQUEST = 72200201
ABODE_LAMP_ACTIVE = 12207830  # the lamp's active flag: lit for this player
SCENARIOS["abode-lamp-broken"] = {
    "about": "a guest's Abode lamp light request that lit nothing is cleared as Hemwick loads; "
             "the host lights the lamp after the Witch's kill and the guest gets it lit, never the "
             "host's request",
    "roles": {
        "host": [START, f"set-flag {WITCH_DEFEATED} 1", f"travel {WITCH_ABODE_LAMP}", "godmode",
                 f"expect-map {HEMWICK}", "wait-session 2 300", "sync lamp-ready",
                 f"set-flag {ABODE_LAMP_REQUEST} 1", f"wait-flag {ABODE_LAMP_ACTIVE} 1 20",
                 "sync lamp-lit", "sync lamp-checked", "expect-session 2"],
        "guest": [START, f"set-flag {WITCH_DEFEATED} 1", f"set-flag {ABODE_LAMP_ACTIVE} 0",
                  f"set-flag {ABODE_LAMP_REQUEST} 1", f"travel {WITCH_ABODE_LAMP}", "godmode",
                  f"expect-map {HEMWICK}", f"wait-flag {ABODE_LAMP_REQUEST} 0 10",
                  "wait-session 2 300", "sync lamp-ready", "sync lamp-lit", "wait 15",
                  f"expect-flag {ABODE_LAMP_ACTIVE} 1", f"expect-flag {ABODE_LAMP_REQUEST} 0",
                  "sync lamp-checked", "expect-session 2"],
    },
    "syncs": {name: {"chapter": "abode-lamp-broken", "roles": ["host", "guest"]}
              for name in ("lamp-ready", "lamp-lit", "lamp-checked")},
    "expect": {"guest": [r"lamp light requests that lit nothing cleared on map 0x16000000: "
                         rf"{ABODE_LAMP_REQUEST}\b",
                         r"lamps[^\n]* 2201951[^\n]* are lit for this player too"]},
    "forbid": {"host": SESSION_HOLDS,
               "guest": [*SESSION_HOLDS,
                         rf"the host's story steps followed here[^\n]* {ABODE_LAMP_REQUEST}\b"]},
    "timeout": 300,
}


# Kaniel's playtest 2026-10-20: during the Witch of Hemwick fight krolebord (a guest) saw the Witch's
# Abode lamp, which shows only once she is dead (m22_00 common event 7000: character 2200951 and
# object 2201951 switched off until flag 12201800), while the host was far; near the host it was
# gone again. The lamp's character stayed off in his log; these look at the lamp object too
# (`object`, its enable switch), with Charnel Lane's lit lamp (2201950) beside it.
ABODE_LAMP_CHARACTER = 2200951
LAMP_OBJECTS = "object 2201951 2201950"
ABODE_LAMP_SHOWN = [rf"disabled 0 at [^\n]* entity {ABODE_LAMP_CHARACTER}\b",
                    r"autoplay object 2201951: enabled 1"]
SCENARIOS["abode-lamp-far"] = {
    "about": "the Witch's Abode lamp (character and object) stays off for a guest by it while the "
             "Witch lives, with the host far away and with the host beside it",
    "roles": {
        "host": [START, f"set-flag {WITCH_DEFEATED} 0", f"travel {CHARNEL_LANE_LAMP}", "godmode",
                 f"expect-map {HEMWICK}", "wait-session 2 300", "sync lamp-ready", "wait 20",
                 "census 60", LAMP_OBJECTS, "sync lamp-far", f"travel {WITCH_ABODE_LAMP}",
                 "wait 15", "census 60", LAMP_OBJECTS, "sync lamp-near", "wait 10",
                 "sync lamp-checked", f"expect-flag {WITCH_DEFEATED} 0", "expect-session 2"],
        "guest": [*HEMWICK_START, "wait 2", LAMP_OBJECTS, "wait 5", LAMP_OBJECTS,
                  "wait-session 2 300", LAMP_OBJECTS,
                  "sync lamp-ready", "wait 10", LAMP_OBJECTS, "wait 10", "census 60", LAMP_OBJECTS,
                  "sync lamp-far", "sync lamp-near", "wait 5", "census 60", LAMP_OBJECTS,
                  "sync lamp-checked", f"expect-flag {WITCH_DEFEATED} 0", "expect-session 2"],
    },
    "syncs": {name: {"chapter": "abode-lamp-far", "roles": ["host", "guest"]}
              for name in ("lamp-ready", "lamp-far", "lamp-near", "lamp-checked")},
    "expect": {role: [r"autoplay object 2201951: enabled 0"] for role in ("host", "guest")},
    "forbid": {"host": SESSION_HOLDS, "guest": [*SESSION_HOLDS, *ABODE_LAMP_SHOWN]},
    "timeout": 300,
}
# The playtest's order: both at the Abode with the fight on (her fight starts with flag 12204800),
# then the host leaves for Charnel Lane and comes back.
SCENARIOS["abode-lamp-host-leaves"] = {
    "about": "the Witch's Abode lamp (character and object) stays off for a guest by it while the "
             "Witch lives, after the host was beside it, once the host leaves and once it is back",
    "roles": {
        "host": [*HEMWICK_START, "wait-session 2 300", "set-flag 12204800 1", "sync lamp-ready",
                 "wait 10", "census 60", LAMP_OBJECTS, f"travel {CHARNEL_LANE_LAMP}", "wait 20",
                 "sync lamp-far", "sync lamp-far-checked", f"travel {WITCH_ABODE_LAMP}", "wait 10",
                 "sync lamp-near", "sync lamp-checked", f"expect-flag {WITCH_DEFEATED} 0",
                 "expect-session 2"],
        "guest": [*HEMWICK_START, "wait-session 2 300", "sync lamp-ready", "wait 10", "census 60",
                  LAMP_OBJECTS, "sync lamp-far", "wait 5", "census 60", LAMP_OBJECTS, "wait 10",
                  LAMP_OBJECTS, "sync lamp-far-checked", "sync lamp-near", "wait 5", "census 60",
                  LAMP_OBJECTS, "sync lamp-checked", f"expect-flag {WITCH_DEFEATED} 0",
                  "expect-session 2"],
    },
    "syncs": {name: {"chapter": "abode-lamp-host-leaves", "roles": ["host", "guest"]}
              for name in ("lamp-ready", "lamp-far", "lamp-far-checked", "lamp-near",
                           "lamp-checked")},
    "expect": {"guest": [r"autoplay object 2201951: enabled 0"]},
    "forbid": {"host": SESSION_HOLDS, "guest": [*SESSION_HOLDS, *ABODE_LAMP_SHOWN]},
    "timeout": 300,
}


# #329 (rig test 2026-10-21): common event 5500 (a slot per Uncanny/Lost DLC weapon) ends unless
# the local player is a host-type character, and an ended event sets its own flag, so a guest's
# game turned 5500-5521 on at the join's load and its Bath messenger sold them all.
# KeepOwnedWeaponFlagsOwn clears them again (the rig's characters hold none of those weapons).
DLC_WEAPON_FLAGS = range(5500, 5522)
SCENARIOS["shop-own-weapons"] = {
    "about": "#329: on new characters (rig/saves/new-character: no DLC weapons), a guest joins "
             "the host's session: the Uncanny/Lost DLC weapon flags 5500-5521 end up off in the "
             "guest's game",
    "saves": "new-character",
    "roles": {
        "host": ["godmode", "wait-session 2 300", "sync shop-checked",
                 *[f"expect-flag {flag} 0" for flag in DLC_WEAPON_FLAGS], "expect-session 2"],
        "guest": ["godmode", "wait-session 2 300", "wait 15",
                  *[f"expect-flag {flag} 0" for flag in DLC_WEAPON_FLAGS], "wait 20",
                  *[f"expect-flag {flag} 0" for flag in DLC_WEAPON_FLAGS], "sync shop-checked",
                  "expect-session 2"],
    },
    "syncs": {"shop-checked": {"chapter": "shop-own-weapons", "roles": ["host", "guest"]}},
    "forbid": {role: SESSION_HOLDS for role in ("host", "guest")},
    "timeout": 300,
}


# Kaniel's playtest 2026-10-22 (v1.1.7): the party of three (harder enemies x3) wiped fighting
# Darkbeast Paarl (m23_00, entity 2300810, on map 0x17000000) and woke at a lamp on the next map
# (0x1c000000, 2802952); walking back across the border, every game had Paarl at 0 HP and his
# defeat event (12301700: it waits for him dead) gave the kill, the item and the echoes. Not
# reproduced here yet: this checks he stands. In the rig the guests woke with his health bar still
# up (a party death skipped the game's Failed_BossAreaMission): every game clears it.
PAARL_LAMP_MAP = "0x1c000000"
PAARL_LAMP = 2802952
PAARL_MAP = "0x17000000"
PAARL = "0x1000c05a"
PAARL_DEFEATED = 12301700
# Positions are each map's own. The tunnel from the lamp's map (Kaniel's trail): this side of the
# border, then the other; a teleport from the lamp straight into the arena fell through the world.
INTO_PAARL_MAP = ["teleport 164.1 -114.3 -99.0", "wait 3", "teleport 156.7 -115.4 -83.9",
                  f"wait-map {PAARL_MAP} 20"]
# Where Kaniel fell in the arena (his bloodstain).
PAARL_SPOT = "109.6 -120.7 -56.3"
# Everything his defeat sets, off (the rig's saves had his arena lamp to light mid-fight, Kaniel
# 2026-10-22): 9454 (the boss down), 2301 (his lamp), 12301701 (after the defeat).
PAARL_FRESH = [f"set-flag {flag} 0" for flag in (PAARL_DEFEATED, 12301701, 9454, 2301)]
PAARL_START = [START, *PAARL_FRESH, f"travel {PAARL_LAMP}", "godmode",
               f"expect-map {PAARL_LAMP_MAP}"]
# His fight's flags (12304700 the fog entered, 12304702 and 12304704 his start): every load puts
# them off, and the team's world set them again in the guests' games, so his fight started over
# at the lamp (his health bar up for them, Kaniel watching 2026-10-23).
PAARL_FIGHT_OFF = [f"expect-flag {flag} 0" for flag in (12304700, 12304702, 12304704)]
PAARL_BACK = [f"ensure-map {PAARL_LAMP_MAP} {PAARL_LAMP} 60", "wait-members 2 120", "wait 15",
              f"expect-flag {PAARL_DEFEATED} 0", *PAARL_FIGHT_OFF, *INTO_PAARL_MAP, "wait 10",
              "census 80", f"expect-flag {PAARL_DEFEATED} 0", f"expect-hp {PAARL} > 0"]
GUEST_TO_PAARL = [*PAARL_START, "wait-session 3 300", "wait 5", *INTO_PAARL_MAP, "wait 5"]
# The guests stay by the border until the host has started the fight beside him, so the boss pick
# (the nearest member) is the host's game, as in Kaniel's fight. A guest picked right after the
# arena loaded did not take him while its vitals board was stale, and no game ran him (he stood
# still, rig 2026-10-23).
# Paarl's own attacks put each one down (Kaniel's party died to him): beside him with 1 HP.
TO_PAARL_HITS = [f"approach {PAARL} 3 0", "hurt self 1"]
GUEST_WIPE = ["sync pw-hurt", "godmode off", *TO_PAARL_HITS, "wait-vital down 90"]
# Back up, godmode again: the lamp's enemies killed the guests a second time (Kaniel watching,
# 2026-10-23).
GUEST_BACK = ["wait-vital dead 90", "wait-vital up 150", "godmode",
              f"ensure-map {PAARL_LAMP_MAP} {PAARL_LAMP} 60", "wait-members 3 120",
              "sync pw-back", *PAARL_BACK[1:], "sync pw-checked", "expect-session 3"]
# A host alone dies by a fall, from wipe3's open-sky spot by the first Central Yharnam lamp: a rise
# from the lamp itself lands on a ledge 11 m above the ground there, and after a respawn no rise
# from the lamp killed (rig 2026-10-24).
SOLO_DEATH = [f"teleport {WIPE3_SPOT['host']}", "wait 2", "hurt self 1", "rise 40",
              "wait-vital dead 30"]
SCENARIOS["solo-death-tick"] = {
    "about": "AtiniirSpectre 2026-10-23 (v1.2.0): a host alone stuttered after a death, its "
             "co-op tick's longest call 21 ms before and 111 ms after. The host plays a minute "
             "and dies three times, 100 s apart: the co-op tick's slowest parts and the busiest "
             "threads (beside each Perf line) before and after each death say what grew",
    "settings": PARTY_DEATH,
    "roles": {
        "host": [START, "wait 70", *SOLO_DEATH, "wait 100", *SOLO_DEATH, "wait 100", *SOLO_DEATH,
                 "wait 100", "snapshot"],
    },
    "expect": {"host": [r"Co-op tick slowest parts: "]},
    "console_expect": {"host": [r"Runtime: CPU over the window: .*busiest: GameMain"]},
    "timeout": 720,
}

SCENARIOS["paarl-after-wipe"] = {
    "about": "Kaniel 2026-10-22: the party of three hurts Paarl and wipes on his map, wakes at the "
             "lamp on the next map and walks back across the border: Paarl stands, his defeat "
             "flag off",
    "settings": {**PARTY_DEATH, "SHADPS4_BLOODBORNE_HARDER_ENEMIES": "3,3,1.5"},
    "roles": {
        "host": [*PAARL_START, "wait-session 3 300", "wait 5", *INTO_PAARL_MAP, "wait 5",
                 f"teleport {PAARL_SPOT}", "wait 8", "census 40", f"hurt {PAARL} 9000",
                 "wait 3", "census 40", f"expect-hp {PAARL} < 10000", "sync pw-hurt",
                 "sync pw-guests-down", "godmode off", *TO_PAARL_HITS, "wait-vital dead 90",
                 "wait-vital up 150", "godmode", f"ensure-map {PAARL_LAMP_MAP} {PAARL_LAMP} 60",
                 "wait-members 3 120", "sync pw-back", *PAARL_BACK[1:], "sync pw-checked",
                 "expect-session 3"],
        "guest": [*GUEST_TO_PAARL, *GUEST_WIPE,
                  "sync pw-guests-down", *GUEST_BACK],
        "guest2": [*GUEST_TO_PAARL, *GUEST_WIPE,
                   "sync pw-guests-down", *GUEST_BACK],
    },
    "syncs": {name: {"chapter": "paarl-after-wipe", "roles": ["host", "guest", "guest2"]}
              for name in ("pw-hurt", "pw-guests-down", "pw-back", "pw-checked")},
    "expect": {role: [rf"boss bars cleared: {PAARL}"] for role in ("guest", "guest2")},
    "forbid": {role: SESSION_HOLDS for role in ("host", "guest", "guest2")},
    "timeout": 900,
}

# Build check on join (Kaniel 2026-10-24, 10-25): only another Seamless Co-op release is refused
# (build-check-version: the guest says another BB_RELEASE_VERSION; the server names both versions
# on its team screen and in its log, and it plays alone). A game whose settings change the game
# (harder enemies here) links beside one without them and plays in its team; its team screen and
# log say its installation differs (build-check-harder). Games on the same settings link with no
# warning (build-check-same).
HARDER = {"SHADPS4_BLOODBORNE_HARDER_ENEMIES": "3,1.5,3"}
LINKED = r"Server link: the server took this game's hello"
DIFFERS = r"installation differs"
# Kaniel 2026-10-25: team duels, from the team screen's Duel (no bell). The host proposes, the guest
# answers its box yes; after the countdown the host strikes the guest (its HP put at 1 first), whose
# lethal hit puts it out of the duel: no party death (no down, no wipe, no map reset, no lamp), the
# YOU DIED and ADVERSARY EXPUNGED banners, and the guest's HP given back at the end. A second duel
# the host cancels: both friendly again at once.
DUEL_HP_FLOOR = 594  # the rig hunters' max HP: the guest has it all back after the duel
# Consumables the duel uses (EquipParamGoods): Throwing Knives, Molotov Cocktails; Pebbles picked up.
DUEL_KNIVES, DUEL_MOLOTOVS, DUEL_PEBBLES = "goods:1240", "goods:1200", "goods:1230"
# What a party death does, none of it in a duel: a down, a real death, a wipe, a trip or move to a
# lamp.
DUEL_NO_DEATH = [r"Bloodborne party death: down \d", r"died for real", r"killed while down",
                 r"Bloodborne party wipe", r"Bloodborne revive: (respawn point|moved to last lamp)"]
SCENARIOS["duel"] = {
    "about": "team duels: proposed from the team screen, accepted in the world, a lethal hit puts "
             "the guest out (no party death, wipe or map reset), the end gives its HP back; a "
             "second duel cancelled from the menu",
    "roles": {
        "host": [START, "godmode", "wait-session 2 300", "wait 5", "sync together",
                 "duel-propose", "wait-duel countdown 40", "wait-duel fighting 10",
                 "sync fighting", "sync guest-low", "duel-strike 60", "wait-duel none 20",
                 "sync over", "wait 3", "duel-propose", "wait-duel countdown 40",
                 "wait-duel fighting 10", "wait 2", "duel-cancel", "wait-duel none 10",
                 "sync cancelled", "wait 3", "snapshot", "expect-session 2"],
        "guest": [START, "wait-session 2 300", "shot coop-hint", "wait 5", "note-max-hp",
                  # Kaniel 2026-10-25: every consumable used in the duel comes back, picked-up
                  # ones stay (knives and Molotovs taken out stand for thrown ones).
                  f"trade-stock {DUEL_KNIVES} 10", f"trade-stock {DUEL_MOLOTOVS} 3",
                  f"trade-stock {DUEL_PEBBLES} 0", "sync together",
                  "duel-answer yes 40", "wait-duel countdown 10", "wait 1", "shot duel-countdown",
                  "wait-duel fighting 10", f"trade-stock {DUEL_KNIVES} 4",
                  f"trade-stock {DUEL_MOLOTOVS} 0", f"trade-give {DUEL_PEBBLES} 5",
                  # Two duelists: the guest out ends it at once (its HP 1 then, as logged).
                  "sync fighting", "hurt self 1", "sync guest-low", "wait-duel none 60",
                  "sync over", f"expect-held {DUEL_KNIVES} 10 5",
                  f"expect-held {DUEL_MOLOTOVS} 3 5", f"expect-held {DUEL_PEBBLES} 5 5",
                  f"expect-hp self >= {DUEL_HP_FLOOR}", "duel-answer yes 40",
                  "wait-duel fighting 20", "wait-duel none 20", "sync cancelled", "wait 3",
                  "snapshot", "expect-session 2", "expect-max-hp self == noted"],
    },
    "syncs": {name: {"chapter": "duel", "roles": ["host", "guest"]}
              for name in ("together", "fighting", "guest-low", "over", "cancelled")},
    "expect": {
        "host": [r"Co-op duel: duel 1 fights",
                 r"Co-op duel: body 0x[0-9a-f]+ \(team \d+\) is a foe's",
                 r"Co-op duel: duel 1 is over: WON, this player's side won",
                 r"Co-op duel: body 0x[0-9a-f]+ takes its team \d+ back",
                 r"Co-op duel: duel 2 is over: CANCELLED"],
        "guest": [r"Co-op duel: countdown 3 ", r"Co-op duel: countdown 2 ",
                  r"Co-op duel: countdown 1 ", r"Co-op duel: countdown fight",
                  r"Co-op duel: this player is out of duel 1 \(no party death\)",
                  r"Co-op duel: duel 1 is over: WON, this player lost",
                  rf"Co-op duel: given back \(the duel ended\): hp 1 -> {DUEL_HP_FLOOR}, goods "
                  r"\[(?=[^]]*1240 x6)(?=[^]]*1200 x3)",
                  r"Co-op duel: duel 2 is over: CANCELLED"],
    },
    "forbid": {role: SESSION_HOLDS + DUEL_NO_DEATH for role in ("host", "guest")},
    "timeout": 300,
}


SCENARIOS["build-check-version"] = {
    "about": "the guest says another Seamless Co-op release (BB_RELEASE_VERSION 0.0.1, the host "
             "dev): the server refuses the guest's link, saying both versions",
    "role_settings": {"guest": {"BB_RELEASE_VERSION": "0.0.1"}},
    "start_after": {"guest": ["host", LINKED]},
    "roles": {
        "host": ["team-alone", START, "wait 5", "sync bc-refused", "done"],
        "guest": ["team-screen", "title-wait 10", "title-shot refused", "team-alone", START,
                  "wait 5", "sync bc-refused", "done"],
    },
    "syncs": {"bc-refused": {"chapter": "build-check-version", "roles": ["host", "guest"]}},
    "expect": {
        "host": [LINKED, r"Co-op: build fingerprint bb-build 1;version=dev;"],
        "guest": [r"Co-op: build fingerprint bb-build 1;version=0\.0\.1;",
                  r"the server refused this game: Co-op refused: your game is version 0\.0\.1, "
                  r"the games here run dev: update in the launcher \| another build: your game "
                  r"is version 0\.0\.1, the games here run dev: update in the launcher"],
    },
    # (The guest's sign-in hello says no build yet and is welcomed: its build hello is refused.)
    "forbid": {"host": [r"refused this game", DIFFERS], "guest": [DIFFERS]},
    "timeout": 300,
}
SCENARIOS["build-check-harder"] = {
    "about": "the guest has harder enemies (HP x3 damage x1.5 poise x3), the host the normal "
             "game: the guest links and plays in the host's team, warned that its installation "
             "differs (its team screen and log)",
    "role_settings": {"guest": HARDER},
    "start_after": {"guest": ["host", LINKED]},
    "roles": {
        "host": ["team-create", "team-invite rig_guest", "team-wait 2", "team-start", START,
                 "wait-session 2 300", "wait 5", "expect-session 2"],
        "guest": ["team-screen", "title-wait 10", "title-shot warned", "title-wait 3",
                  "team-accept", "team-ready", START, "wait-session 2 300", "wait 5",
                  "expect-session 2"],
    },
    "expect": {
        "host": [LINKED, r"Co-op: build fingerprint bb-build 1;.*;harder=off;"],
        "guest": [LINKED, r"Co-op: build fingerprint bb-build 1;.*;harder=3,1\.5,3;",
                  r"Server link: this game's installation differs: Your installation differs "
                  r"from the others \(harder enemies\): co-op may misbehave \| your "
                  r"installation differs from the games here: harder-enemies setting HP x3 "
                  r"damage x1\.5 poise x3, theirs off"],
    },
    "forbid": {"host": [r"refused this game", DIFFERS, *SESSION_HOLDS],
               "guest": [r"refused this game", *SESSION_HOLDS]},
    "timeout": 300,
}
SCENARIOS["build-check-same"] = {
    "about": "host and guest both with harder enemies: the same build, they link and play in "
             "one session",
    "settings": HARDER,
    "roles": {
        "host": [START, "wait-session 2 300", "wait 5", "expect-session 2"],
        "guest": [START, "wait-session 2 300", "wait 5", "expect-session 2"],
    },
    "expect": {role: [LINKED, r"Co-op: build fingerprint bb-build 1;.*;harder=3,1\.5,3;"]
               for role in ("host", "guest")},
    "forbid": {role: [r"refused this game", DIFFERS, *SESSION_HOLDS]
               for role in ("host", "guest")},
    "timeout": 300,
}

# Kaniel 2026-10-24: every aspect ratio. One visible host (the guest headless beside it, for the
# party list, the compass and a teammate's nametag): the HUD at the output's edges, the party list
# beside the player's bars, the menu's dimming over the whole output, a lock-on marker on its enemy.
ASPECT_HUD_HOST = [START, "godmode", "wait-session 2 300", "wait 25", "shot host-hud",
                   "press options", "wait 2", "shot host-menu", "press circle", "wait 3",
                   f"approach {ENEMY} 0 2 3.14", "wait 2", f"hit {ENEMY} 20", "press r3", "wait 2",
                   "shot host-lockon",
                   "press r3", "wait 2", "expect-session 2", "sync aspect-shots"]
ASPECT_HUD_GUEST = [START, "godmode", "wait-session 2 300", "sync aspect-shots", "expect-session 2"]
SCENARIOS["aspect-hud-wide"] = {
    "about": "a 21:9 output (1680x720): the game sees more to the sides, its HUD at the edges, the "
             "party list beside the player's bars, the menu's dimming and a lock-on marker "
             "(pictures; run visible, not --headless: the guest is headless by itself)",
    "settings": {"ini.output_res": "1680x720", "ini.aspect_ratio": "auto", "rig.headless_roles": "guest"},
    "roles": {"host": ASPECT_HUD_HOST, "guest": ASPECT_HUD_GUEST},
    "syncs": {"aspect-shots": {"chapter": "aspect-hud", "roles": ["host", "guest"]}},
    "console_expect": {"host": [r"the camera at 5 of 5 places", r"Display: aspect ratio 2\.333 \(wider",
                                r"HUD: anchored to the output's edges"]},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "timeout": 300,
}
SCENARIOS["aspect-hud-16x9"] = {
    "about": "the 16:9 regression of aspect-hud-wide at 1280x720: the game's own picture and HUD",
    "settings": {"ini.output_res": "1280x720", "ini.aspect_ratio": "auto", "rig.headless_roles": "guest"},
    "roles": {"host": ASPECT_HUD_HOST, "guest": ASPECT_HUD_GUEST},
    "syncs": {"aspect-shots": {"chapter": "aspect-hud", "roles": ["host", "guest"]}},
    "console_expect": {"host": [r"the camera at 5 of 5 places", r"Display: aspect ratio 1\.778 \(the game's\)"]},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "timeout": 300,
}
# Kaniel 2026-10-26: at 4:3 960x720 the team screen ran off both sides (the overlay's scale stops at
# 0.75); it scales down to the window's width. The Insert menu's Co-op Team row (its rows end Co-op
# Team, Map, Quit) opens it over the game, Circle closes it. Run visible (the guest is headless).
SCENARIOS["aspect-team-43"] = {
    "about": "a 4:3 output (960x720): the team screen opened from the Insert menu fits the window "
             "(pictures), Circle closes it back to the game; the start's resolution line",
    "settings": {"ini.output_res": "960x720", "ini.aspect_ratio": "auto", "rig.headless_roles": "guest"},
    "roles": {
        "host": [START, "godmode", "wait-session 2 300", "wait 25", "shot host-hud",
                 "keys toggle up up up enter", "wait 3", "shot host-team", "keys circle", "wait 3",
                 "shot host-team-closed", "expect-session 2", "sync aspect-shots"],
        "guest": ASPECT_HUD_GUEST,
    },
    "syncs": {"aspect-shots": {"chapter": "aspect-hud", "roles": ["host", "guest"]}},
    "expect": {"host": [r"Co-op team menu: the team screen is open from the port's Insert menu",
                        r"Co-op team menu: the team screen is closed"]},
    "console_expect": {"host": [r"Display: aspect ratio 1\.333 \(taller",
                                r"Resolution: ini 960x720, window windowed, desktop \d+x\d+ -> output 960x720, "
                                r"scene 960x720 \(startup patch\), aspect 1\.333",
                                r"Resolution at start: ini 960x720, window windowed"]},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "timeout": 300,
}

# Kaniel 2026-10-25: frame generation to look at (#395: the generated frames wait for the GPU,
# the status says what really runs). One visible host with frame generation on (FSR 3, x2),
# the guest headless beside it; run with --play, not --headless.
SCENARIOS["frame-gen-look"] = {
    "about": "a visible host with frame generation on (FSR 3, x2) at 1280x720 and a headless guest "
             "joined: to look at the generated frames and the FPS status (run with --play)",
    "settings": {"ini.frame_generation": "1", "ini.frame_gen_provider": "fsr3",
                 "ini.frame_gen_multiplier": "2", "rig.headless_roles": "guest"},
    "roles": {"host": [START, "wait-session 2 300", "wait 10", "expect-session 2"],
              "guest": [START, "wait-session 2 300", "wait 10", "expect-session 2"]},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "timeout": 300,
}

# Audit 2026-10-25 (Kaniel: "everyone is moved but only the host sees the cutscene"): Cainhurst's
# arrival cutscene (m25_00 event 12500000, cutscene 25000000) ended for a client before it
# played: the host's cutscene player went Init, Prologue, Update, Epilog, the guest's stayed at
# Init. Its own flag off, both arrive and both see it.
CAINHURST_LAMP = 2502950
CAINHURST_ARRIVAL = 12500000
SCENARIOS["guest-cutscene-cainhurst"] = {
    "about": "both arrive in Cainhurst with its arrival cutscene unseen: the guest sees it too",
    "roles": {
        role: [START, "godmode", f"set-flag {CAINHURST_ARRIVAL} 0", "wait-session 2 240",
               "wait 20", f"travel {CAINHURST_LAMP}", "wait 30", "expect-session 2"]
        for role in ("host", "guest")
    },
    "expect": {role: [r"cutscene player's step is Update"] for role in ("host", "guest")},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "timeout": 300,
}

# Kaniel 2026-10-25 ("that should be an option to configure instead of nightreign deaths, this one
# should be off by default"): the team's death mode. The leader picks Spectate on the team's lobby
# (death-mode), the guest's game takes it from its own lobby (wait-death-mode); the guest's lethal
# fall while the host stands leaves it out of the fight, its camera over the host's shoulder, until
# it wakes at its own lamp, without a reload.
SPECTATE_HOST = [START, "godmode", "wait-session 2 300", "death-mode spectate 60",
                 "wait-member-vital spectate 120"]
SPECTATE_GUEST = [START, "wait-session 2 300", "wait-death-mode spectate 90", "wait 5",
                  f"teleport {WIPE3_SPOT['guest']}", "wait 3", *LETHAL_FALL,
                  "wait-vital spectate 30", "expect-hp self > 0", "wait 3", "shot spectating",
                  "press r1 200", "wait 2"]
SPECTATE_OUT = r"spectate: out of the fight on map 0x18010000 \(the team's death mode\)"
SPECTATE_SEEN = r"spectate: .+ spectates: its character 0x[0-9a-f]+ here is unseen"
SPECTATE_FORBID = [r"party death: down \d+", r"party guest died"]
SCENARIOS["spectate-lamp"] = {
    "about": "the team spectates (the leader's pick): the guest's lethal fall while the host stands "
             "is no down: it watches the host (camera over the host's shoulder, unseen in the host's "
             "world) until the host uses a lamp (the rig counts the lamp use: its X at a lamp brings "
             "no prompt), then wakes at its own lamp with its HP, no reload",
    "settings": PARTY_DEATH,
    "roles": {
        "host": [*SPECTATE_HOST, "wait 12", "lamp-use", "wait-member-vital up 60", "wait 5",
                 "snapshot", "expect-session 2"],
        "guest": [*SPECTATE_GUEST, "wait-vital up 90", "wait 5", "snapshot", "expect-hp self > 1",
                  "expect-session 2"],
    },
    "expect": {
        "host": [r"Co-op team lobby: the menu asks DEATH_MODE 1", SPECTATE_SEEN,
                 r"spectate: player 0x[0-9a-f]+ is back in the fight: shown again"],
        "guest": [SPECTATE_OUT, r"spectate: watching .+ \(1 of 1 teammates standing here\)",
                  r"Bloodborne camera: over .+'s shoulder",
                  r"spectate: a teammate's lamp wakes this player at its own lamp \d+ \(moved there\)",
                  r"Bloodborne camera: the game's camera is its own again"],
    },
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS + SPECTATE_FORBID + [r"reset=true"]},
    "timeout": 300,
}
SCENARIOS["spectate-give-up"] = {
    "about": "spectating, the guest holds Circle 3 s: it wakes at its own lamp at once (no death, no "
             "reload), the host still standing",
    "settings": PARTY_DEATH,
    "roles": {
        "host": [*SPECTATE_HOST, "wait-member-vital up 60", "wait 5", "snapshot", "expect-session 2"],
        "guest": [*SPECTATE_GUEST, "press circle 3600", "wait-vital up 15", "wait 5", "snapshot",
                  "expect-hp self > 1", "expect-session 2"],
    },
    "expect": {
        "guest": [SPECTATE_OUT, r"spectate: giving up wakes this player at its own lamp \d+"],
    },
    "forbid": {"host": SESSION_HOLDS,
               "guest": SESSION_HOLDS + SPECTATE_FORBID + [r"reset=true", r"spectate: killed"]},
    "timeout": 300,
}
SCENARIOS["spectate-wipe"] = {
    "about": "spectating, the guest sees the host fall with nobody left standing: the party's wipe "
             "as today, both die and wake at their lamps with the map reloaded",
    "settings": PARTY_DEATH,
    "roles": {
        "host": [START, "wait-session 2 300", "death-mode spectate 60",
                 "wait-member-vital spectate 120", "wait 5", f"teleport {WIPE3_SPOT['host']}",
                 "wait 3", *LETHAL_FALL, "wait-vital dead 30",
                 "wait-vital up 120", "wait-members 2 240", "wait 10", "snapshot",
                 "expect-session 2"],
        "guest": [*SPECTATE_GUEST, "wait-vital dead 60", "wait-vital up 120", "wait-members 2 240",
                  "wait 10", "snapshot", "expect-session 2"],
    },
    "expect": {
        "host": [r"party death: died for real on map 0x18010000 \(reset=true\)"],
        "guest": [SPECTATE_OUT,
                  r"spectate: the party's wipe ends spectating; dies \(reset=true",
                  r"travelling there \(reset=true, void fall=false\)"],
    },
    # The wipe's death is the game's own (a party guest's death and revive): no down before it.
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS + [r"party death: down [0-9]+"]},
    "timeout": 300,
}
SCENARIOS["spectate-default-down"] = {
    "about": "the death mode's default (no pick) is party death as before: the guest's lethal fall "
             "puts it down, and it gives up",
    "settings": PARTY_DEATH,
    "roles": {
        "host": [START, "godmode", "wait-session 2 300", "wait-member-vital down 120", "wait 30",
                 "snapshot", "expect-session 2"],
        "guest": [START, "wait-session 2 300", "wait 15", "wait-death-mode down 5",
                  f"teleport {WIPE3_SPOT['guest']}", "wait 3", *GIVE_UP,
                  "wait-vital up 90", "wait 5", "snapshot", "expect-session 2"],
    },
    "expect": {"guest": [r"party death: down \d+ on map 0x18010000",
                         r"party death: giving up ends down \d+; dies \(reset=false"]},
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS + [r"spectate:"]},
    "timeout": 300,
}
# Kaniel 2026-10-25: the free camera (the setting on, its hotkey): the view flies with the left
# stick while the player stands still, and comes back.
SCENARIOS["free-camera"] = {
    "about": "the free camera (bbport.ini free_camera=1): its hotkey takes the view away, the left "
             "stick flies it ahead while the player stays where it was, the hotkey brings it back",
    "settings": {"ini.free_camera": "1"},
    "roles": {"host": [START, "wait 10", "free-camera on", "wait 2", "stick 128 0 2500", "wait 1",
                       "shot flying", "free-camera off", "wait 3", "shot back", "snapshot"]},
    "expect": {"host": [r"Bloodborne camera: the free camera is on at",
                        r"the free camera is off: it flew (?:[3-9]|\d\d)\.\d m from where it began; "
                        r"the player moved 0\.\d m meanwhile"]},
    "timeout": 240,
}

# Kaniel 2026-10-25 ("if you are in the boss fight and you are died like that you should be in the
# new spectator mode instead of respawning", "and then trigger overall death"): in Paarl's fight,
# the game's own kill of the guest (as an arena's kill plane or pit is) while the host fights on is
# no death: the guest spectates, in the default death mode too. The boss's death wakes it at its
# own lamp; the host's death is the party's wipe for both. (Gascoigne's fight raised no boss bar on
# the rig: an approach does not cross his arena's region; Paarl's does, paarl-after-wipe.)
BOSS_HOST_IN = [*PAARL_START, "wait-session 2 300", "wait 5", *INTO_PAARL_MAP, "wait 5",
                f"teleport {PAARL_SPOT}", "wait 8", f"hurt {PAARL} 9000", "wait 3", "sync bd-fight",
                f"approach {PAARL} 6 3.14", "wait 2"]  # the host fights beside him
BOSS_GUEST_IN = [*PAARL_START, "wait-session 2 300", "wait 5", *INTO_PAARL_MAP, "wait 5",
                 "sync bd-fight", f"approach {PAARL} 5 0", "wait 6", "godmode off", "kill self",
                 "wait-vital spectate 20", "expect-hp self > 0"]
BOSS_SPECTATE_OUT = r"spectate: out of the fight on map 0x17000000 \(a death in a boss fight\)"
BOSS_FIGHT_ON = r"party death: boss fight with a teammate in it on"
SCENARIOS["boss-death-spectate"] = {
    "about": "in Paarl's fight (the default death mode) the game kills the guest while the host "
             "fights on: the guest spectates instead of waking at a lamp; the host kills Paarl and "
             "the guest wakes at its own lamp, no reload of the fight's map",
    "settings": PARTY_DEATH,
    "game_kill": True,  # the game's own kill of the guest in a boss fight is what it tests
    "roles": {
        "host": [*BOSS_HOST_IN, "wait-member-vital spectate 60", "wait 8", f"kill {PAARL}",
                 f"wait-flag {PAARL_DEFEATED} 1 60", "wait-member-vital up 90", "wait 5",
                 "snapshot", "expect-session 2"],
        "guest": [*BOSS_GUEST_IN, "wait-vital up 120", "wait 5", "snapshot", "expect-hp self > 1",
                  "expect-session 2"],
    },
    "syncs": {"bd-fight": {"chapter": "boss-death-spectate", "roles": ["host", "guest"]}},
    "expect": {
        "guest": [r"party death: the boss bars are up", BOSS_FIGHT_ON, BOSS_SPECTATE_OUT,
                  r"spectate: the boss down wakes this player at its own lamp \d+"],
    },
    "forbid": {"host": SESSION_HOLDS,
               "guest": SESSION_HOLDS + [r"party death: down \d+", r"party guest died",
                                         r"party death: died for real"]},
    "timeout": 600,
}
SCENARIOS["boss-death-wipe"] = {
    "about": "in Paarl's fight the game kills the guest (it spectates), then the host dies too: the "
             "party's wipe for both, the map and the boss reset",
    "settings": PARTY_DEATH,
    "game_kill": True,
    "roles": {
        "host": [*BOSS_HOST_IN, "wait-member-vital spectate 60", "wait 5", "godmode off",
                 "kill self", "wait-vital dead 30", "wait-vital up 150", "wait-members 2 240",
                 "wait 10", "snapshot", "expect-session 2"],
        "guest": [*BOSS_GUEST_IN, "wait-vital dead 60", "wait-vital up 150", "wait-members 2 240",
                  "wait 10", "snapshot", "expect-session 2"],
    },
    "syncs": {"bd-fight": {"chapter": "boss-death-wipe", "roles": ["host", "guest"]}},
    "expect": {
        "host": [r"party death: died for real on map 0x17000000 \(reset=true\)"],
        "guest": [BOSS_FIGHT_ON, BOSS_SPECTATE_OUT,
                  r"spectate: the party's wipe ends spectating; dies \(reset=true",
                  r"travelling there \(reset=true, void fall=false\)"],
    },
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS + [r"party death: down [0-9]+"]},
    "timeout": 600,
}

# Kaniel 2026-10-25: guests saw no boss spells (Logarius's flying swords, the Shadows of Yharnam's
# snakes). Every game makes its own copies' bullets (CreateBulletOffset), so each game must make
# them itself: wait-bullets checks it. Cainhurst's Logarius (m25_00 entity 2500800, defeat flag
# 12501800, 9081 HP) fights 20 m off the Logarius' Seat lamp; in his second phase (SpEffect 5633)
# his event 12504807, run by every game on its own, fires bullet 223200590 every second.
CAINHURST_LOGARIUS_LAMP = 2502952
CAINHURST_QUEEN_LAMP = 2502951  # 150 m from him, up the castle
LOGARIUS_DEFEATED = 12501800
LOGARIUS = "0x1001c052"  # rig census 2026-10-25
LOGARIUS_SWORDS = 223200590


def _logarius(role: str, *, look: bool = False, leave: bool = False) -> list[str]:
    steps = [START, f"set-flag {LOGARIUS_DEFEATED} 0", f"travel {CAINHURST_LOGARIUS_LAMP}",
             "godmode", "wait-session 2 300", f"watch-effects {LOGARIUS} 240"]
    if role == "host":  # the host's game runs him: his second phase
        steps += ["wait 10", f"hurt {LOGARIUS} 4000"]
    steps += [f"wait-bullets {LOGARIUS_SWORDS} 10 90", "sync logarius-swords"]
    if look:
        steps += [f"approach {LOGARIUS} {0 if role == 'host' else 3} 10 3.1416"]
        for index in range(10):
            steps += ["wait 3", f"shot {role}-swords-{index}"]
    if leave:  # the runner leaves: the guest's game goes on making them
        steps += [f"travel {CAINHURST_QUEEN_LAMP}", "wait 20"] if role == "host" else \
                 ["log the host left", f"wait-bullets {LOGARIUS_SWORDS} 15 60"]
        steps += ["sync logarius-left"]
    return steps + ["census 80", "snapshot", "expect-session 2"]


# The Shadows of Yharnam (m27_00 entities 2700800-2700802, defeat flag 12701800) stand 12 m from
# the Forbidden Grave lamp. With two dead, the last one's final phase brings the snakes (events
# 12704806/12704810/12704811 send the three their joint attacks once they hold 5535/5536/5539).
FORBIDDEN_GRAVE_LAMP = 2702951
FORBIDDEN_WOODS_LAMP = 2702950  # 260 m off
SHADOWS_DEFEATED = 12701800
SHADOWS = ("0x1002405f", "0x10024060", "0x10024061")  # their handles in Kaniel's 10-25 log


def _shadows(role: str, *, look: bool = False, leave: bool = False) -> list[str]:
    steps = [START, f"set-flag {SHADOWS_DEFEATED} 0", f"travel {FORBIDDEN_GRAVE_LAMP}", "godmode",
             "wait-session 2 300", f"watch-effects {SHADOWS[2]} 240", "wait 10", "census 80"]
    # Both games kill them: the map's enemy owner's HP would bring a kill in the other back.
    steps += ["sync shadows-kill", f"kill {SHADOWS[0]}", "wait 15", f"kill {SHADOWS[1]}", "wait 5",
              f"expect-hp {SHADOWS[1]} <= 0", "sync shadows-last"]
    if look:
        steps += [f"approach {SHADOWS[2]} {0 if role == 'host' else 3} 10 3.1416"]
        for index in range(12):
            steps += ["wait 3", f"shot {role}-snakes-{index}"]
    else:
        steps += ["wait 30", "census 80", "wait 10"]
    if leave:
        steps += ["sync shadows-leave", "log the host left"]
        steps += [f"travel {FORBIDDEN_WOODS_LAMP}", "wait 30"] if role == "host" else \
                 ["wait 30", "census 80", "wait 20"]
    return steps + ["census 80", "snapshot", "expect-session 2"]


for _name, _steps, _syncs, _about in (
        ("logarius-spells", lambda role: _logarius(role), ("logarius-swords",),
         "the host takes Logarius to his second phase: both games make his flying swords"),
        ("logarius-look", lambda role: _logarius(role, look=True), ("logarius-swords",),
         "logarius-spells to look at (run without --headless): both stand 10 m off him, facing "
         "him, and take a picture every 3 s"),
        ("logarius-runner-leaves", lambda role: _logarius(role, leave=True),
         ("logarius-swords", "logarius-left"),
         "logarius-spells, then the host (his runner) travels to the far lamp: the guest's game "
         "goes on making his swords"),
        ("shadows-spells", lambda role: _shadows(role), ("shadows-last",),
         "the host kills two of the Shadows of Yharnam; both games log the last one's SpEffects "
         "and every bullet they make (its snakes)"),
        ("shadows-look", lambda role: _shadows(role, look=True), ("shadows-last",),
         "shadows-spells to look at (run without --headless): both stand 10 m off the last "
         "Shadow and take a picture every 3 s"),
        ("shadows-runner-leaves", lambda role: _shadows(role, leave=True),
         ("shadows-last", "shadows-leave"),
         "shadows-spells, then the host (the Shadows' runner) travels to the far lamp; the guest "
         "stays and both keep logging")):
    SCENARIOS[_name] = {
        "about": _about,
        "roles": {role: _steps(role) for role in ("host", "guest")},
        "syncs": {sync: {"chapter": _name, "roles": ["host", "guest"]} for sync in _syncs + (("shadows-kill",) if _name.startswith("shadows") else ())},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 600,
    }


# Kaniel 2026-10-28: bosses stood passive in co-op, picking no target. Host and guest (godmode on,
# so they last) stand by Father Gascoigne; his runner's game (the one that started his fight)
# must log him holding a player as a target (LogEnemyTargets: "[enemy <boss>: <targets>]", the
# players' handles are 0x1004400x). Its lines come on a change only, so a check holds the last one
# before a log marker (the one still true then) to it. Baseline 2026-10-28 (4b35e2d2): after a
# death or a revive by him the game handed him back and forth every few seconds and he held none.
def _boss_target(before: str, who: str = "0x1004400[0-9a-f]") -> str:
    return (rf"(?s)Bloodborne enemy targets [^\n]*\[enemy {GASCOIGNE}:(?: 0x[0-9a-f]+)* "
            rf"{who}\b[^\n]*\n(?:(?!Bloodborne enemy targets ).)*{before}")


def _boss_targets_steps(role: str, runner: str, *, runner_dies: bool = False,
                        other_downed: bool = False) -> list[str]:
    other = "guest" if runner == "host" else "host"
    steps = [START, f"set-flag {GASCOIGNE_DEFEATED} 0", f"travel {CENTRAL_YHARNAM_LAMP}",
             "godmode", "wait-session 2 300", "wait 20"]
    if role == runner:  # starts his fight, the other joins it
        steps += [f"approach {GASCOIGNE} 4 0", "wait 3", f"hit {GASCOIGNE} 30", "sync bt-in"]
    else:
        steps += ["sync bt-in", f"approach {GASCOIGNE} -4 0"]
    steps += ["log bt: both by the boss", "wait 25", "wait 20", "log bt: fight checked",
              "census 40"]
    if runner_dies:
        # The runner dies for real (goes down and gives up) and wakes at its lamp on the same map;
        # then it walks back to him.
        if role == runner:
            steps += ["godmode off", *GIVE_UP, "wait-vital up 120",
                      "wait 3", "log bt: up again", f"approach {GASCOIGNE} 4 0", "godmode",
                      "log bt: back at the boss", "wait 20", "log bt: back checked", "sync bt-back"]
        else:
            steps += ["wait-member-vital dead 60", "wait-member-vital up 120", "sync bt-back",
                      "log bt: back checked"]
    if other_downed:
        # The other one goes down by him (1 HP and a fall) and the runner strikes it up again.
        if role == other:
            steps += ["godmode off", *LETHAL_FALL, "wait-vital down 30", "wait-vital up 180",
                      "godmode", f"approach {GASCOIGNE} -4 0", "log bt: revived at the boss",
                      "wait 20", "log bt: revived checked", "sync bt-revived"]
        else:
            steps += ["wait-member-vital down 60", "approach-downed", "wait 2", *REVIVE_HITS,
                      "wait-member-vital up 30", f"approach {GASCOIGNE} 4 0",
                      "log bt: revived at the boss", "wait 20", "log bt: revived checked",
                      "sync bt-revived"]
    return steps + ["census 40", "snapshot", "expect-session 2"]


for _name, _runner, _dies, _downed, _about in (
        ("boss-targets-host", "host", False, False,
         "the host starts Father Gascoigne's fight, the guest joins: he takes a player as a "
         "target in the host's game (his runner)"),
        ("boss-targets-guest", "guest", False, False,
         "the guest starts Father Gascoigne's fight, the host joins: he takes a player as a target "
         "in the guest's game (his runner)"),
        ("boss-targets-runner-dies", "host", True, False,
         "boss-targets-host, then the host dies for real and wakes at its lamp on the same map (no "
         "load): the guest's game takes Gascoigne over and keeps him once the host walks back "
         "(#292), he holds a target there, and the host's game has no death mark left on its own "
         "player"),
        ("boss-targets-teammate-revived", "host", False, True,
         "boss-targets-host, then the guest goes down by Gascoigne and the host strikes it up: "
         "he takes the guest as a target again in the host's game")):
    _syncs = ("bt-in",) + (("bt-back",) if _dies else ()) + (("bt-revived",) if _downed else ())
    # The host's game runs the map's enemies (the game's own runner: no pick line); a guest that
    # starts the fight, or stands by him when the host dies, is picked (the host's log says so),
    # and keeps him (#292): he never goes back to the game's own runner.
    _guest_runs = _runner == "guest" or _dies
    _expect = {_runner: [_boss_target("bt: fight checked")]}
    if _guest_runs:
        _expect["host"] = _expect.get("host", []) + [
            rf"boss {GASCOIGNE} is run by {RIG_GUEST_ID}\b"]
    if _dies:
        _expect["host"].append(  # its own player is a target again, no death mark left
            r"(?s)Bloodborne enemy targets [^\n]*\[player 0x10044000 type 0 team 19 info18 0x0: "
            r"a target\][^\n]*\n(?:(?!Bloodborne enemy targets ).)*bt: back checked")
        _expect["guest"] = [_boss_target("bt: back checked")]
    if _downed:
        _expect[_runner].append(_boss_target("bt: revived checked"))
    _keeps = (rf"(?s)boss {GASCOIGNE} is run by {RIG_GUEST_ID}\b.*boss {GASCOIGNE} is run by "
              r"the game's own runner" if _guest_runs
              else rf"boss {GASCOIGNE} is run by {RIG_GUEST_ID}\b")
    SCENARIOS[_name] = {
        "about": _about,
        "settings": PARTY_DEATH if _dies or _downed else {},
        "roles": {role: _boss_targets_steps(role, _runner, runner_dies=_dies,
                                            other_downed=_downed) for role in ("host", "guest")},
        "syncs": {sync: {"chapter": _name, "roles": ["host", "guest"]} for sync in _syncs},
        "expect": _expect,
        # He goes back and forth between the games (each change restarts his AI): six changes of
        # hands in one game once both stand by him is the baseline's ping-pong, not a death's or a
        # revive's one or two.
        "forbid": {role: SESSION_HOLDS + [rf"(?s)bt: both by the boss(?:.*?enemy {GASCOIGNE[2:]} "
                                          rf"\(\w+\) changes hands){{6}}", _keeps]
                   for role in ("host", "guest")},
        "timeout": 900,
    }
    if _dies:
        # Rig 2026-10-28: the guest's game takes him over cleanly (one change of hands), but his AI
        # starts there with no target and stays so until a player hits him (a hit in this spot
        # brings one at once).
        SCENARIOS[_name]["known_bug"] = ("a boss taken over by another game holds no target "
                                         "until a player hits him")
        SCENARIOS[_name]["known_problems"] = [r"guest: expected, not seen: .*bt: back checked"]


# Player report 2026-10-27 (v1.3.0, AMD integrated GPU, 60 FPS): the joining guest, its game the
# enemy follower on Central Yharnam, stalled ~500 ms once a second (party death avg 22-28 ms, max
# 500-600 ms). The guest stands by the enemies for three minutes as the follower; every co-op tick
# part or step over 4 ms gets a "Co-op tick spike" line (BB_COOP_SPIKE_MS), naming what a slower
# PC stalls in. Run with BB_RIG_FPS=60, as the player played.
def _follower_stutter(about, role_settings=None):
    return {
        "about": about,
        "settings": {"BB_COOP_SPIKE_MS": "4"},
        "role_settings": role_settings or {},
        "roles": {
            "host": [START, "godmode", "wait-session 2 300", "wait 200", "expect-session 2"],
            "guest": [START, "godmode", "wait-session 2 300", "wait 10",
                      f"teleport {GUEST_ALONE_SPOT}", "wait 3", "census 40", "wait 90",
                      "census 40", "wait 90", "census 40", "expect-session 2"],
        },
        "expect": {"host": [r"Co-op tick slowest parts: "],
                   "guest": [r"enemy owner: this game follows", r"Co-op tick slowest parts: "]},
        "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
        "timeout": 600,
    }


SCENARIOS["guest-follower-stutter"] = _follower_stutter(
    "player report 2026-10-27: the guest, the enemy follower, by the enemies for three minutes "
    "at 60 FPS; spike lines over 4 ms name the co-op steps that stalled a slow PC once a second")
SCENARIOS["guest-follower-stutter-harder"] = _follower_stutter(
    "guest-follower-stutter with harder enemies on the guest only (its once-a-second param "
    "scans)", {"guest": HARDER})


# Kaniel 2026-10-28, the boss tempo host rule (docs/BOSS_TEMPO.md): Paarl fights the host alone
# for 90 s at each level (the host's game runs his AI; Gascoigne by the first lamp stays idle, his
# AI only starts with his fight's event, so his fight is no test). Each of his breathers is logged
# with its life before and after ("goal 2000 Wait life 1.20 -> 0.60"). Vanilla changes nothing;
# the levels cut most breathers and turn the boss rows of NpcParam faster (Gascoigne's row
# 271000: 120 degrees a second).
def _boss_tempo_steps() -> list[str]:
    return [*PAARL_START, "wait 5", *INTO_PAARL_MAP, "wait 5", f"teleport {PAARL_SPOT}", "wait 8",
            f"approach {PAARL} 6 3.14", "log tempo: fight on", "wait 30", "wait 30", "wait 30",
            "log tempo: fight checked", "census 80", "snapshot"]


_BOSS_GOAL = (r"Bloodborne boss tempo: boss {boss} \(think \d+\) goal 20\d\d \w+ life "
              r"{life} \({level}\)")
_BOSS_FOUND = rf"Bloodborne boss tempo: \d+ boss\(es\) on map 0x[0-9a-f]+: [^\n]*{PAARL}"
_TEMPO_LEVELS = {
    "vanilla": None,
    "aggressive": ("Aggressive", "0\\.50", "50", "1\\.30", "156"),
    "relentless": ("Relentless", "0\\.25", "25", "1\\.60", "192"),
}
for _word, _level in _TEMPO_LEVELS.items():
    if _level is None:
        _expect = [_BOSS_FOUND,
                   _BOSS_GOAL.format(boss=PAARL, life=r"(\d+\.\d\d) -> \1", level="Vanilla")]
        _forbid = [_BOSS_GOAL.format(boss=r"\S+", life=r"(\d+\.\d\d) -> (?!\1 )\S+",
                                     level="Vanilla"),
                   r"Bloodborne boss tempo: \d+ boss NpcParam rows turn"]
    else:
        _name, _gap, _kept, _turn, _gascoigne = _level
        _expect = [rf"Bloodborne boss tempo: {_name}: breathers x{_gap}, {_kept}% of them kept, "
                   rf"turn x{_turn}",
                   rf"Bloodborne boss tempo: \d+ boss NpcParam rows turn x{_turn} \(Father "
                   rf"Gascoigne {_gascoigne} degrees a second\)",
                   _BOSS_FOUND,
                   _BOSS_GOAL.format(boss=PAARL, life=r"\d+\.\d\d -> 0\.05", level=_name)]
        _forbid = []
    SCENARIOS[f"boss-tempo-{_word}"] = {
        "about": f"the boss tempo rule at {_word}: Paarl fights the host alone for 90 s; his "
                 "breathers and turn speed are logged",
        "settings": {"SHADPS4_BLOODBORNE_HOST_RULES": _word if _level else "off"},
        "roles": {"host": _boss_tempo_steps()},
        "expect": {"host": _expect},
        "forbid": {"host": SESSION_HOLDS + _forbid},
        "timeout": 420,
    }

# The rule is the team's: only the host (the leader) picks Relentless; the guest's game turns the
# boss rows faster too, and the host's game (Gascoigne's runner) cuts his breathers.
SCENARIOS["boss-tempo-party"] = {
    "about": "the boss tempo rule is the team's: the host (the leader) picks Relentless, the guest "
             "nothing; the guest's game turns the bosses faster too, and the host's runs "
             "Paarl's shortened breathers while both stand by him",
    "role_settings": {"host": {"SHADPS4_BLOODBORNE_HOST_RULES": "relentless"}},
    "roles": {
        "host": [*PAARL_START, "wait-session 2 300", "wait 5", *INTO_PAARL_MAP, "wait 5",
                 f"teleport {PAARL_SPOT}", "wait 8", "sync tempo-in", f"approach {PAARL} 6 3.14",
                 "log tempo: both by the boss", "wait 30", "wait 30", "log tempo: fight checked",
                 "census 80", "snapshot", "expect-session 2"],
        "guest": [*PAARL_START, "wait-session 2 300", "wait 5", *INTO_PAARL_MAP, "wait 5",
                  "sync tempo-in", f"teleport {PAARL_SPOT}", "wait 3",
                  f"approach {PAARL} -6 3.14", "log tempo: both by the boss", "wait 30",
                  "wait 30", "log tempo: fight checked", "census 80", "snapshot",
                  "expect-session 2"],
    },
    "syncs": {"tempo-in": {"chapter": "boss-tempo-party", "roles": ["host", "guest"]}},
    "expect": {
        "host": [r"Co-op host rules: the team's rules are [^\n]*relentless",
                 r"Bloodborne boss tempo: \d+ boss NpcParam rows turn x1\.60",
                 _BOSS_GOAL.format(boss=PAARL, life=r"\d+\.\d\d -> 0\.05",
                                   level="Relentless")],
        "guest": [r"Co-op host rules: the team's rules are [^\n]*relentless",
                  r"Bloodborne boss tempo: Relentless: breathers x0\.25",
                  r"Bloodborne boss tempo: \d+ boss NpcParam rows turn x1\.60"],
    },
    "forbid": {"host": SESSION_HOLDS, "guest": SESSION_HOLDS},
    "timeout": 600,
}
