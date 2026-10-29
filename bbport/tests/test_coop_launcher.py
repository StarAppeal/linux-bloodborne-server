"""The co-op release inside the launcher (launcher/bbport_coop.py): what the release's bat files and
tools\\setup.ps1 did (Kaniel 2026-10-13: one setup .exe, no bat files, the launcher finds the game)."""
import json
import os
import struct
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))

import bbport_coop as coop  # noqa: E402


def write_sfo(path, values):
    """A minimal PS4 param.sfo with text values (format 0x0204)."""
    keys, data, entries = b'', b'', []
    for key, value in values.items():
        raw = value.encode() + b'\0'
        entries.append((len(keys), 0x0204, len(raw), len(raw), len(data)))
        keys += key.encode() + b'\0'
        data += raw
    keys_at = 20 + 16 * len(entries)
    data_at = keys_at + len(keys)
    head = b'\0PSF' + struct.pack('<IIII', 0x101, keys_at, data_at, len(entries))
    body = b''.join(struct.pack('<HHIII', *entry) for entry in entries)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(head + body + keys + data)


def make_game(folder, version='01.09', title='CUSA03173'):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'eboot.bin').write_bytes(b'eboot ' + version.encode())
    write_sfo(folder / 'sce_sys' / 'param.sfo', {'TITLE_ID': title, 'APP_VER': version})
    events = folder / 'dvdroot_ps4' / 'event'
    events.mkdir(parents=True, exist_ok=True)
    (events / 'common.emevd.dcx').write_bytes(b'events ' + version.encode())
    return folder


