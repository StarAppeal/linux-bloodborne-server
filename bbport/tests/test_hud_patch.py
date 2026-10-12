from paths import ROOT
import os
from pathlib import Path
import struct
import tempfile
import unittest

import hud_patch
import mods

GAME_FE = os.environ.get('BB_GAME_FE')  # the game's menu/fe.gfx, for the check against the real file


def tag(code, body, long=False):
    if len(body) < 0x3F and not long:
        return struct.pack('<H', code << 6 | len(body)) + body
    return struct.pack('<HI', code << 6 | 0x3F, len(body)) + body


def place2(depth, char, name):
    # flags: has name (0x20), has character (0x02), has matrix (0x04: an empty one, a zero byte)
    return tag(26, bytes([0x26]) + struct.pack('<HH', depth, char) + b'\0' + name.encode() + b'\0')


def edit_text(char, html, color=b'\x9b\x21\x02\xff'):
    # rect: nbits 1 (all zero), then flags: HasText | HasColor | HasFontClass + HTML, size 124, var ''
    rect = bytes([0x08, 0x00])
    flags = bytes([0x80 | 0x04, 0x80 | 0x02])
    body = (struct.pack('<H', char) + rect + flags + b'MenuFont_01\0' + struct.pack('<H', 124 * 20) + color +
            b'\0' + html.encode() + b'\0')
    return tag(37, body, long=True)


def sprite(char, *children):
    return tag(39, struct.pack('<HH', char, 1) + b''.join(children) + tag(1, b'') + tag(0, b''), long=True)


def symbol_class(char, name):
    return tag(76, struct.pack('<HH', 1, char) + name.encode() + b'\0')


def movie(*tags):
    body = bytes([0x08, 0x00]) + struct.pack('<HH', 0x1E00, 1) + b''.join(tags) + tag(0, b'')
    return b'GFX\x0b' + struct.pack('<I', 8 + len(body)) + body


DRAW_HTML = ('<p align="center"><font size="124" color="#9b2102" letterSpacing="0.000000" kerning="1">'
             '<b>HUNTED NIGHTMARE</b></font></p>')
DEAD_HTML = DRAW_HTML.replace('9b2102', '960807').replace('HUNTED NIGHTMARE', 'YOU DIED')


def fe_movie():
    """A small fe.gfx: TextEffect_Draw (two layers of its text) and TextEffect_Dead, as the game's is built."""
    return movie(
        edit_text(10, DRAW_HTML), sprite(11, place2(1, 10, 'StaticText_522007')),
        edit_text(12, DRAW_HTML), sprite(13, place2(1, 12, 'StaticText_522007')),
        sprite(14, place2(1, 11, ''), place2(3, 13, '')),
        edit_text(20, DEAD_HTML, b'\x96\x08\x07\xff'), sprite(21, place2(1, 20, 'StaticText_522001')),
        sprite(22, place2(1, 21, '')),
        sprite(30, tag(1, b'')),
        place2(5, 14, 'TextEffect_Draw'), place2(6, 22, 'TextEffect_Dead'), place2(9, 30, 'RemotePCTag0'),
        tag(1, b''),
        symbol_class(14, 'Fe_fla._TextEffect_Draw_195'))


