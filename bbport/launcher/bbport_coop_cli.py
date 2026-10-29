# SPDX-License-Identifier: GPL-2.0-or-later
"""Seamless co-op from the command line (Linux): what BLauncher.exe's co-op page does before PLAY,
without its window. The work is bbport_coop's (the same coop.json, game view, event files, player id
and game environment as on Windows); this file only asks the command line instead of the window.

  bbport_coop_cli.py [--dir DIR] [--host ADDRESS] [--game DIR] [--update DIR] [--harder N]
                     [--p2p-port N] [--rules rot,travel,vote,scale|off] [--unlocked-fps 0|1]
                     [--ini FILE | --no-sync-ini] [--prepare] [--dry-run] [--show] [-- run.sh arguments]

DIR (default $BB_COOP_DIR, else $XDG_DATA_HOME/bloodborne-coop) holds what the release's folder
holds on Windows: user/ (coop.json, saves, logs), coop-game/ (the game with the 1.09 update over it,
links to the player's own folders) and event-patch/ (the co-op event files made from that game).
Options given are saved in user/coop.json for the next start (but --unlocked-fps, which coop.json
does not keep).

The co-op folder has its own bbport.ini (run.sh reads $BB_DATA_DIR/bbport.ini). Every start copies the
launcher's settings (--ini, else $BB_CONFIG, else bbport/bbport.ini) into it: see sync_ini.
"""
import argparse
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bbport_coop  # noqa: E402

BBPORT = HERE.parent
REPO = BBPORT.parent
EVENT_TOOL = REPO / 'tools' / 're' / 'party_boss_events.py'
RULES = ('rot', 'travel', 'vote', 'scale')


def default_dir():
    if os.environ.get('BB_COOP_DIR'):
        return Path(os.environ['BB_COOP_DIR'])
    data = os.environ.get('XDG_DATA_HOME') or str(Path.home() / '.local' / 'share')
    return Path(data) / 'bloodborne-coop'


def arguments(argv):
    parser = argparse.ArgumentParser(description='Bloodborne seamless co-op (Linux, no launcher window)')
    parser.add_argument('--dir', type=Path, default=default_dir(), help='co-op folder (saves, logs, game view)')
    parser.add_argument('--host', help="the host's IP address, a server's name or a region's https:// directory")
    parser.add_argument('--game', help='the dumped game folder (CUSA03173, with eboot.bin)')
    parser.add_argument('--update', help='the dumped 1.09 update folder, when it is not beside the game')
    parser.add_argument('--harder', help="harder enemies (the launcher's choices), 'off' for none")
    parser.add_argument('--p2p-port', help="the game's P2P port, 'auto' for the default")
    parser.add_argument('--rules', help="host rules: a comma list of rot, travel, vote, scale, or 'off'")
    parser.add_argument('--unlocked-fps', choices=('0', '1'),
                        help='1: the experimental unlocked frame rate (this start only; BB_FPS sets the mode)')
    parser.add_argument('--ini', type=Path, help="the launcher's bbport.ini to copy into the co-op folder "
                        '(default: $BB_CONFIG, else bbport/bbport.ini)')
    parser.add_argument('--no-sync-ini', action='store_true', help="keep the co-op folder's own bbport.ini as it is")
    parser.add_argument('--prepare', action='store_true', help='make the game view and event files again')
    parser.add_argument('--dry-run', action='store_true', help='prepare and print the start, do not start')
    parser.add_argument('--show', action='store_true', help='print the saved settings and exit')
    parser.add_argument('run_args', nargs='*', help='arguments for run.sh (after --)')
    return parser.parse_args(argv)


def install_tools(install):
    """bbport_coop looks for the event patcher at <install>/tools/party_boss_events.py (the release's
    layout): the repository's own, linked, so a newer one makes the event files again (events_current)."""
    tools = install / 'tools'
    tools.mkdir(parents=True, exist_ok=True)
    link = tools / 'party_boss_events.py'
    if link.is_symlink() or link.exists():
        if link.resolve() == EVENT_TOOL.resolve():
            return
        link.unlink()
    link.symlink_to(EVENT_TOOL)


def launcher_ini():
    return Path(os.environ.get('BB_CONFIG') or BBPORT / 'bbport.ini')


def ini_keys(lines):
    return {line.split('=', 1)[0].strip() for line in lines if '=' in line and not line.lstrip().startswith('#')}


def sync_ini(source, target):
    """The launcher's graphics and control settings for the co-op game: `source` (the launcher's
    bbport.ini) over `target` (<co-op folder>/bbport.ini). The launcher's lines win, with its comments
    and order; keys only the target has (set in the game's menu) are kept after them. Returns whether
    the target changed; a missing source leaves it alone."""
    source, target = Path(source), Path(target)
    if not source.is_file() or (target.exists() and source.resolve() == target.resolve()):
        return False
    lines = source.read_text(encoding='utf-8').splitlines()
    try:
        own = target.read_text(encoding='utf-8').splitlines()
    except OSError:
        own = []
    seen = ini_keys(lines)
    kept = [line for line in own if '=' in line and not line.lstrip().startswith('#')
            and line.split('=', 1)[0].strip() not in seen]
    text = '\n'.join(lines + kept) + '\n'
    if target.exists() and target.read_text(encoding='utf-8') == text:
        return False
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding='utf-8')
    return True