class AnswersTests(unittest.TestCase):
    def test_the_fields_take_what_setup_took(self):
        self.assertTrue(coop.valid_ip('26.12.34.56'))
        self.assertFalse(coop.valid_ip('127.0.0.1'), 'the guests cannot reach loopback')
        self.assertFalse(coop.valid_ip('300.1.1.1'))
        self.assertFalse(coop.valid_ip('host'))
        self.assertTrue(coop.valid_name('Lisara'))
        self.assertFalse(coop.valid_name('ab'))
        self.assertFalse(coop.valid_name('with space'))
        self.assertTrue(coop.valid_secret('pass_1'))
        self.assertFalse(coop.valid_secret(''))
        self.assertTrue(coop.valid_harder('off'))
        self.assertTrue(coop.valid_harder('3,1.5,3'))
        self.assertFalse(coop.valid_harder('3,1.5'))

    def test_players_set_their_own_enemy_multipliers(self):
        """Kaniel 2026-10-21: the presets were too specific; a custom setting takes any multipliers the game
        takes (1 to 10 each, as ParseHarderEnemies), and all 1 is the normal game."""
        self.assertEqual(coop.harder_value('2', '1.25', '4'), '2,1.25,4')
        self.assertEqual(coop.harder_value(1, 1, 1), 'off')
        self.assertEqual(coop.harder_value('25', '0.5', 'x'), '10,1,1', 'held to what the game takes')
        self.assertEqual(coop.harder_value(1.333, 2, 2), '1.33,2,2')
        self.assertEqual(coop.harder_value('1,5', ' 2 ', '2,25'), '1.5,2,2.25', 'a decimal comma is a point')
        for preset in (coop.HARDER_BIT, coop.HARDER_ON, coop.HARDER_MUCH):
            self.assertTrue(coop.valid_harder(preset), preset)
        self.assertFalse(coop.valid_harder('11,1,1'), 'the game drops a multiplier above 10')
        self.assertFalse(coop.valid_harder('0.5,1,1'), 'and one below 1')
        self.assertFalse(coop.valid_harder('1,1,1'))
        env = coop.game_environment({}, dict(coop.DEFAULTS, harder='2,1.25,4'), Path('.'), Path('.'))
        self.assertEqual(env['SHADPS4_BLOODBORNE_HARDER_ENEMIES'], '2,1.25,4')

    def test_host_rules_are_off_until_the_host_turns_them_on(self):
        """Kaniel 2026-10-25: the host rules (Rot, travel together with its vote, enemies scaling with
        the team), each off by default; the leader's launcher's are the team's (the game sends them)."""
        env = coop.game_environment({'SHADPS4_BLOODBORNE_HOST_RULES': 'rot'}, dict(coop.DEFAULTS),
                                    Path('.'), Path('.'))
        self.assertNotIn('SHADPS4_BLOODBORNE_HOST_RULES', env, 'off by default, whatever the env had')
        self.assertEqual(coop.host_rules_value(coop.DEFAULTS), 'off')
        on = dict(coop.DEFAULTS, rule_rot='1', rule_travel='1', rule_vote='1', rule_scale='1')
        self.assertEqual(coop.host_rules_value(on), 'rot,travel,vote,scale')
        env = coop.game_environment({}, on, Path('.'), Path('.'))
        self.assertEqual(env['SHADPS4_BLOODBORNE_HOST_RULES'], 'rot,travel,vote,scale')
        self.assertEqual(coop.host_rules_value(dict(coop.DEFAULTS, rule_vote='1')), 'off',
                         'the vote is a part of travelling together')
        self.assertEqual(coop.host_rules_value(dict(coop.DEFAULTS, rule_scale='1')), 'scale')
        with tempfile.TemporaryDirectory() as folder:
            coop.save_settings(folder, dict(coop.DEFAULTS, rule_travel='1'))
            kept = coop.read_settings(folder)
            self.assertEqual((kept['rule_travel'], kept['rule_rot']), ('1', ''))

    def test_missing_answers_are_named(self):
        # No party settings: teams are made in the game (phase 6), so no party name or password.
        self.assertNotIn('party_name', coop.DEFAULTS)
        self.assertNotIn('party_secret', coop.DEFAULTS)
        # Phase 11 (Kaniel 2026-10-15): no account name or password to type; the player id is made.
        settings = dict(coop.DEFAULTS, game='', role='guest', host_ip='', account='', password='')
        self.assertEqual(coop.missing(settings), ['game', 'host_ip'])
        settings.update(game='G', host_ip='26.1.2.3')
        self.assertEqual(coop.missing(settings), [])

    def test_a_player_id_is_made_once_and_kept(self):
        settings = coop.with_player_id(dict(coop.DEFAULTS))
        self.assertTrue(coop.valid_name(settings['account']), settings['account'])
        self.assertTrue(coop.valid_secret(settings['password']))
        self.assertGreaterEqual(len(settings['password']), 32, 'the id is the secret: long and random')
        self.assertEqual(coop.with_player_id(dict(settings)), settings, 'made once, then kept')
        other = coop.with_player_id(dict(coop.DEFAULTS))
        self.assertNotEqual(other['account'], settings['account'])
        # A player who had an account keeps it, and with it its teams and worlds.
        old = coop.with_player_id(dict(coop.DEFAULTS, account='kaniel', password='pw'))
        self.assertEqual((old['account'], old['password']), ('kaniel', 'pw'))
        with tempfile.TemporaryDirectory() as folder:
            install = Path(folder)
            made = coop.load_settings(install)
            self.assertTrue(coop.valid_name(made['account']), 'a new install has its id at once')
            self.assertEqual(coop.load_settings(install)['account'], made['account'], 'saved')

    def test_an_id_whose_address_is_taken_is_made_again(self):
        # docs/HOSTED_SERVER.md: a server refuses a new id whose virtual address (the 24-bit hash the
        # games compute, net_link.h VirtualAddressOf) another account has; its refusal starts with
        # 'address taken:', and the game's log says it (p2p_sockets.cpp OnWelcome, on stderr: console.txt).
        self.assertEqual(coop.virtual_address('Kaniel'), '10.137.200.5')
        self.assertEqual(coop.virtual_address('Ada'), '10.195.165.91')
        taken = ("GPU [Lib.Net] <Error> p2p_sockets.cpp:856 OnWelcome: Server link: the server refused this game: "
                 "address taken: another account's game has this id's virtual address; the launcher needs a new id\n")
        build = 'GPU [Lib.Net] <Error> p2p_sockets.cpp:856 OnWelcome: Server link: the server refused this game: ' \
                'this server runs build abc\n'
        self.assertTrue(coop.address_taken(taken))
        self.assertTrue(coop.address_taken('[Lib.Net] linking\n' + build + taken + '[Render] frame\n'),
                        'the launcher reads the log several lines at a time')
        self.assertFalse(coop.address_taken(build), 'another refusal: a new id would not help')
        self.assertFalse(coop.address_taken('address taken: a line the server did not send\n'))
        self.assertEqual([coop.id_retry(taken, tries) for tries in range(coop.NEW_ID_TRIES + 1)],
                         ['retry'] * coop.NEW_ID_TRIES + ['give_up'])
        self.assertIsNone(coop.id_retry(build, 0))
        self.assertIsNone(coop.id_retry('[Lib.Net] Server link: the server took this game\'s hello\n', 0))
        # The new id is made as the first was ('p' and 14 hex digits, a 32-digit secret), never at the
        # address just refused; the other answers stay.
        settings = dict(coop.DEFAULTS, account='p0123456789abcd', password='a' * 32, host_ip='play.example.com')
        with mock.patch.object(coop.secrets, 'token_hex', side_effect=['0123456789abcd', 'ffeeddccbbaa99', 'b' * 32]):
            fresh = coop.with_new_player_id(settings)
        self.assertEqual((fresh['account'], fresh['password']), ('pffeeddccbbaa99', 'b' * 32))
        self.assertEqual(settings['account'], 'p0123456789abcd', 'a copy: the old answers are not changed')
        self.assertEqual(fresh['host_ip'], 'play.example.com')
        fresh = coop.with_new_player_id(settings)
        self.assertTrue(coop.valid_name(fresh['account']) and coop.valid_secret(fresh['password']))
        self.assertGreaterEqual(len(fresh['password']), 32)
        self.assertNotEqual(coop.virtual_address(fresh['account']), coop.virtual_address(settings['account']))

    def test_a_new_id_is_only_for_the_server_that_refused_the_old_one(self):
        # A public server refusing the id's address must not change the id the player's friends' home
        # host knows (their teams and worlds there): the new id is kept for that server only.
        settings = dict(coop.DEFAULTS, account='p0123456789abcd', password='a' * 32, host_ip='26.1.2.3')
        self.assertEqual(coop.player_id_for(settings, 'Play.Example.com'), ('p0123456789abcd', 'a' * 32),
                         'no override: the one id')
        made = coop.with_server_id(settings, ' Play.Example.com ')
        self.assertEqual((made['account'], made['password']), ('p0123456789abcd', 'a' * 32), 'the default id stays')
        self.assertNotIn('server_ids', settings, 'a copy')
        account, secret = coop.player_id_for(made, 'play.example.com')
        self.assertNotEqual(account, 'p0123456789abcd')
        self.assertTrue(coop.valid_name(account) and coop.valid_secret(secret) and len(secret) >= 32)
        self.assertNotEqual(coop.virtual_address(account), coop.virtual_address('p0123456789abcd'))
        self.assertEqual(coop.player_id_for(made, 'PLAY.example.com'), (account, secret), 'the name as typed, any case')
        self.assertEqual(coop.player_id_for(made, '26.1.2.3'), ('p0123456789abcd', 'a' * 32), 'other hosts: the one id')
        self.assertEqual(coop.player_id_for(made, 'other.example.com'), ('p0123456789abcd', 'a' * 32))
        again = coop.with_server_id(made, 'play.example.com')
        self.assertNotEqual(coop.player_id_for(again, 'play.example.com')[0], account, 'refused again: another')
        self.assertNotEqual(coop.virtual_address(coop.player_id_for(again, 'play.example.com')[0]),
                            coop.virtual_address(account))
        self.assertEqual(coop.player_id_for(made, 'play.example.com'), (account, secret), 'the copy is not changed')
        with tempfile.TemporaryDirectory() as folder:
            install = Path(folder)
            # An old coop.json (no overrides) reads as before and is written back the same.
            old = {key: '' for key in coop.DEFAULTS}
            old.update(game='G', host_ip='26.1.2.3', account='kaniel', password='pw', role='guest',
                       p2p_port='auto', harder='off')
            coop.settings_path(install).parent.mkdir(parents=True)
            coop.settings_path(install).write_text(json.dumps(old, indent=2) + '\n', encoding='utf-8')
            loaded = coop.load_settings(install)
            self.assertEqual((loaded['account'], loaded['password']), ('kaniel', 'pw'))
            self.assertEqual(coop.player_id_for(loaded, 'play.example.com'), ('kaniel', 'pw'))
            coop.save_settings(install, loaded)
            saved = json.loads(coop.settings_path(install).read_text(encoding='utf-8'))
            # but for this PC, noted at load (with_own_player_id)
            self.assertEqual(dict(saved, machine=''), old)
            # The override is saved and read back; a broken one is ignored.
            coop.save_settings(install, coop.with_server_id(loaded, 'play.example.com'))
            back = coop.load_settings(install)
            self.assertEqual(back['account'], 'kaniel')
            self.assertNotEqual(coop.player_id_for(back, 'play.example.com')[0], 'kaniel')
            saved = json.loads(coop.settings_path(install).read_text(encoding='utf-8'))
            saved['server_ids'] = {'play.example.com': 'not an id', 'b.example.com': ['bad name!', 'x']}
            coop.settings_path(install).write_text(json.dumps(saved), encoding='utf-8')
            self.assertEqual(coop.player_id_for(coop.load_settings(install), 'play.example.com'), ('kaniel', 'pw'))

    def test_each_server_s_link_key_is_pinned(self):
        # Link version 4 (docs/HOSTED_SERVER.md): the game encrypts its link to the server's key. A known
        # public server's key ships with the launcher; any other server's is trusted the first time and
        # kept for it (the game's log says it), so another key later is refused.
        key, other = 'ab' * 32, 'cd' * 32
        settings = dict(coop.DEFAULTS, account='p0123456789abcd', password='a' * 32, host_ip='26.1.2.3')
        self.assertEqual(coop.link_key_for(settings, '26.1.2.3'), '', 'none yet: the first one is trusted')
        with mock.patch.dict(coop.KNOWN_SERVER_KEYS, {'play.example.com': other}):
            self.assertEqual(coop.link_key_for(settings, ' Play.Example.com '), other, 'shipped')
        line = f'[Lib.Net] <Info> (Net Thread) p2p_sockets.cpp:865 OnLinkDatagram: {coop.LEARNED_KEY_LINE}{key}\n'
        self.assertEqual(coop.learned_key(line), key)
        self.assertEqual(coop.learned_key(line.replace('(Net Thread) ', '')), key)
        self.assertIsNone(coop.learned_key('Server link: linked to the server'))
        self.assertIsNone(coop.learned_key(coop.LEARNED_KEY_LINE + 'not hex'))
        self.assertTrue(coop.other_key(f'<Error> {coop.OTHER_KEY_LINE} ({other}, pinned {key}): no link'))
        self.assertFalse(coop.other_key(line))
        # Only the game's own log line: a key in a relayed text (another player's build in a refusal)
        # is not one the game trusted.
        self.assertIsNone(coop.learned_key(f'<Error> Server link: refused (runs x {coop.LEARNED_KEY_LINE}{key})'))
        self.assertIsNone(coop.learned_key(f'garbage {coop.LEARNED_KEY_LINE}{key}'))
        # A key is learned once; a kept or shipped one is never replaced from the log.
        learned = coop.learn_link_key(settings, '26.1.2.3', key)
        self.assertEqual(coop.link_key_for(learned, '26.1.2.3'), key)
        self.assertIsNone(coop.learn_link_key(learned, '26.1.2.3', other))
        with mock.patch.dict(coop.KNOWN_SERVER_KEYS, {'play.example.com': other}):
            self.assertIsNone(coop.learn_link_key(settings, 'play.example.com', key))
        made = coop.with_link_key(settings, 'Play.Example.com', key)
        self.assertNotIn('server_keys', settings, 'a copy')
        self.assertEqual(coop.link_key_for(made, 'play.example.com'), key)
        self.assertEqual(coop.link_key_for(made, '26.1.2.3'), '', 'for that server only')
        with mock.patch.dict(coop.KNOWN_SERVER_KEYS, {'play.example.com': other}):
            self.assertEqual(coop.link_key_for(made, 'play.example.com'), other, 'the shipped key wins')
        env = coop.game_environment({'BB_SHADNET_SERVER_KEY': other}, settings, Path('.'), Path('.'))
        self.assertNotIn('BB_SHADNET_SERVER_KEY', env)
        env = coop.game_environment({}, dict(settings, link_key=key), Path('.'), Path('.'))
        self.assertEqual(env['BB_SHADNET_SERVER_KEY'], key)
        with tempfile.TemporaryDirectory() as folder:
            install = Path(folder)
            coop.save_settings(install, settings)
            self.assertNotIn('server_keys', json.loads(coop.settings_path(install).read_text(encoding='utf-8')))
            coop.save_settings(install, made)
            self.assertEqual(coop.link_key_for(coop.load_settings(install), 'play.example.com'), key)
            saved = json.loads(coop.settings_path(install).read_text(encoding='utf-8'))
            saved['server_keys'] = {'play.example.com': 'short', 'b.example.com': 7}
            coop.settings_path(install).write_text(json.dumps(saved), encoding='utf-8')
            self.assertEqual(coop.load_settings(install)['server_keys'], {}, 'broken keys are ignored')

    def test_a_public_server_is_typed_by_its_name_or_address(self):
        # Play on a public server (docs/HOSTED_SERVER.md): Host's IP address also takes a server's name.
        for good in ('play.example.com', 'yharnam-together.net', 'eu-1.Server.example.org', '26.1.2.3', '203.0.113.7'):
            self.assertTrue(coop.valid_host(good), good)
        for bad in ('', 'localhost', '127.0.0.1', '1.2.3', '300.1.1.1', 'http://play.example.com',
                    'play.example.com:31314', 'with space.com', '-bad.example.com', 'bad-.example.com',
                    'play..example.com', 'a' * 64 + '.com', 'x.' * 127 + 'com'):
            self.assertFalse(coop.valid_host(bad), bad)
        settings = dict(coop.DEFAULTS, game='G', host_ip='play.example.com')
        self.assertEqual(coop.missing(settings), [])

        def nobody(_name):
            raise OSError('not found')
        # An address is used as it is; a name is looked up when PLAY starts (the link and the HTTP ports
        # then get one address, as with a host's IP).
        self.assertEqual(coop.resolve_host('26.1.2.3', lookup=nobody), '26.1.2.3')
        self.assertEqual(coop.resolve_host(' play.example.com ', lookup=lambda name: {'play.example.com': '203.0.113.7'}[name]),
                         '203.0.113.7')
        self.assertEqual(coop.resolve_host('play.example.com', lookup=nobody), '')
        self.assertEqual(coop.resolve_host('play.example.com', lookup=lambda _n: '127.0.0.1'), '', 'not this PC')
        self.assertEqual(coop.resolve_host('localhost', lookup=lambda _n: '203.0.113.7'), '', 'not a valid answer')
        self.assertIn('{}', coop.UNKNOWN_SERVER_TEXT)

    def test_a_region_is_typed_by_its_directory(self):
        # A region of shards (docs/SCALING.md): Host's IP address may be its directory, over HTTPS.
        for good in ('https://eu.example.com', 'https://eu.example.com/', 'https://eu.example.com:8443/region'):
            self.assertTrue(coop.region_url(good), good)
            self.assertTrue(coop.valid_host(good), good)
        for bad in ('http://eu.example.com', 'https://', 'https://bad host.com', 'eu.example.com', 'ftp://x.com',
                    'https://eu.example.com:99999', 'https://eu.example.com?x=1'):
            self.assertEqual(coop.region_url(bad), '', bad)
        self.assertEqual(coop.region_url(' https://EU.example.com/ '), 'https://eu.example.com')
        self.assertEqual(coop.missing(dict(coop.DEFAULTS, game='G', host_ip='https://eu.example.com')), [])

    def test_the_launcher_starts_on_the_players_shard_or_the_least_loaded(self):
        key = 'ab' * 32
        shards = [{'name': 'eu-1', 'address': '203.0.113.1:31314', 'key': key, 'players': 700, 'capacity': 800,
                   'draining': False},
                  {'name': 'eu-2', 'address': '203.0.113.2:31314', 'key': key, 'players': 100, 'capacity': 800,
                   'draining': False},
                  {'name': 'eu-3', 'address': '203.0.113.3:40000', 'key': key, 'players': 0, 'capacity': 800,
                   'draining': True},
                  {'name': 'bad', 'address': 'nowhere', 'key': key, 'players': 0, 'capacity': 800}]
        self.assertEqual(coop.pick_shard(shards)['name'], 'eu-2')
        self.assertEqual(coop.pick_shard(shards, shards[0])['name'], 'eu-1', 'where the player is, or played')
        self.assertEqual(coop.pick_shard(shards, shards[2])['name'], 'eu-2', 'not a draining one')
        self.assertEqual(coop.pick_shard(shards, dict(shards[0], players=800))['name'], 'eu-2', 'not a full one')
        self.assertIsNone(coop.pick_shard([shards[2], shards[3]]))
        asked = []

        def fetch(url):
            asked.append(url)
            if '/v1/where' in url:
                if 'player=Ada' in url:
                    return {'player': 'Ada', 'online': False, 'shard': shards[0]}
                return None  # 404: nowhere yet
            return {'shards': shards}
        self.assertEqual(coop.resolve_region('https://eu.example.com', 'Ada', fetch=fetch),
                         ('203.0.113.1', 31314, key, 'eu-1'))
        self.assertEqual(asked, ['https://eu.example.com/v1/where?player=Ada', 'https://eu.example.com/v1/shards'])
        self.assertEqual(coop.resolve_region('https://eu.example.com', 'Bob', fetch=fetch)[:2], ('203.0.113.2', 31314))
        with self.assertRaises(RuntimeError):
            coop.resolve_region('https://eu.example.com', 'Bob', fetch=lambda url: {'shards': [shards[2]]})

        def down(url):
            raise OSError('no route')
        with self.assertRaises(RuntimeError):
            coop.resolve_region('https://eu.example.com', 'Bob', fetch=down)
        # The game links to the shard's port; the region's key ships with the launcher and wins over the
        # directory's (the directory can only name an address, never stand in for the region).
        settings = dict(coop.DEFAULTS, account='Ada', password='a' * 32, host_ip='https://eu.example.com')
        other = 'cd' * 32
        made = coop.region_settings(settings, fetch=fetch)
        self.assertEqual((made['host_ip'], made['link_port'], made['link_key'], made['server']),
                         ('203.0.113.1', '31314', key, 'https://eu.example.com'))
        with mock.patch.dict(coop.KNOWN_SERVER_KEYS, {'https://eu.example.com': other}):
            self.assertEqual(coop.region_settings(settings, fetch=fetch)['link_key'], other)
        env = coop.game_environment({}, dict(made, link_key=key), Path('.'), Path('.'))
        self.assertEqual(env['BB_SHADNET_SERVER'], '203.0.113.1:31314')
        self.assertEqual(env['BB_SHADNET_WEBAPI'], 'http://203.0.113.1:31315')
        env = coop.game_environment({}, dict(made, link_port='40000'), Path('.'), Path('.'))
        self.assertEqual(env['BB_SHADNET_SERVER'], '203.0.113.1:40000')
        env = coop.game_environment({}, dict(coop.DEFAULTS, host_ip='26.1.2.3', account='a', password='b'), Path('.'),
                                    Path('.'))
        self.assertEqual(env['BB_SHADNET_SERVER'], f'26.1.2.3:{coop.MATCHING_PORT}', 'a home host as before')

    def test_a_copied_install_makes_its_own_player_id(self):
        # Player report 2026-10-24: a friend's install copied with its user\\coop.json gave two PCs one
        # id; their games took the server's link from each other and a new team left at once.
        made = coop.with_own_player_id(dict(coop.DEFAULTS), 'pc-a')
        self.assertEqual(made['machine'], 'pc-a')
        self.assertEqual(coop.with_own_player_id(dict(made), 'pc-a'), made, 'kept on its own PC')
        copied = coop.with_own_player_id(dict(made, host_ip='26.1.2.3'), 'pc-b')
        self.assertNotEqual(copied['account'], made['account'])
        self.assertNotEqual(copied['password'], made['password'])
        self.assertEqual((copied['machine'], copied['host_ip']), ('pc-b', '26.1.2.3'), 'other answers stay')
        # An install from before the PC was noted keeps its id (its teams and worlds) and notes the PC.
        old = coop.with_own_player_id(dict(coop.DEFAULTS, account='kaniel', password='pw'), 'pc-a')
        self.assertEqual((old['account'], old['password'], old['machine']), ('kaniel', 'pw', 'pc-a'))
        # No fingerprint (it could not be read): nothing changes.
        self.assertEqual(coop.with_own_player_id(dict(made), ''), made)
        with tempfile.TemporaryDirectory() as folder:
            install = Path(folder)
            coop.save_settings(install, dict(made, machine='another-pc'))
            loaded = coop.load_settings(install)
            if coop.this_machine():
                self.assertNotEqual(loaded['account'], made['account'])
                self.assertEqual(coop.load_settings(install)['account'], loaded['account'], 'saved')

    def test_the_old_settings_bat_carries_over(self):
        with tempfile.TemporaryDirectory() as folder:
            bat = Path(folder) / 'settings.bat'
            bat.write_text('@rem Written by 1 - SETUP.bat\nset "GAME=D:\\Games\\CUSA03173"\nset "UPDATE=none"\n'
                           'set "ROLE=host"\nset "HOSTIP=26.1.2.3"\nset "NPID=kaniel"\nset "PASS=pw"\n'
                           'set "PARTY=hunters:secret"\nset "P2PPORT=auto"\nset "FPS=60"\nset "HARDER=3,1.5,3"\n',
                           encoding='ascii')
            settings = coop.from_settings_bat(bat)
        self.assertEqual(settings['game'], 'D:\\Games\\CUSA03173')
        self.assertEqual(settings['update'], '')
        self.assertEqual((settings['role'], settings['host_ip'], settings['account'], settings['password']),
                         ('host', '26.1.2.3', 'kaniel', 'pw'))
        self.assertNotIn('party_secret', settings)
        self.assertEqual(settings['harder'], '3,1.5,3')

    def test_settings_are_kept_in_the_user_folder(self):
        with tempfile.TemporaryDirectory() as folder:
            install = Path(folder)
            self.assertEqual(dict(coop.load_settings(install), account='', password='', machine=''), coop.DEFAULTS)
            coop.save_settings(install, dict(coop.DEFAULTS, account='Lisara', password='pw'))
            self.assertEqual(json.loads((install / 'user' / 'coop.json').read_text('utf-8'))['account'], 'Lisara')
            self.assertEqual(coop.load_settings(install)['account'], 'Lisara')


