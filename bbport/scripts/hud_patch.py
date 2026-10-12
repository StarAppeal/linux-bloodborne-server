#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The party HUD: the game's own HUD movie (menu/fe.gfx) with party death's downed banner, made at every start
from the player's copy (or the mod that replaces it, such as Compact HUD) and put over the mods as the last
layer. Nothing of the game's is kept in the port: the movie is read and patched where it is.

The banner is the game's TextEffect_Draw (a PvP result, never shown in co-op; SetTextEffect id 17): its text
fields are renamed, so that the game no longer fills them from its text tables (fields named StaticText_<id>),
and get our words in the game's font, in yellow. The game's fade-in and blur trail stay as they are.

The party list is three more of the game's own nameplate tags (the sprite RemotePCTag0 shows), placed at the
top level as CoopParty0-2: party death fills and places them each frame through the game's own tag code
(bloodborne_party_death.cpp), so the list has the game's font, bars and fades and sits under its menus. Left of
each sits CoopPartyIcon0-2, a sprite whose frames are our own icon drawings (scripts/party_hud_icons.bin, made by
party_icons_atlas.py; HUD_ICON_FRAMES): party death shows the member's with GotoAndStop."""
import hashlib
import zlib
from pathlib import Path
import struct

FE = 'dvdroot_ps4/menu/fe.gfx'
LAYER = 'party HUD'
DRAW_BANNER = 'Fe_fla._TextEffect_Draw_195'
DRAW_FIELD = 'StaticText_522007'
DOWNED_FIELD = 'CoopDownedText'
DOWNED_TEXT = 'YOU ARE DOWN'
DOWNED_COLOR = 'd8b03c'

PARTY_TAG = 'RemotePCTag0'
PARTY_LIST = ('CoopParty0', 'CoopParty1', 'CoopParty2')
PARTY_ICONS = ('CoopPartyIcon0', 'CoopPartyIcon1', 'CoopPartyIcon2')
HUD_ICONS = Path(__file__).resolve().parent / 'party_hud_icons.bin'
# The icon sprite's frames (1-based, as GotoAndStop takes them; PartyListIconFrame in bloodborne_party.cpp).
HUD_ICON_FRAMES = ('none', 'hunter', 'elsewhere', 'fallen') + tuple(f'downed {p}%' for p in range(0, 101, 10))
# Where the list's tags stand on the HUD's 1920x1080 stage (PartyListPosition in bloodborne_party.cpp), and each
# icon's centre from its tag, its size in pixels.
LIST_X, LIST_Y, LIST_STEP = 204, 330, 95  # each bar starts where the player's HP bar does (x 107)
ICON_FROM_TAG, ICON_SIZE = (-128, 1), 36  # centred on the tag's HP bar
# Character ids for what the patch defines, above any the game's movies use.
FIRST_NEW_ID = 50000

SHOW_FRAME, PLACE_OBJECT2, PLACE_OBJECT3, EDIT_TEXT, SPRITE, SYMBOL_CLASS = 1, 26, 70, 37, 39, 76


class Tag:
    """One tag: its code and body; a sprite's body is its header and its own tags."""

    def __init__(self, code, body, long):
        self.code, self.body, self.long = code, body, long
        self.children = None
        if code == SPRITE:
            self.children = read_tags(body, 4)
            self.body = body[:4]

    def encode(self):
        body = self.body + (b''.join(child.encode() for child in self.children) if self.children is not None else b'')
        if len(body) < 0x3F and not self.long:
            return struct.pack('<H', self.code << 6 | len(body)) + body
        return struct.pack('<HI', self.code << 6 | 0x3F, len(body)) + body


def read_tags(data, offset):
    tags = []
    while offset < len(data):
        header, = struct.unpack_from('<H', data, offset)
        offset += 2
        code, length, long = header >> 6, header & 0x3F, False
        if length == 0x3F:
            length, = struct.unpack_from('<I', data, offset)
            offset, long = offset + 4, True
        if offset + length > len(data):
            raise ValueError('a tag runs past the end of the movie')
        tags.append(Tag(code, data[offset:offset + length], long))
        offset += length
        if code == 0:
            break
    return tags


def rect_size(data, offset):
    bits = data[offset] >> 3
    return (5 + 4 * bits + 7) // 8


def read_movie(data):
    """(header, tags) of an uncompressed GFX/SWF movie."""
    if data[:3] not in (b'GFX', b'FWS') or len(data) < 12:
        raise ValueError('not an uncompressed GFX movie')
    if struct.unpack_from('<I', data, 4)[0] != len(data):
        raise ValueError('the movie\'s length does not match its file')
    start = 8 + rect_size(data, 8) + 4
    return data[:start], read_tags(data, start)


