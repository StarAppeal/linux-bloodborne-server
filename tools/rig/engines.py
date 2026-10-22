# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""The two builds the rig plays Bloodborne with: the emulator (shadPS4, the default) and the
native port (Bloodborne PC, bbport/ in this repository, staged by tools/build-bbport.ps1).

Each keeps its own rig folders, saves and baseline (the emulator's in rig/<role>, the port's in
rig/port/<role>), plays on the same servers with the same accounts, and logs in shadPS4's format
to user/log/shad_log.txt, which the rig's checks read (tools/re/check_party_log.py).
"""
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys

BB_ROOT = pathlib.Path(os.environ.get("BB_ROOT", r"C:\bb"))
SHADNET_PORT = 32313
WEBAPI_PORT = SHADNET_PORT + 2
BACKEND_PORT = 20453
GAME_SERVER = "https://ss4.scej-network.jp:20443"
# The rig's accounts on its shadNet (rig-setup.ps1, run_scenario.py --add-guest2).
ACCOUNTS = {"host": "rig_host", "guest": "rig_guest", "guest2": "rig_guest2"}
PASSWORD = "rig"
# Every build's game process, the rig's own and anyone else's (a player's copy).
GAME_PROCESSES = ("shadPS4.exe", "bb-probe.exe")
# A lane (run_scenario.py --lanes): one of several rig runs side by side, each with its own
# servers and ports (LANE_PORT_STEP apart) and the port's own game folders (rig/port/lanes/<n>).
# Lane 0 is the rig as it always was.
LANE_PORT_STEP = 100
LANE = 0


def use_lane(lane: int):
    """This process plays in `lane`: its servers' ports and the port's role folders."""
    global LANE, SHADNET_PORT, WEBAPI_PORT, BACKEND_PORT
    LANE = lane
    SHADNET_PORT = 32313 + lane * LANE_PORT_STEP
    WEBAPI_PORT = SHADNET_PORT + 2
    BACKEND_PORT = 20453 + lane * LANE_PORT_STEP

# The port's settings on the rig (bbport.ini, read by the port and its scripts): as low as the
# emulator rig's (rig-lowspec.ps1), so two or three games fit on one PC. Set before every run,
# over what a tester or an older rig left (Kaniel 2026-10-20: his PC froze under the rig; every
# run, headless too, is 720p and lowest); the file's other lines stay.
LOW_SPEC_SETTINGS = """# The rig's low-spec profile (tools/rig/engines.py).
upscaler=off
preset=0
output_res=1280x720
window_mode=windowed
frame_generation=0
show_fps=0
live_resolution=0
model_lod=2
effect_chromatic_aberration=0
effect_dof=0
effect_motion_blur=0
effect_ssao=0
effect_game_aa=0
effect_dynamic_shadows=0
skip_intro=1
tweak_no_camera_rotation=1
"""


def low_spec(current: str) -> str:
    """`current` (a bbport.ini) with every LOW_SPEC_SETTINGS key at the rig's value."""
    forced = dict(line.split("=", 1) for line in LOW_SPEC_SETTINGS.splitlines()
                  if "=" in line and not line.startswith("#"))
    kept = [line for line in current.splitlines()
            if line.split("=", 1)[0].strip() not in forced and line.strip() and
            line != LOW_SPEC_SETTINGS.splitlines()[0]]
    return LOW_SPEC_SETTINGS + "".join(f"{line}\n" for line in kept)


class Emulator:
    """shadPS4 (out/emulator/build, tools/build-emu.ps1) on the rig's low-spec game."""

    name = "emulator"
    # What one game commits (its 8.25 GB backing for the PS4's memory, then Vulkan and the rest):
    # measured 12.3 GB. Windows refuses a game that does not fit ("Insufficient system resources").
    commit_gb = 12.5
    # Each game starts once the one before is in the world: two loading at once were slow.
    staggered_start = True
    # Phase 6: the port plays only in teams made on the title (the runner makes the scenario's).
    teams = False

    def __init__(self, root: pathlib.Path = BB_ROOT):
        self.rig = root / "rig"
        self.exe = root / "out" / "emulator" / "build" / "shadPS4.exe"
        self.game = self.rig / "game" / "CUSA03173" / "eboot.bin"

    def role_dir(self, role: str) -> pathlib.Path:
        return self.rig / role

    def user_dir(self, role: str) -> pathlib.Path:
        return self.role_dir(role) / "user"

    def saves(self, role: str) -> pathlib.Path:
        return self.user_dir(role) / "home" / "1000" / "savedata"

    def log_path(self, role: str) -> pathlib.Path:
        return self.user_dir(role) / "log" / "shad_log.txt"

    def shot_request(self, role: str) -> pathlib.Path:
        """Where a script's `shot <name>` asks the game for a picture of its next frame (the
        port's presenter takes it: BB_SHOT_TRIGGER)."""
        return self.role_dir(role) / "shot.request"

    def shots(self, role: str) -> pathlib.Path:
        """Where those pictures land, as <name>.png (AutoplayShotPath)."""
        return self.role_dir(role) / "shots"

    def command(self, role: str) -> list[str]:
        return [str(self.exe), "-g", str(self.game)]

    def environment(self, role: str) -> dict[str, str]:
        return {}  # its settings are in the role's user folder (rig-setup.ps1)

    def prepare(self, role: str, backend_port: int = BACKEND_PORT):
        pass

    def missing(self) -> str | None:
        """What the rig lacks to play with this build, if anything."""
        if not self.exe.is_file():
            return f"no emulator build at {self.exe} (tools/build-emu.ps1)"
        if not self.game.is_file():
            return f"no game at {self.game} (tools/rig-lowspec.ps1)"
        return None