class FindGameTests(unittest.TestCase):
    def test_a_game_folder_is_found_inside_the_one_picked(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            game = make_game(root / 'Bloodborne' / 'CUSA03173', version='01.00')
            make_game(root / 'Bloodborne' / 'CUSA03173-patch')
            self.assertEqual(coop.find_game_folder(root), game, 'not the update next to it')
            self.assertEqual(coop.find_game_folder(game), game)
            self.assertIsNone(coop.find_game_folder(root / 'nothing'))
            self.assertEqual(coop.update_folder(game), root / 'Bloodborne' / 'CUSA03173-patch')
            self.assertEqual(coop.game_version(game), ('CUSA03173', '01.00'))

    def test_the_search_looks_near_the_install_and_on_the_drives(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            install = root / 'Programs' / 'Bloodborne Seamless Co-op'
            install.mkdir(parents=True)
            game = make_game(root / 'drive' / 'Games' / 'PS4' / 'CUSA03173')
            (root / 'drive' / 'Windows' / 'System32').mkdir(parents=True)
            found = coop.search_game(install, drives=[root / 'drive'], homes=[])
            self.assertEqual(found, game)
            self.assertIsNone(coop.search_game(install, drives=[root / 'empty'], homes=[]))

    def test_an_install_in_games_finds_the_game_beside_it_but_never_walks_the_drive(self):
        # the setup's default folder is D:\\Games\\Yharnam Together (Kaniel 2026-10-20): its folder's games
        # are looked at first, the drive root is left to the timed search
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            install = root / 'Games' / 'Yharnam Together'
            install.mkdir(parents=True)
            game = make_game(root / 'Games' / 'CUSA03173')
            self.assertEqual(coop.search_game(install, drives=[], homes=[]), game)
        from unittest import mock
        walked = []
        with mock.patch.object(coop, 'find_game_folder', lambda f, depth=5: walked.append(Path(f))), \
                mock.patch.object(Path, 'is_dir', lambda self: True):
            coop.search_game(Path('/Games/Yharnam Together'), drives=[], homes=[], budget_s=0)
        self.assertIn(Path('/Games'), walked)
        self.assertNotIn(Path('/'), walked)

    def test_another_game_is_not_taken_for_bloodborne(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            make_game(root / 'drive' / 'CUSA00001', title='CUSA00001')
            self.assertIsNone(coop.search_game(root / 'install', drives=[root / 'drive'], homes=[]))


# Folder links: junctions on Windows, symbolic links elsewhere (make_junction).
class GameViewTests(unittest.TestCase):
    def test_the_game_view_stays_beside_the_install_where_links_work(self):
        # Linux took the folder under the home: the probe link was removed with rmdir, which a
        # symbolic link refuses, so links counted as not working.
        with tempfile.TemporaryDirectory() as folder:
            install = Path(folder)
            self.assertTrue(coop.junctions_work(install / 'coop-game'))
            self.assertEqual(coop.view_root(install), install / 'coop-game')
            self.assertEqual(list((install / 'coop-game').iterdir()), [])


    def test_the_update_goes_over_the_game_through_links(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            game = make_game(root / 'CUSA03173', version='01.00')
            (game / 'dvdroot_ps4' / 'map').mkdir()
            (game / 'dvdroot_ps4' / 'map' / 'm.dcx').write_bytes(b'map')
            update = make_game(root / 'CUSA03173-patch')
            view = root / 'coop-game' / 'CUSA03173'
            coop.build_game_view(game, update, view)
            self.assertEqual(coop.game_version(view), ('CUSA03173', '01.09'))
            self.assertEqual((view / 'eboot.bin').read_bytes(), b'eboot 01.09')
            self.assertEqual((view / 'dvdroot_ps4' / 'map' / 'm.dcx').read_bytes(), b'map')
            # Built again (another game): only the links go, never the game behind them.
            coop.build_game_view(game, None, view)
            self.assertEqual(coop.game_version(view), ('CUSA03173', '01.00'))
            self.assertTrue((game / 'dvdroot_ps4' / 'map' / 'm.dcx').is_file())
            self.assertTrue((update / 'eboot.bin').is_file())
            coop.remove_view(view)
            self.assertFalse(view.exists())
            self.assertTrue((game / 'eboot.bin').is_file())


class EventFilesTests(unittest.TestCase):
    def test_made_files_are_compared_with_the_tested_ones(self):
        reference = {'files': {'a.emevd.dcx': {'output_sha256': '1'}, 'b.emevd.dcx': {'output_sha256': '2'}}}
        made = {'files': {'a.emevd.dcx': {'output_sha256': '1'}, 'b.emevd.dcx': {'output_sha256': '9'},
                          'c.emevd.dcx': {'output_sha256': '3'}}}
        self.assertEqual(coop.event_differences(made, reference), ['b.emevd.dcx', 'c.emevd.dcx'])
        self.assertEqual(coop.event_differences(reference, reference), [])

    def test_a_new_event_patcher_makes_the_files_again(self):
        # Kaniel's playtest 2026-10-17: the release fixing Vicar Amelia's skull for guests changed
        # party_boss_events.py, but an install made its event files once (Oct 14) and kept them.
        with tempfile.TemporaryDirectory() as folder:
            install = Path(folder)
            (install / 'tools').mkdir()
            (install / 'event-patch').mkdir()
            tool = install / 'tools' / 'party_boss_events.py'
            tool.write_bytes(b'old patcher')
            self.assertFalse(coop.events_current(install), 'no manifest yet')
            manifest = install / 'event-patch' / 'manifest.json'
            manifest.write_text(json.dumps({'tool': 'party_boss_events.py', 'files': {}}), encoding='utf-8')
            self.assertFalse(coop.events_current(install), 'a manifest from before the hash')
            manifest.write_text(json.dumps({'tool_sha256': coop.file_sha256(tool), 'files': {}}),
                                encoding='utf-8')
            self.assertTrue(coop.events_current(install))
            tool.write_bytes(b'new patcher')
            self.assertFalse(coop.events_current(install))


class PlayTests(unittest.TestCase):
    def test_the_game_starts_as_2_play_started_it(self):
        install = Path('C:/Co-op')
        settings = dict(coop.DEFAULTS, role='guest', host_ip='26.1.2.3', account='Lisara', password='pw',
                        harder='3,1.5,3')
        env = coop.game_environment({'BB_FPS': 'uncap', 'BB_FPS_LIMIT': '240', 'KEEP': '1',
                                     'SHADPS4_BLOODBORNE_PARTY': 'old:x', 'SHADPS4_BLOODBORNE_PARTY_ROLE': 'host'},
                                    settings, install, Path('C:/Co-op/coop-game'))
        self.assertEqual(env['KEEP'], '1')
        self.assertEqual(env['BB_FPS'], '60')
        self.assertNotIn('BB_FPS_LIMIT', env)
        self.assertEqual(env['BB_ONLINE'], '1')
        self.assertEqual(env['BB_GAME_DIR'], str(Path('C:/Co-op/coop-game/CUSA03173')))
        self.assertEqual(env['BB_USER_DIR'], str(install / 'user'))
        # Phase 11, one port: the game signs in on the link's UDP port.
        self.assertEqual(env['BB_SHADNET_SERVER'], '26.1.2.3:31314')
        self.assertEqual(env['BB_SHADNET_WEBAPI'], 'http://26.1.2.3:31315')
        self.assertEqual((env['BB_SHADNET_NPID'], env['BB_SHADNET_PASSWORD']), ('Lisara', 'pw'))
        self.assertEqual(env['SHADPS4_BLOODBORNE_SEAMLESS_COOP'], '1')
        # The party is the team made in the game: no party or role setting reaches it.
        self.assertNotIn('SHADPS4_BLOODBORNE_PARTY', env)
        self.assertNotIn('SHADPS4_BLOODBORNE_PARTY_ROLE', env)
        self.assertEqual(env['SHADPS4_BLOODBORNE_HARDER_ENEMIES'], '3,1.5,3')
        self.assertNotIn('SHADPS4_P2P_PORT', env)
        env = coop.game_environment({}, dict(settings, harder='off', p2p_port='51700'), install, install)
        self.assertNotIn('SHADPS4_BLOODBORNE_HARDER_ENEMIES', env)
        self.assertEqual(env['SHADPS4_P2P_PORT'], '51700')

    def test_unlocked_frame_rate_is_opt_in(self):
        # Kaniel 2026-10-22: an opt-in unlocked frame rate in co-op. On, the launcher's mode and cap
        # stay and run.py hears of it (it caps at 120 and paces the game's packets); off, 60.
        install = Path('C:/Co-op')
        settings = dict(coop.DEFAULTS, host_ip='26.1.2.3', account='Lisara', password='pw')
        launcher = {'BB_FPS': 'uncap', 'BB_FPS_LIMIT': '144'}
        env = coop.game_environment(launcher, dict(settings, unlocked_fps='1'), install, install)
        self.assertEqual((env['BB_FPS'], env['BB_FPS_LIMIT']), ('uncap', '144'))
        self.assertEqual(env['BB_ONLINE_UNLOCKED_FPS'], '1')
        env = coop.game_environment({'BB_ONLINE_UNLOCKED_FPS': '1', **launcher}, settings, install, install)
        self.assertEqual(env['BB_FPS'], '60')
        self.assertNotIn('BB_FPS_LIMIT', env)
        self.assertNotIn('BB_ONLINE_UNLOCKED_FPS', env)

    def test_the_frame_rate_choice_unlocks_co_op(self):
        # Kaniel 2026-10-22: no switch (unintuitive); the frame rate choice itself unlocks co-op,
        # with a warning when it is picked. 60 and 30 keep the fixed step.
        self.assertTrue(coop.unlocked_fps('uncap'))
        self.assertTrue(coop.unlocked_fps('90'))
        self.assertFalse(coop.unlocked_fps('60'))
        self.assertFalse(coop.unlocked_fps('30'))
        self.assertFalse(coop.unlocked_fps(''))

    def test_this_pc_hosts_when_the_host_ip_is_its_own(self):
        """No Player/Host switch (Kaniel 2026-10-16): this PC hosts when the host's IP is its own."""
        import socket
        own = [ip for ip in socket.gethostbyname_ex(socket.gethostname())[2] if not ip.startswith('127.')]
        if own:
            self.assertTrue(coop.is_this_pc(own[0]), own[0])
        self.assertFalse(coop.is_this_pc('192.0.2.77'), "a documentation address is nobody's PC")
        self.assertFalse(coop.is_this_pc(''))
        self.assertFalse(coop.is_this_pc('not an ip'))

    def test_this_pcs_vpn_address_is_found_radmin_first(self):
        """The connection help offers the host their own VPN address (Kaniel 2026-10-21: players got lost at
        the VPN step)."""
        self.assertEqual(coop.vpn_name('26.12.34.56'), 'Radmin VPN')
        self.assertEqual(coop.vpn_name('100.101.7.9'), 'Tailscale')
        self.assertEqual(coop.vpn_name('100.128.0.1'), '', 'outside the 100.64.0.0/10 range')
        self.assertEqual(coop.vpn_name('192.168.1.20'), '')
        self.assertEqual(coop.vpn_name('not an ip'), '')
        self.assertEqual(coop.vpn_addresses(['192.168.1.20', '100.101.7.9', '26.12.34.56', '26.12.34.56']),
                         [('Radmin VPN', '26.12.34.56'), ('Tailscale', '100.101.7.9')])
        self.assertEqual(coop.vpn_addresses(['192.168.1.20']), [])
        self.assertIsInstance(coop.vpn_addresses(), list)

    def test_start_server_asks_the_router_only_without_a_vpn(self):
        """UPnP (Kaniel 2026-10-21): the router opens the host's ports when the host's address is its own
        LAN or internet one; on a VPN address the friends come over the VPN."""
        with mock.patch.dict(os.environ, {'BB_UPNP': '1'}):
            self.assertTrue(coop.wants_router('192.168.4.25'))
            self.assertFalse(coop.wants_router('26.12.34.56'), 'Radmin VPN')
            self.assertFalse(coop.wants_router('100.101.7.9'), 'Tailscale')
            self.assertFalse(coop.wants_router(''))
        with mock.patch.dict(os.environ, {'BB_UPNP': '0'}):
            self.assertFalse(coop.wants_router('192.168.4.25'), 'turned off')

    def test_the_host_overrides_point_the_game_at_the_host(self):
        self.assertEqual(json.loads(coop.host_overrides('26.1.2.3')),
                         {'https://ss4.scej-network.jp:20443': 'http://26.1.2.3:20443'})

    def test_a_crash_goes_to_the_host_with_its_logs(self):
        # Phase 11: after a crash, the next PLAY sends the dump and the crashed run's logs (the
        # .prev ones, which the start just made) to the host's server, once each.
        with tempfile.TemporaryDirectory() as folder:
            log = Path(folder)
            for name in ('shad_log.prev.txt', 'console.prev.txt', 'shad_log.txt'):
                (log / name).write_text('log', encoding='utf-8')
            self.assertEqual(coop.reports_to_send(log), [], 'no crash: nothing')
            (log / 'crash-20261015-174001.dmp').write_bytes(b'MDMP')
            self.assertEqual([path.name for path in coop.reports_to_send(log)],
                             ['crash-20261015-174001.dmp', 'console.prev.txt', 'shad_log.prev.txt'])
            coop.note_reports_sent(log, coop.reports_to_send(log))
            self.assertEqual(coop.reports_to_send(log), [], 'sent once')
            (log / 'crash-20261016-090000.dmp').write_bytes(b'MDMP')
            self.assertEqual([path.name for path in coop.reports_to_send(log)][0], 'crash-20261016-090000.dmp')
        request = coop.report_request('26.1.2.3', Path('crash-1.dmp'), b'MDMP', 'p0123', 'secret')
        self.assertEqual(request.full_url, 'http://26.1.2.3:31315/bb/report/crash-1.dmp')
        self.assertEqual((request.get_header('X-player'), request.get_header('X-secret')), ('p0123', 'secret'))
        self.assertEqual(request.get_method(), 'POST')

    def test_the_server_runs_under_its_watchdog(self):
        # Phase 11: shadnet --watchdog keeps the co-op server running (it starts it again after a
        # crash or a hang); stopping stops the watchdog first, so it starts nothing again.
        install = Path('C:/Co-op')
        self.assertEqual(coop.server_command(install), [str(install / 'server' / 'shadnet.exe'), '--watchdog'])
        found = [(11, 'shadnet.exe', 'shadnet.exe --beat'), (12, 'python.exe', 'bb_dummy_server.py'),
                 (10, 'shadnet.exe', 'shadnet.exe --watchdog')]
        self.assertEqual([pid for pid, _name, _command in coop.stop_order(found)], [10, 11, 12])

    def test_the_server_log_reaches_shadnet_txt(self):
        # Players' "Failed to acquire Bloodborne server information" (2026-10-22): user\\log\\shadnet.txt
        # was empty on every PC. Qt sends its log to the debugger when the process has no console
        # (the launcher starts shadnet detached), unless QT_FORCE_STDERR_LOGGING is set.
        env = coop.server_environment('26.1.2.3', {'PATH': 'C:/Windows'})
        self.assertEqual(env['QT_FORCE_STDERR_LOGGING'], '1')
        self.assertEqual(env['SHADNET_BACKEND_ADDRESS'], '26.1.2.3')
        self.assertEqual(env['PATH'], 'C:/Windows', 'the rest of the environment stays')
        self.assertEqual(env['QT_LOGGING_RULES'], '*.debug=false', 'no per-request debug lines')
        # Each start writes a fresh log; the one before stays as shadnet.prev.txt.
        with tempfile.TemporaryDirectory() as folder:
            install = Path(folder)
            log = install / 'user' / 'log'
            log.mkdir(parents=True)
            (log / 'shadnet.txt').write_text('last start', encoding='utf-8')
            with mock.patch.object(coop, 'servers_running', return_value=False), \
                    mock.patch.object(coop, 'port_open', return_value=True), \
                    mock.patch.object(coop.subprocess, 'Popen') as popen:
                coop.start_servers(install, '26.1.2.3', wait_s=0)
            self.assertEqual(popen.call_args.kwargs['env']['QT_FORCE_STDERR_LOGGING'], '1')
            self.assertEqual((log / 'shadnet.prev.txt').read_text(encoding='utf-8'), 'last start')
            self.assertEqual((log / 'shadnet.txt').read_text(encoding='utf-8'), '')

    def test_the_server_state_knows_when_the_games_port_is_silent(self):
        # "Running" asked only port 31313, so it showed while nothing answered the game's port 20443
        # and the host's game could not get ss.info (2026-10-22).
        def answering(*ports):
            return lambda port, *args, **kwargs: port in ports
        with mock.patch.object(coop, 'port_open', answering(coop.SHADNET_PORT, coop.BACKEND_PORT)):
            self.assertEqual(coop.server_state(), 'running')
        with mock.patch.object(coop, 'port_open', answering(coop.SHADNET_PORT)):
            self.assertEqual(coop.server_state(), 'no_backend')
        with mock.patch.object(coop, 'port_open', answering(coop.BACKEND_PORT)):
            self.assertEqual(coop.server_state(), 'stopped')
        with mock.patch.object(coop, 'port_open', answering()):
            self.assertEqual(coop.server_state(), 'stopped')

    def test_the_silent_port_warning_is_translated_everywhere(self):
        import bbport_lang
        for language in ('ru', 'ar', 'es', 'pt', 'fr', 'de', 'it', 'pl', 'tr', 'zh', 'ja', 'ko'):
            self.assertTrue(bbport_lang.table(language).get(coop.NO_BACKEND_TEXT), language)

    def test_the_public_server_and_new_id_texts_are_translated_everywhere(self):
        import bbport_lang
        texts = [coop.UNKNOWN_SERVER_TEXT, coop.PUBLIC_SERVER_HELP, coop.NEW_ID_TEXT, coop.NEW_ID_FAILED_TEXT]
        launcher = (Path(__file__).resolve().parents[1] / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        for name in ('UNKNOWN_SERVER_TEXT', 'PUBLIC_SERVER_HELP', 'NEW_ID_TEXT', 'NEW_ID_FAILED_TEXT'):
            self.assertTrue(f'_(bbport_coop.{name})' in launcher, f'the launcher says {name}')
        for language in ('ru', 'ar', 'es', 'pt', 'fr', 'de', 'it', 'pl', 'tr', 'zh', 'ja', 'ko'):
            table = bbport_lang.table(language)
            for text in texts:
                self.assertTrue(table.get(text), f'{language}: {text}')
                self.assertEqual(table[text].count('{}'), text.count('{}'), f'{language}: {text}')

    def test_every_release_of_the_game_is_known(self):
        # The game's own content-id table (01.09 eboot): Japan, US, EU and Asia releases, each with its
        # Game of the Year edition. A Japanese player's CUSA03014 was not on the list (2026-10-22).
        for title in ('CUSA00299', 'CUSA03014', 'CUSA00900', 'CUSA03179', 'CUSA00207', 'CUSA03173',
                      'CUSA00208', 'CUSA01363', 'CUSA03023'):
            self.assertIn(title, coop.TITLE_IDS)

    def test_an_untested_game_gets_a_warning(self):
        # Kaniel 2026-10-22: co-op is tested on the EU edition only; other editions should work since #355
        # and the launcher says they were not tested properly. The Unlock Game Region patch gets no
        # warning: #357 fixed it (Xirato confirmed that patch was the cause).
        self.assertIsNone(coop.untested_warning('CUSA03173'))
        text, args = coop.untested_warning('CUSA03179')
        self.assertEqual(text.format(*args).count('CUSA03179'), 1)
        self.assertIn('not tested', text)
        self.assertFalse(hasattr(coop, 'UNTESTED_REGION_TEXT'))
        self.assertFalse(hasattr(coop, 'region_unlocked'))
        import bbport_lang
        for language in ('ru', 'ar', 'es', 'pt', 'fr', 'de', 'it', 'pl', 'tr', 'zh', 'ja', 'ko'):
            template = coop.UNTESTED_EDITION_TEXT
            translated = bbport_lang.table(language).get(template)
            self.assertTrue(translated, f'{language}: {template}')
            self.assertEqual(translated.count('{}'), template.count('{}'), language)

    def test_nothing_registers_the_account(self):
        # Phase 11: the host's server makes a player's account at its first sign-in.
        self.assertFalse(hasattr(coop, 'register'))
        self.assertFalse(hasattr(coop, 'register_command'))

    def test_the_firewall_rules_cover_the_game_and_for_the_host_the_servers(self):
        install = Path('C:/Co-op')
        guest = coop.firewall_commands(install, host=False)
        self.assertEqual(len(guest), 1)
        self.assertIn(str(install / 'bin' / 'bb-probe.exe'), guest[0])
        host = coop.firewall_commands(install, host=True)
        self.assertEqual(len(host), 3)
        # Phase 11: the game's HTTP and one UDP port; shadNet's TCP port stays on the host's PC.
        self.assertTrue(any('localport=31315,20443 ' in line for line in host))
        self.assertTrue(any('protocol=UDP' in line and 'localport=31314 ' in line for line in host))
        self.assertFalse(any('31313' in line for line in host))


class UpdateTests(unittest.TestCase):
    """Kaniel 2026-10-16: the launcher updates the install from this repository's releases, every release
    counted (pre-releases too), and only a file that matches its SHA-256 is run."""

    @staticmethod
    def release(tag, prerelease=False, draft=False, setup=True, sums=True):
        name = f'BloodborneSeamlessCoop-{tag}-setup.exe'
        assets = []
        if setup:
            assets.append({'name': name, 'browser_download_url': f'https://example.test/{name}'})
        if sums:
            assets.append({'name': name + '.sha256', 'browser_download_url': f'https://example.test/{name}.sha256'})
        return {'tag_name': tag, 'prerelease': prerelease, 'draft': draft, 'assets': assets,
                'html_url': f'https://example.test/{tag}', 'body': f'notes of {tag}'}

    def test_versions_sort_with_a_suffix_below_the_plain_one(self):
        self.assertLess(coop.version_key('0.3.3-local'), coop.version_key('0.3.3'))
        self.assertLess(coop.version_key('0.3.3'), coop.version_key('0.3.4-local'))
        self.assertLess(coop.version_key('v0.1.4'), coop.version_key('0.3.3-local'))
        self.assertLess(coop.version_key('0.3.9'), coop.version_key('0.3.10'))
        self.assertEqual(coop.version_key('nonsense')[0], ())

    def test_the_newest_release_with_a_setup_is_offered_pre_releases_too(self):
        releases = [self.release('v0.3.4-local', prerelease=True), self.release('v0.4.0', draft=True),
                    self.release('v0.3.5', sums=False), self.release('v0.1.2', setup=False), self.release('v0.1.4')]
        best = coop.newest_release(releases)
        self.assertEqual(best['version'], '0.3.4-local', 'drafts and releases without a setup and its sum do not count')
        self.assertTrue(best['setup'].endswith('-setup.exe'))
        self.assertTrue(best['sha256'].endswith('.sha256'))
        self.assertIsNone(coop.newest_release([]))
        with self.assertRaises(ValueError):
            coop.newest_release({'message': 'rate limited'})

    def test_only_a_newer_release_is_offered(self):
        best = coop.newest_release([self.release('v0.3.4-local', prerelease=True)])
        self.assertIs(coop.update_offer('0.3.3-local', best), best)
        self.assertIsNone(coop.update_offer('0.3.4-local', best))
        self.assertIsNone(coop.update_offer('0.3.5', best))
        self.assertIsNone(coop.update_offer('', best), 'a source tree has no release version')

    def test_the_sum_file_is_read_as_the_release_writes_it(self):
        digest = 'ab' * 32
        self.assertEqual(coop.expected_sum(f'{digest.upper()} *Setup.exe\n', 'Setup.exe'), digest)
        self.assertEqual(coop.expected_sum(f'{digest}  Setup.exe', 'Setup.exe'), digest)
        self.assertEqual(coop.expected_sum(digest, 'Setup.exe'), digest)
        with self.assertRaises(ValueError):
            coop.expected_sum(f'{digest} *Other.exe', 'Setup.exe')

    def test_a_download_that_does_not_match_its_sum_is_thrown_away(self):
        import hashlib
        import io
        from unittest import mock
        body = b'setup bytes'
        release = coop.newest_release([self.release('v0.3.4')])

        class Response(io.BytesIO):
            headers = {'Content-Length': str(len(body))}

            def __enter__(self):
                return self

            def __exit__(self, *_a):
                return False

        def opener(digest):
            def open_url(request, timeout=0):
                url = request.full_url if hasattr(request, 'full_url') else request
                return Response(f'{digest} *{release["name"]}'.encode() if url.endswith('.sha256') else body)
            return open_url

        with tempfile.TemporaryDirectory() as tmp:
            seen = []
            with mock.patch.object(coop.urllib.request, 'urlopen', opener(hashlib.sha256(body).hexdigest())):
                path = coop.download_update(release, tmp, progress=seen.append)
            self.assertEqual(path.read_bytes(), body)
            self.assertEqual(seen[-1], 1.0)
            path.unlink()
            with mock.patch.object(coop.urllib.request, 'urlopen', opener('00' * 32)):
                with self.assertRaises(ValueError):
                    coop.download_update(release, tmp)
            self.assertFalse((Path(tmp) / release['name']).exists(), 'a file that does not match is gone')

    def test_patch_notes_are_every_release_newest_first(self):
        # Kaniel 2026-10-20: Home lists every release's notes from the latest one back to the first
        def notes(tag, body, draft=False, published='2026-10-20T14:06:33Z'):
            return {'tag_name': tag, 'name': f'Co-op {tag}', 'draft': draft, 'published_at': published,
                    'body': body}
        listed = [
            notes('v0.3.9-local', 'Intro.\n\n**No game files are included.**\n\n1. Download `x-setup.exe`.\n'
                  '**What\'s new since v0.3.8-local**\n- Map screen: **your** position.\n- `R3` switches floors.\n\n'
                  'Everyone in a team must use this same release.', published='2026-10-18T11:25:01Z'),
            notes('v1.0.0', "**What's new since v0.3.18-local**\r\n- Chalice dungeons together.\r\n- Fewer crashes.\r\n\r\n"
                  'Known: a Root chalice needs a teammate in the Dream.\r\n\r\nBuilt from deaad75.'),
            notes('v0.1.1', '### Getting started\n- run SETUP\n### Fixed in v0.1.1\n- **Portraits show.** Replace v0.1.0.\n'
                  '### What\'s in this release\n- **Party death**:\n  - a sub point\n### Known gaps\n- Joining stalls.',
                  published='2026-10-13T20:11:47Z'),
            notes('v0.1.2', '\n\nEveryone in a party must use this same release.\n', published='2026-10-13T21:32:57Z'),
            notes('v2.0.0', '- not out yet', draft=True),
            None, 'junk', {'tag_name': 'nightly'},
        ]
        got = coop.release_notes(listed)
        self.assertEqual([r['version'] for r in got], ['1.0.0', '0.3.9-local', '0.1.1'],
                         'newest first; drafts, junk and releases without notes left out')
        self.assertEqual(got[0]['notes'], ['Chalice dungeons together.', 'Fewer crashes.'])
        self.assertEqual(got[0]['known'], ['A Root chalice needs a teammate in the Dream.'])
        self.assertEqual(got[0]['date'], '20 Oct 2026')
        self.assertEqual(got[1]['notes'], ['Map screen: your position.', 'R3 switches floors.'],
                         'markdown marks are gone, and the install steps are not notes')
        self.assertEqual(got[2]['notes'], ['Portraits show. Replace v0.1.0.', 'Party death: a sub point'],
                         'the getting started and known gaps lists are not news; a sub point joins its point')
        self.assertEqual(got[2]['known'], ['Joining stalls.'])
        self.assertEqual(coop.release_notes({'message': 'rate limited'}), [])

    def test_every_release_is_listed_for_the_notes(self):
        self.assertIn('per_page=100', coop.RELEASES_API)

    def test_the_update_and_patch_notes_texts_are_translated_everywhere(self):
        import bbport_lang
        texts = ['Updating to v{}', 'Patch notes', "What's new in v{}", 'Known issue: {}', 'Earlier releases',
                 'Checking for updates…', 'You have the latest version.',
                 "Downloading the update. Don't close the launcher or start the game: the installer opens next, "
                 "then the launcher opens again by itself.",
                 "The installer window shows the progress. Don't close it or start the game: the launcher "
                 "opens again when it is done."]
        for language in ('ru', 'ar', 'es', 'pt', 'fr', 'de', 'it', 'pl', 'tr', 'zh', 'ja', 'ko'):
            table = bbport_lang.table(language)
            for text in texts:
                self.assertTrue(table.get(text), f'{language}: {text}')
                self.assertEqual(table[text].count('{}'), text.count('{}'), f'{language}: {text}')

    def test_the_connection_help_is_translated_everywhere_and_links_the_website_guide(self):
        import ast
        import bbport_lang
        launcher = Path(__file__).resolve().parents[1] / 'launcher' / 'bbport_launcher_win.py'
        source = launcher.read_text(encoding='utf-8')
        names = {'build_coop_help', 'coop_help_refresh', 'coop_help_refresh_public', 'coop_router_said',
                 'build_coop_custom'}
        pieces = [f for f in ast.walk(ast.parse(source)) if isinstance(f, ast.FunctionDef) and f.name in names]
        self.assertEqual({f.name for f in pieces}, names)
        texts = {n.args[0].value for f in pieces for n in ast.walk(f) if isinstance(n, ast.Call)
                 and isinstance(n.func, ast.Name) and n.func.id == '_' and n.args and isinstance(n.args[0], ast.Constant)}
        self.assertGreater(len(texts), 10)
        self.assertIn("_('How to connect', 'Как подключиться'), self.open_coop_help", source,
                      'the co-op card opens the help')
        for language in ('ru', 'ar', 'es', 'pt', 'fr', 'de', 'it', 'pl', 'tr', 'zh', 'ja', 'ko'):
            table = bbport_lang.table(language)
            for text in texts:
                if language == 'ru' and text in bbport_lang.KEYS:
                    continue  # the launcher's older texts carry their Russian beside them in the code
                self.assertTrue(table.get(text), f'{language}: {text}')
                self.assertEqual(table[text].count('{}'), text.count('{}'), f'{language}: {text}')
        self.assertIn("RADMIN_GUIDE = 'https://bloodborne.seamlesscoop.dev/docs/radmin'", source)
        docs = Path(__file__).resolve().parents[2] / 'website' / 'src' / 'content' / 'docs.ts'
        if docs.exists():
            self.assertIn('slug: "radmin"', docs.read_text(encoding='utf-8'), 'the page the help links to')

    def test_the_enemy_choices_are_translated_everywhere(self):
        import ast
        import bbport_lang
        launcher = Path(__file__).resolve().parents[1] / 'launcher' / 'bbport_launcher_win.py'
        tree = ast.parse(launcher.read_text(encoding='utf-8'))
        choices = next(n.value for n in tree.body if isinstance(n, ast.Assign)
                       and getattr(n.targets[0], 'id', '') == 'HARDER_CHOICES')
        texts = [t.elts[1].elts[0].value for t in choices.elts] + ['Enemies (the same for every player)']
        self.assertIn('Custom', texts)
        for language in ('ru', 'ar', 'es', 'pt', 'fr', 'de', 'it', 'pl', 'tr', 'zh', 'ja', 'ko'):
            table = bbport_lang.table(language)
            for text in texts:
                self.assertTrue(table.get(text), f'{language}: {text}')

    def test_the_setup_updates_this_install_silently(self):
        command = coop.setup_command(Path('C:/Temp/Setup.exe'), Path('C:/Games/Co-op'))
        self.assertIn('/CLOSEAPPLICATIONS', command)
        self.assertIn(f'/DIR={Path("C:/Games/Co-op")}', command)

    def test_the_update_keeps_a_window_on_screen(self):
        # Kaniel 2026-10-20: with /VERYSILENT the launcher seemed gone; /SILENT shows the progress, and the
        # update cannot be cancelled halfway
        command = coop.setup_command(Path('C:/Temp/Setup.exe'), Path('C:/Games/Co-op'))
        self.assertIn('/SILENT', command)
        self.assertNotIn('/VERYSILENT', command)
        self.assertIn('/NOCANCEL', command)
        # LauncherBar dresses the uninstall's window too, where WizardSilent may not be called (the
        # uninstaller would not start): only InitializeWizard reads it
        code = (Path(__file__).resolve().parents[2] / 'tools' / 'release' / 'setup.iss').read_text(encoding='utf-8-sig')
        bar = code[code.index('procedure LauncherBar'):code.index('procedure StopBar')]
        self.assertNotIn('WizardSilent', bar)
        self.assertIn('NoClose := WizardSilent;', code[code.index('procedure InitializeWizard;'):])
        iss = (Path(__file__).resolve().parents[2] / 'tools' / 'release' / 'setup.iss').read_text(encoding='utf-8-sig')
        self.assertRegex(iss, r"(?m)^UpdateNote=.*Don't close this window")
        self.assertIn("CustomMessage('UpdateNote')", iss)
        for language in ('russian', 'arabic', 'spanish', 'brazilianportuguese', 'french', 'german', 'italian',
                         'polish', 'turkish', 'chinesesimplified', 'japanese', 'korean'):
            self.assertRegex(iss, rf'(?m)^{language}\.UpdateNote=.+', language)

    def test_the_setup_script_is_utf8_with_a_bom(self):
        # ISCC reads a script without a BOM as ANSI: the translated update notes would be garbled
        data = (Path(__file__).resolve().parents[2] / 'tools' / 'release' / 'setup.iss').read_bytes()
        self.assertTrue(data.startswith(b'\xef\xbb\xbf'))

    def test_the_default_folder_is_games_on_d_or_c(self):
        iss = (Path(__file__).resolve().parents[2] / 'tools' / 'release' / 'setup.iss').read_text(encoding='utf-8-sig')
        self.assertRegex(iss, r'(?m)^DefaultDirName=\{code:DefaultInstallDir\}$')
        self.assertNotIn('DefaultDirName={localappdata}', iss)
        self.assertIn("'D:\\Games\\{#ShortName}'", iss)
        self.assertIn("ExpandConstant('{sd}') + '\\Games\\{#ShortName}'", iss)
        self.assertIn('#define ShortName "Yharnam Together"', iss)
        self.assertRegex(iss, r'(?m)^PrivilegesRequired=lowest$')  # no Program Files: updates run without admin

    def test_an_update_leaves_the_desktop_shortcut_where_it_is(self):
        # Kaniel 2026-10-20: a new shortcut on every install lost the spot the player dragged it to
        iss = (Path(__file__).resolve().parents[2] / 'tools' / 'release' / 'setup.iss').read_text(encoding='utf-8-sig')
        desktop = [line for line in iss.splitlines() if line.startswith('Name: "{autodesktop}')]
        self.assertEqual(len(desktop), 1, desktop)
        self.assertIn('Tasks: desktopicon', desktop[0])
        self.assertIn('Check: NoDesktopShortcut', desktop[0])
        self.assertIn('function NoDesktopShortcut: Boolean;', iss)

    def test_the_setup_file_has_the_new_name(self):
        # Kaniel 2026-10-20; older launchers find any '-setup.exe' of a release, so they still update
        root = Path(__file__).resolve().parents[2] / 'tools'
        iss = (root / 'release' / 'setup.iss').read_text(encoding='utf-8-sig')
        release = (root / 'make-release.ps1').read_text(encoding='utf-8-sig')
        self.assertRegex(iss, r'(?m)^OutputBaseFilename=YharnamTogether-v\{#AppVersion\}-setup$')
        self.assertIn('$setupName = "YharnamTogether-$tag-setup.exe"', release)
        self.assertTrue(coop.SETUP_SUFFIX == '-setup.exe')

    def test_the_setup_has_the_new_name(self):
        # Kaniel 2026-10-20: Yharnam Together in the launcher and the setup; the old Start menu entries go
        iss = (Path(__file__).resolve().parents[2] / 'tools' / 'release' / 'setup.iss').read_text(encoding='utf-8-sig')
        self.assertIn('#define AppName "Yharnam Together: Bloodborne Seamless Co-op"', iss)
        self.assertIn('Type: files; Name: "{autoprograms}\\Bloodborne Seamless Co-op.lnk"', iss)
        self.assertNotRegex(iss, r'\{(autoprograms|autodesktop)\}\\\{#AppName\}', 'no colon in a shortcut name')

    def test_the_setup_starts_the_launcher_again_after_a_silent_update(self):
        iss = (Path(__file__).resolve().parents[2] / 'tools' / 'release' / 'setup.iss').read_text(encoding='utf-8')
        self.assertRegex(iss, r'Filename: "\{app\}\\BLauncher\.exe";[^\n]*Flags: nowait skipifnotsilent')


class LogRotationTests(unittest.TestCase):
    """Kaniel's playtest 2026-10-22: two starts after the host's stuck rejoin, its .prev logs were
    another run's. The last few runs' logs stay in user\\log\\older, named by when each ended."""

    def test_the_last_runs_logs_are_kept_by_their_time(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / 'user' / 'log'
            log.mkdir(parents=True)
            for run in range(coop.KEPT_RUN_LOGS + 2):
                for name in ('shad_log', 'console'):
                    path = log / f'{name}.txt'
                    path.write_text(f'{name} run {run}', encoding='utf-8')
                    os.utime(path, (1792698000 + 60 * run, 1792698000 + 60 * run))
                coop.rotate_logs(tmp)
            self.assertEqual((log / 'shad_log.prev.txt').read_text(encoding='utf-8'),
                             f'shad_log run {coop.KEPT_RUN_LOGS + 1}')
            kept = sorted(path.name for path in (log / 'older').iterdir())
            self.assertEqual(len(kept), 2 * coop.KEPT_RUN_LOGS)
            first = time.strftime('%Y%m%d-%H%M%S', time.localtime(1792698000 + 120))
            self.assertEqual(kept[0], f'{first}-console.txt')
            self.assertEqual((log / 'older' / kept[-1]).read_text(encoding='utf-8'),
                             f'shad_log run {coop.KEPT_RUN_LOGS + 1}')

    def test_no_logs_yet(self):
        with tempfile.TemporaryDirectory() as tmp:
            coop.rotate_logs(tmp)
            self.assertFalse((Path(tmp) / 'user' / 'log' / 'older').exists())



class SaveBackupTests(unittest.TestCase):
    """Kaniel 2026-10-25: players need a way back to an earlier save (the broken chalice altar of
    2026-10-24 was mended by hand from user\\save-backups). The launcher backs the saves up before each
    PLAY and puts a chosen backup back; the saves it replaces are kept first."""

    SLOT = 'savedata/1/CUSA03173/SPRJ0005/userdata0000'

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.install = Path(self.tmp.name)
        self.user = self.install / 'user'

    def write(self, relative, text, under=None):
        path = (under or self.user) / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')

    def game_backup(self, name, text):
        """One of the game's own backups (StartSaveBackups): no launcher.txt in it."""
        self.write(self.SLOT, text, self.user / coop.SAVE_BACKUPS / name)

    def test_a_backup_holds_the_saves_and_the_party_files_only(self):
        self.write(self.SLOT, 'slot')
        self.write('home/1000/savedata/CUSA03173/SPRJ0005/userdata0001', 'emulator slot')
        self.write('bloodborne_party_world_slot0.txt', '2000 1')
        self.write('log/shad_log.txt', 'not a save')
        self.write('coop.json', '{}')
        made = coop.back_up_saves(self.install, 'before PLAY', now=1792698000)
        self.assertEqual(made.name, time.strftime('%Y%m%d-%H%M%S', time.localtime(1792698000)))
        self.assertEqual(made.parent, self.user / 'save-backups')
        self.assertEqual(set(coop.save_files(made)), {self.SLOT, 'home/1000/savedata/CUSA03173/SPRJ0005/userdata0001',
                                                      'bloodborne_party_world_slot0.txt'})
        self.assertEqual((made / 'launcher.txt').read_text(encoding='utf-8').strip(), 'before PLAY')
        [listed] = coop.save_backups(self.install)
        self.assertEqual((listed['name'], listed['note'], listed['launcher']), (made.name, 'before PLAY', True))

    def test_no_saves_or_the_same_saves_make_no_backup(self):
        self.assertIsNone(coop.back_up_saves(self.install, 'before PLAY'))
        self.write(self.SLOT, 'one')
        self.game_backup('20261024-231630', 'one')
        self.assertIsNone(coop.back_up_saves(self.install, 'before PLAY'), 'the newest backup holds them')
        self.write(self.SLOT, 'two')
        self.assertIsNotNone(coop.back_up_saves(self.install, 'before PLAY'))

    def test_backups_are_listed_newest_first_and_strangers_are_left_out(self):
        for name in ('20261024-120000', '20261025-090000', '20261024-231630'):
            self.game_backup(name, name)
        (self.user / coop.SAVE_BACKUPS / 'my copy').mkdir()
        self.write('README.txt', 'x', self.user / coop.SAVE_BACKUPS)
        listed = coop.save_backups(self.install)
        self.assertEqual([b['name'] for b in listed], ['20261025-090000', '20261024-231630', '20261024-120000'])
        self.assertEqual(listed[0]['note'], '')
        self.assertFalse(listed[0]['launcher'])
        self.assertEqual(listed[0]['when'].tm_hour, 9)

    def test_restoring_puts_the_backup_back_and_keeps_the_saves_it_replaces(self):
        self.game_backup('20261024-231630', 'before the broken altar')
        self.write('bloodborne_party_world_slot0.txt', 'old flags', self.user / coop.SAVE_BACKUPS / '20261024-231630')
        self.write(self.SLOT, 'broken altar')
        self.write('savedata/1/CUSA03173/SPRJ0006/userdata0000', 'a slot the backup does not have')
        self.write('bloodborne_party_world_slot0.txt', 'new flags')
        self.write('log/shad_log.txt', 'kept')
        safety = coop.restore_saves(self.install, '20261024-231630')
        self.assertEqual((self.user / self.SLOT).read_text(encoding='utf-8'), 'before the broken altar')
        self.assertEqual((self.user / 'bloodborne_party_world_slot0.txt').read_text(encoding='utf-8'), 'old flags')
        self.assertFalse((self.user / 'savedata/1/CUSA03173/SPRJ0006').exists(), 'the saves are the backup, whole')
        self.assertTrue((self.user / 'log/shad_log.txt').exists())
        self.assertEqual(coop.save_files(safety)[self.SLOT], b'broken altar')
        self.assertIn('20261024-231630', (safety / 'launcher.txt').read_text(encoding='utf-8'))
        # Undoing the restore is restoring the backup it made.
        coop.restore_saves(self.install, safety.name)
        self.assertEqual((self.user / self.SLOT).read_text(encoding='utf-8'), 'broken altar')
        self.assertEqual((self.user / 'bloodborne_party_world_slot0.txt').read_text(encoding='utf-8'), 'new flags')

    def test_a_missing_or_empty_backup_changes_nothing(self):
        self.write(self.SLOT, 'mine')
        (self.user / coop.SAVE_BACKUPS / '20261024-231630').mkdir(parents=True)
        for name in ('20261024-231630', '20991231-000000', '..'):
            with self.assertRaises(ValueError):
                coop.restore_saves(self.install, name)
        self.assertEqual((self.user / self.SLOT).read_text(encoding='utf-8'), 'mine')

    def test_the_launcher_keeps_its_newest_backups_and_leaves_the_games(self):
        self.game_backup('20261001-000000', 'the game\'s')
        for minute in range(coop.KEPT_LAUNCHER_BACKUPS + 3):
            self.write(self.SLOT, f'play {minute}')
            coop.back_up_saves(self.install, 'before PLAY', now=1792698000 + 60 * minute)
        listed = coop.save_backups(self.install)
        self.assertEqual(sum(b['launcher'] for b in listed), coop.KEPT_LAUNCHER_BACKUPS)
        self.assertIn('20261001-000000', [b['name'] for b in listed])
        self.assertEqual(coop.save_files(listed[0]['path'])[self.SLOT], f'play {coop.KEPT_LAUNCHER_BACKUPS + 2}'.encode())

    def test_two_backups_in_one_second_get_their_own_names(self):
        self.write(self.SLOT, 'one')
        first = coop.back_up_saves(self.install, 'before PLAY', now=1792698000)
        self.write(self.SLOT, 'two')
        second = coop.back_up_saves(self.install, 'before PLAY', now=1792698000)
        self.assertNotEqual(first, second)
        self.assertEqual(coop.save_files(first)[self.SLOT], b'one')

if __name__ == '__main__':
    unittest.main()