def write_movie(movie):
    header, tags = movie
    body = header[8:] + b''.join(tag.encode() for tag in tags)
    return header[:4] + struct.pack('<I', 8 + len(body)) + body


class Bits:
    def __init__(self, data, offset):
        self.data, self.offset, self.bit = data, offset, 0

    def read(self, count):
        value = 0
        for _ in range(count):
            byte = self.data[self.offset]
            value = value << 1 | (byte >> (7 - self.bit)) & 1
            self.bit += 1
            if self.bit == 8:
                self.offset, self.bit = self.offset + 1, 0
        return value

    def end(self):
        return self.offset + (1 if self.bit else 0)


def skip_matrix(data, offset):
    bits = Bits(data, offset)
    for _ in range(2):  # scale, rotate/skew
        if bits.read(1):
            bits.read(2 * bits.read(5))
    bits.read(2 * bits.read(5))  # translate
    return bits.end()


def skip_cxform(data, offset):
    bits = Bits(data, offset)
    add, mult, size = bits.read(1), bits.read(1), bits.read(4)
    bits.read(size * 4 * (add + mult))
    return bits.end()


def place_parts(tag):
    """(character or None, name or None, (start, end) of the name's bytes) of a PlaceObject2/3."""
    body = tag.body
    flags, offset = body[0], 1
    flags2 = 0
    if tag.code == PLACE_OBJECT3:
        flags2, offset = body[1], 2
    offset += 2  # depth
    if tag.code == PLACE_OBJECT3 and (flags2 & 0x08 or (flags2 & 0x10 and flags & 0x02)):
        offset = body.index(b'\0', offset) + 1  # class name
    character = None
    if flags & 0x02:
        character, = struct.unpack_from('<H', body, offset)
        offset += 2
    if flags & 0x04:
        offset = skip_matrix(body, offset)
    if flags & 0x08:
        offset = skip_cxform(body, offset)
    if flags & 0x10:
        offset += 2
    if not flags & 0x20:
        return character, None, None
    end = body.index(b'\0', offset)
    return character, body[offset:end].decode('latin-1'), (offset, end)


def rename(tag, name):
    _, _, (start, end) = place_parts(tag)
    tag.body = tag.body[:start] + name.encode('latin-1') + tag.body[end:]


def edit_text_parts(tag):
    """{field: (start, end)} of a DefineEditText's colour and initial text."""
    body = tag.body
    offset = 2
    offset += rect_size(body, offset)
    first, second = body[offset], body[offset + 1]
    offset += 2
    parts = {}
    if first & 0x01:
        offset += 2  # font id
    if second & 0x80:
        offset = body.index(b'\0', offset) + 1  # font class
    if first & 0x01 or second & 0x80:
        offset += 2  # height
    if first & 0x04:
        parts['color'] = (offset, offset + 4)
        offset += 4
    if first & 0x02:
        offset += 2
    if second & 0x20:
        offset += 9
    offset = body.index(b'\0', offset) + 1  # variable name
    if first & 0x80:
        parts['text'] = (offset, body.index(b'\0', offset))
    return parts


def downed_html(html):
    """The banner's html with our words and colour, its layout kept."""
    head, bold, rest = html.partition('<b>')
    _, close, tail = rest.partition('</b>')
    if not bold or not close or 'color="#' not in head:
        raise ValueError('the Draw banner\'s text is not the html it should be')
    color = head.index('color="#') + len('color="#')
    head = head[:color] + DOWNED_COLOR + head[color + 6:]
    return f'{head}<b>{DOWNED_TEXT}</b>{tail}'


class BitWriter:
    def __init__(self):
        self.bits = []

    def put(self, value, count, signed=False):
        if signed and value < 0:
            value += 1 << count
        self.bits += [(value >> (count - 1 - i)) & 1 for i in range(count)]

    def data(self):
        bits = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def signed_bits(*values):
    return max(max(abs(v) for v in values).bit_length() + 1, 1)


def matrix(scale, tx, ty):
    """A MATRIX: scale (16.16) and translation (twips), no rotation."""
    bits = BitWriter()
    if scale is None:
        bits.put(0, 1)
    else:
        fixed = round(scale * 65536)
        n = signed_bits(fixed)
        bits.put(1, 1)
        bits.put(n, 5)
        bits.put(fixed, n, True)
        bits.put(fixed, n, True)
    bits.put(0, 1)
    n = signed_bits(tx, ty)
    bits.put(n, 5)
    bits.put(tx, n, True)
    bits.put(ty, n, True)
    return bits.data()


def rect(width, height):
    n = signed_bits(width, height)
    bits = BitWriter()
    bits.put(n, 5)
    for value in (0, width, 0, height):
        bits.put(value, n, True)
    return bits.data()