class Port(Emulator):
    """The native port, started through its launcher script (run.py prepares the game image)."""

    name = "port"
    # The PS4's memory (5056 MiB direct, 448 MiB flexible: runtime_memory.c) and the renderer;
    # an estimate until measured on the rig.
    commit_gb = 7.5
    # The port's games load side by side: all start at once.
    staggered_start = False
    teams = True

    def __init__(self, root: pathlib.Path = BB_ROOT):
        super().__init__(root)
        # run.py, scripts, patches; the build in bin. BB_RIG_PORT_HOME runs another staged build
        # (a branch's own) without touching the shared one.
        self.home = pathlib.Path(os.environ.get("BB_RIG_PORT_HOME", root / "out" / "bbport"))
        self.exe = self.home / "bin" / "bb-probe.exe"
        # The game with its 1.09 update over it (merge_game, run_scenario.py --setup).
        self.game_dir = self.rig / "port" / "game" / "CUSA03173"
        # Potatoborne (rig-lowspec.ps1) and the co-op event files; BB_RIG_MODS runs a branch's own
        # (its party_boss_events.py output) without touching the shared folder.
        self.mods = pathlib.Path(os.environ.get("BB_RIG_MODS", self.rig / "game" / "CUSA03173-mods"))

    def role_dir(self, role: str, lane: int | None = None) -> pathlib.Path:
        lane = LANE if lane is None else lane
        if lane == 0:
            return self.rig / "port" / role
        return self.rig / "port" / "lanes" / str(lane) / role

    def saves(self, role: str) -> pathlib.Path:
        return self.user_dir(role) / "savedata"

    def command(self, role: str) -> list[str]:
        return [sys.executable, str(self.home / "run.py")]

    def environment(self, role: str) -> dict[str, str]:
        return {
            "BB_DATA_DIR": str(self.role_dir(role)),
            "BB_USER_DIR": str(self.user_dir(role)),
            "BB_GAME_DIR": str(self.game_dir),
            "BB_PROBE": str(self.exe),
            "BB_MODS_DIR": str(self.mods),
            "BB_FPS": os.environ.get("BB_RIG_FPS", "30"),  # BB_RIG_FPS: a --play at another rate
            "BB_DMEM_MB": "5056",
            "BB_LOG_FILE": str(self.log_path(role)),
            "BB_LOG_LEVEL": "info",
            "BB_ONLINE": "1",
            # Phase 11, one port: shadNet's link UDP port (the TCP port's + 1) signs the game in.
            "BB_SHADNET_SERVER": f"127.0.0.1:{SHADNET_PORT + 1}",
            "BB_SHADNET_WEBAPI": f"http://127.0.0.1:{WEBAPI_PORT}",
            "BB_SHADNET_NPID": ACCOUNTS[role],
            "BB_SHADNET_PASSWORD": PASSWORD,
            "BB_USER_NAME": ACCOUNTS[role],
            "BB_SHOT_TRIGGER": str(self.shot_request(role)),
            # The game's own button icons (run.py would draw them for a start without the launcher);
            # BB_RIG_ICONS=1 (a --play to look at them): as a player's start, bbport.ini's button_icons.
            "BB_ICONS": os.environ.get("BB_RIG_ICONS", "0"),
        }

    def missing(self) -> str | None:
        if not self.exe.is_file() or not (self.home / "run.py").is_file():
            return f"no port build at {self.home} (tools/build-bbport.ps1)"
        if not (self.game_dir / "eboot.bin").is_file():
            return (f"no game for the port at {self.game_dir} "
                    "(tools/rig/run_scenario.py --engine port --setup)")
        return None

    def prepare(self, role: str, backend_port: int = BACKEND_PORT):
        """The role's settings, and its game server: the rig's backend, or what stands in front
        of it at `backend_port` (run.py uses host_overrides.custom.json as it is)."""
        self.user_dir(role).mkdir(parents=True, exist_ok=True)
        settings = self.role_dir(role) / "bbport.ini"
        settings.write_text(low_spec(settings.read_text(encoding="utf-8")
                                     if settings.exists() else ""),
                            encoding="utf-8", newline="\n")
        overrides = {GAME_SERVER: f"http://127.0.0.1:{backend_port}"}
        (self.user_dir(role) / "host_overrides.custom.json").write_text(
            json.dumps(overrides, indent=2) + "\n", encoding="utf-8", newline="\n")
        # The shared folder follows this checkout's patcher; BB_RIG_MODS (a branch's own) is left
        # as it was made (and a rig with no game yet has nothing to make them from).
        game_events = self.game_dir / "dvdroot_ps4" / "event"
        if "BB_RIG_MODS" not in os.environ and (game_events / "common.emevd.dcx").is_file():
            refresh_events(self.mods, game_events)


