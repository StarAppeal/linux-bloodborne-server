"""The GTK launcher's co-op page (launcher/bbport_launcher.py): the answers it keeps in coop.json, the
commands it runs (coop-run.sh) and the launcher's bbport.ini copied into the co-op folder's own
(bbport_coop_cli.sync_ini). The window test needs a display and GTK 4 with libadwaita."""
import io
import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

LAUNCHER = Path(__file__).resolve().parents[1] / 'launcher'
sys.path.insert(0, str(LAUNCHER))

import bbport_coop  # noqa: E402
import bbport_coop_cli as cli  # noqa: E402


def gtk_missing():
    probe = "import gi; gi.require_version('Gtk','4.0'); gi.require_version('Adw','1'); from gi.repository import Adw"
    if subprocess.run([sys.executable, '-I', '-c', probe], capture_output=True).returncode:
        return 'GTK 4 / libadwaita (PyGObject) not installed'
    if not (os.environ.get('WAYLAND_DISPLAY') or os.environ.get('DISPLAY')):
        return 'no display'
    return None


class SyncIniTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.source, self.target = self.dir / 'launcher.ini', self.dir / 'coop' / 'bbport.ini'

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_launchers_settings_become_the_co_op_folders(self):
        self.source.write_text('# bbport settings\nupscaler=fsr3\noutput_res=2560x1440\n', encoding='utf-8')
        self.assertTrue(cli.sync_ini(self.source, self.target))
        self.assertEqual(self.target.read_text(encoding='utf-8'), self.source.read_text(encoding='utf-8'))

    def test_the_launchers_value_wins_and_keys_only_the_game_set_stay(self):
        self.source.write_text('upscaler=fsr3\npreset=2\n', encoding='utf-8')
        self.target.parent.mkdir()
        self.target.write_text('upscaler=fsr4\nfov=50\n', encoding='utf-8')
        cli.sync_ini(self.source, self.target)
        self.assertEqual(self.target.read_text(encoding='utf-8').splitlines(), ['upscaler=fsr3', 'preset=2', 'fov=50'])

    def test_an_unchanged_copy_says_nothing_changed(self):
        self.source.write_text('upscaler=fsr3\n', encoding='utf-8')
        self.assertTrue(cli.sync_ini(self.source, self.target))
        self.assertFalse(cli.sync_ini(self.source, self.target))

    def test_no_launcher_ini_leaves_the_co_op_ini_alone(self):
        self.target.parent.mkdir()
        self.target.write_text('upscaler=fsr4\n', encoding='utf-8')
        self.assertFalse(cli.sync_ini(self.dir / 'missing.ini', self.target))
        self.assertEqual(self.target.read_text(encoding='utf-8'), 'upscaler=fsr4\n')

    def test_the_same_file_is_not_copied_over_itself(self):
        self.source.write_text('upscaler=fsr3\n', encoding='utf-8')
        self.assertFalse(cli.sync_ini(self.source, self.source))


