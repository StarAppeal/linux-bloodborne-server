# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for the rig's runner and scenarios, without games: a scenario with a typo would
otherwise only fail ten minutes into a run.

Run: py -3.12 -m unittest discover -s tools\\rig\\tests
"""
import os
import pathlib
import re
import sys
import tempfile
import threading
import time
import unittest
import unittest.mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import engines  # noqa: E402
import run_scenario  # noqa: E402
import scenarios  # noqa: E402
import suites  # noqa: E402

# The game's autoplay verbs (AutoplayVerbs, bloodborne_autoplay.cpp; VerbsTest keeps them equal).
VERBS = {
    "wait", "wait-session", "wait-members", "wait-map", "travel", "ensure-map", "hurt", "kill",
    "mark", "approach", "attack", "teleport", "press", "cutscene-options", "stick", "set-flag",
    "expect-walks", "place",  # playtest 2026-10-18: a player kept in a talk never walks
    "expect-flag",
    "wait-flag", "godmode", "snapshot", "log", "expect-hp", "expect-session", "expect-map", "done",
    "wait-solo", "note-max-hp", "expect-max-hp", "expect-effect", "expect-others-near", "expect-others-shown",
    "expect-compass",  # Kaniel 2026-10-17: the teammate compass
    "expect-stains", "pickup-stain",  # Kaniel 2026-10-20: bloodstains on the compass and map
    "pickup", "hit", "rise", "approach-downed", "revive-downed", "wait-vital", "wait-member-vital",
    "shot", "expect-hidden", "expect-disabled", "set-disabled", "census", "peek", "poke", "dump", "raise-stat",
    "add-effect",  # #300: Insight for Hemwick's mist ambush
    "object",  # 2026-10-20: a map object's enable switch (the Abode lamp)
    "sync", "chapter", "keep-going", "glide",  # the suites' (suites.py)
    "return-point",
    "team-create", "team-invite", "team-accept", "team-wait", "team-leave",  # the lobby's
    "team-screen", "team-ready", "team-start", "team-alone",  # the team screen's buttons
    "team-worlds",  # its world list
    "title-press", "title-wait", "title-shot", "title-keys", "keys",  # the port's menu keys
    "menu-invite", "menu-kick", "menu-join", "menu-leave",  # the team menu in game
    "trade-give", "trade-stock", "trade-offer", "trade-answer", "expect-held", "wait-trade",
    "trade-socket", "expect-socketed",
    "cut-link",  # phase 7: a dropped connection
    "watch-unseen",  # a story NPC never drawn after a load
}
VITALS = {"up", "down", "dead"}
BUTTONS = {"cross", "circle", "square", "triangle", "options", "up", "right", "down", "left",
           "l1", "r1", "l2", "r2", "l3", "r3"}


AUTOPLAY = (pathlib.Path(__file__).resolve().parents[3] / "emulator" / "src" / "core" /
            "bloodborne_autoplay.cpp")


class VerbsTest(unittest.TestCase):
    def test_the_verbs_are_the_games(self):
        text = AUTOPLAY.read_text(encoding="utf-8")
        table = text[text.index("AutoplayVerbs ="):]
        table = table[:table.index("});")]
        self.assertEqual(set(re.findall(r'\{"([a-z-]+)", Autoplay\w+\}', table)), VERBS)


class ScenarioTest(unittest.TestCase):
    def test_every_command_is_one_the_game_runs(self):
        for name, scenario in scenarios.SCENARIOS.items():
            for role, script in scenario["roles"].items():
                for command in sum(run_scenario.stages_of(script), []):
                    words = command.split()
                    with self.subTest(scenario=name, role=role, command=command):
                        self.assertIn(words[0], VERBS)
                        if words[0] == "press":
                            self.assertIn(words[1], BUTTONS)
                        if words[0] in ("wait-vital", "wait-member-vital"):
                            self.assertIn(words[1], VITALS)

    def test_every_shot_has_a_plain_name_of_its_own(self):
        for name, scenario in scenarios.SCENARIOS.items():
            for role, script in scenario["roles"].items():
                shots = [command.split() for command in sum(run_scenario.stages_of(script), [])
                         if command.split()[0] == "shot"]
                with self.subTest(scenario=name, role=role):
                    for words in shots:
                        self.assertEqual(len(words), 2, words)
                        self.assertRegex(words[1], r"^[A-Za-z0-9_-]{1,40}$")
                    names = [words[1] for words in shots]
                    self.assertEqual(len(names), len(set(names)), "a later one would replace it")

    def test_party_death_scenarios_switch_it_on(self):
        for name, scenario in scenarios.SCENARIOS.items():
            commands = [command.split()[0] for script in scenario["roles"].values()
                        for command in sum(run_scenario.stages_of(script), [])]
            uses = any(verb in ("approach-downed", "revive-downed", "wait-vital", "wait-member-vital")
                       for verb in commands)
            with self.subTest(scenario=name):
                self.assertEqual(
                    scenario.get("settings", {}).get("SHADPS4_BLOODBORNE_PARTY_DEATH") == "1", uses)

    def test_nothing_plays_without_party_death_or_dies_by_the_games_own_kill(self):
        # Party death is always on in the port (Kaniel 2026-10-13): a player dies by going down and
        # giving up, by a wipe or alone on its map, never by the game's own kill of itself.
        plays = [(name, scenario.get("settings", {}),
                  [command for script in scenario["roles"].values()
                   for command in sum(run_scenario.stages_of(script), [])])
                 for name, scenario in scenarios.SCENARIOS.items()]
        plays += [(name, scenario["settings"],
                   [command for script in suite_scripts(scenario).values() for command in script])
                  for name, scenario in suites.SUITE_SCENARIOS.items()]
        for name, settings, commands in plays:
            with self.subTest(play=name):
                self.assertNotEqual(settings.get("SHADPS4_BLOODBORNE_PARTY_DEATH"), "0")
                self.assertNotIn("kill self", commands)

    def test_every_role_is_a_rig_game(self):
        for name, scenario in scenarios.SCENARIOS.items():
            with self.subTest(scenario=name):
                self.assertTrue(set(scenario["roles"]) <= set(run_scenario.ROLES))
                for key in ("expect", "forbid"):
                    self.assertTrue(set(scenario.get(key, {})) <= set(scenario["roles"]),
                                    f"{key} names a role the scenario does not run")

    def test_every_pattern_compiles(self):
        for name, scenario in scenarios.SCENARIOS.items():
            for key in ("expect", "forbid"):
                for patterns in scenario.get(key, {}).values():
                    for pattern in patterns:
                        with self.subTest(scenario=name, pattern=pattern):
                            re.compile(pattern)
            for pattern in scenario.get("allow", []):
                re.compile(pattern)

    def test_every_script_starts_where_the_baseline_is(self):
        for name, scenario in scenarios.SCENARIOS.items():
            if "saves" in scenario or scenario.get("no_baseline"):  # where it was left
                continue
            for role, script in scenario["roles"].items():
                with self.subTest(scenario=name, role=role):
                    first = run_scenario.stages_of(script)[0]
                    # The lobby's and the title's steps (team-*, title-*) run before the world.
                    first = [step for step in first if not step.startswith(("team-", "title-"))]
                    self.assertEqual(first[0], scenarios.START)

    def test_only_the_kill_made_is_posted(self):
        [pattern] = scenarios.only_kill("0x1001400b")
        made = ("Bloodborne party: kill board gets 1 kills, 0 back (visit 0x1801000000000001): "
                "0x1001400b (dead 0x6 hp 0)")
        false = ("Bloodborne party: kill board gets 2 kills, 0 back (visit 0x1801000000000001): "
                 "0x1001409c (dead 0x1 hp 0) 0x1001409e (dead 0x1 hp 0)")
        self.assertIsNone(re.search(pattern, made))
        self.assertIsNotNone(re.search(pattern, false))

    def test_an_attack_is_one_step_with_a_time_limit(self):
        self.assertEqual(scenarios.attack("marked"), ["attack marked 40"])


SUITE_VERBS = VERBS
PLAIN_NAME = r"^[A-Za-z0-9_-]{1,40}$"  # AutoplaySyncPath's


def suite_scripts(scenario: dict) -> dict[str, list[str]]:
    """Each game's commands of a compiled suite, its stages one after the other."""
    return {role: sum(run_scenario.stages_of(script), [])
            for role, script in scenario["roles"].items()}


