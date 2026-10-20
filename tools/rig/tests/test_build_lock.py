# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for the PC's build lock (build_lock.py): build slots, games, and the memory guard.

Run: py -3.12 -m unittest discover -s tools\\rig\\tests
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import build_lock  # noqa: E402


def gone_pid() -> int:
    """A process that has ended."""
    process = subprocess.Popen([sys.executable, "-c", "pass"])
    process.wait()
    return process.pid


class BuildLockTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.lock = pathlib.Path(self.folder.name) / "build.lock"

    def tearDown(self):
        self.folder.cleanup()

    def sleeper(self) -> int:
        """A live process other than this one and its parent."""
        process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        self.addCleanup(process.wait)
        self.addCleanup(process.kill)
        return process.pid

    @unittest.mock.patch.dict(os.environ, {"BB_BUILD_SLOTS": "1"})
    def test_a_build_holds_it_until_it_lets_go(self):
        build_lock.take("emu", os.getpid(), self.lock, games=lambda: [], sleep=self.fail)
        self.assertEqual(build_lock.holder(self.lock), ("emu", os.getpid()))
        self.assertFalse(build_lock.try_take("port", os.getppid(), self.lock),
                         "a second build waits")
        build_lock.release(os.getppid(), self.lock)
        self.assertIsNotNone(build_lock.holder(self.lock), "only its holder lets go")
        build_lock.release(os.getpid(), self.lock)
        self.assertIsNone(build_lock.holder(self.lock))

    def test_a_killed_builds_lock_is_taken_over(self):
        self.lock.write_text(f"old\n{gone_pid()}\nthen\n", encoding="utf-8")
        self.assertIsNone(build_lock.holder(self.lock))
        self.assertTrue(build_lock.try_take("new", os.getpid(), self.lock))
        self.assertEqual(build_lock.holder(self.lock), ("new", os.getpid()))

    def test_any_number_of_builds_run_at_once_by_default(self):
        # Kaniel 2026-10-20 22:45Z: "build as many as you want ... just keep track of memory".
        with unittest.mock.patch.dict(os.environ, {"BB_BUILD_SLOTS": ""}):
            for pid in (os.getpid(), os.getppid(), self.sleeper()):
                self.assertTrue(build_lock.try_take("build", pid, self.lock))
            self.assertEqual(len(build_lock.holders(self.lock)), 3)

    def test_builds_wait_past_a_set_cap(self):
        with unittest.mock.patch.dict(os.environ, {"BB_BUILD_SLOTS": "2"}):
            build_lock.take("emu", os.getpid(), self.lock, games=lambda: [], sleep=self.fail)
            build_lock.take("port", os.getppid(), self.lock, games=lambda: [], sleep=self.fail)
            self.assertFalse(build_lock.try_take("server", self.sleeper(), self.lock),
                             "a third waits")
            self.assertEqual(sorted(build_lock.holders(self.lock)),
                             sorted([("emu", os.getpid()), ("port", os.getppid())]))
            build_lock.release(os.getpid(), self.lock)
            self.assertEqual(build_lock.holders(self.lock), [("port", os.getppid())])

    def test_builds_run_beside_games_by_default(self):
        with unittest.mock.patch.dict(os.environ, {"BB_BUILD_BESIDE_GAMES": ""}):
            build_lock.take("port", os.getpid(), self.lock, games=lambda: [1234],
                            sleep=self.fail)
            self.lock.write_text(f"emu\n{os.getpid()}\nnow\n", encoding="utf-8")
            build_lock.wait_for_builds(self.lock, sleep=self.fail)  # a game starts beside it

    def test_a_build_waits_for_the_games(self):
        # Kaniel 2026-10-20 22:20Z: builds beside rig games froze his PC.
        seen = iter([[1234, 5678], [1234], []])
        naps = []
        with unittest.mock.patch.dict(os.environ, {"BB_BUILD_BESIDE_GAMES": "0"}):
            build_lock.take("port", os.getpid(), self.lock, games=lambda: next(seen),
                            sleep=naps.append)
        self.assertEqual(len(naps), 2)
        self.assertEqual(build_lock.holder(self.lock), ("port", os.getpid()))

    def test_a_player_may_let_a_build_run_beside_games(self):
        with unittest.mock.patch.dict(os.environ, {"BB_BUILD_BESIDE_GAMES": "1"}):
            build_lock.take("port", os.getpid(), self.lock, games=lambda: [1234],
                            sleep=self.fail)
        self.assertEqual(build_lock.holder(self.lock), ("port", os.getpid()))

    def test_a_game_waits_for_the_build(self):
        self.lock.write_text(f"emu\n{os.getpid()}\nnow\n", encoding="utf-8")
        naps = []

        def nap(seconds):
            naps.append(seconds)
            if len(naps) == 3:
                build_lock.release(os.getpid(), self.lock)

        with unittest.mock.patch.dict(os.environ, {"BB_BUILD_BESIDE_GAMES": "0"}):
            build_lock.wait_for_builds(self.lock, sleep=nap)
            self.assertEqual(len(naps), 3)
            build_lock.wait_for_builds(self.lock, sleep=self.fail)  # free: no wait

    def test_nothing_starts_while_memory_is_low(self):
        # Kaniel 2026-10-20 22:42Z: the freeze was most likely leaked memory.
        gib = 1 << 30
        with unittest.mock.patch.dict(os.environ, {"BB_MIN_FREE_RAM_GB": "",
                                                   "BB_MIN_FREE_COMMIT_GB": "",
                                                   "BB_MEMORY_GUARD": ""}):
            self.assertIsNone(build_lock.memory_short(lambda: (8 * gib, 20 * gib)))
            self.assertIn("1.5 GB free RAM (at least 2",
                          build_lock.memory_short(lambda: (int(1.5 * gib), 20 * gib)))
            self.assertIn("3.0 GB of commit left (at least 4",
                          build_lock.memory_short(lambda: (8 * gib, 3 * gib)))
            self.assertIsNone(build_lock.memory_short(lambda: None), "unknown: no check")
            with self.assertRaises(SystemExit) as refused:
                build_lock.refuse_if_memory_low("a rig game", lambda: (gib, gib))
            self.assertIn("a rig game", str(refused.exception))
            self.assertIn("reboot", str(refused.exception))
        with unittest.mock.patch.dict(os.environ, {"BB_MEMORY_GUARD": "0"}):
            self.assertIsNone(build_lock.memory_short(lambda: (0, 0)))
        with unittest.mock.patch.dict(os.environ, {"BB_MIN_FREE_RAM_GB": "0.5",
                                                   "BB_MIN_FREE_COMMIT_GB": "1"}):
            self.assertIsNone(build_lock.memory_short(lambda: (gib, int(1.5 * gib))))


if __name__ == "__main__":
    unittest.main()
