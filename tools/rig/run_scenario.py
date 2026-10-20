# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Runs a seamless co-op test on the rig with no one at the pad.

Starts the rig's servers afresh for each scenario (no sign, claim or party board left over from
the last one), then each role's game with an autoplay script (SHADPS4_BLOODBORNE_AUTOPLAY, see
ParseAutoplayScript in bloodborne_party.h), waits until every game logs "Bloodborne autoplay:
done" or the time runs out, stops the games and checks the logs: every step ok, the scenario's
expected lines there, and no known problem (check_party_log.py). Each run's game and server logs
stay in rig/runs/<when>/<scenario>/ (the last RUNS_KEPT batches).

  py -3.12 tools/rig/run_scenario.py <scenario> [<scenario>...] [--keep-servers] [--list]
  py -3.12 tools/rig/run_scenario.py --engine port smoke   (a suite: suites.py)
  py -3.12 tools/rig/run_scenario.py --engine port trips --chapter dog   (one of its chapters)
  py -3.12 tools/rig/run_scenario.py --engine port --setup-lanes 3   (once: lanes 1 and 2)
  py -3.12 tools/rig/run_scenario.py --engine port --lanes 3 smoke trips together ...
  py -3.12 tools/rig/run_scenario.py --engine port --headless --lanes 3 --tier full
  py -3.12 tools/rig/run_scenario.py --engine port --lanes-status   (who holds which lane)
  py -3.12 tools/rig/run_scenario.py --save-baseline
  py -3.12 tools/rig/run_scenario.py --add-guest2   (a third game for parties of three)
  py -3.12 tools/rig/run_scenario.py --boot-check host [--seconds 40]   (tools/smoke-boot.ps1)
  py -3.12 tools/rig/run_scenario.py --engine port --headless --lane 2 --boot-check host \
      --cache-twice <a cache a game with a GPU wrote>   (one pipeline cached twice: skipped)
  py -3.12 tools/rig/run_scenario.py --engine port --play down-revive   (games to play by hand)

A suite (suites.py) runs many chapters in one session from one boot: its games wait for each
other at sync steps (the runner writes each go file once every game in it got there) and each
chapter is judged on its own lines of the logs.

Every scenario starts from the same saves: --save-baseline keeps each role's save and party files
(rig/<role>/baseline) and every run puts them back first, so one test's progress (a boss killed,
a lamp lit, a remembered spot) never leaks into the next.

--play <scenario> starts that scenario's games with its settings (down-revive: two players with
party death on, down-revive3: three), lets them get into the world and into one session by
themselves, then leaves them to the person at the PC; the servers stay up until every game is
closed.

--engine port plays with the native port (Bloodborne PC, tools/build-bbport.ps1) instead of the
emulator, in rig/port/<role> (engines.py); --engine port --setup makes those folders from the
emulator rig's (its game with the update, each role's baseline saves and party files).

Time budget and tiers (budget.py): a scenario's timeout is at most 300 s, a suite chapter's
budget 120 s (the game gives a chapter up past it), no fixed wait over 30 s; --tier quick|full|long
runs a tier (quick: smoke and the suites named after it). --list shows each one's time.

Lanes are shared between threads (lanes.py): a run takes its lane (rig/lanes/<n>/owner, named by
--owner or BB_RIG_OWNER) and frees it at its end; --lanes N takes the N lowest free ones. A lane
whose games outlive their run and cannot be stopped is marked blocked until they are gone.
"""
import argparse
import math
import os
import pathlib
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "re"))
import budget  # noqa: E402
import build_lock  # noqa: E402
import check_party_log  # noqa: E402
import engines  # noqa: E402
import lanes  # noqa: E402
import latency  # noqa: E402
from engines import BACKEND_PORT, BB_ROOT, SHADNET_PORT, WEBAPI_PORT  # noqa: E402
from scenarios import SCENARIOS, START  # noqa: E402
from suites import SUITE_SCENARIOS, SUITES, compile_suite  # noqa: E402

# What the runner runs by name: the scenarios and the suites (suites.py), one kind of thing.
RUNNABLE = {**SCENARIOS, **SUITE_SCENARIOS}

RIG = BB_ROOT / "rig"
RUNS = RIG / "runs"
RUNS_KEPT = 20

# Each role's game: its P2P port and party role. Its folder (its own user dir, save and account)
# is the build's (engines.py).
ROLES = {
    "host": {"port": 51600, "role": "host"},
    "guest": {"port": 51601, "role": "guest"},
    "guest2": {"port": 51602, "role": "guest"},
}

# The build the games run: the emulator unless --engine says otherwise.
ENGINE = engines.ENGINES["emulator"]

# The lane this process plays in (--lane, engines.use_lane): 0 is the rig as it always was;
# lane n has its own servers (rig/lanes/<n>), ports and the port's game folders, so several runs
# go side by side (--lanes). A lane's games are the ones this process started (LAUNCHED): it
# stops only those, never another lane's.
LANE = 0
# Whether this process is one lane of a --lanes batch, lane 0 too (rig 2026-10-13: lane 0's end of
# a run closed the other lanes' games, which then ran again as crashed).
IN_LANES = False
LAUNCHED: list[subprocess.Popen] = []
# Each lane's P2P ports this far after the last's (the roles' own differ by 1).
LANE_P2P_STEP = 10


def use_lane(lane: int, in_lanes: bool = False):
    global LANE, IN_LANES, SHADNET_PORT, WEBAPI_PORT, BACKEND_PORT
    engines.use_lane(lane)
    LANE = lane
    IN_LANES = in_lanes or lane != 0
    SHADNET_PORT, WEBAPI_PORT, BACKEND_PORT = (engines.SHADNET_PORT, engines.WEBAPI_PORT,
                                               engines.BACKEND_PORT)


def lane_dir(lane: int | None = None) -> pathlib.Path:
    """Where a lane's servers live: rig/ itself for lane 0."""
    lane = LANE if lane is None else lane
    return RIG if lane == 0 else RIG / "lanes" / str(lane)


def p2p_port(role: str) -> int:
    return ROLES[role]["port"] + LANE * LANE_P2P_STEP


def use_engine(name: str):
    global ENGINE
    ENGINE = engines.ENGINES[name]


# The round trip (ms, low and high) the games have to each other and to the servers, or None:
# their P2P packets are held by the games themselves, the servers' traffic by delaying proxies
# (latency.py) 1000 ports above each server (the port's games only: the emulator's server
# addresses are in its own settings).
PING = None
DELAYED_PORT_OFFSET = 1000


def use_ping(ping: tuple[int, int] | None):
    global PING
    PING = ping


def delayed_ports() -> dict[int, int]:
    """Each delaying proxy's port and the server port it passes on to."""
    return {port + DELAYED_PORT_OFFSET: port for port in (SHADNET_PORT, WEBAPI_PORT, BACKEND_PORT)}


def server_port(port: int) -> int:
    """Where the games reach a server: through its proxy when there is a ping."""
    return port + DELAYED_PORT_OFFSET if PING else port


def matching_udp_port() -> int:
    """Where shadNet takes the games' STUN pings: the port they reach it on + 1, as the games
    derive it (np_matching2_mm.cpp). With a ping that is the proxy's port + 1, and shadNet itself
    listens there: UDP must come straight from each game, whose address it records."""
    return server_port(SHADNET_PORT) + 1


def with_setting(config: str, key: str, value: int) -> str:
    """shadnet.cfg's text with `key` set to `value`."""
    return re.sub(rf"(?m)^{key}=\d+$", f"{key}={value}", config)


def with_value(config: str, key: str, value) -> str:
    """shadnet.cfg's text with `key` set to `value`, added under [General] when it is missing."""
    line = f"{key}={value}"
    if re.search(rf"(?m)^{key}=", config):
        return re.sub(rf"(?m)^{key}=.*$", lambda _: line, config)
    if re.search(r"(?m)^\[General\]$", config):
        return re.sub(r"(?m)^\[General\]$", lambda _: "[General]\n" + line, config, count=1)
    return f"[General]\n{line}\n" + config


# The game's online services (ss.info, login, messages): shadnet answers them itself
# (BloodborneBackend). BB_PYTHON_BACKEND=1 runs backend/bb_dummy_server.py for them as before.
def builtin_backend() -> bool:
    return os.environ.get("BB_PYTHON_BACKEND") != "1"


def with_backend(config: str, builtin: bool) -> str:
    """shadnet.cfg's text with its own backend on the lane's backend port, or off."""
    config = with_value(config, "BloodborneBackend", "true" if builtin else "false")
    config = with_value(config, "BackendPort", BACKEND_PORT)
    return with_value(config, "BackendAddress", "127.0.0.1")


def with_matching_udp_port(config: str, port: int) -> str:
    """shadnet.cfg's text with its MatchingUdpPort set to `port`."""
    return with_setting(config, "MatchingUdpPort", port)


def configure_shadnet():
    """The lane's shadNet on the lane's ports (lane 0's are the ones it always had)."""
    config = lane_dir() / "server" / "shadnet.cfg"
    if config.is_file():
        text = config.read_text(encoding="utf-8")
        wanted = with_matching_udp_port(text, matching_udp_port())
        wanted = with_setting(wanted, "UnsecuredPort", SHADNET_PORT)
        wanted = with_setting(wanted, "WebApiPort", WEBAPI_PORT)
        wanted = with_backend(wanted, builtin_backend())
        # BB_RIG_BACKEND_STORE=1: the backend keeps messages and chalice dungeons in a fresh file.
        store = lane_dir() / "server" / "rig-content.sqlite"
        store.unlink(missing_ok=True)
        wanted = with_value(wanted, "BackendStore",
                            store.as_posix() if os.environ.get("BB_RIG_BACKEND_STORE") == "1" else "")
        if wanted != text:
            config.write_text(wanted, encoding="utf-8", newline="")


# The servers' delaying proxies of this run (a suite's outage holds them).
PROXIES: list[latency.DelayProxy] = []


def start_delay_proxies() -> list[latency.DelayProxy]:
    if not PING:
        return []
    proxies = [latency.DelayProxy(listen, target, latency.one_way(PING))
               for listen, target in delayed_ports().items()]
    for proxy in proxies:
        proxy.start()
    print(f"ping {PING[0]}-{PING[1]} ms between the games and to the servers")
    return proxies

# --headless: the port draws nothing (its command processor still writes the game's labels, so
# the game runs as with a GPU) and plays no sound.
HEADLESS = {"BB_NULL_GPU": "1", "BB_AUDIO": "none"}

DONE = re.compile(r"Bloodborne autoplay: done \((ok|failed)\)")
IN_WORLD = "Bloodborne autoplay: in the world"
STEP_FAILED = re.compile(r"Bloodborne autoplay: step .* FAILED: .*")
# A suite (suites.py): its games mark each chapter, wait for each other at syncs, and with
# keep-going on give up a chapter whose step failed (bloodborne_re.cpp).
SYNC_REACHED = re.compile(r"Bloodborne autoplay: sync (\S+) reached")
CHAPTER = re.compile(r"Bloodborne autoplay: chapter (\S+)[ \t\r]*$", re.M)
CHAPTER_GIVEN_UP = re.compile(r"Bloodborne autoplay: chapter (\S*) given up")


def syncs_reached(text: str) -> set[str]:
    return set(SYNC_REACHED.findall(text))


