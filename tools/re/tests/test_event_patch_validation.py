# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for party_boss_events.validate: every patched event file is checked against the game's
before it is written, so a patch that moves what it did not mean to move (2026-10-12: the shop
event's parameters, which opened every weapon line) stops the tool instead of reaching players.

Run: py -3.12 -m unittest discover -s tools\\re\\tests
"""
import contextlib
import hashlib
import io
import json
import os
import pathlib
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import party_boss_events as tool  # noqa: E402
from test_party_boss_events import (boss_intro_event, build_emevd, host_fog_event,  # noqa: E402
                                    phantom_fog_event)

SHOP = 12101000
DREAM = 'm21_00_00_00.emevd.dcx'
GAME = pathlib.Path(os.environ.get('BB_ROOT', r'C:\bb' if os.name == 'nt' else '~/bb')).expanduser() / 'rig' / 'game'
GAME_EVENTS = [GAME / 'CUSA03173' / 'dvdroot_ps4' / 'event',
               GAME / 'CUSA03173-patch' / 'dvdroot_ps4' / 'event']


def shop_lines() -> list[tuple[int, int, bytes]]:
    """m21_00 12101000: a slot's shop line opens when the player has its badge (3:4)."""
    return [(1003, 2, struct.pack('<BBBxi', 0, 1, 0, 0)),
            (3, 4, struct.pack('<BBxxiB', 0, 3, 0, 1)),
            (2004, 26, struct.pack('<ii', 0, 1))]


DCX = b'DCX\0' + bytes(0x48)  # a DCX header (save_dcx fills in the sizes)
SHOP_PARAMETERS = {SHOP: [(0, 4, 0, 4), (1, 4, 4, 4), (2, 0, 8, 4)]}


def shop_file() -> bytes:
    return build_emevd({SHOP: shop_lines(), 12101020: [(2000, 2, struct.pack('<i', 0))]},
                       SHOP_PARAMETERS)


def fields_of(emevd: bytes) -> list[int]:
    return list(struct.unpack_from('<16q', emevd, 16))


def with_fields(emevd: bytes, fields: list[int]) -> bytes:
    out = bytearray(emevd)
    struct.pack_into('<16q', out, 16, *fields)
    return bytes(out)


def event_entry(emevd: bytes, wanted: int) -> tuple[int, list]:
    fields = fields_of(emevd)
    for index in range(fields[0]):
        at = fields[1] + index * 48
        entry = list(struct.unpack_from('<qqqqqii', emevd, at))
        if entry[0] == wanted:
            return at, entry
    raise KeyError(wanted)


def without_parameter_fix(emevd: bytes, event: int) -> bytes:
    """The shop patch as it was before c0319164: the instructions moved one down, the event's
    parameters still named the old instruction numbers."""
    out = bytearray(emevd)
    fields = fields_of(emevd)
    _, entry = event_entry(emevd, event)
    for k in range(entry[3]):
        at = fields[9] + entry[4] + k * tool.PARAMETER_SIZE
        struct.pack_into('<q', out, at, struct.unpack_from('<q', out, at)[0] - 1)
    return bytes(out)


class ValidatePassesTheIntendedPatches(unittest.TestCase):

    def test_an_unpatched_file_is_valid(self):
        tool.validate(shop_file(), shop_file(), [])

    def test_a_local_event_with_its_parameters_moved_along(self):
        edits = []
        patched, count = tool.make_events_local(shop_file(), {SHOP}, edits)
        self.assertEqual(count, 1)
        self.assertEqual(edits, [{'event': SHOP, 'action': 'insert', 'before': 0,
                                  'bank': 2000, 'id': 2, 'why': 'no network sync'}])
        tool.validate(shop_file(), patched, edits, 'm21_00_00_00.emevd.dcx')

    def test_the_boss_fog_patch_that_grows_the_argument_table(self):
        original = build_emevd({12414731: phantom_fog_event(), 12414730: host_fog_event(),
                                12411702: boss_intro_event()})
        edits = []
        patched, count = tool.open_boss_fog_for_guests(original, edits)
        self.assertEqual(count, 4)
        self.assertGreater(len(patched), len(original), 'the argument table grew')
        self.assertIn({'event': 12414731, 'action': 'change', 'instruction': 5, 'bank': 2003,
                       'id': 22, 'why': 'a guest entering sets the host flag too'}, edits)
        tool.validate(original, patched, edits)

    def test_the_boss_insight_and_client_patches_name_their_instructions(self):
        original = build_emevd({
            12411700: [(2003, 12, struct.pack('<i', 2410800)),
                       (1003, 105, struct.pack('<BBxx', 0, 1)),
                       (2003, 4, struct.pack('<i', 24100))],
            12410995: [(1003, 6, struct.pack('<BBxx', 0, 1)),
                       (2000, 0, struct.pack('<iii', 0, tool.INSIGHT_EVENT, 1))],
        })
        edits = []
        patched, _ = tool.patch_events(original, edits)
        patched, _ = tool.let_guests_earn_insight(patched, edits)
        self.assertEqual(sorted((edit['event'], edit['instruction'], edit['bank'], edit['id'])
                                for edit in edits),
                         [(12410995, 0, 1003, 6), (12411700, 1, 1003, 105),
                          (12411700, 2, 2003, 36)])
        tool.validate(original, patched, edits)

    def test_a_dcx_is_read_like_the_game_reads_it(self):
        edits = []
        patched, _ = tool.make_events_local(shop_file(), {SHOP}, edits)
        tool.validate(tool.save_dcx(DCX, shop_file()), tool.save_dcx(DCX, patched), edits)


class ValidateCatchesWhatMoved(unittest.TestCase):

    def assertRejected(self, original: bytes, patched: bytes, edits: list, *words: str):
        with self.assertRaises(tool.EventPatchError) as caught:
            tool.validate(original, patched, edits, DREAM)
        message = str(caught.exception)
        for word in (DREAM, *words):
            self.assertIn(word, message)
        return message

    def test_the_shop_patch_before_the_parameter_fix(self):
        # c0319164: the badge check (3:4) took nothing and every weapon line opened.
        edits = []
        patched, _ = tool.make_events_local(shop_file(), {SHOP}, edits)
        message = self.assertRejected(shop_file(), without_parameter_fix(patched, SHOP), edits,
                                      str(SHOP), 'parameter 1', '3:4')
        self.assertIn('instruction 1', message)

    def test_an_insert_nobody_intended(self):
        patched, _ = tool.make_events_local(shop_file(), {SHOP})
        self.assertRejected(shop_file(), patched, [], str(SHOP), 'instructions')

    def test_an_argument_changed_without_an_edit(self):
        patched = bytearray(shop_file())
        fields = fields_of(patched)
        _, entry = event_entry(patched, SHOP)
        _, _, length, offset, _ = struct.unpack_from('<iiqqq', patched, fields[3] + entry[2] + 32)
        patched[fields[13] + offset + length - 1] ^= 1
        self.assertRejected(shop_file(), bytes(patched), [], str(SHOP), 'instruction 1', '3:4',
                            'argument')

    def test_an_instruction_replaced_without_an_edit(self):
        patched = bytearray(shop_file())
        fields = fields_of(patched)
        _, entry = event_entry(patched, SHOP)
        struct.pack_into('<i', patched, fields[3] + entry[2] + 32 * 2 + 4, 27)
        self.assertRejected(shop_file(), bytes(patched), [], str(SHOP), 'instruction 2',
                            '2004:26')

    def test_the_file_size_in_the_header(self):
        patched = bytearray(shop_file())
        struct.pack_into('<I', patched, 0x0C, len(patched) - 4)
        self.assertRejected(shop_file(), bytes(patched), [], 'size')

    def test_a_section_past_the_end(self):
        fields = fields_of(shop_file())
        fields[9] = len(shop_file())
        self.assertRejected(shop_file(), with_fields(shop_file(), fields), [], 'parameters')

    def test_strings_that_were_not_shifted(self):
        # An insert that forgets one of the header offsets after the instructions.
        edits = []
        patched, _ = tool.make_events_local(shop_file(), {SHOP}, edits)
        fields = fields_of(patched)
        fields[15] -= tool.INSTRUCTION_SIZE
        self.assertRejected(shop_file(), with_fields(patched, fields), edits, 'strings')

    def test_a_parameter_that_would_overwrite_the_patched_byte(self):
        # A started event copying its argument over the patched client state would undo it.
        ends = [(1003, 6, struct.pack('<BBxx', 0, 1))]
        original = build_emevd({9400: ends}, {9400: [(0, 0, 0, 4)]})
        edits = []
        patched, count = tool.patch_client_events(original, {9400}, edits)
        self.assertEqual(count, 1)
        self.assertRejected(original, patched, edits, '9400', 'parameter 0', 'patched')

    def test_an_edit_for_an_event_that_is_not_there(self):
        edit = {'event': 1, 'action': 'change', 'instruction': 0, 'bank': 1003, 'id': 6}
        self.assertRejected(shop_file(), shop_file(), [edit], 'event 1')


class PatchErrorsAreClear(unittest.TestCase):

    def test_no_zero_word_for_the_inserted_argument(self):
        lines = [(1003, 2, struct.pack('<BBBxi', 1, 1, 1, 7))]
        with self.assertRaises(tool.EventPatchError) as caught:
            tool.make_events_local(build_emevd({SHOP: lines}), {SHOP})
        self.assertIn('zero', str(caught.exception))


class TheToolWritesAManifest(unittest.TestCase):

    def run_tool(self, game: pathlib.Path, out: pathlib.Path) -> int:
        with contextlib.redirect_stdout(io.StringIO()):
            return tool.main([str(out), str(game)])

    def test_outputs_and_manifest(self):
        with tempfile.TemporaryDirectory() as temp:
            game, out = pathlib.Path(temp, 'game'), pathlib.Path(temp, 'out')
            game.mkdir()
            original = tool.save_dcx(DCX, shop_file())
            (game / DREAM).write_bytes(original)
            self.assertEqual(self.run_tool(game, out), 0)
            written = (out / DREAM).read_bytes()
            manifest = json.loads((out / tool.MANIFEST).read_text())
            entry = manifest['files'][DREAM]
            self.assertEqual(entry['input_sha256'], hashlib.sha256(original).hexdigest())
            self.assertEqual(entry['output_sha256'], hashlib.sha256(written).hexdigest())
            self.assertEqual([(edit['event'], edit['action']) for edit in entry['edits']],
                             [(SHOP, 'insert')])

    def test_nothing_is_written_when_a_patch_moves_something_else(self):
        real = tool.make_events_local

        def broken(emevd, wanted, edits=None):
            patched, count = real(emevd, wanted, edits)
            return without_parameter_fix(patched, SHOP), count

        with tempfile.TemporaryDirectory() as temp:
            game, out = pathlib.Path(temp, 'game'), pathlib.Path(temp, 'out')
            game.mkdir()
            (game / DREAM).write_bytes(tool.save_dcx(DCX, shop_file()))
            tool.make_events_local = broken
            try:
                with self.assertRaises(tool.EventPatchError):
                    self.run_tool(game, out)
            finally:
                tool.make_events_local = real
            self.assertFalse((out / DREAM).exists())
            self.assertFalse((out / tool.MANIFEST).exists())


def game_event_files() -> dict[str, pathlib.Path]:
    files = {}
    for folder in GAME_EVENTS:
        for path in sorted(folder.glob('*.emevd.dcx')):
            files[path.name] = path  # the update's file replaces the base game's
    return files


@unittest.skipUnless(game_event_files(), f'no game event files under {GAME}')
class TheGamesEventFiles(unittest.TestCase):
    """The real 01.09 event files: every patch the tool makes passes validate."""

    def test_the_game_files_are_valid_as_they_are(self):
        for name, path in game_event_files().items():
            emevd = tool.load_dcx(path.read_bytes())
            tool.validate(emevd, emevd, [], name)

    def test_every_patched_file_passes(self):
        patched = 0
        for name, path in game_event_files().items():
            original = path.read_bytes()
            output, edits, _ = tool.patch_file(name, original)
            if output is not None:
                tool.validate(original, output, edits, name)
                patched += 1
        self.assertGreater(patched, 10)

    def test_the_shop_patch_before_the_parameter_fix_on_the_real_dream(self):
        original = tool.load_dcx(game_event_files()[DREAM].read_bytes())
        edits = []
        patched, _ = tool.make_events_local(original, tool.LOCAL_EVENTS[DREAM], edits)
        tool.validate(original, patched, edits, DREAM)
        with self.assertRaises(tool.EventPatchError) as caught:
            tool.validate(original, without_parameter_fix(patched, SHOP), edits, DREAM)
        self.assertIn(str(SHOP), str(caught.exception))


if __name__ == '__main__':
    unittest.main()
