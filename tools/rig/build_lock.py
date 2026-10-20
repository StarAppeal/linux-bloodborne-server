# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds on the PC take a build slot, and nothing starts while memory runs low.

Kaniel 2026-10-20 22:20Z: one build at a time, never beside rig games (his PC froze). 22:42Z,
after a reboot: the freeze was most likely leaked memory (crashed games leak commit), so up to
two builds may run at once and beside games. Both limits stay one env var away:

  BB_BUILD_SLOTS=N          builds at once (default 2; 1 = one at a time)
  BB_BUILD_BESIDE_GAMES=0   a build waits for every game, and a game for every build (default 1)
  BB_MIN_FREE_RAM_GB=G      a build or rig game refuses to start below G GB free RAM (default 2)
  BB_MIN_FREE_COMMIT_GB=G   ... or below G GB of commit left (default 4)
  BB_MEMORY_GUARD=0         no memory check

A build holds one of C:\\bb\\build.lock, build-2.lock, ... ("<owner>\\n<pid>\\n<when>") while it
runs. A lock whose process is gone (a build killed outright) is stale and taken over.
tools\\build-*.ps1 and make-release.ps1 build under it; any other build too:

  py -3.12 tools\\rig\\build_lock.py run --owner <who> -- <command ...>
  py -3.12 tools\\rig\\build_lock.py status