def apply_options(settings, options):
    settings = dict(settings)
    for key, value in (('host_ip', options.host), ('game', options.game), ('update', options.update),
                       ('harder', options.harder), ('p2p_port', options.p2p_port)):
        if value is not None:
            settings[key] = value.strip()
    if options.harder is not None and options.harder != 'off' and not bbport_coop.valid_harder(options.harder):
        raise SystemExit(f'--harder {options.harder}: not one of the launcher\'s choices')
    if options.rules is not None:
        words = set() if options.rules.strip() == 'off' else {w.strip() for w in options.rules.split(',') if w.strip()}
        unknown = words - set(RULES)
        if unknown:
            raise SystemExit(f'--rules: unknown {", ".join(sorted(unknown))} (rot, travel, vote, scale or off)')
        for word in RULES:
            settings[f'rule_{word}'] = '1' if word in words else ''
    if options.unlocked_fps is not None:
        settings['unlocked_fps'] = options.unlocked_fps
    return settings


def server_settings(settings):
    """The host's address and this player's id and key on it, as the launcher's PLAY does."""
    if bbport_coop.region_url(settings['host_ip']):
        return bbport_coop.region_settings(settings)
    address = bbport_coop.resolve_host(settings['host_ip'])
    if not address:
        raise SystemExit(bbport_coop.UNKNOWN_SERVER_TEXT.format(settings['host_ip']))
    account, secret = bbport_coop.player_id_for(settings, settings['host_ip'])
    return dict(settings, host_ip=address, server=settings['host_ip'], account=account, password=secret,
                link_key=bbport_coop.link_key_for(settings, settings['host_ip']))


def main(argv=None):
    options = arguments(sys.argv[1:] if argv is None else argv)
    install = options.dir.expanduser().resolve()
    install.mkdir(parents=True, exist_ok=True)
    install_tools(install)
    settings = bbport_coop.load_settings(install)
    settings = apply_options(settings, options)
    bbport_coop.save_settings(install, settings)
    if options.show:
        shown = {key: value for key, value in settings.items() if key != 'password'}
        for key, value in sorted(shown.items()):
            print(f'{key} = {value}')
        return 0
    if not settings.get('host_ip'):
        raise SystemExit("No host: --host <the host's IP address or server name> (saved for next time)")
    if not settings.get('game'):
        raise SystemExit('No game: --game <the dumped CUSA03173 folder> (saved for next time)')

    if not options.no_sync_ini and sync_ini(options.ini or launcher_ini(), install / 'bbport.ini'):
        print(f'Synced the launcher settings into {install / "bbport.ini"}')

    root = bbport_coop.view_root(install)
    if options.prepare or bbport_coop.needs_prepare(install, settings):
        try:
            root, note = bbport_coop.prepare(install, settings)
        except RuntimeError as error:
            raise SystemExit(str(error))
        if note:
            print(f'Note: {note}')
        settings['prepared'] = bbport_coop.prepared_key(settings)
        bbport_coop.save_settings(install, settings)
    else:
        bbport_coop.copy_events(install, root)

    playing = server_settings(settings)
    user = install / 'user'
    (user / 'log').mkdir(parents=True, exist_ok=True)
    overrides = user / 'host_overrides.custom.json'
    overrides.write_text(bbport_coop.host_overrides(playing['host_ip']), encoding='utf-8')
    env = bbport_coop.game_environment(os.environ, playing, install, root)
    if not options.no_sync_ini:
        # run.sh reads $BB_CONFIG before $BB_DATA_DIR/bbport.ini: the co-op folder's, never the launcher's.
        env['BB_CONFIG'] = str(install / 'bbport.ini')
    # run.py points the game at this file (online_overrides); run.sh leaves it to the caller.
    env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'] = str(overrides)
    command = ['bash', str(BBPORT / 'run.sh'), *options.run_args]
    print(f'Co-op: {playing.get("server", playing["host_ip"])} ({playing["host_ip"]}), '
          f'player id {playing["account"]}, game {env["BB_GAME_DIR"]}')
    if options.dry_run:
        for key in sorted(set(env) - set(os.environ) | {k for k in env if env[k] != os.environ.get(k)}):
            value = '<secret>' if key in ('BB_SHADNET_PASSWORD', 'BB_SHADNET_SERVER_KEY') else env[key]
            print(f'  {key}={value}')
        print('  ' + ' '.join(command))
        return 0
    os.execvpe(command[0], command, env)


if __name__ == '__main__':
    sys.exit(main())
