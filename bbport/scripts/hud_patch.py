#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The party HUD: the game's own HUD movie (menu/fe.gfx) with party death's downed banner, made at every start
from the player's copy (or the mod that replaces it, such as Compact HUD) and put over the mods as the last
layer. Nothing of the game's is kept in the port: the movie is read and patched where it is.

The banner is the game's TextEffect_Draw (a PvP result, never shown in co-op; SetTextEffect id 17): its text
fields are renamed, so that the game no longer fills them from its text tables (fields named StaticText_<id>),
and get our words in the game's font, in yellow. The game's fade-in and blur trail stay as they are."""
import hashlib
from pathlib import Path
import struct

FE = 'dvdroot_ps4/menu/fe.gfx'
LAYER = 'party HUD'
DRAW_BANNER = 'Fe_fla._TextEffect_Draw_195'
DRAW_FIELD = 'StaticText_522007'
DOWNED_FIELD = 'CoopDownedText'
DOWNED_TEXT = 'YOU ARE DOWN'
DOWNED_COLOR = 'd8b03c'

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


def patch_fe(data):
    """The game's HUD movie with the downed banner in place of the Draw banner (see the module's note)."""
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
        digest = hashlib.sha256(data).hexdigest()[:16]
        folder = Path(out) / 'party-hud' / digest
        target = folder / FE
        if not target.is_file():
            patched = patch_fe(data)
            target.parent.mkdir(parents=True, exist_ok=True)
            partial = target.with_suffix('.part')
            partial.write_bytes(patched)
            partial.replace(target)
    except (OSError, ValueError, IndexError, struct.error) as error:
        return layers, [f'The party HUD is off for this run: {source} ({error})']
    return [*layers, (LAYER, folder)], []
