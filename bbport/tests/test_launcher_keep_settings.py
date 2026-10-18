"""The player's bbport.ini survives the launcher, its updates and the setup (Kaniel 2026-10-18: "make sure
that key remaps between installing new releases saves, and does not get overriden").

The launcher read bbport.ini once when it opened and wrote every key.* / pad.* line and its own keys from
that copy on each PLAY: a remap made in the game's menu (or a second launcher, or a hand edit) after it
opened was put back, and a pad line the game had added was dropped. Now it writes only what was changed in
the launcher, onto the file as it is. Run: python -m unittest tests.test_launcher_keep_settings"""
import os
import re
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(ROOT / 'launcher'))

if sys.platform == 'win32':
    import bbport_launcher_win as launcher


class Var:
    def __init__(self, value, store='ini'):
        self.value, self.store = value, store

    def get(self):
        return self.value


@unittest.skipUnless(sys.platform == 'win32', 'the Windows launcher')
class KeepSettingsTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.folder = Path(folder.name)
        self.path = self.folder / 'bbport.ini'
        for patch in (mock.patch.dict(os.environ, {'BB_CONFIG': str(self.path)}),
                      mock.patch.object(launcher, 'CONFIG_DIR', self.folder / 'config'),
                      mock.patch.object(launcher, 'CONFIG_FILE', self.folder / 'config' / 'settings.json'),
                      mock.patch.object(launcher, 'reshade_ready', lambda: False)):
            patch.start()
            self.addCleanup(patch.stop)

    def opened(self, app=None, upscaler=None):
        """A launcher as it opens: its copy of bbport.ini and the settings vars its pages built from it."""
        ini, saved = launcher.launcher_ini(app or {})
        return types.SimpleNamespace(
            ini=ini, ini_saved=saved, app={}, tk=types.SimpleNamespace(TclError=Exception),
            vars={'upscaler': Var(upscaler or ini['upscaler'])},
            reshade_preset=Var(''), reshade_started='', mod_order=[], patch_vars={})

    def lines(self):
        return self.path.read_text(encoding='utf-8').splitlines()

    def test_a_remap_made_in_the_game_survives_play(self):
        self.path.write_text('upscaler=dlss\npad.cross=b\nkey.circle=Space\n', encoding='utf-8')
        window = self.opened()
        # The game's Controls tab (or a hand edit) changes the file while the launcher is open.
        self.path.write_text('upscaler=fsr\npad.cross=a\npad.circle=x\n', encoding='utf-8')
        launcher.Launcher.collect(window)
        text = self.lines()
        self.assertIn('pad.cross=a', text)
        self.assertIn('pad.circle=x', text)  # a line the game added stays
        self.assertNotIn('key.circle=Space', text)  # one the game removed stays removed
        self.assertIn('upscaler=fsr', text)  # and the other settings the game changed

    def test_play_twice_still_keeps_what_the_game_changed(self):
        self.path.write_text('upscaler=dlss\n', encoding='utf-8')
        window = self.opened()
        launcher.Launcher.collect(window)
        self.path.write_text('upscaler=fsr\npad.cross=a\n', encoding='utf-8')
        launcher.Launcher.collect(window)
        self.path.write_text('upscaler=fsr\npad.cross=y\n', encoding='utf-8')
        launcher.Launcher.collect(window)
        self.assertIn('pad.cross=y', self.lines())
        self.assertIn('upscaler=fsr', self.lines())

    def test_what_the_launcher_changes_is_still_written(self):
        self.path.write_text('# mine\nupscaler=dlss\npad.cross=b\n', encoding='utf-8')
        window = self.opened()
        self.path.write_text('# mine\nupscaler=dlss\npad.cross=a\n', encoding='utf-8')
        window.ini['key.circle'] = 'Left Ctrl'
        window.ini.pop('pad.cross')  # reset in the launcher: the line goes
        window.vars['upscaler'].value = 'fsr'
        launcher.Launcher.collect(window)
        text = self.lines()
        self.assertEqual(text[0], '# mine')
        self.assertIn('key.circle=Left Ctrl', text)
        self.assertIn('upscaler=fsr', text)
        self.assertFalse([line for line in text if line.startswith('pad.cross')])

    def test_a_new_file_gets_the_launchers_settings(self):
        window = self.opened(app={'fullscreen': True})
        launcher.Launcher.collect(window)
        text = self.lines()
        self.assertIn('window_mode=borderless', text)  # the old launcher switch, moved once
        for key, value in launcher.INI_DEFAULTS.items():
            self.assertIn(f'{key}={value}' if key not in ('window_mode', 'hdr') else key, '\n'.join(text))
        self.assertFalse([line for line in text if line.startswith(('key.', 'pad.'))])  # defaults: no lines

    def test_a_missing_setting_is_added_without_touching_the_controls(self):
        self.path.write_text('pad.cross=b\n', encoding='utf-8')
        window = self.opened()
        launcher.Launcher.collect(window)
        text = self.lines()
        self.assertEqual(text[0], 'pad.cross=b')
        self.assertIn('upscaler=' + launcher.INI_DEFAULTS['upscaler'], text)


@unittest.skipUnless(sys.platform == 'win32', 'the Windows launcher')
class UpdateKeepsSettingsTests(unittest.TestCase):
    def test_the_update_copy_skips_bbport_ini(self):
        """install_update copies the new version over the old one, bbport.ini of the old one stays."""
        with tempfile.TemporaryDirectory() as folder:
            new, old = Path(folder) / 'new', Path(folder) / 'old'
            new.mkdir()
            old.mkdir()
            (new / 'bbport.ini').write_text('pad.cross=a\n', encoding='utf-8')
            (new / 'run.py').write_text('new\n', encoding='utf-8')
            (old / 'bbport.ini').write_text('pad.cross=b\nkey.circle=F\n', encoding='utf-8')
            with mock.patch.object(launcher, 'PORT_DIR', new):
                launcher.shutil.copytree(new, old, dirs_exist_ok=True, ignore=launcher.update_ignore(old))
            self.assertEqual((old / 'bbport.ini').read_text(encoding='utf-8'), 'pad.cross=b\nkey.circle=F\n')
            self.assertEqual((old / 'run.py').read_text(encoding='utf-8'), 'new\n')


class SetupKeepsSettingsTests(unittest.TestCase):
    def test_the_setup_never_installs_the_players_files(self):
        """The setup (a fresh install and the launcher's silent update) copies the release folder over the
        install: even a release that carried a bbport.ini by mistake leaves the player's in place."""
        source = (REPO / 'tools' / 'release' / 'setup.iss').read_text(encoding='utf-8')
        files = [line for line in source.splitlines() if line.startswith('Source: "{#SourceDir}')]
        self.assertEqual(len(files), 1, files)
        excludes = re.search(r'Excludes:\s*"([^"]*)"', files[0])
        self.assertIsNotNone(excludes, files[0])
        names = {name.strip().lower() for name in excludes.group(1).split(',')}
        self.assertTrue({'bbport.ini', 'mods.json', 'patches.json'} <= names, names)
        self.assertNotIn('[InstallDelete]', source)  # nothing of an older install is deleted first

    def test_the_release_check_refuses_a_bbport_ini(self):
        sys.path.insert(0, str(REPO / 'tools' / 'release'))
        import check_release
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'bbport.ini').write_text('pad.cross=a\n', encoding='utf-8')
            self.assertTrue([p for p in check_release.check(Path(folder)) if 'bbport.ini' in p])


if __name__ == '__main__':
    unittest.main()