def syncs_to_release(syncs: dict[str, dict], reached: dict[str, set[str]], given_up: set[str],
                     finished: set[str], released: set[str]) -> list[str]:
    """The syncs to let go now, in the suite's order: every game in one reached it (a game whose
    script ended or crashed never will), or a game gave up its chapter (the others would wait in
    vain)."""
    return [name for name, sync in syncs.items()
            if name not in released and (
                sync["chapter"] in given_up or
                all(role in finished or name in reached.get(role, set())
                    for role in sync["roles"]))]


def sync_dir(role_dir: pathlib.Path) -> pathlib.Path:
    """Where the games look for their go files (AutoplaySyncPath: beside autoplay.txt)."""
    return role_dir / "sync"


def write_go_file(role_dir: pathlib.Path, name: str):
    folder = sync_dir(role_dir)
    folder.mkdir(parents=True, exist_ok=True)
    (folder / name).write_text("go\n", encoding="utf-8")


def clear_go_files(role_dir: pathlib.Path):
    shutil.rmtree(sync_dir(role_dir), ignore_errors=True)


def chapter_sections(text: str) -> dict[str, str]:
    """A suite game's log by chapter, in the order they began: each chapter's lines up to the
    next chapter's mark (a game restarted in a chapter marks it again: its lines join it)."""
    marks = list(CHAPTER.finditer(text))
    sections: dict[str, str] = {}
    for index, mark in enumerate(marks):
        end = marks[index + 1].start() if index + 1 < len(marks) else len(text)
        sections[mark.group(1)] = sections.get(mark.group(1), "") + text[mark.start():end]
    return sections


# How far (m, on the ground) an enemy that comes for a player moves between two censuses.
EnemyMovesM = 1.5
# How far (m) an enemy may drop below its first census before it counts as fallen through.
GroundedDropM = 3.0


def judge(spec: dict, texts: dict[str, str]) -> list[str]:
    """What is wrong in the roles' logs by a scenario's (or a suite chapter's) checks: failed
    steps, expected lines missing, forbidden lines seen, the shared enemy's HP and hits, and the
    known problems (check_party_log.py) it does not allow. Each problem starts with its role."""
    problems = []
    if handle := spec.get("same_hp"):
        for index in [*spec.get("same_hp_census", []), -1]:
            where = "last census" if index < 0 else f"census {index + 1}"
            if problem := same_hp_problem(texts, handle, index):
                problems.append(f"all: {where}: {problem}")
    if handle := spec.get("hp_sums_hits"):
        if problem := hits_sum_problem(texts, handle):
            problems.append(f"all: {problem}")
    if limit := spec.get("max_hp_lag"):
        lag = hp_board_lag(texts, spec["hp_sums_hits"])
        # In milliseconds: the games' clock sums leave float noise (0.15 as 0.1500000000014).
        if lag is None or round(lag, 3) > limit:
            problems.append(f"all: {spec['hp_sums_hits']} reached the other games' bars in "
                            f"{'?' if lag is None else f'{lag:.2f}'} s (at most {limit} s)")
    if limit := spec.get("max_hp_lag_each"):
        for (hitter, other), lag in hp_lag_pairs(texts, spec["hp_sums_hits"]).items():
            if round(lag, 3) > limit:
                problems.append(f"all: {hitter}'s hits reached {other}'s bar in {lag:.2f} s "
                                f"(at most {limit} s)")
    if limit := spec.get("max_kill_lag"):
        lags = kill_lags(texts, spec["killed"])
        if not lags:
            problems.append(f"all: no game saw {spec['killed']} die from the killing hit")
        for other, lag in lags.items():
            if lag is None or round(lag, 3) > limit:
                problems.append(f"{other}: {spec['killed']} died "
                                f"{'never' if lag is None else f'{lag:.2f} s'} after the killing "
                                f"hit (at most {limit} s)")
    # "moves": an enemy that comes for the role's player moves between its first and last census
    # there (Kaniel 2026-10-14: beside a guest whose host was far away, the enemies stood still).
    for role, handle in spec.get("moves", {}).items():
        places = [(float(x), float(z)) for x, z in re.findall(
            rf"autoplay census: {re.escape(handle)} hp .* at [\d.]+ m \((-?[\d.]+), -?[\d.]+, "
            rf"(-?[\d.]+)\)", texts.get(role, ""))]
        if len(places) < 2 or math.dist(places[0], places[-1]) < EnemyMovesM:
            problems.append(f"{role}: {handle} stood still beside it "
                            f"({'no census' if len(places) < 2 else places[0]} -> "
                            f"{places[-1] if places else ''})")
    # "position_rate": the role's game took [handle]'s position at least [per s] times a second
    # in some 10 s of its follower's log (Kaniel 2026-10-15: every enemy by a far guest lagged).
    for role, (handle, per_s) in spec.get("position_rate", {}).items():
        bare = handle.lower().removeprefix("0x")
        rates = [float(rate) for rate in re.findall(
            rf"positions taken per s by this player: .*?\b{re.escape(bare)} ([\d.]+)",
            texts.get(role, ""))]
        if not rates or max(rates) < per_s:
            problems.append(f"{role}: took {handle}'s position "
                            f"{f'{max(rates):.1f}' if rates else 'never'} times a second at best "
                            f"(at least {per_s} wanted)")
    # "grounded": an enemy by the role stays on the ground in its censuses: never more than
    # GroundedDropM below where it was first (phase 8: collision under enemies far from the owner).
    for role, handle in spec.get("grounded", {}).items():
        heights = [float(y) for y in re.findall(
            rf"autoplay census: {re.escape(handle)} hp .* at [\d.]+ m \(-?[\d.]+, (-?[\d.]+), "
            rf"-?[\d.]+\)", texts.get(role, ""))]
        if len(heights) < 2:
            problems.append(f"{role}: {handle} has {len(heights)} census, at least 2 wanted")
        elif min(heights) < heights[0] - GroundedDropM:
            problems.append(f"{role}: {handle} fell from {heights[0]:.1f} to {min(heights):.1f}")
    # "max_join_s": a game started again in the chapter joins within that many seconds of its
    # load's end (its last chapter mark: the restarted game's).
    for role, limit in spec.get("max_join_s", {}).items():
        text = texts.get(role, "")
        last = text.rfind("Bloodborne autoplay: chapter ")
        joined = join_times({role: text[last:] if last >= 0 else text}).get(role)
        if joined is None or joined > limit:
            problems.append(f"{role}: joined {'never' if joined is None else f'{joined:.0f} s'} "
                            f"after its load (at most {limit} s)")
    # "stays": the role's own character stays within that many metres between its first and last
    # census (Kaniel 2026-10-15: one guest's backstab dragged the other guest to the enemy).
    for role, metres in spec.get("stays", {}).items():
        places = [(float(x), float(z)) for x, z in re.findall(
            r"autoplay census: map 0x[0-9a-f]+, \d+ characters within [\d.]+ m of "
            r"\((-?[\d.]+), -?[\d.]+, (-?[\d.]+)\)", texts.get(role, ""))]
        if len(places) < 2 or math.dist(places[0], places[-1]) > metres:
            problems.append(f"{role}: its own character moved without it "
                            f"({'no census' if len(places) < 2 else places[0]} -> "
                            f"{places[-1] if places else ''})")
    for role, text in texts.items():
        problems += [f"{role}: {failed}" for failed in STEP_FAILED.findall(text)]
        for pattern in spec.get("expect", {}).get(role, []):
            if not re.search(pattern, text):
                problems.append(f"{role}: expected, not seen: {pattern}")
        for pattern in spec.get("forbid", {}).get(role, []):
            if found := re.search(pattern, text):
                problems.append(f"{role}: must not happen, seen: {found.group(0)[:160]}")
        _, known = check_party_log.summarize(text)
        allowed = spec.get("allow", [])
        problems += [f"{role}: PROBLEM {problem}" for problem in known
                     if not any(re.search(pattern, problem) for pattern in allowed)]
    return problems


def port_open(port: int) -> bool:
    with socket.socket() as sock:
        sock.settimeout(0.5)
        return sock.connect_ex(("127.0.0.1", port)) == 0


def split_games(listing: str, exe: pathlib.Path,
                rig_folder: pathlib.Path | None = None) -> tuple[list[int], list[int]]:
    """The rig's games (the build's game executable, or any build playing in the rig's folders:
    another thread's staged build in its lane) and every other one (Kaniel's own copy), from
    "<pid>\t<path>[\t<command line>]" lines."""
    rig, others = [], []
    folder = str(rig_folder).lower().rstrip("\\/") + os.sep if rig_folder else None
    for line in listing.splitlines():
        pid, _, rest = line.strip().partition("\t")
        path, _, command = rest.partition("\t")
        if pid.isdigit():
            same = path.strip().lower() == str(exe).lower()
            in_rig = folder is not None and folder in command.lower().replace("/", os.sep)
            (rig if same or in_rig else others).append(int(pid))
    return rig, others


def outside_lanes(listing: str, exe: pathlib.Path, rig_folder: pathlib.Path) -> list[int]:
    """The rig's games that are not in a lane (rig/port/lanes/<n>): what a run with no lane may
    stop. Another thread's lane is its own (rig 2026-10-18: a run with no --lane closed a lane-3
    run's games)."""
    lanes_folder = (str(rig_folder).lower().rstrip("\\/") + os.sep + "port" + os.sep + "lanes"
                    + os.sep)
    commands = {}
    for line in listing.splitlines():
        pid, _, rest = line.strip().partition("\t")
        if pid.isdigit():
            commands[int(pid)] = rest.partition("\t")[2].lower().replace("/", os.sep)
    return [pid for pid in split_games(listing, exe, rig_folder)[0]
            if lanes_folder not in commands.get(pid, "")]


def games_listing() -> str:
    names = " OR ".join(f"Name='{name}'" for name in engines.GAME_PROCESSES)
    return subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         f"Get-CimInstance Win32_Process -Filter \"{names}\" | "
         "ForEach-Object { \"$($_.ProcessId)`t$($_.ExecutablePath)`t$($_.CommandLine)\" }"],
        capture_output=True, text=True).stdout


def stoppable_games() -> list[int]:
    """The games a run with no lane closes: the rig's own outside the lanes."""
    ignored = {int(pid) for pid in os.environ.get("BB_RIG_IGNORE_PIDS", "").split(",")
               if pid.strip().isdigit()}
    return [pid for pid in outside_lanes(games_listing(), ENGINE.exe, RIG) if pid not in ignored]


def games() -> tuple[list[int], list[int]]:
    """Every build's games: someone playing the emulator stops a port run too."""
    names = " OR ".join(f"Name='{name}'" for name in engines.GAME_PROCESSES)
    listing = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         f"Get-CimInstance Win32_Process -Filter \"{names}\" | "
         "ForEach-Object { \"$($_.ProcessId)`t$($_.ExecutablePath)`t$($_.CommandLine)\" }"],
        capture_output=True, text=True).stdout
    rig, others = split_games(listing, ENGINE.exe, RIG)
    # BB_RIG_IGNORE_PIDS: games that cannot be ended and run nothing any more (a window closed
    # while the GPU teardown hung, 2026-10-14), so they hold up no run.
    ignored = {int(pid) for pid in os.environ.get("BB_RIG_IGNORE_PIDS", "").split(",")
               if pid.strip().isdigit()}
    return ([pid for pid in rig if pid not in ignored],
            [pid for pid in others if pid not in ignored])