def read_strip(data):
    """(count, [RGBA bytes of each 128-pixel icon]) of a party_icons_atlas.py strip."""
    if data[:4] != b'BBIC':
        raise ValueError('not a party icon strip')
    width, height, count = struct.unpack_from('<HHH', data, 4)
    rgba = zlib.decompress(data[10:])
    if height != 128 or width != 128 * count or len(rgba) != width * height * 4:
        raise ValueError('the party icon strip is not a row of 128-pixel icons')
    icons = []
    for index in range(count):
        icon = bytearray()
        for y in range(128):
            start = (y * width + index * 128) * 4
            icon += rgba[start:start + 128 * 4]
        icons.append(bytes(icon))
    return count, icons


def bitmap(char, rgba):
    """DefineBitsLossless2: 128x128 premultiplied ARGB."""
    argb = bytearray(len(rgba))
    for i in range(0, len(rgba), 4):
        r, g, b, a = rgba[i:i + 4]
        argb[i:i + 4] = bytes((a, r * a // 255, g * a // 255, b * a // 255))
    return Tag(36, struct.pack('<HBHH', char, 5, 128, 128) + zlib.compress(bytes(argb), 9), True)


def bitmap_square(char, bitmap_char):
    """DefineShape: a 128-pixel square filled with the bitmap (clipped), its top left at the origin."""
    side = 128 * 20
    fill = bytes([1, 0x41]) + struct.pack('<H', bitmap_char) + matrix(20.0, 0, 0)
    bits = BitWriter()
    bits.put(1, 4)  # fill index bits
    bits.put(0, 4)  # line index bits
    # Style change: fill style 1, move to (0, 0).
    bits.put(0, 1); bits.put(0, 1); bits.put(0, 1); bits.put(1, 1); bits.put(0, 1); bits.put(1, 1)
    bits.put(1, 5); bits.put(0, 1, True); bits.put(0, 1, True)
    bits.put(1, 1)
    n = signed_bits(side)
    for dx, dy in ((side, 0), (0, side), (-side, 0), (0, -side)):
        bits.put(1, 1); bits.put(1, 1); bits.put(n - 2, 4); bits.put(0, 1)
        bits.put(1 if dx == 0 else 0, 1)
        bits.put(dy if dx == 0 else dx, n, True)
    bits.put(0, 6)  # end of shape
    return Tag(2, struct.pack('<H', char) + rect(side, side) + fill + b'\0' + bits.data(), False)


def place(char, depth, name=None, place_matrix=None, replace=False):
    flags = 0x02 | (0x01 if replace else 0) | (0x04 if place_matrix else 0) | (0x20 if name else 0)
    body = bytes([flags]) + struct.pack('<HH', depth, char) + (place_matrix or b'')
    return Tag(PLACE_OBJECT2, body + (name.encode('latin-1') + b'\0' if name else b''), False)


def icon_sprite(char, shapes):
    """A sprite showing nothing in its first frame, then each shape in a frame of its own."""
    frames = [Tag(SHOW_FRAME, b'', False)]
    for index, shape in enumerate(shapes):
        frames.append(place(shape, 1, replace=index > 0))
        frames.append(Tag(SHOW_FRAME, b'', False))
    sprite = Tag(SPRITE, struct.pack('<HH', char, len(shapes) + 1), True)
    sprite.children = frames + [Tag(0, b'', False)]
    return sprite


def party_icon_tags(depth, strip):
    """The tags that define the icon sprite and place CoopPartyIcon0-2 (from `depth` up)."""
    count, icons = read_strip(strip)
    if count != len(HUD_ICON_FRAMES):
        raise ValueError(f'the party icon strip has {count} icons, not {len(HUD_ICON_FRAMES)}')
    tags, shapes, char = [], [], FIRST_NEW_ID
    for rgba in icons[1:]:  # the first frame shows nothing
        tags += [bitmap(char, rgba), bitmap_square(char + 1, char)]
        shapes.append(char + 1)
        char += 2
    tags.append(icon_sprite(char, shapes))
    scale = ICON_SIZE / 128
    for index, name in enumerate(PARTY_ICONS):
        depth += 2
        cx = LIST_X + ICON_FROM_TAG[0]
        cy = LIST_Y + LIST_STEP * index + ICON_FROM_TAG[1]
        tags.append(place(char, depth, name, matrix(scale, (cx - ICON_SIZE // 2) * 20, (cy - ICON_SIZE // 2) * 20)))
    return tags


def place_depth(tag):
    return struct.unpack_from('<H', tag.body, 2 if tag.code == PLACE_OBJECT3 else 1)[0]


def add_party_list(tags, strip=None):
    """Places CoopParty0-2 (the tag sprite RemotePCTag0 shows) and, with the icon strip, CoopPartyIcon0-2 at the
    top level, above everything, in its first frame; nothing when they are there already."""
    first_frame = next(i for i, tag in enumerate(tags) if tag.code == SHOW_FRAME)
    places = [tag for tag in tags[:first_frame] if tag.code in (PLACE_OBJECT2, PLACE_OBJECT3)]
    names = {place_parts(tag)[1] for tag in places}
    if names >= set(PARTY_LIST):
        return
    tag_sprite = next((place_parts(tag)[0] for tag in places if place_parts(tag)[1] == PARTY_TAG), None)
    if tag_sprite is None:
        return  # a HUD without the remote players' tags: the overlay keeps drawing the list
    depth = max(place_depth(tag) for tag in places)
    added = []
    for name in PARTY_LIST:
        depth += 2
        # PlaceObject2: has a name (0x20), a matrix (0x04, the identity: one zero byte), a character (0x02).
        added.append(Tag(PLACE_OBJECT2, bytes([0x26]) + struct.pack('<HH', depth, tag_sprite) + b'\0' +
                         name.encode('latin-1') + b'\0', False))
    if strip is not None:
        added += party_icon_tags(depth, strip)
    tags[first_frame:first_frame] = added


def patch_fe(data, strip=None):
    """The game's HUD movie with the downed banner in place of the Draw banner and the party list (see the
    module's note); `strip` is the list's icons (party_hud_icons.bin), none without it."""
    header, tags = read_movie(data)
    defined = {struct.unpack_from('<H', tag.body)[0]: tag for tag in tags if tag.code in (SPRITE, EDIT_TEXT)}
    banner = None
    for tag in tags:
        if tag.code == SYMBOL_CLASS:
            count, = struct.unpack_from('<H', tag.body)
            offset = 2
            for _ in range(count):
                character, = struct.unpack_from('<H', tag.body, offset)
                end = tag.body.index(b'\0', offset + 2)
                if tag.body[offset + 2:end] == DRAW_BANNER.encode():
                    banner = character
                offset = end + 1
    if banner not in defined or defined[banner].code != SPRITE:
        raise ValueError(f'no {DRAW_BANNER} in this HUD')
    fields = []
    for layer in defined[banner].children:
        if layer.code not in (PLACE_OBJECT2, PLACE_OBJECT3):
            continue
        character, _, _ = place_parts(layer)
        holder = defined.get(character)
        if holder is None or holder.code != SPRITE:
            continue
        for place in holder.children:
            if place.code not in (PLACE_OBJECT2, PLACE_OBJECT3):
                continue
            field, name, _ = place_parts(place)
            if name in (DRAW_FIELD, DOWNED_FIELD) and field in defined and defined[field].code == EDIT_TEXT:
                fields.append((place, defined[field]))
    if not fields:
        raise ValueError(f'the Draw banner has no {DRAW_FIELD} text')
    for place, text in fields:
        if place_parts(place)[1] != DOWNED_FIELD:
            rename(place, DOWNED_FIELD)
        parts = edit_text_parts(text)
        if 'text' not in parts or 'color' not in parts:
            raise ValueError('the Draw banner\'s text field has no text or colour')
        if text.body[slice(*parts['color'])] == bytes.fromhex(DOWNED_COLOR) + b'\xff':
            continue  # done before
        start, end = parts['text']
        html = downed_html(text.body[start:end].decode('latin-1')).encode('latin-1')
        body = text.body[:start] + html + text.body[end:]
        c0, c1 = parts['color']
        text.body = body[:c0] + bytes.fromhex(DOWNED_COLOR) + b'\xff' + body[c1:]
    add_party_list(tags, strip)
    return write_movie((header, tags))


def hud_source(game, layers):
    """The fe.gfx the game would load with these layers: the last layer that has one, else the game's."""
    import mods
    found = Path(game) / FE
    for _name, root in layers:
        for relative, source in mods.mod_files(root):
            if relative.as_posix().casefold() == FE.casefold():
                found = source
    return found


def with_party_hud(game, layers, out):
    """(layers, notes): the layers with the party HUD over them, made from the HUD they would show (kept in
    out/party-hud/<its hash>, so the same HUD is patched once). A HUD that cannot be patched leaves the layers
    as they are and a note says why: nothing here may stop the game from starting."""
    source = hud_source(game, layers)
    try:
        data = source.read_bytes()
        strip = HUD_ICONS.read_bytes() if HUD_ICONS.is_file() else None
        digest = hashlib.sha256(data + (strip or b'')).hexdigest()[:16]
        folder = Path(out) / 'party-hud' / digest
        target = folder / FE
        if not target.is_file():
            patched = patch_fe(data, strip)
            target.parent.mkdir(parents=True, exist_ok=True)
            partial = target.with_suffix('.part')
            partial.write_bytes(patched)
            partial.replace(target)
    except (OSError, ValueError, IndexError, struct.error) as error:
        return layers, [f'The party HUD is off for this run: {source} ({error})']
    return [*layers, (LAYER, folder)], []
