# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for check_release.py on small hand-made release folders.

Run: py -3.12 -m unittest discover -s tools\\release\\tests
"""
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import check_release as tool  # noqa: E402


def make_release(root: pathlib.Path, extra: tuple[str, ...] = (), leave_out: tuple[str, ...] = ()):
    for relative in tool.REQUIRED + extra:
        if relative in leave_out:
            continue
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(LAUNCHER if relative == "BLauncher.exe" else b"x")


# A launcher as PyInstaller writes it with package.sh's version file: a version resource naming the product.
LAUNCHER = b"MZ" + "VS_VERSION_INFO".encode("utf-16-le") + "Yharnam Together".encode("utf-16-le")


class CheckReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_a_complete_release_passes(self):
        make_release(self.root, extra=("licenses/MIT.txt", "bin/reshade/shaders/CAS.fx"))
        self.assertEqual(tool.check(self.root), [])

    def test_a_missing_file_is_reported(self):
        make_release(self.root, leave_out=("server/shadnet.exe",))
        problems = tool.check(self.root)
        self.assertEqual(len(problems), 1)
        self.assertIn("server/shadnet.exe", problems[0])

    def test_a_release_without_dlss_is_reported(self):
        # Releases built without the DLSS bridge had the launcher's DLSS option disabled on RTX GPUs
        # (Kaniel 2026-10-15): bbport_dlss.dll and NVIDIA's nvngx_dlss.dll go next to bb-probe.exe.
        for dll in ("bin/bbport_dlss.dll", "bin/nvngx_dlss.dll"):
            with self.subTest(dll=dll), tempfile.TemporaryDirectory() as folder:
                root = pathlib.Path(folder)
                make_release(root, leave_out=(dll,))
                problems = tool.check(root)
                self.assertEqual(len(problems), 1)
                self.assertIn(dll, problems[0])

    def test_game_files_are_refused(self):
        for game_file in ("game/CUSA03173/eboot.bin", "x/sce_sys/param.sfo",
                          "event-patch/m21_00_00_00.emevd.dcx", "dvdroot_ps4/menu/common.tpf.dcx",
                          "data/dvdroot_ps4/chr/c0000.chrbnd.dcx", "x/sce_module/libc.prx"):
            with self.subTest(game_file=game_file):
                temp = tempfile.TemporaryDirectory()
                root = pathlib.Path(temp.name)
                make_release(root, extra=(game_file,))
                problems = tool.check(root)
                temp.cleanup()
                self.assertTrue(any(game_file in problem for problem in problems), problems)

    def test_player_data_is_refused(self):
        for player_file in ("settings.bat", "user/savedata/1/CUSA03173/SPRJ0005/userdata0010",
                            "server/db/shadnet.db", "user/log/shad_log.txt", "scripts/__pycache__/a.pyc"):
            with self.subTest(player_file=player_file):
                temp = tempfile.TemporaryDirectory()
                root = pathlib.Path(temp.name)
                make_release(root, extra=(player_file,))
                problems = tool.check(root)
                temp.cleanup()
                self.assertTrue(problems, player_file)

    def test_there_are_no_bat_files(self):
        # Kaniel 2026-10-13: one setup .exe and the launcher, no bat files.
        make_release(self.root, extra=("2 - PLAY.bat",))
        problems = tool.check(self.root)
        self.assertTrue(any("2 - PLAY.bat" in problem for problem in problems), problems)
        self.assertNotIn("1 - SETUP.bat", tool.REQUIRED)
        self.assertNotIn("tools/setup.ps1", tool.REQUIRED)

    def test_the_event_reference_holds_hashes_only(self):
        make_release(self.root, extra=("event-reference.json",))
        (self.root / "event-reference.json").write_text(
            '{"files": {"common.emevd.dcx": {"input_sha256": "aa", "output_sha256": "bb"}}}')
        self.assertEqual(tool.check(self.root), [])
        (self.root / "event-reference.json").write_text("not json")
        self.assertTrue(tool.check(self.root))


    def test_a_launcher_without_a_version_resource_is_reported(self):
        # An unsigned exe with no version resource (who made it, what it is) is what Defender's
        # machine learning flagged as Trojan:Win32/Bearfoos.A!ml in v1.3.0 (player report 2026-10-26).
        make_release(self.root)
        (self.root / "BLauncher.exe").write_bytes(b"MZ" + b"\0" * 64)
        problems = tool.check(self.root)
        self.assertEqual(len(problems), 1)
        self.assertIn("version resource", problems[0])

    def test_a_upx_packed_launcher_is_reported(self):
        # UPX (PyInstaller uses it whenever upx.exe is on PATH) is a packer malware uses too.
        make_release(self.root)
        (self.root / "BLauncher.exe").write_bytes(LAUNCHER + b"UPX0\0\0\0\0UPX1\0\0\0\0")
        problems = tool.check(self.root)
        self.assertEqual(len(problems), 1)
        self.assertIn("UPX", problems[0])


if __name__ == "__main__":
    unittest.main()