def player_blocks() -> bool:
    """Whether a game someone plays keeps the rig from starting its own. BB_RIG_BESIDE_PLAYER=1:
    a lane's headless games may run beside it (the player said so; the rig only ever closes its
    own games)."""
    return bool(games()[1]) and not (IN_LANES and os.environ.get("BB_RIG_BESIDE_PLAYER") == "1")


def games_running() -> bool:
    rig, others = games()
    return bool(rig or others)


def stop_games():
    """Closes the rig's games, never another copy, and waits until they are gone (their logs
    stay open until then). A window close first: a game stopped hard leaves some of its memory
    with the GPU driver until Windows restarts (0.2-0.4 GB a game, measured), which over a day of
    runs left no room for three games. A game that does not close in 15 s is stopped."""
    if IN_LANES:
        stop_lane_games()
        return
    for pid in stoppable_games():
        subprocess.run(["taskkill", "/PID", str(pid)], capture_output=True)
    deadline = time.time() + 15
    while time.time() < deadline and stoppable_games():
        time.sleep(0.5)
    for pid in stoppable_games():
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
    deadline = time.time() + 20
    while time.time() < deadline and stoppable_games():
        time.sleep(0.5)


def stop_lane_games():
    """A lane's games: the ones this process started (the port's launcher and the game under
    it), a window close first as stop_games does."""
    running = [game for game in LAUNCHED if game.poll() is None]
    for game in running:
        subprocess.run(["taskkill", "/T", "/PID", str(game.pid)], capture_output=True)
    deadline = time.time() + 15
    while time.time() < deadline and any(game.poll() is None for game in running):
        time.sleep(0.5)
    for game in running:
        stop_game(game)
    LAUNCHED.clear()


def stop_lane_strays(roles: list[str]):
    """The lane's games that outlived its run (a hung game the window close and taskkill /T
    missed): stopped by their command line (the lane's role folders), never another lane's. A
    game that still will not go marks the lane blocked (lanes.py) for the runs after this one."""
    if ENGINE.name != "port":
        return
    folders = [ENGINE.role_dir(role) for role in roles]

    def listing() -> str:
        return subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "Get-CimInstance Win32_Process -Filter \"Name='bb-probe.exe'\" | "
             "ForEach-Object { \"$($_.ProcessId)`t$($_.CommandLine)\" }"],
            capture_output=True, text=True).stdout

    found = lanes.strays(listing(), folders)
    if not found:
        return
    print(f"  lane {LANE}: games left after the run ({', '.join(map(str, found))}): stopping them")
    for pid in found:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], capture_output=True)
    deadline = time.time() + 20
    while time.time() < deadline and lanes.strays(listing(), folders):
        time.sleep(1)
    if left := lanes.strays(listing(), folders):
        lanes.block(RIG, LANE, left)
        print(f"  lane {LANE}: blocked, {', '.join(map(str, left))} will not stop")


def listening_pids(netstat: str, port: int) -> set[int]:
    """The processes `netstat -ano -p TCP` shows listening on `port`."""
    pids = set()
    for line in netstat.splitlines():
        parts = line.split()
        if (len(parts) >= 5 and parts[0] == "TCP" and parts[1].endswith(f":{port}")
                and parts[3] == "LISTENING" and parts[4].isdigit()):
            pids.add(int(parts[4]))
    return pids


def stop_servers():
    """Stops whatever serves the rig's ports (only the rig's: the online kit's are others)."""
    netstat = subprocess.run(["netstat", "-ano", "-p", "TCP"], capture_output=True,
                             text=True).stdout
    for pid in listening_pids(netstat, SHADNET_PORT) | listening_pids(netstat, BACKEND_PORT):
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
    deadline = time.time() + 15
    while time.time() < deadline and (port_open(SHADNET_PORT) or port_open(BACKEND_PORT)):
        time.sleep(0.5)


def start_server(command: list[str], cwd: pathlib.Path, log: pathlib.Path | None):
    env = dict(os.environ, SHADNET_BLOODBORNE_RE_TRACE="1")  # every summon request's body
    if log is None:
        subprocess.Popen(command, cwd=cwd, env=env, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NEW_CONSOLE)
        return
    log.parent.mkdir(parents=True, exist_ok=True)
    with open(log, "wb") as output:  # the server keeps its own handle
        subprocess.Popen(command, cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT,
                         creationflags=subprocess.CREATE_NEW_CONSOLE)


def install_shadnet(build: str | None, server: pathlib.Path | None = None) -> bool:
    """BB_RIG_SHADNET: a thread's own shadnet.exe, put into the lane's server folder when it
    differs (shadnet reads its settings beside itself: the lane's ports and accounts are there;
    the lanes held servers of different days, 2026-10-15). The one there before stays as
    shadnet.exe.prev. Whether it was put in."""
    if not build:
        return False
    source = pathlib.Path(build)
    target = (server or lane_dir() / "server") / "shadnet.exe"
    if target.is_file() and target.read_bytes() == source.read_bytes():
        return False
    if target.is_file():
        shutil.copy2(target, target.with_name("shadnet.exe.prev"))
    shutil.copy2(source, target)
    print(f"  shadnet from {source} put into {target.parent}")
    return True


def start_servers(log_dir: pathlib.Path | None = None):
    """The rig's shadNet and backend, the ones not up yet, their output in log_dir."""
    if not port_open(SHADNET_PORT):
        configure_shadnet()
        install_shadnet(os.environ.get("BB_RIG_SHADNET"))
        start_server([str(lane_dir() / "server" / "shadnet.exe")], lane_dir() / "server",
                     log_dir / "shadnet.log" if log_dir else None)
        # shadnet's own backend takes the backend port; the Python one only when it did not.
        wanted = [SHADNET_PORT] + ([BACKEND_PORT] if builtin_backend() else [])
        deadline = time.time() + 15
        while time.time() < deadline and not all(map(port_open, wanted)):
            time.sleep(0.25)
    if not port_open(BACKEND_PORT):
        start_server([sys.executable, "bb_dummy_server.py", "--address", "127.0.0.1", "--port",
                      str(BACKEND_PORT), "--shadnet-webapi", f"http://127.0.0.1:{WEBAPI_PORT}"],
                     lane_dir() / "backend", log_dir / "backend.log" if log_dir else None)
    deadline = time.time() + 30
    while time.time() < deadline and not (port_open(SHADNET_PORT) and port_open(BACKEND_PORT)):
        time.sleep(0.5)


# What a lane's copy of the rig leaves out: the port's prepared game image (run.py makes it again,
# hundreds of GB of links), logs, pictures, sync files, save backups and old server binaries.
LANE_SKIPPED = shutil.ignore_patterns("out", "log", "captures", "shots", "sync", "save-backups",
                                      "*.bak", "*.err", "*.log", "*.out", "*-before-*", "trace*")


def setup_lane(lane: int) -> list[pathlib.Path]:
    """Lane `lane`'s own copy of the rig: its servers (rig/lanes/<n>/server and backend, the
    same accounts) and each role's port folder (saves, baseline, settings), from lane 0's. What
    exists stays as it is. The folders made."""
    made = []
    for name in ("server", "backend"):
        source, target = lane_dir(0) / name, lane_dir(lane) / name
        if source.is_dir() and not target.exists():
            shutil.copytree(source, target, ignore=LANE_SKIPPED)
            made.append(target)
    for role in ROLES:
        source, target = ENGINE.role_dir(role, 0), ENGINE.role_dir(role, lane)
        if source.is_dir() and not target.exists():
            shutil.copytree(source, target, ignore=LANE_SKIPPED)
            (target / "user" / "log").mkdir(parents=True, exist_ok=True)
            made.append(target)
    return made


# --lanes: a lane starts its first run this long after the one before (their games load at once
# otherwise, and their logins and joins meet on one PC at the same moment).
LANE_STAGGER = 45


def run_in_lanes(names: list[str], lanes: int | list[int], runner, stagger: float = LANE_STAGGER,
                 sleep=time.sleep) -> dict[str, bool]:
    """Runs `names` across the lanes (a list, or a count: 0..lanes-1), each lane one run at a
    time and taking the next name as it finishes (the longest first, as given).
    runner(lane, name) -> passed."""
    lane_list = list(range(lanes)) if isinstance(lanes, int) else list(lanes)
    pending = list(names)
    lock = threading.Lock()
    results: dict[str, bool] = {}

    def lane_worker(lane: int):
        sleep(lane_list.index(lane) * stagger)
        while True:
            with lock:
                if not pending:
                    return
                name = pending.pop(0)
            passed = runner(lane, name)
            with lock:
                results[name] = passed

    workers = [threading.Thread(target=lane_worker, args=(lane,)) for lane in lane_list]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()
    return {name: results.get(name, False) for name in names}


def lane_command(lane: int, name: str, batch: pathlib.Path) -> list[str]:
    """This runner again, for one run in one lane, into the batch's folder."""
    ping = "0" if PING is None else f"{PING[0]}-{PING[1]}"
    return [sys.executable, "-u", str(pathlib.Path(__file__).resolve()), "--engine", ENGINE.name,
            "--lane", str(lane), "--batch", str(batch), "--ping", ping, name]


# Set for a lane's run started by --lanes: the lane it was given (the --lanes run holds it).
LANE_HELD = "BB_RIG_LANE_HELD"


def run_lanes(names: list[str], lanes: list[int], batch: pathlib.Path) -> dict[str, bool]:
    """--lanes: each run in its own process and lane, its output in <batch>/lane-<n>-<name>.txt."""
    batch.mkdir(parents=True, exist_ok=True)

    def runner(lane: int, name: str) -> bool:
        output = batch / f"lane-{lane}-{name}.txt"
        print(f"lane {lane}: {name} (output in {output})", flush=True)
        with open(output, "wb") as out:
            code = subprocess.run(lane_command(lane, name, batch), stdout=out,
                                  stderr=subprocess.STDOUT,
                                  env=dict(os.environ, **{LANE_HELD: str(lane)})).returncode
        print(f"lane {lane}: {name} {'passed' if code == 0 else 'FAILED'}", flush=True)
        return code == 0

    return run_in_lanes(names, lanes, runner)


def prune_runs(runs: pathlib.Path, kept: int):
    """Removes all but the newest `kept` batch folders (named by time, so they sort by it)."""
    batches = sorted(path for path in runs.iterdir() if path.is_dir()) if runs.is_dir() else []
    for old in batches[:-kept] if kept > 0 else batches:
        shutil.rmtree(old, ignore_errors=True)


def user_dir(role: str) -> pathlib.Path:
    return ENGINE.user_dir(role)


def save_baseline(role: str, engine: engines.Emulator | None = None):
    """Keeps the role's save and party files (rig/<role>/baseline) for every later run."""
    engine = engine or ENGINE
    baseline = engine.role_dir(role) / "baseline"
    if baseline.exists():
        shutil.rmtree(baseline)
    shutil.copytree(engine.saves(role), baseline / "savedata")
    for party_file in engine.user_dir(role).glob("bloodborne_party_*.txt"):
        shutil.copy2(party_file, baseline / party_file.name)