class SuiteTest(unittest.TestCase):
    def test_every_command_is_one_the_game_runs(self):
        for name, scenario in suites.SUITE_SCENARIOS.items():
            for role, script in suite_scripts(scenario).items():
                for command in script:
                    words = command.split()
                    with self.subTest(suite=name, role=role, command=command):
                        self.assertIn(words[0], SUITE_VERBS)
                        if words[0] == "press":
                            self.assertIn(words[1], BUTTONS)
                        if words[0] in ("wait-vital", "wait-member-vital"):
                            self.assertIn(words[1], VITALS)
                        if words[0] == "keep-going":
                            self.assertIn(words[1], ("on", "off"))

    def test_every_game_starts_where_the_baseline_is_and_marks_each_chapter(self):
        for name, scenario in suites.SUITE_SCENARIOS.items():
            chapters = [chapter["name"] for chapter in scenario["chapters"]]
            for role, script in suite_scripts(scenario).items():
                with self.subTest(suite=name, role=role):
                    self.assertEqual(run_scenario.stages_of(scenario["roles"][role])[0][0],
                                     scenarios.START)
                    marked = list(dict.fromkeys(command.split()[1] for command in script
                                                if command.startswith("chapter ")))
                    self.assertEqual(marked, chapters)

    def test_chapter_and_sync_names_are_plain_and_unique(self):
        for name, suite in suites.SUITES.items():
            chapters = [chapter["name"] for chapter in suite["chapters"]]
            with self.subTest(suite=name):
                self.assertEqual(len(chapters), len(set(chapters)))
                for chapter in chapters:
                    self.assertRegex(chapter, PLAIN_NAME)
            for role, script in suite_scripts(suites.SUITE_SCENARIOS[name]).items():
                syncs = [command.split()[1] for command in script if command.startswith("sync ")]
                with self.subTest(suite=name, role=role):
                    self.assertEqual(len(syncs), len(set(syncs)), "a game passes each once")
                    for sync in syncs:
                        self.assertRegex(sync, PLAIN_NAME)

    def test_every_sync_has_two_games_and_they_meet_in_one_order(self):
        # A sync waits for every game that has it: one only it has would wait for nobody, and two
        # games passing two syncs in opposite orders would wait for each other forever.
        for name, scenario in suites.SUITE_SCENARIOS.items():
            scripts = suite_scripts(scenario)
            order = {role: [command.split()[1] for command in script
                            if command.startswith("sync ")] for role, script in scripts.items()}
            for sync, spec in scenario["syncs"].items():
                with self.subTest(suite=name, sync=sync):
                    self.assertGreaterEqual(len(spec["roles"]), 2)
                    self.assertEqual(sorted(spec["roles"]),
                                     sorted(role for role in scripts if sync in order[role]))
            for first in scripts:
                for second in scripts:
                    shared = [sync for sync in order[first] if sync in order[second]]
                    with self.subTest(suite=name, games=(first, second)):
                        self.assertEqual(shared, [sync for sync in order[second]
                                                  if sync in order[first]])

    def test_every_chapter_waits_for_every_game_first(self):
        for name, scenario in suites.SUITE_SCENARIOS.items():
            for chapter in scenario["chapters"]:
                with self.subTest(suite=name, chapter=chapter["name"]):
                    self.assertEqual(sorted(scenario["syncs"][chapter["name"]]["roles"]),
                                     sorted(scenario["roles"]))

    def test_checks_name_the_suites_games_and_compile(self):
        for name, scenario in suites.SUITE_SCENARIOS.items():
            for chapter in scenario["chapters"]:
                with self.subTest(suite=name, chapter=chapter["name"]):
                    for key in ("expect", "forbid"):
                        self.assertTrue(set(chapter.get(key, {})) <= set(scenario["roles"]))
                        for patterns in chapter.get(key, {}).values():
                            for pattern in patterns:
                                re.compile(pattern)
                    for pattern in chapter.get("allow", []):
                        re.compile(pattern)
                    if "max_hp_lag" in chapter:
                        self.assertIn("hp_sums_hits", chapter)

    def test_suites_play_as_players_do_unless_they_test_without_party_death(self):
        for name, scenario in suites.SUITE_SCENARIOS.items():
            settings = scenario["settings"]
            commands = [command.split()[0] for script in suite_scripts(scenario).values()
                        for command in script]
            with self.subTest(suite=name):
                self.assertEqual(settings.get("BB_FPS"), "30", "uncapped: no third summon (join3-uncap)")
                if any(verb in ("approach-downed", "revive-downed", "wait-vital",
                                "wait-member-vital") for verb in commands):
                    self.assertEqual(settings.get("SHADPS4_BLOODBORNE_PARTY_DEATH"), "1")

    def test_a_restart_starts_the_game_again_in_the_same_chapter(self):
        suite = {"about": "x", "roles": ("host", "guest"), "chapters": [
            suites.chapter("join", "j", {"host": ["wait 1"]}, fatal=True, setup=True),
            suites.chapter("crash", "c", {"guest": ["sync c-ready", suites.RESTART, "wait 2"],
                                          "host": ["sync c-ready"]}),
            suites.chapter("trip", "t", {"host": ["wait 3"]}),
        ]}
        compiled = suites.compile_suite(suite)
        self.assertEqual(compiled["roles"]["guest"], [
            [scenarios.START, "chapter join 120", "keep-going off", "sync join",
             "chapter crash 220", "keep-going on", "sync crash", "sync c-ready"],
            ["chapter crash 220", "keep-going on", "wait 2", "chapter trip 120", "keep-going on",
             "sync trip"]])
        self.assertEqual(compiled["roles"]["host"][-3:], ["keep-going on", "sync trip", "wait 3"])
        self.assertEqual(compiled["syncs"]["c-ready"], {"chapter": "crash",
                                                        "roles": ["host", "guest"]})
        self.assertEqual(compiled["fatal"], ["join"])
        only = suites.compile_suite(suite, only=["trip"])
        self.assertEqual([c["name"] for c in only["chapters"]], ["join", "trip"],
                         "a chapter alone still gets its join")