class GfxTests(unittest.TestCase):
    def test_an_unchanged_movie_is_written_back_byte_for_byte(self):
        data = fe_movie()
        self.assertEqual(hud_patch.write_movie(hud_patch.read_movie(data)), data)

    def test_the_downed_banner_replaces_the_draw_banner(self):
        patched = hud_patch.patch_fe(fe_movie())
        self.assertEqual(struct.unpack_from('<I', patched, 4)[0], len(patched))
        self.assertIn(b'<b>YOU ARE DOWN</b>', patched)
        self.assertIn(b'color="#' + hud_patch.DOWNED_COLOR.encode() + b'"', patched)
        self.assertEqual(patched.count(b'YOU ARE DOWN'), 2)  # both layers of the Draw banner
        # The game fills fields named StaticText_<id> from its text tables: the banner's fields are renamed.
        self.assertNotIn(b'StaticText_522007', patched)
        self.assertEqual(patched.count(hud_patch.DOWNED_FIELD.encode()), 2)
        # Only the Draw banner changes: YOU DIED and its field stay as they were.
        self.assertIn(b'<b>YOU DIED</b>', patched)
        self.assertIn(b'StaticText_522001', patched)
        self.assertEqual(hud_patch.write_movie(hud_patch.read_movie(patched)), patched)

    def test_the_party_list_is_three_more_nameplate_tags_on_top(self):
        _, tags = hud_patch.read_movie(hud_patch.patch_fe(fe_movie()))
        first_frame = next(i for i, t in enumerate(tags) if t.code == hud_patch.SHOW_FRAME)
        places = {hud_patch.place_parts(t)[1]: t for t in tags[:first_frame] if t.code == hud_patch.PLACE_OBJECT2}
        self.assertEqual([places[name].body[3:5] for name in hud_patch.PARTY_LIST], [struct.pack('<H', 30)] * 3)
        self.assertEqual([hud_patch.place_depth(places[name]) for name in hud_patch.PARTY_LIST], [11, 13, 15])

    def test_the_list_icons_are_a_sprite_of_our_drawings_beside_each_tag(self):
        strip = hud_patch.HUD_ICONS.read_bytes()
        patched = hud_patch.patch_fe(fe_movie(), strip)
        self.assertEqual(hud_patch.write_movie(hud_patch.read_movie(patched)), patched)
        _, tags = hud_patch.read_movie(patched)
        bitmaps = [t for t in tags if t.code == 36]
        self.assertEqual(len(bitmaps), len(hud_patch.HUD_ICON_FRAMES) - 1)
        sprites = {struct.unpack_from('<H', t.body)[0]: t for t in tags if t.code == hud_patch.SPRITE}
        first_frame = next(i for i, t in enumerate(tags) if t.code == hud_patch.SHOW_FRAME)
        places = {hud_patch.place_parts(t)[1]: t for t in tags[:first_frame] if t.code == hud_patch.PLACE_OBJECT2}
        icon_char = hud_patch.place_parts(places['CoopPartyIcon0'])[0]
        self.assertEqual(struct.unpack_from('<H', sprites[icon_char].body, 2)[0], len(hud_patch.HUD_ICON_FRAMES))
        self.assertEqual({hud_patch.place_parts(places[name])[0] for name in hud_patch.PARTY_ICONS}, {icon_char})
        # Defined before they are placed, with ids of their own.
        defined = [struct.unpack_from('<H', t.body)[0] for t in tags if t.code in (2, 36, hud_patch.SPRITE)]
        self.assertEqual(len(defined), len(set(defined)))
        self.assertLess(tags.index(sprites[icon_char]), tags.index(places['CoopPartyIcon0']))
        self.assertEqual(hud_patch.patch_fe(patched, strip), patched, 'once')

    def test_the_matrix_and_rect_bits(self):
        self.assertEqual(hud_patch.matrix(None, 0, 0), bytes([0x02, 0x00]))  # no scale, no rotation, 1-bit moves
        self.assertEqual(hud_patch.rect(0, 0)[0] >> 3, 1)
        bits = hud_patch.BitWriter()
        bits.put(-1, 3, True)
        self.assertEqual(bits.data(), bytes([0b11100000]))

    def test_a_hud_without_remote_tags_gets_the_banner_but_no_list(self):
        patched = hud_patch.patch_fe(fe_movie().replace(b'RemotePCTag0', b'RemotePCTagX'))
        self.assertIn(b'YOU ARE DOWN', patched)
        self.assertNotIn(b'CoopParty0', patched)

    def test_the_patch_is_idempotent(self):
        once = hud_patch.patch_fe(fe_movie())
        self.assertEqual(hud_patch.patch_fe(once), once)

    def test_a_movie_without_the_draw_banner_is_refused(self):
        with self.assertRaises(ValueError):
            hud_patch.patch_fe(movie(sprite(14, tag(1, b''))))

    def test_other_files_are_refused(self):
        with self.assertRaises(ValueError):
            hud_patch.patch_fe(b'CWS\x0a' + b'\0' * 20)

    @unittest.skipUnless(GAME_FE and Path(GAME_FE or '.').is_file(), 'BB_GAME_FE: the game\'s menu/fe.gfx')
    def test_the_game_hud(self):
        data = Path(GAME_FE).read_bytes()
        self.assertEqual(hud_patch.write_movie(hud_patch.read_movie(data)), data)
        patched = hud_patch.patch_fe(data)
        self.assertEqual(patched.count(b'YOU ARE DOWN'), 2)  # its two text fields (one shown twice)
        self.assertNotIn(b'StaticText_522007', patched)
        self.assertIn(b'StaticText_522001', patched)
        for name in hud_patch.PARTY_LIST:
            self.assertEqual(patched.count(name.encode() + b'\0'), 1)
        with_icons = hud_patch.patch_fe(data, hud_patch.HUD_ICONS.read_bytes())
        self.assertEqual(hud_patch.write_movie(hud_patch.read_movie(with_icons)), with_icons)
        for name in hud_patch.PARTY_ICONS:
            self.assertEqual(with_icons.count(name.encode() + b'\0'), 1)


