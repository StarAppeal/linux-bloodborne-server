# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""The reused release checkout's emulator test decision (tools\\release-src.ps1 runs it).

Releases build in one checkout, <BB_ROOT>\\release-src, so the port, the server and the emulator
tests build incrementally, and the emulator tests run only when the emulator changed since the
last commit they passed on (Kaniel 2026-10-22). The release ships no emulator. That commit is kept
in a stamp file (<BB_ROOT>\\out\\release-emu\\tested-commit.txt).

Usage: release_src.py emulator-check --root <checkout> --stamp <file>
           prints EMULATOR_TESTS=needed|skip and why
       release_src.py emulator-mark --root <checkout> --stamp <file>
           after the tests passed: stores the checkout's commit
"""
import argparse
import pathlib
import subprocess
import sys

# Paths whose change means the emulator tests run again.
EMULATOR_PATHS = ("emulator/", "tools/build-emu.ps1")


class JjError(Exception):
    pass


def run_jj(root, *args):
    result = subprocess.run(["jj", "-R", str(root), *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise JjError(result.stderr.strip() or f"jj {' '.join(args)} failed")
    return result.stdout


def emulator_changed(paths):
    return bool(emulator_files(paths))


def emulator_files(paths):
    files = []
    for path in paths:
        path = path.replace("\\", "/")
        if any(path == prefix or (prefix.endswith("/") and path.startswith(prefix)) for prefix in EMULATOR_PATHS):
            files.append(path)
    return files


def read_stamp(stamp):
    try:
        return pathlib.Path(stamp).read_text(encoding="utf-8").strip() or None
    except FileNotFoundError:
        return None


def write_stamp(stamp, commit):
    stamp = pathlib.Path(stamp)
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text(commit + "\n", encoding="utf-8")


def decide(root, stamp, jj=run_jj):
    """(needed, why): whether the emulator tests must run for the checkout at root."""
    tested = read_stamp(stamp)
    if not tested:
        return True, "the emulator tests never passed in this checkout"
    try:
        changed = jj(root, "diff", "--name-only", "--from", tested, "--to", "@").split()
    except JjError as error:
        return True, f"cannot compare with {tested}: {error}"
    files = emulator_files(changed)
    if files:
        shown = ", ".join(files[:5]) + (f" and {len(files) - 5} more" if len(files) > 5 else "")
        return True, f"the emulator changed since {tested}: {shown}"
    return False, f"the emulator is unchanged since {tested}, where its tests passed"


def mark(root, stamp, jj=run_jj):
    # @- : the release commit itself (the checkout's @ is the empty change on top of it, which jj
    # drops at the next `jj new`).
    commit = jj(root, "log", "-r", "@-", "--no-graph", "-T", "commit_id").strip()
    write_stamp(stamp, commit)
    return commit


def main(argv=None, run_jj=run_jj):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=("emulator-check", "emulator-mark"))
    parser.add_argument("--root", required=True)
    parser.add_argument("--stamp", required=True)
    args = parser.parse_args(argv)
    if args.command == "emulator-check":
        needed, why = decide(args.root, args.stamp, run_jj)
        print(f"EMULATOR_TESTS={'needed' if needed else 'skip'} ({why})")
    else:
        print(f"Emulator tests marked as passed at {mark(args.root, args.stamp, run_jj)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