class EdgeTest(unittest.TestCase):
    def test_a_border_is_crossed_there_and_back_by_both(self):
        border = {"lamp": 2412951, "from": "0x18010000", "to": "0x18000000",
                  "trail": "1.0 2.0 3.0 4.0 5.0 6.0 7.0 8.0 9.0"}
        crossing = suites.border_crossings("cy-ward", border)
        steps = crossing["steps"]["host"]
        self.assertEqual(steps, crossing["steps"]["guest"], "together")
        glides = [step for step in steps if step.startswith("glide ")]
        self.assertEqual(len(glides), 2 * suites.BORDER_CROSSINGS)
        self.assertEqual(glides[0], "glide 1.0 2.0 3.0 4.0 5.0 6.0 7.0 8.0 9.0")
        self.assertEqual(glides[1], "glide 7.0 8.0 9.0 4.0 5.0 6.0 1.0 2.0 3.0", "the way back")
        self.assertIn("teleport 1.0 2.0 3.0", steps)
        self.assertEqual(steps.count("expect-map 0x18000000"), suites.BORDER_CROSSINGS)
        suite = {"about": "x", "roles": ("host", "guest"),
                 "chapters": [suites.join(("host", "guest")), crossing]}
        compiled = suites.compile_suite(suite)
        for role, script in suite_scripts(compiled).items():
            self.assertTrue(all(command.split()[0] in SUITE_VERBS for command in script))
            for command in script:
                if command.startswith("glide "):
                    self.assertEqual(len(command.split()[1:]) % 3, 0, "x y z points")

    def test_a_restart_can_put_in_a_new_character_first(self):
        suite = {"about": "x", "roles": ("host", "guest"), "chapters": [
            suites.join(("host", "guest")),
            suites.chapter("new", "n", {"guest": [suites.restart_with("new-character"), "wait 1",
                                                  suites.RESTART, "wait 2"]}),
        ]}
        compiled = suites.compile_suite(suite)
        self.assertEqual(len(compiled["roles"]["guest"]), 3, "two restarts")
        self.assertEqual(compiled["stage_saves"], {"guest": {1: "new-character"}},
                         "only the first restart puts other saves in")

    def test_shadnet_restarts_mid_session_in_the_edges_suite(self):
        # shadNet stops and starts again while two games play on (a host's PC hiccup): the team
        # ends with the server (Kaniel 2026-10-22: "players should drop out on server restart,
        # that's ok"), so the session may drop; both games start again and join once more, so the
        # chapters after it play in a team.
        edges = suites.SUITE_SCENARIOS["edges"]
        self.assertEqual(edges["server_restarts"], ["restart-go"])
        chapter = next(c for c in suites.SUITES["edges"]["chapters"]
                       if c["name"] == "server-restart")
        self.assertNotIn("forbid", chapter, "the session may drop with the server")
        for role in ("host", "guest"):
            with self.subTest(role=role):
                steps = chapter["steps"][role]
                self.assertIn(suites.RESTART, steps, "joins again from the title")
                self.assertIn("expect-session 2", steps[steps.index(suites.RESTART):])

    def test_insert_is_pressed_in_the_world_while_the_team_load_screen_is_up(self):
        # The team's load screen is in the world since the join's reload: title-keys did nothing
        # there (rig 2026-10-22), so the presses come after the guest's arrival, an even number.
        chapter = next(c for c in suites.SUITES["joins"]["chapters"]
                       if c["name"] == "insert-while-loading")
        steps = chapter["steps"]["guest"]
        self.assertNotIn("title-keys toggle", steps)
        presses = [at for at, step in enumerate(steps) if step == "keys toggle"]
        self.assertGreaterEqual(len(presses), 4)
        self.assertEqual(len(presses) % 2, 0, "the menu ends closed")
        self.assertLess(steps.index(scenarios.START), presses[0])
        self.assertLess(presses[-1], steps.index("expect-session 2"))

    def test_a_joins_budget_covers_the_team_worlds_reload(self):
        # A join reloads the team's world and its load screen settles 5 s: 19-24 s measured.
        self.assertGreaterEqual(suites.JOIN_AFTER_LOAD_S, 25)

    def test_an_outage_starts_at_a_sync_of_its_chapter(self):
        for name, scenario in suites.SUITE_SCENARIOS.items():
            for sync in [*scenario["outages"], *scenario["server_restarts"]]:
                with self.subTest(suite=name, sync=sync):
                    self.assertIn(sync, scenario["syncs"])
            for role, saves in scenario["stage_saves"].items():
                for stage, name_of_saves in saves.items():
                    with self.subTest(suite=name, role=role):
                        self.assertTrue(0 < stage < len(run_scenario.stages_of(
                            scenario["roles"][role])))
                        self.assertRegex(name_of_saves, PLAIN_NAME)
            for chapter in scenario["chapters"]:
                for key in ("files_present", "files_absent"):
                    with self.subTest(suite=name, chapter=chapter["name"]):
                        self.assertTrue(set(chapter.get(key, {})) <= set(scenario["roles"]))