def restore_baseline(role: str, saves_from: pathlib.Path | None = None,
                     keep_party_files: bool = False) -> bool:
    """The role's baseline saves and party files put back; with `saves_from` (a scenario's
    "saves", under rig/saves), that savedata instead and no party files: a new character. With
    `keep_party_files` (a suite's restart with other saves) the party files stay as they are: a
    player making a new character in the same install keeps the old one's."""
    baseline = ENGINE.role_dir(role) / "baseline"
    source = saves_from if saves_from is not None else baseline / "savedata"
    if not source.is_dir():
        return False
    saves = ENGINE.saves(role)
    shutil.rmtree(saves, ignore_errors=True)
    shutil.copytree(source, saves)
    if keep_party_files:
        return True
    for party_file in user_dir(role).glob("bloodborne_party_*.txt"):
        party_file.unlink()
    for party_file in baseline.glob("bloodborne_party_*.txt") if saves_from is None else []:
        shutil.copy2(party_file, user_dir(role) / party_file.name)
    return True


def setup_port(emulator: engines.Emulator, port: engines.Port) -> list[str]:
    """The port's rig folders, from the emulator rig's: the game with its update in one folder
    (the port reads one), and each role's saves and party files (its baseline, else its current
    ones) as the port's own baseline. A role the port has a folder for already stays as it is.
    The roles made."""
    game = emulator.game.parent
    engines.merge_game(game, game.with_name(f"{game.name}-patch"), port.game_dir)
    made = []
    for role in ROLES:
        if not emulator.user_dir(role).is_dir() or port.user_dir(role).exists():
            continue
        baseline = emulator.role_dir(role) / "baseline"
        saves = baseline / "savedata" if baseline.is_dir() else emulator.saves(role)
        party_files = baseline if baseline.is_dir() else emulator.user_dir(role)
        port.saves(role).mkdir(parents=True)
        if saves.is_dir():
            engines.copy_saves(saves, port.saves(role))
        for party_file in party_files.glob("bloodborne_party_*.txt"):
            shutil.copy2(party_file, port.user_dir(role) / party_file.name)
        port.prepare(role)
        save_baseline(role, port)
        made.append(role)
    return made


def add_guest2():
    """rig/guest2: the guest's folder (its save, settings, mods) with its own account
    rig_guest2, registered on the rig's shadNet."""
    source = user_dir("guest")
    target = user_dir("guest2")
    if target.exists():
        print(f"{target} exists already")
    else:
        shutil.copytree(source, target, ignore=shutil.ignore_patterns("log", "captures"))
        (target / "log").mkdir()
        users_file = target / "users.json"  # the emulator's account (the port's: engines.py)
        if users_file.exists():
            users = users_file.read_text(encoding="utf-8")
            users = users.replace('"shadnet_npid": "rig_guest"', '"shadnet_npid": "rig_guest2"')
            users = users.replace('"user_name": "rig_guest"', '"user_name": "rig_guest2"')
            users_file.write_text(users, encoding="utf-8", newline="\n")
        print(f"made {target}")
    baseline = ENGINE.role_dir("guest") / "baseline"
    if baseline.is_dir() and not (ENGINE.role_dir("guest2") / "baseline").exists():
        shutil.copytree(baseline, ENGINE.role_dir("guest2") / "baseline")  # starts where guest does
        print("guest2: baseline copied from the guest's")
    start_servers()
    sample = RIG / "server" / "clientsample"
    result = subprocess.run([str(sample / "shadnet-sample.exe"), "127.0.0.1", str(SHADNET_PORT),
                             "register", "rig_guest2", "rig", "rig_guest2.rig@p2p.db"],
                            cwd=sample, capture_output=True, text=True, timeout=30)
    print(f"register rig_guest2: {(result.stdout + result.stderr).strip()[:200]}")
    stop_servers()


def server_is_current(log_dir: pathlib.Path | None = None) -> bool:
    """Whether the rig's shadNet is a current one: it keeps the parties' worlds (phase 3: its log
    says so at its start, "party worlds: ..."), or, for a run of an older game build, it has the
    newest HTTP boards (/bloodborne/party/shown)."""
    if log_dir is not None:
        log = log_dir / "shadnet.log"
        deadline = time.time() + 10
        while time.time() < deadline:
            if log.is_file() and "party worlds:" in log.read_text(encoding="utf-8",
                                                                  errors="replace"):
                return True
            time.sleep(0.5)
    request = urllib.request.Request(f"http://127.0.0.1:{WEBAPI_PORT}/bloodborne/party/shown",
                                     data=b"probe 0",
                                     headers={"X-ShadPS4-Bloodborne-Party": "probe:probe"})
    try:
        with urllib.request.urlopen(request, timeout=3) as response:
            return response.status == 200
    except (urllib.error.URLError, OSError):
        return False


def log_path(role: str) -> pathlib.Path:
    return ENGINE.log_path(role)


def console_path(role: str) -> pathlib.Path:
    """What the port prints (its loader and runtime) beside the log; the emulator prints nothing
    the log has not."""
    return ENGINE.user_dir(role) / "log" / "console.txt"


def read_log(role: str) -> str:
    try:
        return log_path(role).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def forget_portraits(role: str) -> bool:
    """A role's kept party portraits (user/portraits) gone, for a scenario about taking them."""
    folder = user_dir(role) / "portraits"
    if not folder.is_dir():
        return False
    shutil.rmtree(folder)
    return True


def team_prelude(role: str, roles: list[str], stage: int) -> list[str]:
    """The title's team verbs that put a role's game into the scenario's team (the port plays only
    in teams, phase 6): the host makes it, invites the guests and starts it once they are in; each
    guest accepts and is ready. A game started again (a later stage) does the same: Kaniel
    2026-10-17, a team lives only while one of its games is there (the verbs pass at once when it
    still does: team-create and team-accept are done in a team); a game alone plays alone."""
    if len(roles) == 1:
        return ["team-alone"]
    if role != "host":
        return ["team-accept", "team-ready"]
    invites = [f"team-invite {engines.ACCOUNTS[r]}" for r in roles if r != "host"]
    return ["team-create", *invites, f"team-wait {len(roles)}", "team-start"]


def with_team(script: list[str] | None, role: str, roles: list[str], stage: int,
              teams: bool) -> list[str] | None:
    """`script` after the role's team prelude when the build plays in teams and the script does not
    make its team itself."""
    if script is None or not teams or any(line.startswith("team-") for line in script):
        return script
    return [*team_prelude(role, roles, stage), *script]


def stages_of(script: list) -> list[list[str]]:
    """A role's scripts in turn: one script, or a list of them (the game is stopped once a stage
    is done, as a crash or a quit would, and started again with the next)."""
    return script if script and isinstance(script[0], list) else [script]


# Settings a scenario switches on for its games ("settings"), and what they are otherwise (the save
# backups off); never taken from the PC's own. Party death is always on in the port (Kaniel
# 2026-10-13); the emulator's is its own default unless a scenario switches it on.
SCENARIO_SETTINGS = {
    "SHADPS4_BLOODBORNE_SAVE_BACKUPS": "off",
}


def game_environment(role: str, script: pathlib.Path | None, party: str,
                     settings: dict[str, str] | None = None) -> dict[str, str]:
    """A role's game's settings: co-op on in `party`, the trace on, the autoplay script if any,
    the scenario's own settings, and what the build needs besides."""
    env = dict(os.environ)
    env.update({
        "SHADPS4_BLOODBORNE_SEAMLESS_COOP": "1",
        "SHADPS4_P2P_PORT": str(p2p_port(role)),
        "SHADPS4_BLOODBORNE_PARTY": party,
        "SHADPS4_BLOODBORNE_PARTY_ROLE": ROLES[role]["role"],
        "SHADPS4_BLOODBORNE_RE_TRACE": "1",
    })
    env.pop("SHADPS4_BLOODBORNE_AUTOPLAY", None)
    if script is not None:
        env["SHADPS4_BLOODBORNE_AUTOPLAY"] = str(script)
    env.pop("SHADPS4_BLOODBORNE_PARTY_DEATH", None)
    env.update(SCENARIO_SETTINGS)
    env.update(ENGINE.environment(role))
    env.update(settings or {})  # over the engine's own (a suite plays at the kit's frame rate)
    env.pop("SHADPS4_P2P_DELAY_MS", None)
    if ENGINE.name == "port":
        # keys / title-keys: the overlay's menu keys (Insert and the menu's buttons).
        env["BB_MENU_KEYS_FILE"] = str(menu_keys_path(role))
        env["SHADPS4_NET_FIREWALL"] = firewall_setting()
    if PING:
        env["SHADPS4_P2P_DELAY_MS"] = latency.game_setting(PING)
        if "BB_SHADNET_SERVER" in env:
            # The link (UDP) goes straight to shadNet, which listens at the proxy's port + 1.
            env["BB_SHADNET_SERVER"] = f"127.0.0.1:{matching_udp_port()}"
            env["BB_SHADNET_WEBAPI"] = f"http://127.0.0.1:{server_port(WEBAPI_PORT)}"
    # In the background: the games' windows (SDL, both engines) open and come up without taking
    # the focus from whatever the person at the PC is doing.
    env["SDL_WINDOW_ACTIVATE_WHEN_SHOWN"] = "0"
    env["SDL_WINDOW_ACTIVATE_WHEN_RAISED"] = "0"
    return env


def firewall_setting() -> str:
    """Phase 9's proof test: the only addresses a port game reaches (SHADPS4_NET_FIREWALL, a
    firewall in each game: Windows does not filter loopback), the lane's servers. A datagram or
    connection to anything else, another game above all, is blocked and logged, and the run
    fails on it (check_party_log)."""
    ports = (server_port(SHADNET_PORT), matching_udp_port(), server_port(WEBAPI_PORT),
             server_port(BACKEND_PORT))
    return ",".join(f"127.0.0.1:{port}" for port in ports)


def menu_keys_path(role: str) -> pathlib.Path:
    return ENGINE.role_dir(role) / "menu-keys.txt"


def background_window_options() -> dict:
    """Popen options for a game started in the background: no console window of its own, and
    its first window shown without being activated."""
    if os.name != "nt":
        return {}
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 4  # SW_SHOWNOACTIVATE
    return {"startupinfo": startup, "creationflags": subprocess.CREATE_NO_WINDOW}


def set_aside(path: pathlib.Path):
    """The last game's file as <name>.prev<suffix>, for the next game's to start afresh."""
    for _ in range(20):  # a game that just stopped may still hold it for a moment
        try:
            if path.exists():
                path.replace(path.with_name(f"{path.stem}.prev{path.suffix}"))
            return
        except PermissionError:
            time.sleep(0.5)


def launch(role: str, script: list[str] | None, party: str = "rig:hunters",
           settings: dict[str, str] | None = None) -> subprocess.Popen:
    folder = ENGINE.role_dir(role)
    script_path = None
    if script is not None:
        script_path = folder / "autoplay.txt"
        script_path.write_text("\n".join(script) + "\n", encoding="utf-8", newline="\n")
    set_aside(log_path(role))
    menu_keys_path(role).unlink(missing_ok=True)  # a key the last game never took
    ENGINE.prepare(role, backend_port=server_port(BACKEND_PORT))
    build_lock.refuse_if_memory_low(f"the {role} game")  # leaked memory froze the PC
    build_lock.wait_for_builds()  # beside builds unless BB_BUILD_BESIDE_GAMES=0
    env = game_environment(role, script_path, party, settings)
    if ENGINE.name == "emulator":
        game = subprocess.Popen(ENGINE.command(role), cwd=folder, env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                **background_window_options())
        LAUNCHED.append(game)
        return game
    console = console_path(role)
    set_aside(console)
    console.parent.mkdir(parents=True, exist_ok=True)
    with open(console, "wb") as output:  # the game keeps its own handle
        game = subprocess.Popen(ENGINE.command(role), cwd=folder, env=env,
                                stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                                **background_window_options())
    LAUNCHED.append(game)
    return game