class ReleaseVersionTests(unittest.TestCase):
    """The build check on join: the game says which co-op release it is, as the server's VERSION.txt does."""

    def test_the_version_comes_from_version_txt(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'VERSION.txt'
            path.write_text('Yharnam Together: Bloodborne Seamless Co-op v1.3.0 (Linux)\nGame: 1.09\n',
                            encoding='utf-8')
            self.assertEqual(bbport_coop.release_version(path), '1.3.0')

    def test_a_missing_or_unreadable_file_is_no_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(bbport_coop.release_version(Path(tmp) / 'VERSION.txt'), '')
            (Path(tmp) / 'VERSION.txt').write_text('no version here\n', encoding='utf-8')
            self.assertEqual(bbport_coop.release_version(Path(tmp) / 'VERSION.txt'), '')

    def test_a_git_checkout_without_version_txt_takes_its_release_tag(self):
        with tempfile.TemporaryDirectory() as tmp:
            git = ['git', '-C', tmp, '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false',
                   '-c', 'tag.gpgsign=false']
            try:
                subprocess.run(['git', 'init', '-q', tmp], check=True, capture_output=True)
                subprocess.run(git + ['commit', '-q', '--allow-empty', '-m', 'release'], check=True, capture_output=True)
                subprocess.run(git + ['tag', 'v9.8.7'], check=True, capture_output=True)
                subprocess.run(git + ['commit', '-q', '--allow-empty', '-m', 'after'], check=True, capture_output=True)
            except (OSError, subprocess.CalledProcessError):
                self.skipTest('git is not available')
            self.assertEqual(bbport_coop.git_release_tag(tmp), '9.8.7')
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(bbport_coop.git_release_tag(tmp), '')

    def test_the_co_op_game_gets_it_in_its_environment(self):
        settings = {'host_ip': '10.0.0.5', 'account': 'a', 'password': 'p'}
        with mock.patch.object(bbport_coop, 'release_version', return_value='1.3.0'):
            env = bbport_coop.game_environment({'BB_RELEASE_VERSION': 'dev'}, settings, '/x', '/x/game')
        self.assertEqual(env['BB_RELEASE_VERSION'], '1.3.0')


class CliStartTests(unittest.TestCase):
    """coop-run.sh's start (bbport_coop_cli.main --dry-run) syncs the ini and points the game at it."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / 'game').mkdir()
        (self.dir / 'game' / 'eboot.bin').write_bytes(b'')
        self.ini = self.dir / 'launcher.ini'
        self.ini.write_text('upscaler=fsr3\n', encoding='utf-8')
        self.coop = self.dir / 'coop'

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, *extra):
        argv = ['--dir', str(self.coop), '--host', 'play.example.com', '--game', str(self.dir / 'game'),
                '--dry-run', *extra]
        playing = {'host_ip': '10.0.0.5', 'server': 'play.example.com', 'account': 'a', 'password': 'p'}
        out = io.StringIO()
        with mock.patch.object(bbport_coop, 'prepare', return_value=(self.coop / 'coop-game', '')), \
                mock.patch.object(bbport_coop, 'needs_prepare', return_value=True), \
                mock.patch.object(cli, 'server_settings', return_value=playing), \
                mock.patch.dict(os.environ, {'BB_CONFIG': str(self.ini)}), redirect_stdout(out):
            self.assertEqual(cli.main(argv), 0)
        return out.getvalue()

    def test_the_launcher_ini_is_copied_and_the_game_reads_the_copy(self):
        out = self.run_cli('--ini', str(self.ini))
        self.assertEqual((self.coop / 'bbport.ini').read_text(encoding='utf-8'), 'upscaler=fsr3\n')
        self.assertIn(f'BB_CONFIG={self.coop / "bbport.ini"}', out)

    def test_no_sync_ini_keeps_the_co_op_folders_own(self):
        self.run_cli('--ini', str(self.ini), '--no-sync-ini')
        self.assertFalse((self.coop / 'bbport.ini').exists())


@unittest.skipIf(gtk_missing(), gtk_missing() or '')
class LauncherWindowTests(unittest.TestCase):
    """The real window, in its own process (its own XDG_CONFIG_HOME, BB_COOP_DIR and BB_DATA_DIR)."""

    SCRIPT = textwrap.dedent('''
        import json, sys
        sys.path.insert(0, sys.argv[1])
        import bbport_launcher as L
        from gi.repository import GLib

        result = {}

        class App(L.LauncherApp):
            def __init__(self):
                # its own id and no single instance: a launcher the player has open must not take the start over
                L.Adw.Application.__init__(self, application_id="io.github.bbport.LauncherTest",
                                           flags=L.Gio.ApplicationFlags.NON_UNIQUE)

            def do_activate(self):
                window = L.LauncherWindow(self)
                window.present()
                result['pages'] = [window.stack.get_pages().get_item(i).get_name()
                                   for i in range(window.stack.get_pages().get_n_items())]
                # nothing typed: PLAY refuses and runs nothing
                started = []
                window.start_process = lambda command: started.append(command)
                window.on_coop_play(None)
                result['started_without_answers'] = list(started)
                # the answers
                window.coop_host_row.set_text('10.0.0.5')
                window.coop['game'] = sys.argv[2]
                window.coop_rule_rows['rule_travel'].set_active(True)
                window.coop_rule_rows['rule_vote'].set_active(True)
                window.coop_port_row.set_text('51700')
                window.on_coop_play(None)
                result['started'] = list(started)
                result['vote_sensitive'] = window.coop_rule_rows['rule_vote'].get_sensitive()
                window.coop_rule_rows['rule_travel'].set_active(False)
                result['vote_sensitive_off'] = window.coop_rule_rows['rule_vote'].get_sensitive()
                window.store()
                result['ini'] = str(L.ini_path())
                GLib.idle_add(lambda: self.quit() and False)

        code = App().run([])
        result['exit'] = code
        print(json.dumps(result))
    ''')

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / 'game').mkdir()
        (self.dir / 'game' / 'eboot.bin').write_bytes(b'')

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_window_starts_with_a_co_op_page_that_keeps_its_answers(self):
        env = dict(os.environ, XDG_CONFIG_HOME=str(self.dir / 'config'), BB_COOP_DIR=str(self.dir / 'coop'),
                   BB_DATA_DIR=str(self.dir / 'data'), HOME=str(self.dir))
        env.pop('BB_CONFIG', None)
        (self.dir / 'data').mkdir()
        done = subprocess.run([sys.executable, '-c', self.SCRIPT, str(LAUNCHER), str(self.dir / 'game')],
                              env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(done.returncode, 0, done.stderr)
        warnings = [line for line in done.stderr.splitlines() if 'Traceback' in line or 'Error' in line]
        self.assertEqual(warnings, [], done.stderr)
        result = json.loads(done.stdout.strip().splitlines()[-1])
        self.assertEqual(result['pages'], ['settings', 'coop', 'log'])
        self.assertEqual(result['started_without_answers'], [])
        command = result['started'][0]
        self.assertEqual(command[:3], ['setsid', 'bash', str(LAUNCHER.parent / 'coop-run.sh')])
        self.assertEqual(command[command.index('--dir') + 1], str(self.dir / 'coop'))
        self.assertEqual(command[command.index('--ini') + 1], result['ini'])
        self.assertTrue(result['vote_sensitive'])
        self.assertFalse(result['vote_sensitive_off'])
        saved = json.loads((self.dir / 'coop' / 'user' / 'coop.json').read_text(encoding='utf-8'))
        self.assertEqual((saved['host_ip'], saved['game'], saved['p2p_port']),
                         ('10.0.0.5', str(self.dir / 'game'), '51700'))
        self.assertEqual((saved['rule_travel'], saved['rule_vote']), ('', '1'))


class ProblemTests(unittest.TestCase):
    def setUp(self):
        if gtk_missing():
            self.skipTest(gtk_missing())
        import bbport_launcher
        self.launcher = bbport_launcher

    def test_a_host_and_a_game_folder_are_needed(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'eboot.bin').write_bytes(b'')
            problem = self.launcher.coop_problem
            self.assertIsNotNone(problem({'host_ip': '', 'game': tmp}))
            self.assertIsNotNone(problem({'host_ip': '10.0.0.5', 'game': ''}))
            self.assertIsNotNone(problem({'host_ip': 'not a host', 'game': tmp}))
            self.assertIsNone(problem({'host_ip': '10.0.0.5', 'game': tmp}))
            self.assertIsNone(problem({'host_ip': 'play.example.com', 'game': tmp}))

    def test_unlocked_fps_modes_are_passed_on(self):
        command = self.launcher.coop_play_command
        self.assertIn('--unlocked-fps', command({}, 'uncap', '/x.ini'))
        self.assertNotIn('--unlocked-fps', command({}, '60', '/x.ini'))

    def test_the_server_listens_on_the_hosts_address(self):
        command = self.launcher.coop_server_command('10.0.0.5')
        self.assertEqual(command[-3:], ['server', '--listen', '10.0.0.5'])


if __name__ == '__main__':
    unittest.main()