class TeamTest(unittest.TestCase):
    """Phase 6: the port plays only in teams (the team screen holds every load), so the runner makes
    the scenario's team on the title before its script, unless the script makes its own."""

    def test_only_the_port_plays_in_teams(self):
        self.assertTrue(engines.ENGINES["port"].teams)
        self.assertFalse(engines.ENGINES["emulator"].teams)

    def test_the_host_makes_the_team_and_the_guests_join_it(self):
        roles = ["host", "guest", "guest2"]
        self.assertEqual(run_scenario.team_prelude("host", roles, 0),
                         ["team-create", "team-invite rig_guest", "team-invite rig_guest2",
                          "team-wait 3", "team-start"])
        self.assertEqual(run_scenario.team_prelude("guest", roles, 0), ["team-accept", "team-ready"])
        self.assertEqual(run_scenario.team_prelude("guest2", roles, 0), ["team-accept", "team-ready"])

    def test_a_restarted_game_makes_or_finds_its_team(self):
        # Kaniel 2026-10-17: a team lives only while one of its games is there, so a game started
        # again does the whole prelude (team-create and team-accept pass at once in a team).
        roles = ["host", "guest"]
        self.assertEqual(run_scenario.team_prelude("host", roles, 1),
                         run_scenario.team_prelude("host", roles, 0))
        self.assertEqual(run_scenario.team_prelude("guest", roles, 2), ["team-accept", "team-ready"])

    def test_a_game_alone_plays_alone(self):
        self.assertEqual(run_scenario.team_prelude("host", ["host"], 0), ["team-alone"])

    def test_the_prelude_goes_before_a_script_without_team_verbs_only(self):
        roles = ["host", "guest"]
        self.assertEqual(run_scenario.with_team(["wait 1"], "guest", roles, 0, teams=True),
                         ["team-accept", "team-ready", "wait 1"])
        self.assertEqual(run_scenario.with_team(["wait 1"], "guest", roles, 0, teams=False), ["wait 1"])
        own = ["team-accept", "team-ready", "wait 1"]
        self.assertEqual(run_scenario.with_team(own, "guest", roles, 0, teams=True), own)
        self.assertIsNone(run_scenario.with_team(None, "guest", roles, 0, teams=True))