def stop_game(game: subprocess.Popen):
    """One game, at once (as a crash would), and wait until it is gone: whatever it started
    goes with it (the port's launcher starts the game itself)."""
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(game.pid)], capture_output=True)
    game.kill()
    try:
        game.wait(timeout=20)
    except subprocess.TimeoutExpired:
        pass


def new_crashes(role: str, since: float) -> list[str]:
    """The first lines of the crash reports a role's game wrote since `since`."""
    folder = user_dir(role) / "log"
    reports = []
    for report in sorted(folder.glob("crash-*.txt")):
        if report.stat().st_mtime >= since:
            reports.append(report.read_text(errors="replace").splitlines()[0])
    return reports


def keep_logs(run_dir: pathlib.Path, roles: list[str], suffix: str = ""):
    """Each role's game log and script, next to the servers' logs."""
    run_dir.mkdir(parents=True, exist_ok=True)
    for role in roles:
        script = ENGINE.role_dir(role) / "autoplay.txt"
        for source, kept in ((log_path(role), f"{role}{suffix}.log"),
                             (console_path(role), f"{role}{suffix}-console.txt"),
                             (script, f"{role}{suffix}-script.txt")):
            try:
                shutil.copy2(source, run_dir / kept)
            except OSError:
                pass


def keep_shots(run_dir: pathlib.Path, roles: list[str]):
    """Each role's pictures (its script's `shot <name>` steps) moved next to its log as
    <role>-<name>.png, and a request no frame took dropped: the next run starts with neither."""
    for role in roles:
        ENGINE.shot_request(role).unlink(missing_ok=True)
        shots = ENGINE.shots(role)
        for shot in sorted(shots.glob("*.png")) if shots.is_dir() else []:
            run_dir.mkdir(parents=True, exist_ok=True)
            shutil.move(str(shot), str(run_dir / f"{role}-{shot.name}"))


# --boot-check --cache-twice: a pipeline cache entry copied under another name. Lisara's v0.3.11
# cache held one pipeline in two files, and the second stopped the port at every start
# (LoadGraphicsPipeline: ASSERT(is_new)); the port now skips it and removes its file.
CACHE_TWICE_NAME = "g_0x0000000000000001.key"
CACHE_TWICE_SKIPPED = "skipped: it is in the cache twice"


def pipeline_cache(role: str) -> pathlib.Path:
    """The port's shader and pipeline cache for the game (BB_GPU_USER_DIR's cache/<serial>)."""
    return user_dir(role) / "cache" / "CUSA03173"


def seed_cache_twice(cache: pathlib.Path, source: pathlib.Path | None = None
                     ) -> pathlib.Path | None:
    """The first graphics pipeline entry of `cache` twice: a copy of it under CACHE_TWICE_NAME.
    `source` (a cache a game with a GPU wrote: a headless game draws nothing, so its own cache
    has no pipelines) replaces `cache` first. The entry copied, or None when there is none."""
    if source is not None:
        shutil.rmtree(cache, ignore_errors=True)
        shutil.copytree(source, cache)
    copy = cache / CACHE_TWICE_NAME
    copy.unlink(missing_ok=True)
    entries = sorted(cache.glob("g_*.key"))
    if not entries:
        return None
    copy.write_bytes(entries[0].read_bytes())
    return entries[0]


def cache_twice_problems(text: str, original: pathlib.Path, copy: pathlib.Path) -> list[str]:
    """What is wrong after a boot with one pipeline in the cache twice: the game must go on, say
    it skipped the second, and keep one of the two files (whichever it read first)."""
    problems = []
    if failure := boot_failure(text):
        problems.append(f"the game stopped at an assertion: {failure}")
    elif "Assertion Failed!" in text:
        problems.append("the game stopped at an assertion")
    if CACHE_TWICE_SKIPPED not in text:
        problems.append(f"no entry was skipped as \"{CACHE_TWICE_SKIPPED}\"")
    left = [path for path in (original, copy) if path.exists()]
    if len(left) == 2:
        problems.append(f"both files are left ({original.name}, {copy.name})")
    elif not left:
        problems.append(f"neither file is left ({original.name}, {copy.name})")
    return problems


def boot_check(role: str, seconds: int, cache_twice: str | None = None,
               owner: str | None = None) -> int:
    """The boot smoke test (tools/smoke-boot.ps1): the role's game alone, in a party of its own
    so it never meets a real one, for `seconds`; every co-op patch and hook must be in by then
    (check_party_log.py --boot). Needs no player and no server. In a lane (--lane N) it takes the
    lane and runs beside other lanes' games. `cache_twice` ("" or a cache folder to start from):
    one pipeline is in the cache twice (seed_cache_twice), and the game must skip it."""
    if missing := ENGINE.missing():
        print(missing)
        return 2
    if player_blocks() if IN_LANES else games_running():
        print("A game is running; not starting another.")
        return 2
    if IN_LANES and not lanes.claim_when_free(RIG, LANE, owner or lanes.default_owner()):
        print(f"Lane {LANE} is held or blocked (--lanes-status): not starting the game.")
        return 2
    try:
        return boot_and_judge(role, seconds, cache_twice)
    finally:
        if IN_LANES:
            lanes.release(RIG, LANE)


def boot_and_judge(role: str, seconds: int, cache_twice: str | None) -> int:
    original = copy = None
    if cache_twice is not None:
        if ENGINE.name != "port":
            print("--cache-twice is the port's pipeline cache (--engine port)")
            return 2
        cache = pipeline_cache(role)
        original = seed_cache_twice(cache, pathlib.Path(cache_twice) if cache_twice else None)
        if original is None:
            print(f"no pipeline in {cache} to copy: give --cache-twice a cache a game with a GPU "
                  "wrote (a headless game draws none)")
            return 2
        copy = cache / CACHE_TWICE_NAME
        print(f"{original.name} copied to {copy.name} in {cache}")
    started = time.time()
    game = launch(role, None, party="smoke:boot")
    while time.time() < started + seconds and game.poll() is None:
        time.sleep(1)
    exited = game.poll()
    stop_game(game)
    stop_games()
    text = read_log(role)
    print(f"{log_path(role)} (build {check_party_log.revision(text)})")
    if exited is not None:
        print(f"  the game ended by itself after {time.time() - started:.0f} s (code {exited}): "
              f"{boot_failure(text) or 'see ' + str(console_path(role))}")
    failures = check_party_log.check_boot(text)
    if original is not None:
        failures += [f"cache twice: {problem}"
                     for problem in cache_twice_problems(text, original, copy)]
    for failure in failures:
        print(f"  FAIL {failure}")
    for warning in check_party_log.trace_warnings(text):
        print(f"  warning (trace only) {warning}")
    print("  boot: " + ("FAILED" if failures else
                        f"ok, {len(check_party_log.BOOT_MARKERS)} parts in"))
    return 1 if failures else 0


def free_commit_gb() -> float:
    """What Windows can still commit (its commit limit minus what is committed), in GB."""
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "$c = (Get-Counter '\\Memory\\Committed Bytes','\\Memory\\Commit Limit')"
         ".CounterSamples; $c[1].CookedValue - $c[0].CookedValue"],
        capture_output=True, text=True).stdout.strip()
    try:
        return float(out) / 2**30
    except ValueError:
        return float("inf")  # not known: let the games try


# A lane's run waits this long for another lane's games to give back their memory (rig
# 2026-10-15: a party of three beside a party of two found 18-19 GB of the 22.5 it needs).
MEMORY_WAIT_S = 1800


def wait_for_memory(need: float, wait: float = MEMORY_WAIT_S, sleep=time.sleep,
                    free=None) -> float:
    """What can be committed, once it is `need` GB or the wait is over."""
    free = free or free_commit_gb
    deadline = time.time() + wait
    now = free()
    if now < need:
        print(f"  waiting for memory: {now:.1f} GB can be committed, {need} GB needed")
    while now < need and time.time() < deadline:
        sleep(15)
        now = free()
    return now


def crashed_in_known_bug(scenario: dict, text: str) -> dict | None:
    """The chapter a crashed game was in, when that chapter is a known bug's (a crash it
    reproduces: judged as KNOWN instead of run again); a single scenario with known_bug counts."""
    if not scenario.get("chapters"):
        return scenario if scenario.get("known_bug") else None
    sections = list(chapter_sections(text))
    if not sections:
        return None
    return next((chapter for chapter in scenario["chapters"]
                 if chapter["name"] == sections[-1] and chapter.get("known_bug")), None)


def boot_failure(text: str) -> str | None:
    """Why a game stopped before it got going: the emulator's failed assertion and its reason."""
    found = re.search(r"Assertion Failed!\s*\n(.*)", text)
    return found.group(1).strip() or "assertion failed" if found else None


def until_done(text: str) -> str:
    """A log up to its script's end: what follows is the rig stopping the games (the others see
    the first one go and leave its room)."""
    found = DONE.search(text)
    return text[:found.end()] if found else text


def role_text(run_dir: pathlib.Path, role: str, stages: int) -> str:
    """Everything a role's games logged while their scripts ran, stage after stage."""
    texts = []
    for stage in range(1, stages):
        try:
            texts.append((run_dir / f"{role}-stage{stage}.log").read_text(encoding="utf-8",
                                                                           errors="replace"))
        except OSError:
            pass
    return "\n".join(until_done(text) for text in texts + [read_log(role)])


def failed_fatal_chapter(text: str, status: str, scenario: dict) -> bool:
    """A suite game's script ended (failed) in a chapter the rest needs (the join): the suite
    stops, and the game is not started again for its next stage."""
    ended_in = list(chapter_sections(text))
    return status == "failed" and bool(ended_in) and ended_in[-1] in scenario.get("fatal", [])


