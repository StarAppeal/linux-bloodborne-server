#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The button icons of one start (launcher/bbport_icons.py draws them): which set the game shows, and the
generated layer run.py puts under the mods. The launcher's Play asks here for its start; run.py asks itself
when the launcher did not (the kit's PLAY.bat runs BLauncher --run) and after a restart from the game's
menu, where the icon style may have changed (Kaniel 2026-10-11)."""
import subprocess
import sys
from pathlib import Path

PORT = Path(__file__).resolve().parent.parent
for _folder in (PORT / 'launcher', PORT / 'scripts'):
    if str(_folder) not in sys.path:
        sys.path.append(str(_folder))


def read_ini(path):
    """{key: value} of bbport.ini (comments and lines without '=' skipped; a BOM from Notepad dropped)."""
    values = {}
    try:
        text = Path(path).read_text(encoding='utf-8-sig', errors='replace')
    except OSError:
        return values
    for line in text.splitlines():
        key, eq, value = line.partition('=')
        if eq and key.strip() and not key.lstrip().startswith('#'):
            values[key.strip()] = value.strip()
    return values


def list_gamepads(tool):
    """[(GUID, name)] of the connected gamepads (`bb-gpu-capabilities --gamepads`), None when it cannot run."""
    try:
        result = subprocess.run([str(tool), '--gamepads'], capture_output=True, text=True, encoding='utf-8',
                                errors='replace', timeout=30, stdin=subprocess.DEVNULL,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode:
        return None
    pads = []
    for line in result.stdout.splitlines():
        guid, tab, name = line.partition('\t')
        if tab and guid.strip() and name.strip():
            pads.append((guid.strip(), name.strip()))
    return pads


def icon_set(ini, gamepad='', gamepads=None):
    """'playstation', 'xbox' or 'keyboard': the button icons of this start (as environment() picks them), for the
    overlay's glyphs. gamepads: as for environment(), asked only for the automatic choice."""
    try:
        import bbport_controls
    except ImportError:
        return 'playstation'
    choice = ini.get('button_icons', bbport_controls.ICON_DEFAULT)
    if choice not in [value for value, _text in bbport_controls.ICON_CHOICES]:
        choice = bbport_controls.ICON_DEFAULT
    pads = gamepads() if choice == 'auto' and gamepads else None
    return bbport_controls.resolve_icon_set(choice, pads, str(gamepad).strip())


def environment(game, out_dir, ini, mods_dir, mods_config, mods_enabled, gamepad='', gamepads=None):
    """The environment entries of the icons: BB_ICONS_DIR (the generated common.tpf.dcx's folder) and
    BB_ICONS_NOTE (a line for the log). {} when the game keeps its own icons (PlayStation, or no game file to
    redraw). ini: bbport.ini's values (button_icons, and the key.* bindings the keyboard glyphs show);
    gamepads: a function returning the connected gamepads, asked only for the automatic choice; gamepad: the
    one the player chose (BB_GAMEPAD), '' the first connected. Nothing here may stop the game from starting."""
    try:
        import bbport_controls
    except ImportError as error:  # a staged build without the launcher's modules
        return {'BB_ICONS_NOTE': f'Button icons are off for this run: {error}'}
    choice = ini.get('button_icons', bbport_controls.ICON_DEFAULT)
    if choice not in [value for value, _text in bbport_controls.ICON_CHOICES]:
        choice = bbport_controls.ICON_DEFAULT
    game = Path(game)
    if choice == 'playstation' or not (game / 'dvdroot_ps4' / 'menu' / 'common.tpf.dcx').is_file():
        return {}
    pads = gamepads() if choice == 'auto' and gamepads else []
    icon_set = bbport_controls.resolve_icon_set(choice, pads, str(gamepad).strip())
    if icon_set == 'playstation':
        return {}
    try:
        import bbport_icons
        import mods
    except ImportError as error:  # a source tree without Pillow
        return {'BB_ICONS_NOTE': f'Button icons are off for this run: {error}'}
    try:
        note = mods.icons_note(mods.user_layers(game, mods_dir, mods_config, mods_enabled))
        if note:
            return {'BB_ICONS_NOTE': note}
        folder, built, kept = bbport_icons.ensure_layer(game, Path(out_dir), icon_set, ini)
    except Exception as error:  # nothing about the icons may stop the game from starting
        return {'BB_ICONS_NOTE': f'Button icons are off for this run: {error}'}
    note = f'Button icons: {icon_set} ({"made" if built else "from the cache"})'
    if kept:
        note += f'; {kept} baked buttons kept their PlayStation art'
    return {'BB_ICONS_DIR': str(folder), 'BB_ICONS_NOTE': note}