class LaneTest(unittest.TestCase):
    def tearDown(self):
        run_scenario.use_lane(0)

    def test_lane_zero_of_a_batch_stops_only_its_own_games(self):
        # Rig 2026-10-13: lane 0's end of smoke closed lane 1's games (together ran again).
        run_scenario.use_lane(0)
        self.assertFalse(run_scenario.IN_LANES, "the rig on its own stops every rig game")
        run_scenario.use_lane(0, True)
        self.assertTrue(run_scenario.IN_LANES)
        run_scenario.use_lane(1)
        self.assertTrue(run_scenario.IN_LANES)
        self.assertEqual(run_scenario.lane_command(0, "smoke", run_scenario.RIG)[5:9],
                         ["--lane", "0", "--batch", str(run_scenario.RIG)])

    def test_a_lane_has_its_own_ports_and_folders(self):
        run_scenario.use_lane(0)
        self.assertEqual((run_scenario.SHADNET_PORT, run_scenario.WEBAPI_PORT,
                          run_scenario.BACKEND_PORT), (32313, 32315, 20453), "the rig as it was")
        self.assertEqual(run_scenario.p2p_port("guest"), 51601)
        self.assertEqual(run_scenario.lane_dir(), run_scenario.RIG)
        port = engines.ENGINES["port"]
        self.assertEqual(port.role_dir("guest"), port.rig / "port" / "guest")
        run_scenario.use_lane(2)
        self.assertEqual((run_scenario.SHADNET_PORT, run_scenario.WEBAPI_PORT,
                          run_scenario.BACKEND_PORT), (32513, 32515, 20653))
        self.assertEqual(port.environment("host")["BB_SHADNET_SERVER"], "127.0.0.1:32514")
        self.assertEqual(run_scenario.p2p_port("guest"), 51621)
        self.assertEqual(run_scenario.lane_dir(), run_scenario.RIG / "lanes" / "2")
        self.assertEqual(port.role_dir("guest"), port.rig / "port" / "lanes" / "2" / "guest")
        self.assertEqual(port.role_dir("guest", 0), port.rig / "port" / "guest")
        # No two lanes (of up to ten) share a port, with a ping (its proxies) or without.
        lanes_ports = []
        for lane in range(10):
            run_scenario.use_lane(lane)
            ports = set()
            for ping in (None, (50, 100)):
                run_scenario.use_ping(ping)
                ports |= {*run_scenario.delayed_ports(), run_scenario.matching_udp_port(),
                          run_scenario.SHADNET_PORT, run_scenario.WEBAPI_PORT,
                          run_scenario.BACKEND_PORT, *map(run_scenario.p2p_port, run_scenario.ROLES)}
            run_scenario.use_ping(None)
            lanes_ports.append(ports)
        for one in range(10):
            for other in range(one + 1, 10):
                self.assertFalse(lanes_ports[one] & lanes_ports[other], (one, other))

    def test_a_lanes_shadnet_listens_on_its_ports(self):
        config = "[General]\nUnsecuredPort=32313\nMatchingUdpPort=33314\nWebApiPort=32315\n"
        changed = run_scenario.with_setting(run_scenario.with_setting(config, "UnsecuredPort",
                                                                      32413), "WebApiPort", 32415)
        self.assertIn("UnsecuredPort=32413\n", changed)
        self.assertIn("WebApiPort=32415\n", changed)
        self.assertIn("MatchingUdpPort=33314\n", changed)

    def test_a_lanes_shadnet_answers_the_games_services_on_the_lanes_backend_port(self):
        config = "[General]\nUnsecuredPort=32313\nWebApiPort=32315\n"
        changed = run_scenario.with_backend(config, True)
        self.assertIn("BloodborneBackend=true\n", changed)
        self.assertIn(f"BackendPort={run_scenario.BACKEND_PORT}\n", changed)
        self.assertIn("BackendAddress=127.0.0.1\n", changed)
        self.assertTrue(changed.startswith("[General]\n"))
        # Set again (shadnet writes its defaults into the file): replaced, not added twice.
        again = run_scenario.with_backend(changed.replace("=true", "=false"), True)
        self.assertEqual(again.count("BloodborneBackend="), 1)
        self.assertIn("BloodborneBackend=true\n", again)
        self.assertIn("BloodborneBackend=false\n", run_scenario.with_backend(config, False))

    def test_a_lane_is_a_copy_of_the_rig_without_its_game_image_and_logs(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            port = engines.Port(root)
            (root / "rig" / "server" / "db").mkdir(parents=True)
            (root / "rig" / "server" / "db" / "accounts.db").write_text("rig_host")
            (root / "rig" / "server" / "shadnet.exe").write_text("exe")
            (root / "rig" / "server" / "shadnet.exe.pre-party.bak").write_text("old")
            (root / "rig" / "backend").mkdir()
            (root / "rig" / "backend" / "bb_dummy_server.py").write_text("py")
            host = port.role_dir("host", 0)
            (host / "out" / "eboot").mkdir(parents=True)
            (host / "baseline" / "savedata").mkdir(parents=True)
            (host / "user" / "log").mkdir(parents=True)
            (host / "user" / "log" / "shad_log.txt").write_text("old log")
            with unittest.mock.patch.object(run_scenario, "ENGINE", port), \
                    unittest.mock.patch.object(run_scenario, "RIG", root / "rig"):
                made = run_scenario.setup_lane(1)
                lane = root / "rig" / "lanes" / "1"
                self.assertTrue((lane / "server" / "db" / "accounts.db").is_file(), "accounts")
                self.assertTrue((lane / "backend" / "bb_dummy_server.py").is_file())
                self.assertFalse((lane / "server" / "shadnet.exe.pre-party.bak").exists())
                lane_host = port.role_dir("host", 1)
                self.assertTrue((lane_host / "baseline" / "savedata").is_dir())
                self.assertFalse((lane_host / "out").exists(), "run.py makes the image again")
                self.assertEqual(list((lane_host / "user" / "log").iterdir()), [])
                self.assertEqual(len(made), 3)
                self.assertEqual(run_scenario.setup_lane(1), [], "made once")

    def test_runs_go_to_free_lanes_never_more_at_once_than_lanes(self):
        running, most, order = set(), [0], []
        lock = threading.Lock()
        # Lane 1 starts once lane 0 has taken "long", which holds its lane until the other three
        # are done: the same order however slow the machine is.
        long_taken, short_ones_done = threading.Event(), threading.Event()

        def runner(lane, name):
            with lock:
                running.add(lane)
                most[0] = max(most[0], len(running))
                order.append((lane, name))
            if name == "long":
                long_taken.set()
                short_ones_done.wait(10)
            else:
                time.sleep(0.05)
            with lock:
                running.discard(lane)
                if name == "bad":
                    short_ones_done.set()
            return name != "bad"

        names = ["long", "a", "b", "bad", "c"]
        results = run_scenario.run_in_lanes(names, 2, runner, stagger=1,
                                            sleep=lambda seconds: seconds and long_taken.wait(10))
        self.assertEqual(results, {"long": True, "a": True, "b": True, "bad": False, "c": True})
        self.assertEqual(most[0], 2)
        self.assertEqual(order[0][1], "long", "the first name first")
        self.assertEqual({lane for lane, _ in order}, {0, 1})
        self.assertTrue(all(lane == 1 for lane, name in order if name in ("a", "b", "bad")),
                        "the other lane takes the rest while the long one runs")

    def test_a_lanes_run_is_this_runner_again_in_that_lane(self):
        run_scenario.use_ping((50, 100))
        try:
            command = run_scenario.lane_command(2, "smoke", pathlib.Path("runs/b"))
        finally:
            run_scenario.use_ping(None)
        self.assertEqual(command[-9:], ["--engine", run_scenario.ENGINE.name, "--lane", "2",
                                        "--batch", str(pathlib.Path("runs/b")), "--ping",
                                        "50-100", "smoke"])


class RunnerTest(unittest.TestCase):
    def test_a_role_runs_one_script_or_several_in_turn(self):
        self.assertEqual(run_scenario.stages_of(["wait 1", "done"]), [["wait 1", "done"]])
        self.assertEqual(run_scenario.stages_of([["wait 1"], ["snapshot"]]),
                         [["wait 1"], ["snapshot"]], "stopped and started again in between")
        self.assertEqual(run_scenario.stages_of([]), [[]])

    def test_a_scenario_can_start_from_other_saves(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            (root / "baseline" / "savedata").mkdir(parents=True)
            (root / "baseline" / "savedata" / "old").write_text("x")
            (root / "baseline" / "bloodborne_party_1.txt").write_text("old")
            other = root / "new" / "savedata"
            other.mkdir(parents=True)
            (other / "fresh").write_text("y")
            user = root / "user"
            user.mkdir()
            (user / "bloodborne_party_1.txt").write_text("played")
            engine = unittest.mock.Mock()
            engine.role_dir.return_value = root
            engine.saves.return_value = user / "savedata"
            with unittest.mock.patch.object(run_scenario, "ENGINE", engine), \
                    unittest.mock.patch.object(run_scenario, "user_dir", lambda role: user):
                self.assertTrue(run_scenario.restore_baseline("host", other))
                self.assertTrue((user / "savedata" / "fresh").is_file())
                self.assertFalse((user / "bloodborne_party_1.txt").exists(),
                                 "a new character has no party files")
                self.assertTrue(run_scenario.restore_baseline("host"))
                self.assertTrue((user / "savedata" / "old").is_file())
                self.assertEqual((user / "bloodborne_party_1.txt").read_text(), "old")
                # A suite's restart with a new character keeps the old one's party files (rig
                # 2026-10-13: the new-character check had nothing to tell apart).
                (user / "bloodborne_party_1.txt").write_text("played")
                self.assertTrue(run_scenario.restore_baseline("host", other, keep_party_files=True))
                self.assertTrue((user / "savedata" / "fresh").is_file())
                self.assertEqual((user / "bloodborne_party_1.txt").read_text(), "played")


    NETSTAT = "\n".join([
        "Active Connections",
        "  Proto  Local Address          Foreign Address        State           PID",
        "  TCP    127.0.0.1:32313        0.0.0.0:0              LISTENING       4120",
        "  TCP    0.0.0.0:20453          0.0.0.0:0              LISTENING       5532",
        "  TCP    127.0.0.1:31313        0.0.0.0:0              LISTENING       777",
        "  TCP    127.0.0.1:52000        127.0.0.1:32313        ESTABLISHED     9001",
        "  TCP    127.0.0.1:132313       0.0.0.0:0              LISTENING       12",
    ])

    def test_only_the_rigs_listeners_are_stopped(self):
        self.assertEqual(run_scenario.listening_pids(self.NETSTAT, 32313), {4120})
        self.assertEqual(run_scenario.listening_pids(self.NETSTAT, 20453), {5532})
        self.assertEqual(run_scenario.listening_pids(self.NETSTAT, 32314), set(),
                         "the online kit's 31313 and a client's connection are not the rig's")
        self.assertEqual(run_scenario.listening_pids("", 32313), set())

    def test_only_the_rigs_games_are_stopped(self):
        exe = pathlib.Path(r"C:\bb\out\emulator\build\shadPS4.exe")
        listing = "\n".join([
            "4120\tC:\\bb\\out\\emulator\\build\\shadPS4.exe",
            "5532\tc:\\BB\\out\\emulator\\build\\shadps4.exe",
            "777\tC:\\Bloodborne.PC.Online - Copy\\SeamlessCoop\\shadps4.exe",
            "",
            "garbage",
        ])
        self.assertEqual(run_scenario.split_games(listing, exe), ([4120, 5532], [777]),
                         "Kaniel's own game is not the rig's")
        self.assertEqual(run_scenario.split_games("", exe), ([], []))

    def test_a_log_is_judged_up_to_its_scripts_end(self):
        log = ("step 1/2 wait 1 ok\nBloodborne autoplay: done (ok)\n"
               "sceNpMatching2LeaveRoom: called (the rig stopped the other game)\n")
        self.assertNotIn("LeaveRoom", run_scenario.until_done(log))
        self.assertTrue(run_scenario.until_done(log).endswith("done (ok)"))
        self.assertEqual(run_scenario.until_done("no end yet"), "no end yet")

    def test_a_game_that_never_got_going_says_why(self):
        log = ("[Loader] <Info> Files found in game mods folder\n"
               "[Debug] <Critical> (shadPS4:Main) address_space.cpp:200 Impl: Assertion Failed!\n"
               "Insufficient system resources exist to complete the requested service.\n")
        self.assertEqual(run_scenario.boot_failure(log),
                         "Insufficient system resources exist to complete the requested service.")
        self.assertIsNone(run_scenario.boot_failure("a game in the world\n"))

    def test_cache_twice_copies_one_pipeline_entry_under_another_name(self):
        with tempfile.TemporaryDirectory() as folder:
            source = pathlib.Path(folder) / "played"
            source.mkdir()
            (source / "profile.bin").write_bytes(b"profile")
            (source / "g_0x8a00000000000000.key").write_bytes(b"second")
            (source / "g_0x1200000000000000.key").write_bytes(b"first")
            (source / "c_0x0100000000000000.key").write_bytes(b"compute")
            cache = pathlib.Path(folder) / "headless"
            cache.mkdir()
            (cache / "profile.bin").write_bytes(b"old")
            original = run_scenario.seed_cache_twice(cache, source)
            self.assertEqual(original, cache / "g_0x1200000000000000.key")
            self.assertEqual((cache / run_scenario.CACHE_TWICE_NAME).read_bytes(), b"first")
            self.assertEqual((cache / "profile.bin").read_bytes(), b"profile",
                             "the played cache replaces the headless one")
            self.assertTrue((source / "g_0x1200000000000000.key").exists())
            self.assertFalse((source / run_scenario.CACHE_TWICE_NAME).exists(),
                             "the cache it came from is left as it was")
            self.assertEqual(run_scenario.seed_cache_twice(cache), original,
                             "again in place: the copy is not copied")
            empty = pathlib.Path(folder) / "empty"
            empty.mkdir()
            self.assertIsNone(run_scenario.seed_cache_twice(empty),
                              "a headless game draws nothing: no entry to copy")

    def test_cache_twice_wants_the_copy_skipped_and_one_file_left(self):
        with tempfile.TemporaryDirectory() as folder:
            original = pathlib.Path(folder) / "g_0x1200000000000000.key"
            copy = pathlib.Path(folder) / run_scenario.CACHE_TWICE_NAME
            original.write_bytes(b"key")
            skipped = ("[Render.Vulkan] <Warning> vk_pipeline_serialization.cpp:256 "
                       "LoadGraphicsPipeline: Cached pipeline 0x5 skipped: it is in the cache "
                       "twice\n[Render] <Info> Preloaded 12 pipelines\n")
            self.assertEqual(run_scenario.cache_twice_problems(skipped, original, copy), [])
            copy.write_bytes(b"key")
            stopped = ("[Debug] <Critical> vk_pipeline_serialization.cpp:292 LoadGraphicsPipeline: "
                       "Assertion Failed!\n\n")
            problems = run_scenario.cache_twice_problems(stopped, original, copy)
            self.assertEqual(len(problems), 3, problems)
            self.assertIn("stopped", problems[0])
            self.assertIn("skipped", problems[1])
            self.assertIn("both", problems[2])
            original.unlink()
            copy.unlink()
            self.assertIn("neither", run_scenario.cache_twice_problems(skipped, original, copy)[0])

    def test_a_runs_pictures_go_with_its_logs(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            engine = engines.Port(root)
            shots = engine.shots("host")
            shots.mkdir(parents=True)
            (shots / "sees-down.png").write_bytes(b"png")
            engine.shot_request("host").write_text("never taken", encoding="utf-8")
            run_dir = root / "rig" / "runs" / "20261010-040000" / "down-revive"
            with unittest.mock.patch.object(run_scenario, "ENGINE", engine):
                run_scenario.keep_shots(run_dir, ["host", "guest"])  # the guest took none
            self.assertEqual((run_dir / "host-sees-down.png").read_bytes(), b"png")
            self.assertEqual(list(shots.iterdir()), [], "moved: the next run starts with none")
            self.assertFalse(engine.shot_request("host").exists(),
                             "a request no frame took is not taken by the next run's first frame")

    def test_play_gets_the_games_into_the_world_together_and_leaves_the_rest_to_you(self):
        for name, players in (("down-revive", 2), ("down-revive3", 3)):
            scenario = scenarios.SCENARIOS[name]
            scripts = run_scenario.play_scripts(scenario)
            with self.subTest(scenario=name):
                self.assertEqual(list(scripts), list(scenario["roles"]))
                for script in scripts.values():
                    self.assertEqual(script, [scenarios.START, f"wait-session {players} 300"])
                    self.assertTrue(all(step.split()[0] in VERBS for step in script))

    def test_every_game_sees_one_hp_for_the_shared_enemy(self):
        # Kaniel 2026-10-10: two players hitting one enemy saw different HP bars. A scenario's
        # "same_hp" handle must show the same HP in every role's last census, below its max.
        line = "[Debug] <Info> () x: Bloodborne autoplay census: {} hp {}/{} dead 0x0 hidden 0"
        host = "\n".join([line.format("0x1001400b", 484, 484), line.format("0x1001400b", 300, 484)])
        guest = line.format("0x1001400b", 301, 484)
        self.assertEqual(run_scenario.last_census_hp(host, "0x1001400b"), (300, 484))
        self.assertIsNone(run_scenario.same_hp_problem({"host": host, "guest": guest},
                                                       "0x1001400b"))
        self.assertIn("apart", run_scenario.same_hp_problem(
            {"host": host, "guest": line.format("0x1001400b", 400, 484)}, "0x1001400b"))
        self.assertIn("never hit", run_scenario.same_hp_problem(
            {"host": line.format("0x1001400b", 484, 484), "guest": line.format("0x1001400b", 484, 484)},
            "0x1001400b"))
        self.assertIn("no census", run_scenario.same_hp_problem({"host": host, "guest": ""},
                                                                "0x1001400b"))

    def test_the_shared_enemy_took_each_hit_once(self):
        # Kaniel's friend 2026-10-11: each of the guest's hits came back a second time. With
        # "hp_sums_hits", the last census must show max HP less every role's own hits (the game's
        # hit function, "by this player"), a few HP apart at most (shares round). Not the `hit`
        # step's drop: another member's damage written by the board while it swings counts there
        # (rig hit3 2026-10-11: the guest's step saw the host's 484 -> 482).
        census = "[Debug] <Info> () x: Bloodborne autoplay census: {} hp {}/{} dead 0x0 hidden 0"
        hit = ("[Debug] <Info> () x: Bloodborne party: hit on {} by this player 0x10044000: "
               "{} -> {} (t=1.0)")
        step = "[Debug] <Info> () x: Bloodborne autoplay: hit {}: hp {} -> {} in attack cycle 0"
        texts = {
            "host": "\n".join([hit.format("0x1001400b", 484, 430),
                               census.format("0x1001400b", 330, 484)]),
            "guest": "\n".join([step.format("0x1001400b", 430, 420),
                                hit.format("0x1001400b", 430, 380),
                                hit.format("0x1001400b", 380, 330),
                                census.format("0x1001400b", 331, 484)]),
        }
        self.assertEqual(run_scenario.hit_damage(texts["guest"], "0x1001400b"), 100)
        self.assertIsNone(run_scenario.hits_sum_problem(texts, "0x1001400b"))
        twice = dict(texts, host=texts["host"].replace("330/484", "280/484"))
        self.assertIn("took 204", run_scenario.hits_sum_problem(twice, "0x1001400b"))
        self.assertIn("no hit", run_scenario.hits_sum_problem(
            {"host": census.format("0x1001400b", 484, 484)}, "0x1001400b"))

    def test_a_hit_reaches_the_other_games_within_the_lag(self):
        # Kaniel's forest run 2026-10-11: the host's hits showed on the guests' bars a second late.
        # The rig's games share one clock (t=): from each role's own hit to the first lowering of
        # that enemy in each other game, on average (one late round is no trend).
        hit = ("[Debug] <Info> () x: Bloodborne party: hit on {} by this player 0x10044000: "
               "{} -> {} (t={})")
        low = ("[Debug] <Info> () x: Bloodborne party: lowered 1 enemies by the other members' "
               "damage on map 0x18010000: {} {}->{} (t={})")
        texts = {
            "host": hit.format("0x1001400b", 484, 382, "100.0"),
            "guest": "\n".join([low.format("0x1001400b", 484, 382, "100.3"),
                                hit.format("0x1001400b", 382, 280, "110.0")]),
            "guest2": "\n".join([low.format("0x1001400b", 484, 382, "100.6"),
                                 low.format("0x1001400b", 382, 280, "110.2")]),
        }
        self.assertAlmostEqual(run_scenario.hp_board_lag(texts, "0x1001400b"), 1.1 / 3)
        self.assertIsNone(run_scenario.hp_board_lag({"host": texts["host"]}, "0x1001400b"))
        # Rig hit3 2026-10-11: a game the hit reached through the game itself lowers nothing for
        # it; its next lowering (another hit, 35 s on) or a write given back (up) is no lag.
        later = dict(texts, host="\n".join([
            texts["host"], low.format("0x1001400b", 280, 178, "145.0"),
            low.format("0x1001400b", 76, 178, "145.1")]))
        self.assertAlmostEqual(run_scenario.hp_board_lag(later, "0x1001400b"), 1.1 / 3)
        # Rig hit3 2026-10-12 (bdba0cd1, "0.65 s"): the guest's hit reached the host through the
        # game (no lowering); guest2's hit of the same damage 3.4 s later lowered the host's copy,
        # and that lowering is guest2's, not the guest's: it starts from the HP the hit left.
        hits = {
            "guest": hit.format("0x1001400b", 382, 280, "110.0"),
            "guest2": "\n".join([low.format("0x1001400b", 382, 280, "110.1"),
                                 hit.format("0x1001400b", 280, 178, "113.4")]),
            "host": low.format("0x1001400b", 280, 178, "113.5"),
        }
        self.assertAlmostEqual(run_scenario.hp_board_lag(hits, "0x1001400b"), 0.1)

    def test_an_enemy_that_comes_for_a_player_moves_between_its_censuses(self):
        # Kaniel 2026-10-14: with the host far away, the enemies beside a guest stood still.
        census = ("[Debug] <Info> () x: Bloodborne autoplay census: 0x1001400b hp 484/484 dead 0x0 "
                  "hidden 0 disabled 0 at {} m ({}, -54.7, {}) backread 0/0 draw 0x0")
        still = "\n".join([census.format(6.0, -216.7, 178.1), census.format(6.0, -216.6, 178.1)])
        moved = "\n".join([census.format(6.0, -216.7, 178.1), census.format(1.0, -212.0, 180.0)])
        spec = {"moves": {"guest": "0x1001400b"}}
        self.assertTrue([p for p in run_scenario.judge(spec, {"guest": still}) if "stood" in p])
        self.assertFalse([p for p in run_scenario.judge(spec, {"guest": moved}) if "stood" in p])
        self.assertTrue([p for p in run_scenario.judge(spec, {"guest": ""}) if "stood" in p])

    def test_a_player_that_stands_still_stays_put(self):
        # Kaniel 2026-10-15: one guest's backstab dragged the other guest to the enemy.
        census = ("[Debug] <Info> () x: Bloodborne autoplay census: map 0x18010000, 1 characters "
                  "within 5 m of ({}, -50.6, {})")
        still = "\n".join([census.format(-199.7, 179.4), census.format(-200.3, 178.7)])
        dragged = "\n".join([census.format(-199.7, 179.4), census.format(-223.5, 181.9)])
        spec = {"stays": {"guest2": 3.0}}
        self.assertFalse([p for p in run_scenario.judge(spec, {"guest2": still}) if "moved" in p])
        self.assertTrue([p for p in run_scenario.judge(spec, {"guest2": dragged}) if "moved" in p])
        self.assertTrue([p for p in run_scenario.judge(spec, {"guest2": ""}) if "moved" in p])

    def test_a_lag_at_the_limit_passes(self):
        # Rig hit3 2026-10-14: 0.15 s on the games' clock came out as 0.1500000000014552 s, over a
        # 0.15 s limit by the float sums alone.
        hit = ("[Debug] <Info> () x: Bloodborne party: hit on {} by this player 0x10044000: "
               "{} -> {} (t={})")
        low = ("[Debug] <Info> () x: Bloodborne party: lowered 1 enemies by the other members' "
               "damage on map 0x18010000: {} {}->{} (t={})")
        texts = {"host": hit.format("0x1001400b", 484, 382, "57327.10"),
                 "guest": low.format("0x1001400b", 484, 382, "57327.25")}
        spec = {"hp_sums_hits": "0x1001400b", "max_hp_lag": 0.15}
        self.assertFalse([p for p in run_scenario.judge(spec, texts) if "bars" in p])
        late = dict(texts, guest=low.format("0x1001400b", 484, 382, "57327.27"))
        self.assertTrue([p for p in run_scenario.judge(spec, late) if "bars" in p])

    def test_a_sync_is_let_go_once_every_game_in_it_reached_it(self):
        log = ("[Debug] <Info> () x: Bloodborne autoplay: sync join reached (t=10.0)\n"
               "[Debug] <Info> () x: Bloodborne autoplay: step 4/9 sync join ok (t=11.0)\n"
               "[Debug] <Info> () x: Bloodborne autoplay: sync host-back reached (t=40.0)\r\n")
        self.assertEqual(run_scenario.syncs_reached(log), {"join", "host-back"})
        syncs = {"join": {"chapter": "join", "roles": ["host", "guest"]},
                 "host-back": {"chapter": "trip", "roles": ["host", "guest"]},
                 "trip-done": {"chapter": "trip", "roles": ["host", "guest"]}}
        reached = {"host": {"join", "host-back"}, "guest": {"join"}}
        self.assertEqual(run_scenario.syncs_to_release(syncs, reached, set(), set(), set()),
                         ["join"])
        self.assertEqual(run_scenario.syncs_to_release(syncs, reached, set(), set(), {"join"}),
                         [], "once")
        # The guest gave up the trip chapter (a failed step, keep-going on): the host is let past
        # its syncs instead of waiting there in vain.
        self.assertEqual(run_scenario.syncs_to_release(syncs, reached, {"trip"}, set(), {"join"}),
                         ["host-back", "trip-done"])
        # A game whose script ended (or crashed) never gets there either: the others go on as
        # each reaches it.
        self.assertEqual(run_scenario.syncs_to_release(syncs, reached, set(), {"guest"}, {"join"}),
                         ["host-back"])
        with tempfile.TemporaryDirectory() as folder:
            role_dir = pathlib.Path(folder)
            run_scenario.write_go_file(role_dir, "host-back")
            self.assertTrue((role_dir / "sync" / "host-back").is_file())
            run_scenario.clear_go_files(role_dir)
            self.assertFalse((role_dir / "sync").exists(), "the next suite starts with none")

    def test_a_suites_log_is_judged_chapter_by_chapter(self):
        log = "\n".join([
            "[Debug] <Info> () x: Bloodborne autoplay: step 1/9 ensure-map 0x18010000 ok",
            "[Debug] <Info> () x: Bloodborne autoplay: chapter join",
            "[Debug] <Info> () x: Bloodborne autoplay: step 3/9 chapter join ok (t=1.0)",
            "joined",
            "[Debug] <Info> () x: Bloodborne autoplay: chapter trip\r",
            "tripped",
            "[Debug] <Info> () x: Bloodborne autoplay: step 6/9 expect-session 2 FAILED: 1",
            "[Debug] <Info> () x: Bloodborne autoplay: chapter trip given up",
            "[Debug] <Info> () x: Bloodborne autoplay: chapter wipe",
            "wiped",
            "[Debug] <Info> () x: Bloodborne autoplay: chapter trip",  # back after a restart
            "rejoined",
        ])
        sections = run_scenario.chapter_sections(log)
        self.assertEqual(list(sections), ["join", "trip", "wipe"])
        self.assertIn("joined", sections["join"])
        self.assertNotIn("tripped", sections["join"])
        self.assertIn("tripped", sections["trip"])
        self.assertIn("rejoined", sections["trip"], "a restarted game's lines join its chapter")
        self.assertIn("FAILED", sections["trip"])
        self.assertNotIn("FAILED", sections["wipe"])
        self.assertEqual(run_scenario.chapter_sections("no chapters"), {})

    def test_a_failed_join_stops_the_suite_and_restarts_nobody(self):
        # Rig 2026-10-12 smoke: the join timed out, and the guest's game was started again for
        # its next stage anyway (its log of the failure set aside).
        scenario = {"fatal": ["join"]}
        log = ("Bloodborne autoplay: chapter join\nBloodborne autoplay: step 7/50 wait-session 3 "
               "480 FAILED: timed out\nBloodborne autoplay: done (failed)\n")
        self.assertTrue(run_scenario.failed_fatal_chapter(log, "failed", scenario))
        self.assertFalse(run_scenario.failed_fatal_chapter(log, "ok", scenario))
        later = log.replace("chapter join", "chapter join\nBloodborne autoplay: chapter trip")
        self.assertFalse(run_scenario.failed_fatal_chapter(later, "failed", scenario))
        self.assertFalse(run_scenario.failed_fatal_chapter("no chapters", "failed", scenario))

    def test_a_chapter_is_judged_like_a_scenario(self):
        spec = {"expect": {"host": [r"kill board gets 1 kills"]},
                "forbid": {"guest": [r"sceNpMatching2LeaveRoom: called"]}}
        texts = {"host": "Bloodborne party: kill board gets 1 kills", "guest": "fine"}
        self.assertEqual(run_scenario.judge(spec, texts), [])
        problems = run_scenario.judge(spec, {"host": "nothing", "guest":
                                             "sceNpMatching2LeaveRoom: called\n"
                                             "Bloodborne autoplay: step 2/3 hit x FAILED: no"})
        self.assertTrue(any("expected, not seen" in p and p.startswith("host") for p in problems))
        self.assertTrue(any("must not happen" in p and p.startswith("guest") for p in problems))
        self.assertTrue(any("FAILED: no" in p for p in problems))

    def test_a_chapter_checks_the_files_a_game_wrote(self):
        with tempfile.TemporaryDirectory() as folder:
            user = pathlib.Path(folder)
            old = user / "bloodborne_party_pickups_slot1.txt.bak-0900"
            old.write_text("from an earlier run")
            os.utime(old, (1000, 1000))
            spec = {"files_absent": {"guest": ["bloodborne_party_*.bak-*"]},
                    "files_present": {"host": ["bloodborne_party_*.bak-*"]}}
            with unittest.mock.patch.object(run_scenario, "user_dir", lambda role: user):
                problems = run_scenario.file_problems(spec, since=2000)
                self.assertEqual(problems, ["host: no bloodborne_party_*.bak-* written"],
                                 "an earlier run's backup is not this run's")
                (user / "bloodborne_party_pickups_slot1.txt.bak-1000").write_text("new")
                problems = run_scenario.file_problems(spec, since=2000)
                self.assertEqual(len(problems), 1)
                self.assertTrue(problems[0].startswith("guest: a bloodborne_party_*.bak-* written"))

    def test_old_runs_go_newest_stay(self):
        with tempfile.TemporaryDirectory() as folder:
            runs = pathlib.Path(folder)
            for stamp in ("20261009-010000", "20261009-020000", "20261009-030000"):
                (runs / stamp / "join").mkdir(parents=True)
            run_scenario.prune_runs(runs, 2)
            self.assertEqual(sorted(path.name for path in runs.iterdir()),
                             ["20261009-020000", "20261009-030000"])
            run_scenario.prune_runs(runs / "missing", 2)  # no runs yet: nothing to do


if __name__ == "__main__":
    unittest.main()