def run(name: str, batch: pathlib.Path, attempt: int = 1, scenario: dict | None = None) -> bool:
    scenario = scenario or RUNNABLE[name]
    roles = list(scenario["roles"])
    run_dir = batch / (name if attempt == 1 else f"{name}-{attempt}")
    print(f"== {name}: {scenario.get('about', '')}")
    if missing := ENGINE.missing():
        print(f"  {missing}")
        return False
    for role in roles:
        if not user_dir(role).is_dir():
            print(f"  {role}: no rig folder at {ENGINE.role_dir(role)}")
            return False
    if player_blocks():
        print("  another game is running (someone is playing): not starting the rig's")
        return False
    saves_from = RIG / "saves" / scenario["saves"] / "savedata" if "saves" in scenario else None
    for role in roles:
        if not scenario.get("no_baseline") and restore_baseline(role, saves_from):
            print(f"  {role}: saves put back to "
                  f"{saves_from.parent.name if saves_from else 'the baseline'}")
        if scenario.get("no_portraits") and forget_portraits(role):
            print(f"  {role}: kept portraits removed")
    keep_shots(run_dir / "earlier", roles)  # an aborted run's, apart from this one's
    need = ENGINE.commit_gb * len(roles)
    free = wait_for_memory(need) if IN_LANES else free_commit_gb()
    if free < need:
        print(f"  not enough memory for {len(roles)} games: {free:.1f} GB can be committed, "
              f"{need} GB needed (close something big, or give Windows a bigger page file)")
        return False
    stop_servers()
    # Every run starts with no party worlds on the server (shadNet keeps them on disk: phase 3).
    shutil.rmtree(lane_dir() / "server" / "bloodborne_worlds", ignore_errors=True)
    start_servers(run_dir)
    if not server_is_current(run_dir):
        print("  The rig's shadNet is too old (it keeps no party worlds): copy "
              rf"{BB_ROOT}\out\server\shadnet.exe into {RIG}\server.")
        return False
    started = time.time()
    deadline = started + scenario.get("timeout", 600)
    stages = {role: [with_team(script, role, roles, index, ENGINE.teams)
                     for index, script in enumerate(stages_of(scenario["roles"][role]))]
              for role in roles}
    stage = {role: 0 for role in roles}
    syncs = scenario.get("syncs", {})
    reached: dict[str, set[str]] = {role: set() for role in roles}
    given_up: set[str] = set()
    released: set[str] = set()
    processes = {}
    for role in roles:
        clear_go_files(ENGINE.role_dir(role))
        processes[role] = launch(role, stages[role][0], settings=scenario.get("settings"))
        print(f"  started {role}")
        # The next game once this one is in the world, where loading side by side is slow.
        wait_until = time.time() + 90 if ENGINE.staggered_start else 0
        while (time.time() < wait_until and IN_WORLD not in read_log(role)
               and processes[role].poll() is None):
            time.sleep(2)
    done = {}
    crashed = {}
    fatal_failure = False
    while (time.time() < deadline and len(done) + len(crashed) < len(roles)
           and not fatal_failure):
        for role in roles:
            if role in done or role in crashed:
                continue
            text = read_log(role)
            reached[role] |= syncs_reached(text)
            given_up |= set(CHAPTER_GIVEN_UP.findall(text))
            found = DONE.search(text)
            # A suite's game goes on to its next stage even after a failed chapter (keep-going):
            # its later chapters are judged on their own.
            fatal = found is not None and failed_fatal_chapter(text, found.group(1), scenario)
            if (found and (found.group(1) == "ok" or syncs) and not fatal
                    and stage[role] + 1 < len(stages[role])):
                stage[role] += 1
                print(f"  {role} stage {stage[role]} done after {time.time() - started:.0f} s; "
                      "stopping its game and starting it again")
                stop_game(processes[role])
                keep_logs(run_dir, [role], f"-stage{stage[role]}")
                # A suite's restart with other saves (a new character): put in before it starts.
                if saves := scenario.get("stage_saves", {}).get(role, {}).get(stage[role]):
                    # Rig 2026-10-13: dropping the party files left the new-character check
                    # nothing to tell apart (no kept lists, no backups).
                    restore_baseline(role, RIG / "saves" / saves / "savedata",
                                     keep_party_files=True)
                    print(f"  {role}: saves put back to {saves}")
                processes[role] = launch(role, stages[role][stage[role]],
                                         settings=scenario.get("settings"))
            elif found:
                done[role] = found.group(1)
                print(f"  {role} done ({found.group(1)}) after {time.time() - started:.0f} s")
                if fatal:
                    print(f"  {role} failed {list(chapter_sections(text))[-1]}, which the rest "
                          "needs: stopping")
                    fatal_failure = True
            elif reports := new_crashes(role, started):
                crashed[role] = reports[0]
                print(f"  {role} crashed after {time.time() - started:.0f} s: {reports[0]}")
            elif processes[role].poll() is not None:
                # Gone without a crash report: it never got going (the memory ran out, say).
                crashed[role] = boot_failure(read_log(role)) or "the game exited"
                print(f"  {role} stopped after {time.time() - started:.0f} s: {crashed[role]}")
        if crashed or fatal_failure:
            break  # the others wait for it in vain
        for sync in syncs_to_release(syncs, reached, given_up, set(done), released):
            released.add(sync)
            if sync in scenario.get("server_restarts", []):
                # shadNet stops and starts again while the games play on (its world store keeps
                # the party's world: phase 3); its new log goes beside the first one.
                stop_servers()
                start_servers(run_dir / f"restart-{sync}")
                print(f"  servers restarted at {sync}")
            if seconds := scenario.get("outages", {}).get(sync):
                # Before the games go on: the outage starts as they do.
                for proxy in PROXIES:
                    proxy.outage(seconds)
                print(f"  servers unreachable for {seconds} s" if PROXIES else
                      "  no outage without a ping (--ping): the servers stay reachable")
            for role in syncs[sync]["roles"]:
                write_go_file(ENGINE.role_dir(role), sync)
            print(f"  sync {sync} after {time.time() - started:.0f} s")
        time.sleep(1 if syncs else 2)
    stop_games()
    stop_lane_strays(roles)
    keep_logs(run_dir, roles)
    keep_shots(run_dir, roles)
    known_crash = [crashed_in_known_bug(scenario, role_text(run_dir, role, stage[role] + 1))
                   for role in crashed]
    if crashed and all(known_crash):
        print(f"  KNOWN crash in {', '.join(spec.get('name', name) for spec in known_crash)}: "
              f"{known_crash[0]['known_bug']}")
    elif crashed:
        # A crash is the emulator's (the GPU's device lost, so far), not the scenario's: once more.
        if attempt == 1:
            print("  a game crashed; running the scenario once more")
            time.sleep(30)  # what a dead game held goes back to Windows
            return run(name, batch, attempt + 1, scenario)
        print("  FAILED: a game crashed again")
        return False

    texts = {role: role_text(run_dir, role, stage[role] + 1) for role in roles}
    consoles = {role: console_text(run_dir, role, stage[role] + 1) for role in roles}
    passed = True
    for role in roles:
        status = done.get(role, "crashed (known)" if role in crashed else "timed out")
        print(f"  {role}: {status} (build {check_party_log.revision(texts[role])})")
        # A suite's chapters carry their own verdicts; a game that never finished fails it.
        # A known crash ends the run there: no game finished its script.
        if status != "ok" and not (scenario.get("chapters") and status == "failed") and not (
                crashed and all(known_crash)):
            passed = False
    if joins := join_times(texts):
        print("  join: " + ", ".join(f"{role} {seconds:.0f} s after its load"
                                      for role, seconds in joins.items()))
    for problem in suite_problems(scenario, texts) + console_problems(scenario, consoles):
        print(f"    {problem}")
        passed = False
    if chapters := scenario.get("chapters"):
        sections = {role: chapter_sections(text) for role, text in texts.items()}
        for chapter in chapters:
            # Every game that has a part in the chapter, judged on that part of its log.
            part = {role: sections[role].get(chapter["name"]) for role in chapter["roles"]}
            if missing := [role for role, text in part.items() if text is None]:
                problems = [f"{role}: never got to it" for role in missing]
            else:
                problems = (judge(chapter, part) + file_problems(chapter, started) +
                            console_problems(chapter, consoles) +
                            [f"{role}: crashed: {crashed[role]}" for role, spec
                             in zip(crashed, known_crash) if spec is chapter])
            seconds = chapter_seconds(part)
            verdict, ok = chapter_verdict(chapter, problems)
            print(f"  {verdict} {chapter['name']}"
                  f"{f' ({seconds:.0f} s)' if seconds is not None else ''}"
                  f"{': ' + chapter['about'] if problems and chapter.get('about') else ''}")
            for problem in problems:
                print(f"    {problem}")
            if ok and not problems and chapter.get("known_bug"):
                print(f"    known bug no longer seen ({chapter['known_bug']}): drop known_bug")
            passed = passed and ok
    else:
        problems = judge(scenario, texts)
        for problem in problems:
            print(f"    {problem}")
        verdict, ok = chapter_verdict(scenario, problems)
        if problems and verdict == "KNOWN":
            print(f"  KNOWN {name}: {scenario['known_bug']}")
        passed = passed and ok
    print(f"  {'PASSED' if passed else 'FAILED'} in {time.time() - started:.0f} s (logs in "
          f"{run_dir})")
    return passed


# Lines no game of a suite may log (the robustness review's guards, 2026-10-12): a game call
# refused as unsafe (#30), players on different builds (#29); check_party_log.py flags a
# required co-op feature skipped (#32).
NEVER_IN_A_SUITE = [re.compile(pattern) for pattern in (
    r"Bloodborne refused a game call",
    r"runs build \S+, this game runs",
)]
# Lines per second of a game's log (its step lines' t=) above which the log itself costs frames
# (Kaniel 2026-10-11: a lamp line written 430,000 times took the game to 3 FPS).
LOG_LINES_PER_SECOND = 200  # the rig's games log 15-30 a second
STEP_TIME = re.compile(r"Bloodborne autoplay: step \d+/\d+ (\S+).* ok \(t=([\d.]+)\)")


def file_problems(spec: dict, since: float) -> list[str]:
    """A chapter's checks on the files a game wrote in its user folder during the run:
    "files_present" / "files_absent", {role: [glob]} (a backup of the kept pickups for a new
    character, none for a restart)."""
    problems = []
    for key, wanted in (("files_present", True), ("files_absent", False)):
        for role, patterns in spec.get(key, {}).items():
            for pattern in patterns:
                written = [path for path in user_dir(role).glob(pattern)
                           if path.stat().st_mtime >= since]
                if bool(written) != wanted:
                    problems.append(f"{role}: {'no' if wanted else 'a'} {pattern} written"
                                    f"{': ' + written[0].name if written else ''}")
    return problems


def suite_problems(scenario: dict, texts: dict[str, str]) -> list[str]:
    """A suite's checks on every game's whole log: the join within its time (slow joins were the
    first sign of the sign-timer patch skipped), no co-op patch skipped, and a log that does not
    flood."""
    if not scenario.get("chapters"):
        return []
    problems = []
    for role, text in texts.items():
        steps = [(verb, float(at)) for verb, at in STEP_TIME.findall(text)]
        if limit := scenario.get("join_within"):
            joined = next((at for verb, at in steps if verb == "wait-session"), None)
            if steps and joined is not None and joined - steps[0][1] > limit:
                problems.append(f"{role}: joined {joined - steps[0][1]:.0f} s after its first "
                                f"step (at most {limit} s)")
        problems += [f"{role}: co-op patch skipped: {line.strip()[:160]}"
                     for line in text.splitlines() if "signature mismatch" in line]
        problems += [f"{role}: {line.strip()[:200]}" for line in text.splitlines()
                     if any(pattern.search(line) for pattern in NEVER_IN_A_SUITE)]
        if len(steps) >= 2 and steps[-1][1] > steps[0][1]:
            rate = text.count("\n") / (steps[-1][1] - steps[0][1])
            if rate > LOG_LINES_PER_SECOND:
                problems.append(f"{role}: log writes {rate:.0f} lines/s "
                                f"(at most {LOG_LINES_PER_SECOND})")
    return problems


