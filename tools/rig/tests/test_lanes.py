# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Lane claims (lanes.py): two threads never take one lane, a dead run's claim is taken over,
a lane with games that would not stop is skipped.

Run: py -3.12 -m unittest discover -s tools\\rig\\tests
"""
import pathlib
import sys
import os
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import lanes  # noqa: E402


class LaneClaimTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.rig = pathlib.Path(self.folder.name)
        for lane in (1, 2, 3):
            (self.rig / "lanes" / str(lane) / "server").mkdir(parents=True)
        (self.rig / "lanes" / "4").mkdir()  # never set up: no servers
        self.running = {100, 200, 300}

    def tearDown(self):
        self.folder.cleanup()

    def alive(self, pid: int) -> bool:
        return pid in self.running

    def test_the_lanes_set_up(self):
        self.assertEqual(lanes.available(self.rig), [0, 1, 2, 3])

    @unittest.mock.patch.dict(os.environ, {"BB_RIG_MAX_LANES": "4"})
    def test_a_held_lane_is_skipped(self):
        self.assertEqual(lanes.claim_free(self.rig, 2, "phase7", 100, self.alive), [0, 1])
        self.assertEqual(lanes.claim_free(self.rig, 2, "phase8", 200, self.alive), [2, 3])
        self.assertEqual(lanes.claim_free(self.rig, 1, "rig", 300, self.alive), [],
                         "every lane is taken")
        self.assertEqual(lanes.read_claim(self.rig, 2)["owner"], "phase8")

    def test_every_lane_plays_at_once_by_default(self):
        # Kaniel 2026-10-20 22:45Z: "run as many as you want just keep track of memory".
        with unittest.mock.patch.dict(os.environ, {"BB_RIG_MAX_LANES": ""}):
            self.assertIsNone(lanes.max_lanes())
            self.assertEqual(lanes.claim_free(self.rig, 4, "rig", 100, self.alive), [0, 1, 2, 3])

    def test_the_cap_holds_when_set_lower(self):
        # Kaniel 2026-10-20 (his PC froze): BB_RIG_MAX_LANES=2 keeps it to two runs.
        with unittest.mock.patch.dict(os.environ, {"BB_RIG_MAX_LANES": "2"}):
            self.assertTrue(lanes.claim(self.rig, 1, "headstones", 100, self.alive))
            self.assertTrue(lanes.claim(self.rig, 2, "witch", 200, self.alive))
            self.assertFalse(lanes.claim(self.rig, 3, "mist", 300, self.alive), "a third waits")
            self.assertTrue(lanes.claim(self.rig, 2, "witch", 200, self.alive), "already ours")
            lanes.release(self.rig, 1, 100)
            self.assertTrue(lanes.claim(self.rig, 3, "mist", 300, self.alive))
            self.running.discard(200)  # a dead run's lane counts no more
            self.assertTrue(lanes.claim(self.rig, 0, "chstory", 100, self.alive))

    def test_a_run_past_the_cap_waits_its_turn(self):
        # The mist thread, 2026-10-20: a third run was refused as "blocked by games that would
        # not stop"; it waits for a lane to come free and says why.
        lanes.claim(self.rig, 1, "headstones", 100, self.alive)
        lanes.claim(self.rig, 2, "witch", 200, self.alive)
        said, naps = [], []

        def nap(seconds):
            naps.append(seconds)
            if len(naps) == 2:
                lanes.release(self.rig, 1, 100)

        with unittest.mock.patch.dict(os.environ, {"BB_RIG_MAX_LANES": "2"}):
            self.assertTrue(lanes.claim_when_free(self.rig, 3, "mist", 300, self.alive,
                                                  sleep=nap, say=said.append))
        self.assertEqual(len(naps), 2)
        self.assertEqual(len(said), 1, "said once")
        self.assertIn("2 lanes play already (at most 2): lane 1 headstones, lane 2 witch",
                      said[0])
        self.assertEqual(lanes.read_claim(self.rig, 3)["owner"], "mist")

    def test_a_held_lane_is_refused_not_waited_for(self):
        lanes.claim(self.rig, 1, "headstones", 100, self.alive)
        self.assertFalse(lanes.claim_when_free(self.rig, 1, "mist", 300, self.alive,
                                               sleep=self.fail, say=lambda text: None))

    def test_several_lanes_wait_for_the_first_free_one(self):
        lanes.claim(self.rig, 1, "headstones", 100, self.alive)
        lanes.claim(self.rig, 2, "witch", 200, self.alive)
        naps = []

        def nap(seconds):
            naps.append(seconds)
            lanes.release(self.rig, 2, 200)

        with unittest.mock.patch.dict(os.environ, {"BB_RIG_MAX_LANES": "2"}):
            self.assertEqual(lanes.claim_free_waiting(self.rig, 2, "mist", 300, self.alive,
                                                      sleep=nap, say=lambda text: None), [0])
        self.assertEqual(len(naps), 1)

    def test_one_lane_one_owner(self):
        self.assertTrue(lanes.claim(self.rig, 1, "phase7", 100, self.alive))
        self.assertFalse(lanes.claim(self.rig, 1, "phase8", 200, self.alive))
        self.assertTrue(lanes.claim(self.rig, 1, "phase7", 100, self.alive), "already ours")

    def test_a_dead_runs_claim_is_taken_over(self):
        self.assertTrue(lanes.claim(self.rig, 1, "phase7", 100, self.alive))
        self.running.discard(100)
        self.assertTrue(lanes.claim(self.rig, 1, "phase8", 200, self.alive))
        self.assertEqual(lanes.read_claim(self.rig, 1)["pid"], 200)

    def test_release_frees_only_its_own(self):
        lanes.claim(self.rig, 1, "phase7", 100, self.alive)
        lanes.release(self.rig, 1, 200)
        self.assertIsNotNone(lanes.read_claim(self.rig, 1), "not the releaser's")
        lanes.release(self.rig, 1, 100)
        self.assertIsNone(lanes.read_claim(self.rig, 1))
        self.assertIn("lane 1: free", lanes.status(self.rig, self.alive))

    @unittest.mock.patch.dict(os.environ, {"BB_RIG_MAX_LANES": "4"})
    def test_a_blocked_lane_is_skipped_until_its_games_are_gone(self):
        lanes.block(self.rig, 2, [300])
        self.assertEqual(lanes.claim_free(self.rig, 4, "phase7", 100, self.alive), [0, 1, 3])
        self.assertIn("lane 2: blocked by stray games 300", lanes.status(self.rig, self.alive))
        self.running.discard(300)
        self.assertTrue(lanes.claim(self.rig, 2, "phase8", 200, self.alive))
        self.assertFalse((self.rig / "lanes" / "2" / "blocked").exists())

    def test_status_names_the_holder(self):
        lanes.claim(self.rig, 3, "phase8", 200, self.alive)
        [line] = [line for line in lanes.status(self.rig, self.alive) if line.startswith("lane 3")]
        self.assertRegex(line, r"^lane 3: held by phase8 \(pid 200, since \d{4}-")

    def test_a_lanes_strays_are_its_own_games_only(self):
        listing = ("25732\tC:\\bb\\out\\x\\bin\\bb-probe.exe C:\\bb\\rig\\port\\lanes\\2\\host\\out\\"
                   "boot-linked.bin\n"
                   "41\tC:\\bb\\out\\x\\bin\\bb-probe.exe C:\\bb\\rig\\port\\host\\out\\boot.bin\n"
                   "42\tC:\\bb\\out\\x\\bin\\bb-probe.exe C:\\bb\\rig\\port\\lanes\\12\\host\\x\n")
        lane2 = [pathlib.Path(r"C:\bb\rig\port\lanes\2\host"),
                 pathlib.Path(r"C:\bb\rig\port\lanes\2\guest")]
        self.assertEqual(lanes.strays(listing, lane2), [25732])
        self.assertEqual(lanes.strays(listing, [pathlib.Path(r"C:\bb\rig\port\host")]), [41])

    def test_this_process_is_alive(self):
        import os
        self.assertTrue(lanes.pid_alive(os.getpid()))
        self.assertFalse(lanes.pid_alive(0))


if __name__ == "__main__":
    unittest.main()
