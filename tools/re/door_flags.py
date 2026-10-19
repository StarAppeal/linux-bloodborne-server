# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Lists the maps' door flags. A map event that waits for an object act (5:2: a door, gate,
lever, kicked ladder) keeps the door open by flags: the object act's own flag, the event's
"done" flag (its id plus the start's slot: the next load reads it and sets the door open at
once) and any flag it sets on (Cathedral Ward's gate event 12400760 sets 12400160). The party
shares them (DoorFlags in emulator/src/core/bloodborne_flags.h): the game only syncs an object
act to players on the same map, so a door a teammate opened elsewhere stayed shut (Kaniel
2026-10-19).

Run: py -3.12 tools\\re\\door_flags.py <folder with the game's event\\*.emevd.dcx>
"""
import pathlib
import struct
import sys

from party_boss_events import EventPatchError, parse

OBJ_ACT_DONE = (5, 2)  # condition group, object act flag
INIT_EVENT = (2000, 0)  # slot, event, the event's arguments
SET_EVENT_FLAG = (2003, 2)  # flag, state (1 on)
MAP_FLAGS = range(12000000, 14000000)


def int_at(data: bytes, at: int) -> int | None:
    return struct.unpack_from('<i', data, at)[0] if 0 <= at and at + 4 <= len(data) else None


def door_flags(emevd: bytes) -> tuple[set[int], set[int]]:
    """(the flags its door events wait for, set or end with, the flags its events clear); an
    event's argument comes from each of its starts (2000:0) in the same file."""
    events = parse(emevd)['events']
    starts: dict[int, list[tuple[int, bytes]]] = {}
    for event in events:
        for bank, ident, _, _, args in event['instructions']:
            if (bank, ident) == INIT_EVENT and len(args) >= 8:
                starts.setdefault(int_at(args, 4), []).append((int_at(args, 0), args[8:]))

    def values(event: dict, index: int, at: int, args: bytes) -> list[int]:
        sources = [source for instruction, target, source, *_ in event['parameters']
                   if instruction == index and target == at]
        if not sources:
            value = int_at(args, at)
            return [] if value is None else [value]
        found = (int_at(start, sources[0]) for _, start in starts.get(event['id'], []))
        return [value for value in found if value is not None]

    doors, cleared = set(), set()
    for event in events:
        instructions = event['instructions']
        waits, sets, clears = set(), set(), set()
        for index, (bank, ident, _, _, args) in enumerate(instructions):
            if (bank, ident) == OBJ_ACT_DONE:
                waits.update(values(event, index, 4, args))
            elif (bank, ident) == SET_EVENT_FLAG and len(args) >= 5:
                (sets if args[4] == 1 else clears).update(values(event, index, 0, args))
        cleared |= clears
        # A switch (its event turns the object act's flag back off) is no door.
        if waits and not waits & clears:
            slots = [slot for slot, _ in starts.get(event['id'], [])] or [0]
            doors.update(event['id'] + slot for slot in slots)
            doors |= waits | sets
    return doors, cleared


def map_door_flags(files: dict[str, bytes]) -> list[int]:
    """The maps' door flags, leaving out any an event clears again (a switch that turns
    back): only a door that stays open is shared."""
    doors, cleared = set(), set()
    for name, data in files.items():
        if not name.startswith('m'):
            continue
        flags, clears = door_flags(data)
        doors |= flags
        cleared |= clears
    return sorted(flag for flag in doors - cleared if flag in MAP_FLAGS)


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    files = {path.name: path.read_bytes()
             for path in sorted(pathlib.Path(argv[0]).glob('*.emevd.dcx'))}
    flags = map_door_flags(files)
    print(f'inline constexpr std::array<s32, {len(flags)}> DoorFlags{{')
    for at in range(0, len(flags), 9):
        print('    ' + ' '.join(f'{flag},' for flag in flags[at:at + 9]))
    print('};')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except EventPatchError as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