# The co-op event patcher (the launcher's copy makes a player's files the same way).
EVENT_TOOL = pathlib.Path(__file__).resolve().parents[1] / "re" / "party_boss_events.py"


def refresh_events(mods: pathlib.Path, game_events: pathlib.Path,
                   tool: pathlib.Path = EVENT_TOOL) -> bool:
    """Makes `mods`' event files again from `game_events` when another patcher made them (its
    manifest names the patcher's hash, as the launcher's events_current checks). Nothing else did
    for the rig: on 2026-10-22 a full run on main played an Oct 20 m21_00 patch without 12105040
    and failed dream-shop-messengers. Lanes may do this at once: each makes its own work folder
    and the files are the same. Each file is replaced, not written through, since the shared
    files can be hard links to a kit's. True when the files were made."""
    events = mods / "dvdroot_ps4" / "event"
    digest = hashlib.sha256(tool.read_bytes()).hexdigest()
    try:
        made = json.loads((events / "manifest.json").read_text(encoding="utf-8"))
        if made.get("tool_sha256") == digest:
            return False
    except (OSError, ValueError):
        pass
    work = mods.parent / f"{mods.name}-events.tmp{os.getpid()}"
    shutil.rmtree(work, ignore_errors=True)
    try:
        result = subprocess.run([sys.executable, str(tool), str(work), str(game_events)],
                                capture_output=True, text=True)
        if result.returncode != 0 or not (work / "manifest.json").is_file():
            raise RuntimeError(f"the event patcher {tool} failed: {result.stdout}{result.stderr}")
        events.mkdir(parents=True, exist_ok=True)
        # The manifest last: until it is in, the next start makes them again.
        for file in sorted(work.iterdir(), key=lambda path: path.name == "manifest.json"):
            os.replace(file, events / file.name)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"rig: event files in {events} made again by {tool.name} ({digest[:12]})")
    return True


ENGINES = {engine.name: engine for engine in (Emulator(), Port())}


def link(path: pathlib.Path, target: pathlib.Path):
    """`path` shows `target` without copying it: a junction for a folder (a symlink off Windows),
    a hard link for a file, a copy when the file is on another drive."""
    if target.is_dir():
        if os.name == "nt":
            import _winapi
            _winapi.CreateJunction(str(target.resolve()), str(path))
        else:
            path.symlink_to(target.resolve(), target_is_directory=True)
        return
    try:
        os.link(target, path)
    except OSError:
        shutil.copy2(target, path)


def is_link(path: pathlib.Path) -> bool:
    return path.is_symlink() or getattr(os.path, "isjunction", lambda _: False)(path)


def unlink(path: pathlib.Path):
    """Removes a link itself (a junction or folder link is a folder to Windows)."""
    if os.name == "nt" and path.is_dir():
        os.rmdir(path)
    else:
        path.unlink()


def remove_view(view: pathlib.Path):
    """A game view made by merge_game, links and all, never what they show."""
    if is_link(view):
        unlink(view)
        return
    if not view.is_dir():
        return
    for entry in view.iterdir():
        if is_link(entry):
            unlink(entry)
        elif entry.is_dir():
            remove_view(entry)
        else:
            entry.unlink()  # a hard link (the file stays in the game) or a copy
    view.rmdir()


def merge_game(game: pathlib.Path, update: pathlib.Path, view: pathlib.Path):
    """The game with its update over it, in `view`, made of links: the emulator reads a game's
    "-patch" folder over the game itself, the port reads one folder. Neither is changed."""
    remove_view(view)
    view.mkdir(parents=True)
    _merge(game, update if update.is_dir() else None, view)


def _merge(game: pathlib.Path, update: pathlib.Path | None, view: pathlib.Path):
    entries = {entry.name.casefold(): [entry, None] for entry in game.iterdir()}
    for entry in update.iterdir() if update else ():
        entries.setdefault(entry.name.casefold(), [None, None])[1] = entry
    for own, newer in entries.values():
        if newer is None:
            link(view / own.name, own)
        elif own is not None and own.is_dir() and newer.is_dir():
            (view / own.name).mkdir()
            _merge(own, newer, view / own.name)
        else:
            link(view / newer.name, newer)


def copy_saves(emulator_saves: pathlib.Path, port_saves: pathlib.Path, user_id: int = 1):
    """The emulator's saves (<title id>/<save folder>/) where the port keeps them:
    <user id>/<title id>/<save folder>/ (bbport/src/runtime_savedata.c, user 1)."""
    destination = port_saves / str(user_id)
    for title in emulator_saves.iterdir():
        if title.is_dir():
            shutil.rmtree(destination / title.name, ignore_errors=True)
            shutil.copytree(title, destination / title.name)