def chapter_verdict(spec: dict, problems: list[str]) -> tuple[str, bool]:
    """A chapter's (or scenario's) verdict and whether it passes: one with a known bug the phase
    that fixes it has not landed yet ("known_bug": what is wrong) fails as KNOWN without failing
    the run, so a tier stays green until its fix; a fixed one says so."""
    if not problems:
        return "ok  ", True
    # "known_problems": the bug is known only by these problems (patterns); any other fails.
    known = spec.get("known_problems")
    if spec.get("known_bug") and (known is None or all(
            any(re.search(pattern, problem) for pattern in known) for problem in problems)):
        return "KNOWN", True
    return "FAIL", False


def chapter_seconds(texts: dict[str, str | None]) -> float | None:
    """How long a chapter took: from its first step to its last in any game (the games' clocks
    are the one PC's)."""
    times = [float(at) for text in texts.values() if text
             for at in re.findall(r"\(t=([\d.]+)\)", text)]
    return max(times) - min(times) if len(times) >= 2 else None


def console_text(run_dir: pathlib.Path, role: str, stages: int) -> str:
    """What a role's games printed (the port's console: its frame times, its overlay)."""
    texts = []
    for suffix in [f"-stage{stage}" for stage in range(1, stages)] + [""]:
        try:
            texts.append((run_dir / f"{role}{suffix}-console.txt").read_text(encoding="utf-8",
                                                                               errors="replace"))
        except OSError:
            pass
    return "\n".join(texts)


# A frame that takes this long is a freeze (Krolebord, 0.2.4: 24.6 s, then he dropped): the port's
# own "Perf:" line every 30 s has the worst frame. A run's normal worst frames are 0.2-0.35 s.
LONG_FRAME_MS = 2000
WORST_FRAME = re.compile(r"Perf: .*worst frame ([\d.]+) ms")
OVERLAY_TEXTURES = re.compile(r"Overlay: (\d+) textures, pool of (\d+) sets")


def console_problems(spec: dict, consoles: dict[str, str]) -> list[str]:
    """Checks on the games' consoles: no frame over LONG_FRAME_MS ("long_frame_ms" changes it,
    a suite or a single scenario; a chapter checks the overlay only), and with
    "overlay_textures" the overlay's textures seen and within its descriptor pool (0.2.4: icons
    and runes garbled past it)."""
    problems = []
    limit = spec.get("long_frame_ms", LONG_FRAME_MS)
    if "name" not in spec:  # a whole run (a chapter's spec has its name)
        for role, text in consoles.items():
            worst = max((float(ms) for ms in WORST_FRAME.findall(text)), default=0.0)
            if worst > limit:
                problems.append(f"{role}: a frame took {worst / 1000:.1f} s (at most "
                                f"{limit / 1000:g} s)")
    for role in spec.get("overlay_textures", []):
        counts = [(int(used), int(pool)) for used, pool in OVERLAY_TEXTURES.findall(
            consoles.get(role, ""))]
        if not counts:
            problems.append(f"{role}: the overlay never said how many textures it has")
        elif (most := max(counts))[0] > most[1]:
            problems.append(f"{role}: the overlay has {most[0]} textures, its pool {most[1]}")
    # What the port's runtime prints beside the log (its sound: the team's loading screen).
    for role, patterns in spec.get("console_expect", {}).items():
        for pattern in patterns:
            if not re.search(pattern, consoles.get(role, "")):
                problems.append(f"{role}: the console never said /{pattern}/")
    return problems


def join_times(texts: dict[str, str]) -> dict[str, float]:
    """Each game's join: from its first step in the world to its wait-session's end (a guest's
    is how long the team's session took once its own load was done)."""
    joins = {}
    for role, text in texts.items():
        steps = [(verb, float(at)) for verb, at in STEP_TIME.findall(text)]
        joined = next((at for verb, at in steps if verb == "wait-session"), None)
        if steps and joined is not None:
            joins[role] = joined - steps[0][1]
    return joins


def last_census_hp(text: str, handle: str, index: int = -1) -> tuple[int, int] | None:
    """The character's HP and max HP in the role's census number `index` (the autoplay verb;
    -1 the last), if any."""
    found = re.findall(rf"autoplay census: {re.escape(handle)} hp (-?\d+)/(\d+)", text)
    if not -len(found) <= index < len(found):
        return None
    return int(found[index][0]), int(found[index][1])


def same_hp_problem(texts: dict[str, str], handle: str, index: int = -1) -> str | None:
    """What is wrong with the shared enemy's HP across the roles' censuses number `index` (-1 the
    last), or None: every game must show the same HP (2 apart at most: shares round), hurt
    below its max."""
    seen = {role: last_census_hp(text, handle, index) for role, text in texts.items()}
    if missing := [role for role, hp in seen.items() if hp is None]:
        return f"no census of {handle} in {', '.join(missing)}"
    values = [hp for hp, _ in seen.values()]
    shown = ", ".join(f"{role} {hp}/{top}" for role, (hp, top) in seen.items())
    if max(values) - min(values) > 2:
        return f"{handle} HP apart: {shown}"
    if all(hp >= top for hp, top in seen.values()):
        return f"{handle} never hit: {shown}"
    return None


def hit_damage(text: str, handle: str) -> int:
    """What the role's own hits took off the character, as the game's hit function saw them (the
    `hit` step's drop may hold another member's damage the board wrote meanwhile)."""
    found = re.findall(rf"hit on {re.escape(handle)} by this player \S+: (-?\d+) -> (-?\d+)", text)
    return sum(int(before) - int(after) for before, after in found)


def hits_sum_problem(texts: dict[str, str], handle: str) -> str | None:
    """What is wrong with the shared enemy's HP in each role's last census, or None: every game
    must show its max HP less all the roles' hits together, each hit counted once (3 apart at
    most: shares round)."""
    hits = sum(hit_damage(text, handle) for text in texts.values())
    if hits <= 0:
        return f"no hit on {handle}"
    for role, text in texts.items():
        seen = last_census_hp(text, handle)
        if seen is None:
            return f"no census of {handle} in {role}"
        hp, top = seen
        if abs((top - hp) - hits) > 3:
            return f"{role} took {top - hp} of {handle}, the hits {hits}"
    return None


def hp_board_lag(texts: dict[str, str], handle: str) -> float | None:
    """The average time (s, on the rig's one clock) from a role's own hit on the character to
    the lowering of it by that hit in each other role (from the HP the hit started at, by its
    damage, 3 HP either way, within 5 s), or None if none was lowered. A game the hit reached
    through the game itself lowers nothing; another member's later hit of the same damage
    starts from a lower HP, so its lowering is not this hit's (rig hit3 2026-10-12)."""
    pattern = re.escape(handle)
    hits = {role: [(int(a), int(a) - int(b), float(t)) for a, b, t in re.findall(
        rf"hit on {pattern} by this player \S+: (-?\d+) -> (-?\d+) \(t=([\d.]+)\)", text)]
        for role, text in texts.items()}
    lowered = {role: [(int(a), int(a) - int(b), float(t)) for a, b, t in re.findall(
        # The HP board's lowering, or phase 4's follower taking the owner's HP from the log.
        rf"(?:damage|HP from the log: \d+ enemies) on map \S+: (?:\S+ \S+ )*?{pattern} "
        rf"(-?\d+)->(-?\d+)[^\n]*\(t=([\d.]+)\)",
        text)] for role, text in texts.items()}
    lags = []
    for role, own in hits.items():
        for start, damage, at in own:
            for other, seen in lowered.items():
                matching = [t for before, drop, t in seen
                            if drop > 0 and abs(drop - damage) <= 3 and abs(before - start) <= 3
                            and 0 <= t - at <= 5]
                if other != role and matching:
                    lags.append(min(matching) - at)
    return sum(lags) / len(lags) if lags else None


def hit_lowerings(texts: dict[str, str], handle: str):
    """Each role's own hits on the character and the lowerings of it it saw (start HP, drop,
    t), as hp_board_lag reads them."""
    pattern = re.escape(handle)
    hits = {role: [(int(a), int(a) - int(b), float(t)) for a, b, t in re.findall(
        rf"hit on {pattern} by this player \S+: (-?\d+) -> (-?\d+) \(t=([\d.]+)\)", text)]
        for role, text in texts.items()}
    lowered = {role: [(int(a), int(a) - int(b), float(t)) for a, b, t in re.findall(
        rf"(?:damage|HP from the log: \d+ enemies) on map \S+: (?:\S+ \S+ )*?{pattern} "
        rf"(-?\d+)->(-?\d+)[^\n]*\(t=([\d.]+)\)",
        text)] for role, text in texts.items()}
    return hits, lowered


def hp_lag_pairs(texts: dict[str, str], handle: str) -> dict[tuple[str, str], float]:
    """hp_board_lag for each direction (hitter, the other game), on its own (2026-10-09: hit3
    checked host to guest only; Kaniel 2026-10-14: lag between guests too)."""
    hits, lowered = hit_lowerings(texts, handle)
    lags: dict[tuple[str, str], list[float]] = {}
    for role, own in hits.items():
        for start, damage, at in own:
            for other, seen in lowered.items():
                matching = [t for before, drop, t in seen
                            if drop > 0 and abs(drop - damage) <= 3 and abs(before - start) <= 3
                            and 0 <= t - at <= 5]
                if other != role and matching:
                    lags.setdefault((role, other), []).append(min(matching) - at)
    return {pair: sum(found) / len(found) for pair, found in lags.items()}


def kill_lags(texts: dict[str, str], handle: str) -> dict[str, float | None]:
    """From the killing hit (a hit taking the character to 0) to its 0 in each other game (its
    lowering to 0, within 5 s), or None in a game it never reached; empty without a killing
    hit."""
    hits, lowered = hit_lowerings(texts, handle)
    kills = [(role, at) for role, own in hits.items() for start, damage, at in own
             if start - damage <= 0]
    if not kills:
        return {}
    killer, at = kills[0]
    lags: dict[str, float | None] = {}
    for other, seen in lowered.items():
        if other == killer:
            continue
        zero = [t for before, drop, t in seen if before - drop <= 0 and 0 <= t - at <= 5]
        # Phase 10's line for a copy's death (a follower's copy dies by the kill's log entry).
        zero += [float(t) for t in re.findall(
            rf"enemy death seen: {re.escape(handle)} \(t=([\d.]+)\)", texts[other])
            if 0 <= float(t) - at <= 5]
        lags[other] = min(zero) - at if zero else None
    return lags


def play_scripts(scenario: dict) -> dict[str, list[str]]:
    """--play: each of a scenario's roles only gets into the world and into one session with the
    others; the rest is the person's."""
    roles = list(scenario["roles"])
    return {role: [START, f"wait-session {len(roles)} 300"] for role in roles}