class LayerTests(unittest.TestCase):
    FE = 'dvdroot_ps4/menu/fe.gfx'

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / 'CUSA03173'
        (self.game / 'dvdroot_ps4/menu').mkdir(parents=True)
        (self.game / 'eboot.bin').write_bytes(b'exe')
        (self.game / self.FE).write_bytes(fe_movie())
        self.moddir = self.root / 'mods'
        self.moddir.mkdir()
        self.out = self.root / 'out'

    def test_the_layer_patches_the_game_hud_and_goes_over_the_mods(self):
        layers, notes = hud_patch.with_party_hud(self.game, [], self.out)
        self.assertEqual(notes, [])
        self.assertEqual([name for name, _ in layers], [hud_patch.LAYER])
        overlay = mods.build_overlay(self.game, self.out, layers)
        self.assertIn(b'YOU ARE DOWN', (overlay / self.FE).read_bytes())
        self.assertEqual((self.game / self.FE).read_bytes(), fe_movie())

    def test_a_mod_hud_is_the_one_patched(self):
        compact = self.moddir / 'Compact HUD' / 'menu'
        compact.mkdir(parents=True)
        (compact / 'fe.gfx').write_bytes(fe_movie().replace(b'YOU DIED', b'YOU DEAD'))
        layers = mods.user_layers(self.game, self.moddir)
        layers, notes = hud_patch.with_party_hud(self.game, layers, self.out)
        self.assertEqual([name for name, _ in layers], ['Compact HUD', hud_patch.LAYER])
        patched = (mods.build_overlay(self.game, self.out, layers) / self.FE).read_bytes()
        self.assertIn(b'YOU DEAD', patched)
        self.assertIn(b'YOU ARE DOWN', patched)
        self.assertEqual((compact / 'fe.gfx').read_bytes(), fe_movie().replace(b'YOU DIED', b'YOU DEAD'))

    def test_a_hud_that_cannot_be_patched_leaves_the_layers_and_says_why(self):
        (self.game / self.FE).write_bytes(b'not a movie')
        layers, notes = hud_patch.with_party_hud(self.game, [], self.out)
        self.assertEqual(layers, [])
        self.assertEqual(len(notes), 1)
        self.assertIn('fe.gfx', notes[0])

    def test_the_same_hud_reuses_its_generated_file(self):
        hud_patch.with_party_hud(self.game, [], self.out)
        layers, _ = hud_patch.with_party_hud(self.game, [], self.out)
        generated = list((self.out / 'party-hud').glob('*/' + self.FE))
        self.assertEqual(len(generated), 1)
        self.assertEqual(layers[0][1], generated[0].parents[2])

    def test_the_mods_script_takes_the_party_hud(self):
        import subprocess, sys
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/mods.py'), str(self.game), '--out',
                                 str(self.out), '--mods-dir', str(self.moddir), '--party-hud'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        overlay = Path(result.stdout.strip())
        self.assertIn(b'YOU ARE DOWN', (overlay / self.FE).read_bytes())


if __name__ == '__main__':
    unittest.main()