"""
import argparse
import os
import pathlib
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import engines  # noqa: E402
import lanes  # noqa: E402

LOCK = engines.BB_ROOT / "build.lock"
# Set for a build's own process tree: a build script a build runs (make-release.ps1 runs
# build-server.ps1) is inside its lock already.
HELD = "BB_BUILD_LOCK_HELD"
POLL_SECONDS = 10


def slots() -> int:
    value = os.environ.get("BB_BUILD_SLOTS", "")
    return int(value) if value.isdigit() and int(value) > 0 else 2


def beside_games() -> bool:
    return os.environ.get("BB_BUILD_BESIDE_GAMES", "1") != "0"


def slot_path(lock: pathlib.Path, slot: int) -> pathlib.Path:
    return lock if slot == 1 else lock.with_name(f"{lock.stem}-{slot}{lock.suffix}")


def holder(lock: pathlib.Path = LOCK) -> tuple[str, int] | None:
    """Who holds this one lock file (owner, pid), or None: free, or held by a process that is
    gone."""
    try:
        lines = lock.read_text(encoding="utf-8").splitlines()
    except OSError:
        return None
    pid = int(lines[1]) if len(lines) > 1 and lines[1].strip().isdigit() else 0
    if not lanes.pid_alive(pid):
        return None
    return lines[0] if lines else "?", pid


def holders(lock: pathlib.Path = LOCK) -> list[tuple[str, int]]:
    """Every build that holds a slot, those past a lowered BB_BUILD_SLOTS too."""
    paths = [lock, *lock.parent.glob(f"{lock.stem}-*{lock.suffix}")]
    return [held for path in paths if (held := holder(path))]


def games_running() -> list[int]:
    """Every game on the PC, the rig's in any lane and anyone's own."""
    if os.name != "nt":
        return []
    names = " OR ".join(f"Name='{name}'" for name in engines.GAME_PROCESSES)
    listing = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         f"(Get-CimInstance Win32_Process -Filter \"{names}\").ProcessId"],
        capture_output=True, text=True).stdout
    return [int(pid) for pid in listing.split() if pid.isdigit()]


def free_memory() -> tuple[int, int] | None:
    """(free RAM, commit left) in bytes, or None off Windows."""
    if os.name != "nt":
        return None
    import ctypes

    class Status(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

    status = Status(dwLength=ctypes.sizeof(Status))
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        return None
    return status.ullAvailPhys, status.ullAvailPageFile


def gigabytes(name: str, default: float) -> float:
    try:
        return float(os.environ.get(name, "") or default)
    except ValueError:
        return default


def memory_short(read=free_memory) -> str | None:
    """Why memory is too low to start a build or game, or None."""
    if os.environ.get("BB_MEMORY_GUARD") == "0" or (memory := read()) is None:
        return None
    ram, commit = (value / (1 << 30) for value in memory)
    min_ram = gigabytes("BB_MIN_FREE_RAM_GB", 2)
    min_commit = gigabytes("BB_MIN_FREE_COMMIT_GB", 4)
    short = []
    if ram < min_ram:
        short.append(f"{ram:.1f} GB free RAM (at least {min_ram:g} wanted)")
    if commit < min_commit:
        short.append(f"{commit:.1f} GB of commit left (at least {min_commit:g} wanted)")
    return ", ".join(short) or None


def refuse_if_memory_low(what: str, read=free_memory) -> None:
    """Stops `what` from starting while memory runs low (crashed games leak commit)."""
    if short := memory_short(read):
        raise SystemExit(f"Memory guard: not starting {what}: {short}. Close games, or reboot "
                         "to free memory crashed games leaked (BB_MEMORY_GUARD=0 skips this).")


def try_take(owner: str, pid: int, lock: pathlib.Path = LOCK) -> bool:
    """Takes a free or stale slot for `pid`; True when `pid` holds one."""
    if any(held[1] == pid for held in holders(lock)):
        return True
    for slot in range(1, slots() + 1):
        path = slot_path(lock, slot)
        if holder(path):
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.unlink(missing_ok=True)  # stale: its process is gone
        try:
            handle = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            continue  # another build took it just now
        with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as file:
            file.write(f"{owner}\n{pid}\n{time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        return True
    return False


def take(owner: str, pid: int, lock: pathlib.Path = LOCK, games=games_running,
         sleep=time.sleep) -> None:
    """Waits for a free slot (and, with BB_BUILD_BESIDE_GAMES=0, every game), then holds it."""
    said = None
    while True:
        running = [] if beside_games() else games()
        if running:
            why = f"waiting for {len(running)} game(s) to end (pids {running})"
        elif try_take(owner, pid, lock):
            print(f"Build lock: {owner} builds (pid {pid})", flush=True)
            return
        else:
            builds = ", ".join(f"{name} (pid {held})" for name, held in holders(lock))
            why = f"{slots()} build(s) at most at once: waiting for {builds}"
        if why != said:
            print(f"Build lock: {why}", flush=True)
            said = why
        sleep(POLL_SECONDS)


def release(pid: int, lock: pathlib.Path = LOCK) -> None:
    for path in [lock, *lock.parent.glob(f"{lock.stem}-*{lock.suffix}")]:
        held = holder(path)
        if held and held[1] == pid:
            path.unlink(missing_ok=True)


def wait_for_builds(lock: pathlib.Path = LOCK, sleep=time.sleep) -> None:
    """With BB_BUILD_BESIDE_GAMES=0 a game waits while a build runs (run_scenario.py's launch)."""
    said = None
    while not beside_games() and (held := holders(lock)):
        if held != said:
            print(f"Build lock: waiting for the build(s) {held} before starting a game",
                  flush=True)
            said = held
        sleep(POLL_SECONDS)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="action", required=True)
    run = sub.add_parser("run", help="run a build command under the lock")
    run.add_argument("--owner", default=lanes.default_owner())
    run.add_argument("command", nargs=argparse.REMAINDER)
    sub.add_parser("status", help="who holds the lock")
    args = parser.parse_args(argv)
    if args.action == "status":
        held = holders()
        print(", ".join(f"held by {name} (pid {pid})" for name, pid in held) or "free",
              f"({slots()} slot(s); beside games: {'yes' if beside_games() else 'no'})")
        print(memory_short() or "memory: fine")
        return 0
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("run needs a command after --")
    if os.environ.get(HELD) == "1":
        return subprocess.run(command).returncode
    refuse_if_memory_low(f"the build of {args.owner}")
    take(args.owner, os.getpid())
    try:
        return subprocess.run(command, env={**os.environ, HELD: "1"}).returncode
    finally:
        release(os.getpid())


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