def play(name: str, title: bool = False) -> int:
    """--play <scenario>: its games with its settings, left to the person at the PC once they are
    in the world together (with --title: at the title screen, no autoplay at all). The servers
    stay up until every game is closed."""
    scenario = RUNNABLE[name]
    roles = list(scenario["roles"])
    run_dir = RUNS / time.strftime("%Y%m%d-%H%M%S") / f"play-{name}"
    if missing := ENGINE.missing():
        print(missing)
        return 2
    for role in roles:
        if not user_dir(role).is_dir():
            print(f"{role}: no rig folder at {ENGINE.role_dir(role)}")
            return 2
    if games()[1]:
        print("Another game is running (someone is playing): not starting the rig's.")
        return 2
    saves_from = RIG / "saves" / scenario["saves"] / "savedata" if "saves" in scenario else None
    for role in roles:
        if not scenario.get("no_baseline") and restore_baseline(role, saves_from):
            print(f"{role}: saves put back to "
                  f"{saves_from.parent.name if saves_from else 'the baseline'}")
    need = ENGINE.commit_gb * len(roles)
    if (free := free_commit_gb()) < need:
        print(f"Not enough memory for {len(roles)} games: {free:.1f} GB can be committed, "
              f"{need} GB needed.")
        return 2
    stop_servers()
    start_servers(run_dir)
    if not server_is_current(run_dir):
        print(rf"The rig's shadNet is too old (it keeps no party worlds): copy "
              rf"{BB_ROOT}\out\server\shadnet.exe into {RIG}\server.")
        stop_servers()
        return 2
    scripts = {role: with_team(script, role, roles, 0, ENGINE.teams)
               for role, script in play_scripts(scenario).items()}
    processes = {}
    for role in roles:
        processes[role] = launch(role, None if title else scripts[role],
                                 settings=scenario.get("settings"))
        print(f"started {role}")
        wait_until = time.time() + 90 if ENGINE.staggered_start else 0
        while (time.time() < wait_until and IN_WORLD not in read_log(role)
               and processes[role].poll() is None):
            time.sleep(2)
    started = time.time()
    joined = set()
    if title:
        print(f"Started {', '.join(roles)} to the title screen: the games are yours; close them "
              "when you are done.")
    while not title and time.time() < started + 360 and len(joined) < len(roles):
        for role in roles:
            if role not in joined and DONE.search(read_log(role)):
                joined.add(role)
        if any(game.poll() is not None for game in processes.values()):
            break
        time.sleep(2)
    if title:
        pass
    elif len(joined) == len(roles):
        print(f"Ready after {time.time() - started:.0f} s: {', '.join(roles)} are in the world "
              "together. The games are yours; close them when you are done.")
    else:
        print(f"Not together yet ({', '.join(sorted(joined)) or 'nobody'} joined); the games stay "
              "open as they are.")
    while any(game.poll() is None for game in processes.values()):
        time.sleep(5)
    keep_logs(run_dir, roles)
    stop_servers()
    print(f"Every game is closed; logs in {run_dir}")
    return 0


def ping_setting(text: str) -> tuple[int, int] | None:
    try:
        return latency.parse_ping(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("scenarios", nargs="*")
    parser.add_argument("--keep-servers", action="store_true",
                        help="leave the rig's servers running afterwards")
    parser.add_argument("--list", action="store_true",
                        help="list the scenarios and suites, with their times")
    parser.add_argument("--tier", choices=("quick", "full", "long"),
                        help="run a tier (budget.py): quick is smoke and the suites named after "
                             "it, full every suite, long the slow scenarios")
    parser.add_argument("--lanes-status", action="store_true",
                        help="who holds which lane (lanes.py)")
    parser.add_argument("--owner", help="who this run's lane claims name (default BB_RIG_OWNER, "
                                        "else this user and PC)")
    parser.add_argument("--chapter", action="append", metavar="NAME",
                        help="with one suite: run only this chapter (again for more), and the "
                             "setup chapters it needs")
    parser.add_argument("--save-baseline", action="store_true",
                        help="keep every role's saves as the start of later runs")
    parser.add_argument("--add-guest2", action="store_true",
                        help="make the third game's rig folder and account")
    parser.add_argument("--engine", choices=sorted(engines.ENGINES), default="emulator",
                        help="the build the games run (default: the emulator)")
    parser.add_argument("--setup", action="store_true",
                        help="make the port's rig folders from the emulator rig's (--engine port)")
    parser.add_argument("--boot-check", choices=sorted(ROLES), metavar="ROLE",
                        help="boot the role's game alone and check that co-op went in")
    parser.add_argument("--seconds", type=int, default=40,
                        help="how long --boot-check lets the game boot (default 40)")
    parser.add_argument("--cache-twice", nargs="?", const="", metavar="CACHE",
                        help="with --boot-check (port): one pipeline in the shader cache twice; "
                             "the game must skip it. CACHE: a cache a game with a GPU wrote, "
                             "copied over the role's first (a headless game draws none)")
    parser.add_argument("--play", metavar="SCENARIO",
                        help="start a scenario's games with its settings and leave them to you "
                             "once they are in the world together")
    parser.add_argument("--title", action="store_true",
                        help="with --play: leave the games at the title screen, no autoplay")
    parser.add_argument("--lanes", type=int, metavar="N",
                        help="run the scenarios and suites N at a time, each in its own lane "
                             "(own servers, ports and game folders: --setup-lanes first)")
    parser.add_argument("--setup-lanes", type=int, metavar="N",
                        help="make lanes 1..N-1 from the rig's folders (--engine port)")
    parser.add_argument("--lane", type=int, default=0, help=argparse.SUPPRESS)
    parser.add_argument("--batch", type=pathlib.Path, help=argparse.SUPPRESS)
    parser.add_argument("--headless", action="store_true",
                        help="the port's games draw nothing and play no sound (BB_NULL_GPU=1, "
                             "BB_AUDIO=none): nothing on the screen, no pictures")
    parser.add_argument("--ping", type=ping_setting, default=latency.DEFAULT_PING,
                        help="round trip in ms between the games and to the servers, \"80\" "
                             f"or \"50-100\" (default {latency.DEFAULT_PING}; 0 for none)")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    use_engine(args.engine)
    if args.headless:
        # The games take the rig's environment (game_environment).
        os.environ.update(HEADLESS)
    use_ping(args.ping)
    use_lane(args.lane, args.batch is not None)
    runs_games = (args.play or args.scenarios or args.boot_check) and not args.lanes
    proxies = start_delay_proxies() if runs_games else []
    PROXIES[:] = proxies
    try:
        return act(args)
    finally:
        for proxy in proxies:
            proxy.stop()


def act(args: argparse.Namespace) -> int:
    if args.setup:
        if ENGINE.name != "port":
            print("--setup makes the port's rig folders: --engine port --setup")
            return 2
        if games_running():
            print("A game is running; its game folder may be in use.")
            return 2
        made = setup_port(engines.ENGINES["emulator"], ENGINE)
        print(f"game for the port: {ENGINE.game_dir}")
        print(f"made {', '.join(made)}" if made else "every role had a port folder already")
        return 0
    if args.boot_check:
        return boot_check(args.boot_check, args.seconds, args.cache_twice, args.owner)
    if args.play:
        if args.play not in RUNNABLE:
            print(f"No such scenario: {args.play} (--list shows them)")
            return 2
        return play(args.play, title=args.title)
    if args.add_guest2:
        add_guest2()
        return 0
    if args.setup_lanes or args.lanes or args.lane:
        if ENGINE.name != "port":
            print("Lanes are the port's (--engine port): the emulator needs 12.5 GB a game.")
            return 2
        if max(args.setup_lanes or 0, args.lanes or 0, args.lane + 1) > 9:
            print("At most 9 lanes (the tenth's ports would meet lane 0's ping proxies).")
            return 2
    if args.setup_lanes:
        for lane in range(1, args.setup_lanes):
            made = setup_lane(lane)
            print(f"lane {lane}: made {', '.join(str(path) for path in made)}" if made else
                  f"lane {lane}: there already")
        return 0
    if args.save_baseline:
        if games_running():
            print("A game is running; its saves may be half written.")
            return 2
        for role in ROLES:
            if user_dir(role).is_dir():
                save_baseline(role)
                print(f"{role}: baseline saved")
        return 0
    if args.lanes_status:
        print("\n".join(lanes.status(RIG)))
        return 0
    if args.tier:
        tier = budget.tiers(SUITES, SCENARIOS)[args.tier]
        args.scenarios = tier + [name for name in args.scenarios if name not in tier]
    if args.list or not args.scenarios:
        for name, scenario in SCENARIOS.items():
            print(f"{name}: {scenario.get('about', '')} ({', '.join(scenario['roles'])}; "
                  f"{budget.describe(name, scenario)})")
        for name, suite in SUITES.items():
            print(f"suite {name}: {suite['about']} ({', '.join(suite['roles'])}; timeout "
                  f"{budget.suite_timeout(suite)} s): " + ", ".join(
                      f"{chapter['name']} {budget.chapter_budget(chapter)} s"
                      f"{' KNOWN' if chapter.get('known_bug') else ''}"
                      for chapter in suite["chapters"]))
        return 0
    # Another lane's games are the rig's own: only a game someone plays stops a lane's run.
    if player_blocks() if IN_LANES or args.lanes else games_running():
        print("A game is running; not starting the rig.")
        return 2
    unknown = [name for name in args.scenarios if name not in RUNNABLE]
    if unknown:
        print(f"No such scenario: {', '.join(unknown)} (--list shows them)")
        return 2
    chosen = {}
    if args.chapter:
        if len(args.scenarios) != 1 or args.scenarios[0] not in SUITES:
            print("--chapter needs one suite")
            return 2
        suite = SUITES[args.scenarios[0]]
        if missing := set(args.chapter) - {chapter["name"] for chapter in suite["chapters"]}:
            print(f"No such chapter in {args.scenarios[0]}: {', '.join(sorted(missing))}")
            return 2
        chosen[args.scenarios[0]] = compile_suite(suite, only=args.chapter)
    owner = args.owner or lanes.default_owner()
    if args.lanes:
        taken = lanes.claim_free_waiting(RIG, args.lanes, owner)
        if not taken:
            print("Every lane is held or blocked (--lanes-status): not starting the rig.")
            return 2
        print(f"lanes {', '.join(map(str, taken))} taken for {owner}"
              f"{f' (of {args.lanes} asked)' if len(taken) < args.lanes else ''}")
        batch = RUNS / time.strftime("%Y%m%d-%H%M%S")
        prune_runs(RUNS, RUNS_KEPT - 1)
        try:
            results = run_lanes(args.scenarios, taken, batch)
        finally:
            for lane in taken:
                lanes.release(RIG, lane)
        print("\n".join(f"{'ok  ' if ok else 'FAIL'} {name}" for name, ok in results.items()))
        return 0 if all(results.values()) else 1
    # One lane: taken here unless the --lanes run that started this one holds it.
    claimed_by_parent = os.environ.get(LANE_HELD) == str(LANE)
    if not claimed_by_parent and not lanes.claim_when_free(RIG, LANE, owner):
        held = lanes.read_claim(RIG, LANE)
        print(f"Lane {LANE} is " + (f"held by {held['owner']} since {held['since']}"
                                    if held else "blocked by games that would not stop") +
              " (--lanes-status): not starting the rig.")
        return 2
    batch = args.batch or RUNS / time.strftime("%Y%m%d-%H%M%S")
    if not args.batch:  # a lane's run: --lanes keeps the batches
        prune_runs(RUNS, RUNS_KEPT - 1)
    try:
        results = {name: run(name, batch, scenario=chosen.get(name)) for name in args.scenarios}
    finally:
        stop_games()
        if not args.keep_servers:
            stop_servers()
        if not claimed_by_parent:
            lanes.release(RIG, LANE)
    print("\n".join(f"{'ok  ' if ok else 'FAIL'} {name}" for name, ok in results.items()))
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
