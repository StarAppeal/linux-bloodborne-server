# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for the two builds the rig plays with, the emulator and the native port (Bloodborne PC):
their folders, settings and game view, without starting a game.

Run: py -3.12 -m unittest discover -s tools\\rig\\tests
"""
import json
import os
import pathlib
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import engines  # noqa: E402
import run_scenario  # noqa: E402


class EngineTest(unittest.TestCase):
    def setUp(self):
        self.root = pathlib.Path(r"C:\bb")
        self.emulator = engines.Emulator(self.root)
        self.port = engines.Port(self.root)

    def test_each_build_keeps_its_own_rig_folders(self):
        rig = self.root / "rig"
        self.assertEqual(self.emulator.role_dir("host"), rig / "host")
        self.assertEqual(self.port.role_dir("host"), rig / "port" / "host")
        self.assertEqual(self.emulator.saves("guest"),
                         rig / "guest" / "user" / "home" / "1000" / "savedata")
        self.assertEqual(self.port.saves("guest"), rig / "port" / "guest" / "user" / "savedata")
        for engine in (self.emulator, self.port):
            self.assertEqual(engine.log_path("guest2"),
                             engine.role_dir("guest2") / "user" / "log" / "shad_log.txt",
                             "the log the rig's checks read")

    def test_the_emulator_starts_the_rigs_game(self):
        self.assertEqual(self.emulator.command("host"),
                         [str(self.root / "out" / "emulator" / "build" / "shadPS4.exe"), "-g",
                          str(self.root / "rig" / "game" / "CUSA03173" / "eboot.bin")])
        self.assertEqual(self.emulator.environment("host"), {})

    def test_only_the_emulator_starts_its_games_one_after_the_other(self):
        # Two emulator games loading at once were slow; the port's load in parallel.
        self.assertTrue(self.emulator.staggered_start)
        self.assertFalse(self.port.staggered_start)

    def test_the_port_starts_through_its_launcher(self):
        self.assertEqual(self.port.command("host"),
                         [sys.executable, str(self.root / "out" / "bbport" / "run.py")])
        self.assertEqual(self.port.exe, self.root / "out" / "bbport" / "bin" / "bb-probe.exe")

    def test_the_port_plays_on_the_rigs_servers_with_its_own_account(self):
        env = self.port.environment("guest2")
        role = self.root / "rig" / "port" / "guest2"
        self.assertEqual(env["BB_DATA_DIR"], str(role))
        self.assertEqual(env["BB_USER_DIR"], str(role / "user"))
        self.assertEqual(env["BB_GAME_DIR"], str(self.root / "rig" / "port" / "game" / "CUSA03173"))
        self.assertEqual(env["BB_PROBE"], str(self.port.exe))
        self.assertEqual(env["BB_LOG_FILE"], str(role / "user" / "log" / "shad_log.txt"))
        self.assertEqual(env["BB_ONLINE"], "1")
        # Phase 11, one port: the port signs in on shadNet's link UDP port (the TCP port's +1).
        self.assertEqual(env["BB_SHADNET_SERVER"], "127.0.0.1:32314")
        self.assertEqual(env["BB_SHADNET_WEBAPI"], "http://127.0.0.1:32315")
        self.assertEqual(env["BB_SHADNET_NPID"], "rig_guest2")
        self.assertEqual(env["BB_SHADNET_PASSWORD"], "rig")
        self.assertEqual(env["BB_MODS_DIR"], str(self.root / "rig" / "game" / "CUSA03173-mods"),
                         "the emulator rig's low-memory mod")
        self.assertEqual((env["BB_FPS"], env["BB_DMEM_MB"]), ("30", "5056"),
                         "the low-spec profile: 30 FPS, the PS4's own memory")

    def test_only_the_port_takes_pictures_and_in_its_role_folder(self):
        # A script's `shot <name>` writes the request file; the port's presenter takes it.
        role = self.root / "rig" / "port" / "guest"
        self.assertEqual(self.port.environment("guest")["BB_SHOT_TRIGGER"],
                         str(role / "shot.request"))
        self.assertEqual(self.port.shot_request("guest"), role / "shot.request")
        self.assertEqual(self.port.shots("guest"), role / "shots")
        self.assertNotIn("BB_SHOT_TRIGGER", self.emulator.environment("guest"),
                         "the emulator draws no party HUD: its scripts skip their shots")

    def test_the_port_is_made_ready_to_play_on_the_rig(self):
        with tempfile.TemporaryDirectory() as folder,                 unittest.mock.patch.object(engines, "refresh_events") as refresh,                 unittest.mock.patch.dict(os.environ):
            os.environ.pop("BB_RIG_MODS", None)
            port = engines.Port(pathlib.Path(folder))
            GameViewTest.write(port.game_dir / "dvdroot_ps4" / "event" / "common.emevd.dcx", "")
            port.prepare("host")
            # The shared event files follow this checkout's patcher.
            refresh.assert_called_with(port.mods, port.game_dir / "dvdroot_ps4" / "event")
            overrides = port.user_dir("host") / "host_overrides.custom.json"
            self.assertEqual(json.loads(overrides.read_text(encoding="utf-8")),
                             {"https://ss4.scej-network.jp:20443": "http://127.0.0.1:20453"},
                             "the game server is the rig's backend")
            settings = (port.role_dir("host") / "bbport.ini").read_text(encoding="utf-8")
            for line in ("output_res=1280x720", "upscaler=off", "model_lod=2", "skip_intro=1",
                         "effect_ssao=0", "live_resolution=0"):
                self.assertIn(line, settings.splitlines())
            # Kaniel 2026-10-20 (his PC froze under the rig): every run is 720p and lowest, also
            # over a settings file a tester or an older rig left; the rest of the file stays.
            (port.role_dir("host") / "bbport.ini").write_text(
                "output_res=3840x2160\nshow_fps=1\nframe_generation=1\nbutton_icons=xbox\n",
                encoding="utf-8")
            port.prepare("host")
            lines = (port.role_dir("host") / "bbport.ini").read_text(encoding="utf-8").splitlines()
            for line in ("output_res=1280x720", "show_fps=0", "frame_generation=0", "preset=0",
                         "upscaler=off", "window_mode=windowed", "button_icons=xbox"):
                self.assertIn(line, lines)
            self.assertNotIn("output_res=3840x2160", lines)
            self.assertEqual(len([line for line in lines if line.startswith("show_fps=")]), 1)
            port.prepare("host", backend_port=21453)
            self.assertEqual(json.loads(overrides.read_text(encoding="utf-8")),
                             {"https://ss4.scej-network.jp:20443": "http://127.0.0.1:21453"},
                             "or the proxy in front of it")

    def test_a_missing_build_or_game_is_named(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            port = engines.Port(root)
            self.assertIn("tools/build-bbport.ps1", port.missing())
            for path in (port.exe, port.home / "run.py"):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"")
            self.assertIn("--engine port --setup", port.missing())
            port.game_dir.mkdir(parents=True)
            (port.game_dir / "eboot.bin").write_bytes(b"")
            self.assertIsNone(port.missing())
            emulator = engines.Emulator(root)
            self.assertIn("tools/build-emu.ps1", emulator.missing())

    def test_engines_by_name(self):
        self.assertEqual(sorted(engines.ENGINES), ["emulator", "port"])
        self.assertEqual(engines.ENGINES["port"].name, "port")


class GameViewTest(unittest.TestCase):
    @staticmethod
    def write(path: pathlib.Path, text: str):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_the_update_goes_over_the_game(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            base, patch, view = root / "CUSA03173", root / "CUSA03173-patch", root / "view"
            self.write(base / "eboot.bin", "1.00")
            self.write(base / "sce_sys" / "param.sfo", "base sfo")
            self.write(base / "sce_sys" / "icon0.png", "icon")
            self.write(base / "dvdroot_ps4" / "chr" / "c0000.chrbnd.dcx", "hunter")
            self.write(base / "dvdroot_ps4" / "map" / "m21.msb", "yharnam")
            self.write(patch / "eboot.bin", "1.09")
            self.write(patch / "sce_sys" / "param.sfo", "patch sfo")
            self.write(patch / "dvdroot_ps4" / "chr" / "c2000.chrbnd.dcx", "new enemy")
            self.write(patch / "sce_module" / "libc.prx", "libc")

            engines.merge_game(base, patch, view)
            for relative, text in (("eboot.bin", "1.09"), ("sce_sys/param.sfo", "patch sfo"),
                                   ("sce_sys/icon0.png", "icon"),
                                   ("dvdroot_ps4/chr/c0000.chrbnd.dcx", "hunter"),
                                   ("dvdroot_ps4/chr/c2000.chrbnd.dcx", "new enemy"),
                                   ("dvdroot_ps4/map/m21.msb", "yharnam"),
                                   ("sce_module/libc.prx", "libc")):
                self.assertEqual((view / relative).read_text(encoding="utf-8"), text, relative)

            # Made again (a newer update, say): the old view goes, the game and update stay.
            self.write(patch / "eboot.bin", "1.10")
            engines.merge_game(base, patch, view)
            self.assertEqual((view / "eboot.bin").read_text(encoding="utf-8"), "1.10")
            self.assertEqual((base / "eboot.bin").read_text(encoding="utf-8"), "1.00")
            self.assertEqual((base / "dvdroot_ps4" / "map" / "m21.msb").read_text(encoding="utf-8"),
                             "yharnam")
            self.assertEqual(sorted(p.name for p in (base / "dvdroot_ps4" / "chr").iterdir()),
                             ["c0000.chrbnd.dcx"], "nothing of the update lands in the game")

    def test_no_update_means_the_game_as_it_is(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            self.write(root / "game" / "eboot.bin", "1.09")
            engines.merge_game(root / "game", root / "missing", root / "view")
            self.assertEqual((root / "view" / "eboot.bin").read_text(encoding="utf-8"), "1.09")

    def test_emulator_saves_go_where_the_port_keeps_them(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            self.write(root / "emulator" / "CUSA03173" / "SPRJ0005" / "userdata0000", "hunter")
            self.write(root / "emulator" / "CUSA03173" / "SPRJ0005" / "userdata0010", "options")
            self.write(root / "port" / "1" / "CUSA03173" / "SPRJ0005" / "userdata0003", "stale")
            engines.copy_saves(root / "emulator", root / "port")
            game = root / "port" / "1" / "CUSA03173" / "SPRJ0005"
            self.assertEqual(sorted(p.name for p in game.iterdir()),
                             ["userdata0000", "userdata0010"], "the old saves are replaced")
            self.assertEqual((game / "userdata0000").read_text(encoding="utf-8"), "hunter")


class RunnerTest(unittest.TestCase):
    def tearDown(self):
        run_scenario.use_engine("emulator")

    def test_the_runner_plays_with_the_emulator_unless_told(self):
        self.assertEqual(run_scenario.ENGINE.name, "emulator")
        self.assertEqual(run_scenario.log_path("host"),
                         engines.ENGINES["emulator"].log_path("host"))
        run_scenario.use_engine("port")
        self.assertEqual(run_scenario.log_path("host"), engines.ENGINES["port"].log_path("host"))
        self.assertEqual(run_scenario.user_dir("guest"), engines.ENGINES["port"].user_dir("guest"))

    def test_a_game_gets_its_party_its_port_and_its_script(self):
        run_scenario.use_engine("port")
        script = pathlib.Path(r"C:\bb\rig\port\guest\autoplay.txt")
        env = run_scenario.game_environment("guest", script, "rig:hunters")
        self.assertEqual(env["SHADPS4_BLOODBORNE_SEAMLESS_COOP"], "1")
        self.assertEqual(env["SHADPS4_BLOODBORNE_PARTY"], "rig:hunters")
        self.assertEqual(env["SHADPS4_BLOODBORNE_PARTY_ROLE"], "guest")
        self.assertEqual(env["SHADPS4_P2P_PORT"], "51601")
        self.assertEqual(env["SHADPS4_BLOODBORNE_RE_TRACE"], "1")
        self.assertEqual(env["SHADPS4_BLOODBORNE_AUTOPLAY"], str(script))
        self.assertEqual(env["BB_SHADNET_NPID"], "rig_guest", "the port's own settings too")

    def test_test_games_never_take_the_focus(self):
        # Kaniel 2026-10-09: test games must run in the background, not pull his window away.
        for engine in ("emulator", "port"):
            run_scenario.use_engine(engine)
            env = run_scenario.game_environment("host", None, "rig:x")
            self.assertEqual(env["SDL_WINDOW_ACTIVATE_WHEN_SHOWN"], "0", engine)
            self.assertEqual(env["SDL_WINDOW_ACTIVATE_WHEN_RAISED"], "0", engine)
        options = run_scenario.background_window_options()
        if sys.platform == "win32":
            self.assertEqual(options["startupinfo"].wShowWindow, 4)  # SW_SHOWNOACTIVATE
            self.assertTrue(options["creationflags"] & 0x08000000)  # CREATE_NO_WINDOW

    def test_a_scenario_switches_on_its_own_settings_only(self):
        party_death = {"SHADPS4_BLOODBORNE_PARTY_DEATH": "1"}
        env = run_scenario.game_environment("host", None, "rig:x", party_death)
        self.assertEqual(env["SHADPS4_BLOODBORNE_PARTY_DEATH"], "1")
        with unittest.mock.patch.dict(os.environ, {"SHADPS4_BLOODBORNE_PARTY_DEATH": "0"}):
            env = run_scenario.game_environment("host", None, "rig:x")
        # Never from the PC's settings, and never switched off: the port's party death is always on
        # (Kaniel 2026-10-13), the emulator keeps its own default.
        self.assertNotIn("SHADPS4_BLOODBORNE_PARTY_DEATH", env)

    def test_a_ping_delays_the_games_traffic_both_to_each_other_and_to_the_servers(self):
        run_scenario.use_engine("port")
        run_scenario.use_ping((50, 100))
        try:
            env = run_scenario.game_environment("host", None, "rig:x")
            self.assertEqual(env["SHADPS4_P2P_DELAY_MS"], "25-50", "half the ping each way")
            # The link is UDP, straight to shadNet at the proxy's port + 1; the game delays it.
            self.assertEqual(env["BB_SHADNET_SERVER"], "127.0.0.1:33314")
            self.assertEqual(env["BB_SHADNET_WEBAPI"], "http://127.0.0.1:33315")
            self.assertEqual(run_scenario.delayed_ports(), {33313: 32313, 33315: 32315,
                                                            21453: 20453})
            run_scenario.use_ping(None)
            env = run_scenario.game_environment("host", None, "rig:x")
            self.assertNotIn("SHADPS4_P2P_DELAY_MS", env)
            self.assertEqual(env["BB_SHADNET_SERVER"], "127.0.0.1:32314")
        finally:
            run_scenario.use_ping(None)

    def test_a_port_game_reaches_only_the_lanes_servers(self):
        # Phase 9's proof test: every game talks only to the server, never to another game. Each
        # game's own firewall (SHADPS4_NET_FIREWALL) names the servers it reaches: shadNet's TCP
        # and UDP ports, the web API and the backend, through their proxies with a ping.
        run_scenario.use_engine("port")
        env = run_scenario.game_environment("guest", None, "rig:x")
        self.assertEqual(env["SHADPS4_NET_FIREWALL"],
                         "127.0.0.1:32313,127.0.0.1:32314,127.0.0.1:32315,127.0.0.1:20453")
        run_scenario.use_ping((50, 100))
        try:
            env = run_scenario.game_environment("guest", None, "rig:x")
            self.assertEqual(env["SHADPS4_NET_FIREWALL"],
                             "127.0.0.1:33313,127.0.0.1:33314,127.0.0.1:33315,127.0.0.1:21453")
        finally:
            run_scenario.use_ping(None)
        with unittest.mock.patch.dict(os.environ, {"SHADPS4_NET_FIREWALL": "127.0.0.1:*"}):
            env = run_scenario.game_environment("guest", None, "rig:x")
        self.assertNotIn("127.0.0.1:*", env["SHADPS4_NET_FIREWALL"], "never the PC's own")

    def test_the_servers_udp_port_follows_the_one_the_games_reach(self):
        # A game sends its STUN pings to the TCP port it reached + 1 (np_matching2_mm.cpp): with
        # a ping that is the proxy's port + 1, which shadNet must listen on itself (UDP is not
        # proxied: shadNet must see each game's own address; the games hold what they send).
        config = "[General]\nUnsecuredPort=32313\nMatchingUdpPort=32314\nWebApiPort=32315\n"
        run_scenario.use_ping((50, 100))
        try:
            self.assertEqual(run_scenario.matching_udp_port(), 33314)
            self.assertEqual(run_scenario.with_matching_udp_port(config, 33314),
                             config.replace("MatchingUdpPort=32314", "MatchingUdpPort=33314"))
            run_scenario.use_ping(None)
            self.assertEqual(run_scenario.matching_udp_port(), 32314)
        finally:
            run_scenario.use_ping(None)

    def test_the_rig_plays_with_a_ping_unless_told(self):
        self.assertEqual(run_scenario.parse_args([]).ping, (50, 100))
        self.assertIsNone(run_scenario.parse_args(["--ping", "0"]).ping)
        self.assertEqual(run_scenario.parse_args(["--ping", "200"]).ping, (200, 200))

    def test_a_boot_check_plays_nothing_and_meets_no_one(self):
        env = run_scenario.game_environment("host", None, "smoke:boot")
        self.assertNotIn("SHADPS4_BLOODBORNE_AUTOPLAY", env)
        self.assertEqual(env["SHADPS4_BLOODBORNE_PARTY"], "smoke:boot")


class PortSetupTest(unittest.TestCase):
    def tearDown(self):
        run_scenario.use_engine("emulator")

    def test_the_port_rig_is_made_from_the_emulators(self):
        write = GameViewTest.write
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            emulator, port = engines.Emulator(root), engines.Port(root)
            game = root / "rig" / "game"
            write(game / "CUSA03173" / "eboot.bin", "1.00")
            write(game / "CUSA03173-patch" / "eboot.bin", "1.09")
            save = pathlib.Path("CUSA03173", "SPRJ0005", "userdata0000")
            # The host has a baseline (the scenarios' start), the guest only its saves.
            write(emulator.role_dir("host") / "baseline" / "savedata" / save, "host baseline")
            write(emulator.role_dir("host") / "baseline" / "bloodborne_party_world_slot0.txt", "w")
            write(emulator.saves("host") / save, "host played on")
            write(emulator.saves("guest") / save, "guest")
            write(emulator.user_dir("guest") / "bloodborne_party_resume_slot0.txt", "r")

            self.assertEqual(run_scenario.setup_port(emulator, port), ["host", "guest"])
            self.assertEqual((port.game_dir / "eboot.bin").read_text(encoding="utf-8"), "1.09")
            self.assertEqual((port.saves("host") / "1" / save).read_text(encoding="utf-8"),
                             "host baseline")
            self.assertEqual((port.saves("guest") / "1" / save).read_text(encoding="utf-8"),
                             "guest")
            self.assertTrue((port.user_dir("host") / "bloodborne_party_world_slot0.txt").exists())
            self.assertTrue((port.user_dir("guest") / "bloodborne_party_resume_slot0.txt").exists())
            for role in ("host", "guest"):
                baseline = port.role_dir(role) / "baseline"
                self.assertTrue((baseline / "savedata" / "1" / save).exists(),
                                "the port's own baseline")
                self.assertTrue((port.role_dir(role) / "bbport.ini").exists())

            # Made once: a port folder in use stays as it is.
            write(port.saves("host") / "1" / save, "played on the port")
            self.assertEqual(run_scenario.setup_port(emulator, port), [])
            self.assertEqual((port.saves("host") / "1" / save).read_text(encoding="utf-8"),
                             "played on the port")


class EventFilesTest(unittest.TestCase):
    """The rig's shared event files follow this checkout's patcher (2026-10-22: a full run on
    main played an Oct 20 m21_00 patch without 12105040 and failed dream-shop-messengers)."""

    # A stand-in patcher: writes one event file and the manifest naming its own hash, as
    # party_boss_events.py does.
    TOOL = (
        "import hashlib, json, pathlib, sys\n"
        "target = pathlib.Path(sys.argv[1])\n"
        "target.mkdir(parents=True, exist_ok=True)\n"
        "(target / 'm21_00_00_00.emevd.dcx').write_text('new', encoding='utf-8')\n"
        "made = {'tool_sha256': hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()}\n"
        "(target / 'manifest.json').write_text(json.dumps(made), encoding='utf-8')\n"
    )

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.folder.name)
        self.tool = root / "party_boss_events.py"
        self.tool.write_text(self.TOOL, encoding="utf-8")
        self.game_events = root / "game" / "event"
        self.game_events.mkdir(parents=True)
        self.mods = root / "CUSA03173-mods"
        self.events = self.mods / "dvdroot_ps4" / "event"
        self.events.mkdir(parents=True)
        # The old file is a hard link to another folder's copy (a kit's event-patch).
        self.other = root / "kit-event-patch.dcx"
        self.other.write_text("old", encoding="utf-8")
        os.link(self.other, self.events / "m21_00_00_00.emevd.dcx")
        (self.events / "manifest.json").write_text(json.dumps({"tool_sha256": "c6ad97"}),
                                                   encoding="utf-8")

    def tearDown(self):
        self.folder.cleanup()

    def test_files_from_an_older_patcher_are_made_again(self):
        self.assertTrue(engines.refresh_events(self.mods, self.game_events, self.tool))
        self.assertEqual((self.events / "m21_00_00_00.emevd.dcx").read_text(encoding="utf-8"),
                         "new")
        # The other folder's copy keeps its own bytes: the link was replaced, not written through.
        self.assertEqual(self.other.read_text(encoding="utf-8"), "old")
        self.assertFalse(list(self.mods.parent.glob("*.tmp*")), "no work folder left behind")

    def test_files_from_this_patcher_stay(self):
        engines.refresh_events(self.mods, self.game_events, self.tool)
        (self.events / "m21_00_00_00.emevd.dcx").write_text("kept", encoding="utf-8")
        self.assertFalse(engines.refresh_events(self.mods, self.game_events, self.tool))
        self.assertEqual((self.events / "m21_00_00_00.emevd.dcx").read_text(encoding="utf-8"),
                         "kept")

    def test_a_patcher_that_fails_leaves_the_files(self):
        self.tool.write_text("import sys\nsys.exit(1)\n", encoding="utf-8")
        with self.assertRaises(RuntimeError):
            engines.refresh_events(self.mods, self.game_events, self.tool)
        self.assertEqual((self.events / "m21_00_00_00.emevd.dcx").read_text(encoding="utf-8"),
                         "old")


class GamesTest(unittest.TestCase):
    def test_the_rigs_port_games_are_told_from_someone_elses(self):
        exe = pathlib.Path(r"C:\bb\out\bbport\bin\bb-probe.exe")
        listing = "\n".join([
            "4120\tC:\\bb\\out\\bbport\\bin\\bb-probe.exe",
            "777\tC:\\Bloodborne.PC.Online - Copy\\SeamlessCoop\\shadps4.exe",
            "778\tC:\\Games\\bbport-windows\\bin\\bb-probe.exe",
        ])
        self.assertEqual(run_scenario.split_games(listing, exe), ([4120], [777, 778]))


if __name__ == "__main__":
    unittest.main()
