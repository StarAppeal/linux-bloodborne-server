# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""The runner's checks added with the rig refresh (2026-10-15): long frames, the overlay's
textures, known bugs, chapter times, join times, each direction's HP lag and the kill's lag.

Run: py -3.12 -m unittest discover -s tools\\rig\\tests
"""
import pathlib
import re
import sys
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import run_scenario  # noqa: E402

PERF = "Perf: 29.8 FPS over 30 s, 1% low 19.8 FPS, worst frame {} ms, 0 shader/pipeline compiles\n"


class ConsoleTest(unittest.TestCase):
    def test_a_frame_over_two_seconds_fails_the_run(self):
        consoles = {"host": PERF.format("199.9") + PERF.format("24600.0"),
                    "guest": PERF.format("350.5")}
        self.assertEqual(run_scenario.console_problems({"roles": {}}, consoles),
                         ["host: a frame took 24.6 s (at most 2 s)"])
        self.assertEqual(run_scenario.console_problems({"roles": {}, "long_frame_ms": 30000},
                                                       consoles), [])

    def test_a_chapter_checks_only_its_overlay(self):
        consoles = {"host": PERF.format("24600.0")}
        self.assertEqual(run_scenario.console_problems({"name": "trip"}, consoles), [])

    def test_the_overlays_textures_fit_its_pool(self):
        spec = {"name": "overlay-textures", "overlay_textures": ["host", "guest"]}
        consoles = {"host": "Overlay: 3 textures, pool of 64 sets\nOverlay: 21 textures, pool of "
                            "64 sets\n",
                    "guest": "Overlay: 65 textures, pool of 64 sets\n"}
        self.assertEqual(run_scenario.console_problems(spec, consoles),
                         ["guest: the overlay has 65 textures, its pool 64"])
        self.assertEqual(run_scenario.console_problems(spec, {"host": "", "guest": ""}), [
            "host: the overlay never said how many textures it has",
            "guest: the overlay never said how many textures it has"])

    def test_the_console_says_what_is_expected(self):
        spec = {"roles": {}, "console_expect": {"guest": [r"sound back after [1-9]\d* silent"]}}
        self.assertEqual(run_scenario.console_problems(
            spec, {"guest": "Runtime: sound back after 120 silent buffers\n"}), [])
        self.assertEqual(run_scenario.console_problems(
            spec, {"guest": "Runtime: sound back after 0 silent buffers\n"}),
            ["guest: the console never said /sound back after [1-9]\\d* silent/"])

    def test_a_suites_consoles_join_its_stages(self):
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            run_dir = pathlib.Path(folder)
            (run_dir / "guest-stage1-console.txt").write_text("first\n", encoding="utf-8")
            (run_dir / "guest-console.txt").write_text("second\n", encoding="utf-8")
            self.assertEqual(run_scenario.console_text(run_dir, "guest", 2), "first\n\nsecond\n")


class VerdictTest(unittest.TestCase):
    def test_a_known_bug_fails_as_known_without_failing_the_run(self):
        self.assertEqual(run_scenario.chapter_verdict({}, []), ("ok  ", True))
        self.assertEqual(run_scenario.chapter_verdict({}, ["x"]), ("FAIL", False))
        self.assertEqual(run_scenario.chapter_verdict({"known_bug": "phase 10"}, ["x"]),
                         ("KNOWN", True))
        self.assertEqual(run_scenario.chapter_verdict({"known_bug": "phase 10"}, []),
                         ("ok  ", True))
        lag = {"known_bug": "phase 10", "known_problems": [r"reached the other games' bars"]}
        self.assertEqual(run_scenario.chapter_verdict(
            lag, ["all: 0x1 reached the other games' bars in 0.83 s"]), ("KNOWN", True))
        self.assertEqual(run_scenario.chapter_verdict(
            lag, ["all: 0x1 reached the other games' bars in 0.83 s", "all: HP apart"]),
            ("FAIL", False), "another problem than the known one fails")

    def test_a_chapters_time_is_first_to_last_step(self):
        texts = {"host": "step 1/9 sync a ok (t=100.5)\nstep 2/9 wait 5 ok (t=105.5)\n",
                 "guest": "step 1/9 sync a ok (t=100.6)\nstep 2/9 census ok (t=142.0)\n"}
        self.assertAlmostEqual(run_scenario.chapter_seconds(texts), 41.5)
        self.assertIsNone(run_scenario.chapter_seconds({"host": None, "guest": ""}))

    def test_join_times_start_at_the_first_step_in_the_world(self):
        log = ("Bloodborne autoplay: step 1/9 ensure-map 0x18010000 2412950 ok (t=50.0)\n"
               "Bloodborne autoplay: step 5/9 wait-session 2 480 ok (t=62.5)\n")
        self.assertEqual(run_scenario.join_times({"guest": log, "host": "nothing"}),
                         {"guest": 12.5})


def hit(handle: str, before: int, after: int, at: float) -> str:
    return f"hit on {handle} by this player 0x1: {before} -> {after} (t={at})\n"


def lowered(handle: str, before: int, after: int, at: float) -> str:
    return f"damage on map 0x18010000: {handle} {before}->{after} (t={at})\n"


class LagTest(unittest.TestCase):
    ENEMY = "0x1001400b"

    def test_each_direction_on_its_own(self):
        texts = {"host": hit(self.ENEMY, 484, 440, 10.0) + lowered(self.ENEMY, 440, 400, 20.30),
                 "guest": lowered(self.ENEMY, 484, 440, 10.10) + hit(self.ENEMY, 440, 400, 20.0)}
        pairs = run_scenario.hp_lag_pairs(texts, self.ENEMY)
        self.assertAlmostEqual(pairs[("host", "guest")], 0.10)
        self.assertAlmostEqual(pairs[("guest", "host")], 0.30)
        problems = run_scenario.judge({"hp_sums_hits": self.ENEMY, "max_hp_lag_each": 0.15},
                                      texts)
        self.assertIn("all: guest's hits reached host's bar in 0.30 s (at most 0.15 s)", problems)
        self.assertFalse(any("host's hits" in problem for problem in problems))

    def test_the_kill_lands_on_every_screen(self):
        texts = {"host": hit(self.ENEMY, 12, 0, 30.0),
                 "guest": lowered(self.ENEMY, 12, 0, 30.12),
                 "guest2": lowered(self.ENEMY, 12, 0, 31.0)}
        lags = run_scenario.kill_lags(texts, self.ENEMY)
        self.assertAlmostEqual(lags["guest"], 0.12)
        self.assertAlmostEqual(lags["guest2"], 1.0)
        problems = run_scenario.judge({"killed": self.ENEMY, "max_kill_lag": 0.2}, texts)
        self.assertEqual(problems, ["guest2: 0x1001400b died 1.00 s after the killing hit "
                                    "(at most 0.2 s)"])
        texts["guest2"] = "Bloodborne party: enemy death seen: 0x1001400b (t=30.05)\n"
        self.assertAlmostEqual(run_scenario.kill_lags(texts, self.ENEMY)["guest2"], 0.05,
                               msg="phase 10's line for a follower's copy")
        texts["guest2"] = ""
        self.assertIn("guest2: 0x1001400b died never after the killing hit (at most 0.2 s)",
                      run_scenario.judge({"killed": self.ENEMY, "max_kill_lag": 0.2}, texts))
        self.assertEqual(run_scenario.kill_lags({"host": "", "guest": ""}, self.ENEMY), {})


class GamesTest(unittest.TestCase):
    def test_another_threads_build_in_a_rig_lane_is_the_rigs(self):
        exe = pathlib.Path(r"C:\bb\out\rigr-bbport\bin\bb-probe.exe")
        listing = (r"25732	C:\bb\out\clean-final23-bbport\bin\bb-probe.exe	C:\bb\out\clean-final23-"
                   r"bbport\bin\bb-probe.exe C:\bb\rig\port\lanes\2\host\out\boot-linked.bin" "\n"
                   r"777	C:\Games\Bloodborne\bb-probe.exe	C:\Games\Bloodborne\bb-probe.exe x" "\n"
                   r"41	C:\bb\out\rigr-bbport\bin\bb-probe.exe	" "\n")
        self.assertEqual(run_scenario.split_games(listing, exe, pathlib.Path(r"C:\bb\rig")),
                         ([25732, 41], [777]))
        self.assertEqual(run_scenario.split_games(listing, exe), ([41], [25732, 777]),
                         "without the rig's folder: the build's own only")

    def test_a_run_outside_the_lanes_stops_no_lanes_games(self):
        # Rig 2026-10-18: a run with no --lane closed another thread's party-death on lane 3
        # ("Window closed by user" in both of its games).
        exe = pathlib.Path(r"C:\bb\out\rigr-bbport\bin\bb-probe.exe")
        listing = (r"25732	C:\bb\out\x-bbport\bin\bb-probe.exe	C:\bb\out\x-bbport\bin\bb-probe.exe "
                   r"C:\bb\rig\port\lanes\3\host\out\boot-linked.bin" "\n"
                   r"26000	C:\bb\out\x-bbport\bin\bb-probe.exe	C:\bb\out\x-bbport\bin\bb-probe.exe "
                   r"C:\bb\rig\port\host\out\boot-linked.bin" "\n"
                   r"777	C:\Games\Bloodborne\bb-probe.exe	C:\Games\Bloodborne\bb-probe.exe x" "\n")
        self.assertEqual(run_scenario.outside_lanes(listing, exe, pathlib.Path(r"C:\bb\rig")),
                         [26000])

    def test_lanes_run_on_the_lanes_given(self):
        seen = []

        def runner(lane: int, name: str) -> bool:
            seen.append(lane)
            time.sleep(0.1)  # a run takes a while: the other lane takes the next
            return True

        results = run_scenario.run_in_lanes(["a", "b", "c"], [1, 3], runner, stagger=0,
                                            sleep=lambda _: None)
        self.assertEqual(results, {"a": True, "b": True, "c": True})
        self.assertEqual(set(seen), {1, 3})


def census(handle: str, y: float) -> str:
    return (f"Bloodborne autoplay census: {handle} hp 484/484 dead 0x0 hidden 0 disabled 0 at "
            f"7.0 m (-216.7, {y}, 178.1) backread 4/4 draw 0x0\n")


class GroundAndJoinTest(unittest.TestCase):
    ENEMY = "0x1001400b"

    def test_an_enemy_by_a_far_teammate_stays_on_the_ground(self):
        spec = {"grounded": {"guest": self.ENEMY}}
        ground = census(self.ENEMY, -54.7) + census(self.ENEMY, -53.9) + census(self.ENEMY, -55.1)
        self.assertEqual(run_scenario.judge(spec, {"guest": ground}), [])
        fell = census(self.ENEMY, -54.7) + census(self.ENEMY, -80.2)
        self.assertEqual(run_scenario.judge(spec, {"guest": fell}),
                         ["guest: 0x1001400b fell from -54.7 to -80.2"])
        self.assertEqual(run_scenario.judge(spec, {"guest": census(self.ENEMY, -54.7)}),
                         ["guest: 0x1001400b has 1 census, at least 2 wanted"])

    def test_a_far_guest_takes_the_enemys_position_often(self):
        spec = {"position_rate": {"guest": [self.ENEMY, 6.0]}}
        line = ("[Debug] <Info> Bloodborne party: enemy follower on map 0x18010000: positions "
                "taken per s by this player: {}\n")
        often = line.format("10014009 9.8, 1001400b 9.6") + line.format("1001400b 2.0")
        self.assertEqual(run_scenario.judge(spec, {"guest": often}), [])
        slow = line.format("1001400b 2.1, 10014009 0.4") + line.format("none")
        self.assertEqual(run_scenario.judge(spec, {"guest": slow}),
                         ["guest: took 0x1001400b's position 2.1 times a second at best "
                          "(at least 6.0 wanted)"])
        self.assertEqual(run_scenario.judge(spec, {"guest": line.format("none")}),
                         ["guest: took 0x1001400b's position never times a second at best "
                          "(at least 6.0 wanted)"])

    def test_a_restarted_game_joins_within_its_budget(self):
        before = ("Bloodborne autoplay: chapter join-time\n"
                  "Bloodborne autoplay: step 3/9 sync jt ok (t=10.0)\n")
        after = ("Bloodborne autoplay: chapter join-time\n"
                 "Bloodborne autoplay: step 1/9 ensure-map 0x18010000 2412950 ok (t=95.0)\n"
                 "Bloodborne autoplay: step 4/9 wait-session 2 420 ok (t={})\n")
        spec = {"max_join_s": {"guest": 10}}
        self.assertEqual(run_scenario.judge(spec, {"guest": before + after.format(103.0)}), [])
        self.assertEqual(run_scenario.judge(spec, {"guest": before + after.format(112.0)}),
                         ["guest: joined 17 s after its load (at most 10 s)"],
                         "from the restarted game's load, not the chapter's start")
        self.assertEqual(run_scenario.judge(spec, {"guest": before}),
                         ["guest: joined never after its load (at most 10 s)"])


class TitleStepsTest(unittest.TestCase):
    def test_a_restarted_games_title_steps_come_before_its_chapter_mark(self):
        import scenarios
        import suites
        suite = {"about": "x", "roles": ("host", "guest"), "chapters": [
            suites.chapter("join", "j", {"host": ["wait 1"]}, fatal=True, setup=True),
            suites.chapter("insert", "i", {"guest": [suites.RESTART, "team-ready",
                                                     "title-keys toggle", "title-wait 3",
                                                     scenarios.START, "keys back"]}),
        ]}
        guest = suites.compile_suite(suite)["roles"]["guest"]
        self.assertEqual(guest[1], ["team-ready", "title-keys toggle", "title-wait 3",
                                    "chapter insert 220", "keep-going on", scenarios.START,
                                    "keys back"])
        # The runner's team prelude stays out: the stage has its own team step.
        self.assertEqual(run_scenario.with_team(guest[1], "guest", ["host", "guest"], 1, True),
                         guest[1])


class ShadnetTest(unittest.TestCase):
    def test_a_threads_server_goes_into_the_lanes_folder_once(self):
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            (root / "server").mkdir()
            (root / "server" / "shadnet.exe").write_bytes(b"old")
            (root / "mine.exe").write_bytes(b"new")
            self.assertTrue(run_scenario.install_shadnet(str(root / "mine.exe"), root / "server"))
            self.assertEqual((root / "server" / "shadnet.exe").read_bytes(), b"new")
            self.assertEqual((root / "server" / "shadnet.exe.prev").read_bytes(), b"old")
            self.assertFalse(run_scenario.install_shadnet(str(root / "mine.exe"), root / "server"),
                             "the same already")
            self.assertFalse(run_scenario.install_shadnet(None, root / "server"))


class CrashAndMemoryTest(unittest.TestCase):
    def test_a_crash_in_a_known_bugs_chapter_is_known(self):
        scenario = {"chapters": [{"name": "join"}, {"name": "host-crash-apart",
                                                    "known_bug": "eboot+0x13cffba"}]}
        log = ("Bloodborne autoplay: chapter join\nstep\n"
               "Bloodborne autoplay: chapter host-crash-apart\nstep\n")
        self.assertEqual(run_scenario.crashed_in_known_bug(scenario, log)["name"],
                         "host-crash-apart")
        self.assertIsNone(run_scenario.crashed_in_known_bug(
            scenario, "Bloodborne autoplay: chapter join\n"))
        self.assertIsNone(run_scenario.crashed_in_known_bug(scenario, ""))
        single = {"roles": {}, "known_bug": "x"}
        self.assertIs(run_scenario.crashed_in_known_bug(single, ""), single)
        self.assertIsNone(run_scenario.crashed_in_known_bug({"roles": {}}, ""))

    def test_a_lane_waits_for_another_lanes_memory(self):
        seen = iter([18.4, 19.0, 23.0])
        self.assertEqual(run_scenario.wait_for_memory(22.5, wait=60, sleep=lambda _: None,
                                                      free=lambda: next(seen)), 23.0)
        self.assertEqual(run_scenario.wait_for_memory(22.5, wait=0, sleep=lambda _: None,
                                                      free=lambda: 18.4), 18.4)


class OnlyKillTest(unittest.TestCase):
    """trips' dog (rig 2026-10-22): one swing killed the target and the enemy beside it."""
    BOARD = ("Bloodborne party: kill board gets {} kills, {} back (visit 0x1801000000000009):{}"
             "\r\nnext line")

    def forbidden(self, text, *handles):
        import scenarios
        return any(re.search(p, text) for p in scenarios.only_kill(*handles))

    def test_the_enemy_beside_may_die_with_the_target(self):
        both = self.BOARD.format(2, 0, " 0x1001402e (dead 0x0 hp 0) 0x1001402f (dead 0x0 hp 0)")
        self.assertTrue(self.forbidden(both, "0x1001402e"))
        self.assertFalse(self.forbidden(both, "0x1001402e", "0x1001402f"))

    def test_any_other_kill_or_a_kill_put_back_still_fails(self):
        other = self.BOARD.format(2, 0, " 0x1001402e (dead 0x0 hp 0) 0x10014000 (dead 0x0 hp 0)")
        back = self.BOARD.format(0, 1, " back 0x1001402e")
        for text in (other, back):
            self.assertTrue(self.forbidden(text, "0x1001402e", "0x1001402f"))


if __name__ == "__main__":
    unittest.main()
