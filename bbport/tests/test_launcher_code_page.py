"""Players with a Cyrillic Windows user name or game folder (Kaniel 2026-10-27): "the game doesn't start at all and
writes 0 logs". BLauncher.exe runs run.py and the preparation scripts itself (--run, --script). PyInstaller ignores
PYTHONIOENCODING, so their output was in the Windows code page: on a Russian Windows (1251) run.py read mods.py's
game view path as UTF-8, got '????' and the preparation stopped ("prepare failed: No such file"); a letter outside
the code page (Cyrillic on 1252) stopped the script with UnicodeEncodeError and PyInstaller's popup, no log. The
roles now write UTF-8 whatever the code page, and a failure of any role leaves its cause in a log."""
import io
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

LAUNCHER_DIR = Path(__file__).resolve().parents[1] / 'launcher'
sys.path.insert(0, str(LAUNCHER_DIR))

if sys.platform == 'win32':
    import bbport_launcher_win as launcher

CYRILLIC = 'C:\\Users\\\u0418\u0432\u0430\u043d\\\u0442\u0435\u0441\u0442'  # C:\Users\Иван\тест
OUTSIDE_1251 = '\u0395\u03bb\u03bb\u03b7\u03bd\u03b9\u03ba\u03ac \u6d4b\u8bd5'  # Greek and Chinese


def run_launcher(args, code_page, env=None):
    """The launcher started as BLauncher.exe starts: its output in `code_page`, as a frozen launcher's is
    (PYTHONIOENCODING stands in for the code page here; the frozen one ignores the variable)."""
    environment = {**os.environ, **(env or {}), 'PYTHONIOENCODING': code_page, 'PYTHONUTF8': '0'}
    return subprocess.run([sys.executable, str(LAUNCHER_DIR / 'bbport_launcher_win.py'), *args],
                          stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          env=environment, timeout=60)


@unittest.skipUnless(sys.platform == 'win32', 'the Windows launcher')
class CodePageTests(unittest.TestCase):
    def script(self, directory, body):
        path = Path(directory) / 'probe.py'
        path.write_text(textwrap.dedent(body), encoding='utf-8')
        return path

    def test_a_script_prints_a_cyrillic_path_in_utf8_on_a_russian_windows(self):
        with tempfile.TemporaryDirectory() as directory:
            probe = self.script(directory, 'import sys\nprint(sys.argv[1])\n')
            result = run_launcher(['--script', str(probe), CYRILLIC], 'cp1251')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.decode('utf-8').strip(), CYRILLIC)

    def test_a_script_prints_letters_outside_the_code_page(self):
        with tempfile.TemporaryDirectory() as directory:
            probe = self.script(directory, 'import sys\nprint(sys.argv[1])\nprint("x", file=sys.stderr)\n')
            result = run_launcher(['--script', str(probe), CYRILLIC + OUTSIDE_1251], 'cp1252')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.decode('utf-8').strip(), CYRILLIC + OUTSIDE_1251)

    def test_a_failing_script_writes_its_traceback_to_the_log_and_returns_1(self):
        # Frozen, an exception leaving the role became PyInstaller's modal popup and the log stayed empty.
        with tempfile.TemporaryDirectory() as directory:
            probe = self.script(directory, f'raise RuntimeError({CYRILLIC!r})\n')
            stderr = io.StringIO()
            with mock.patch.object(launcher, 'attach_stdio', lambda: None), \
                    mock.patch.object(sys, 'stderr', stderr), mock.patch.object(sys, 'argv', list(sys.argv)):
                code = launcher.run_role(['--script', str(probe)])
        self.assertEqual(code, 1)
        self.assertIn('RuntimeError', stderr.getvalue())
        self.assertIn(CYRILLIC, stderr.getvalue())


@unittest.skipUnless(sys.platform == 'win32', 'the Windows launcher')
class StartupFailureTests(unittest.TestCase):
    def test_a_failure_before_the_window_is_logged_and_shown(self):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory) / CYRILLIC.rsplit('\\', 1)[1]
            shown = []
            # tkinter that cannot start (Tcl's library not found): no window ever opens in this test.
            with mock.patch.object(launcher, 'DATA_DIR', data), \
                    mock.patch.dict(sys.modules, {'tkinter': None}), \
                    mock.patch.object(launcher, 'load_app_settings', lambda: dict(launcher.APP_DEFAULTS)), \
                    mock.patch.object(launcher, 'show_error_box', lambda text: shown.append(text), create=True), \
                    mock.patch.object(sys, 'argv', ['BLauncher.exe']):
                with self.assertRaises(SystemExit) as stop:
                    launcher.main()
            log = data / 'user' / 'log' / 'launcher-errors.txt'
            self.assertNotEqual(stop.exception.code, 0)
            self.assertIn('tkinter', log.read_text(encoding='utf-8'))
            self.assertEqual(len(shown), 1)
            self.assertIn(str(log), shown[0])

    def test_the_log_goes_to_temp_when_the_install_cannot_be_written(self):
        with tempfile.TemporaryDirectory() as directory:
            blocked = Path(directory) / 'blocked'
            blocked.write_text('a file where the folder would be', encoding='utf-8')
            temp = Path(directory) / 'temp'
            temp.mkdir()
            with mock.patch.object(launcher, 'DATA_DIR', blocked), \
                    mock.patch.object(launcher.tempfile, 'gettempdir', lambda: str(temp)):
                log = launcher.record_startup_failure('boom ' + CYRILLIC)
            self.assertEqual(log.parent, temp)
            self.assertIn('boom ' + CYRILLIC, log.read_text(encoding='utf-8'))

    def test_play_failures_are_logged_too(self):
        # Bloodborne.exe and Steam shortcuts start `BLauncher.exe --play`: no window to show a traceback in.
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory)
            shown = []
            settings = {**launcher.APP_DEFAULTS, 'game_dir': directory}
            (data / 'eboot.bin').write_bytes(b'')

            def broken(_settings):
                raise OSError('could not start run.py')
            with mock.patch.object(launcher, 'DATA_DIR', data), \
                    mock.patch.dict(sys.modules, {'tkinter': None}), \
                    mock.patch.object(launcher, 'load_app_settings', lambda: settings), \
                    mock.patch.object(launcher, 'play_without_window', broken), \
                    mock.patch.object(launcher, 'show_error_box', lambda text: shown.append(text), create=True), \
                    mock.patch.object(sys, 'argv', ['BLauncher.exe', '--play']):
                with self.assertRaises(SystemExit):
                    launcher.main()
            text = (data / 'user' / 'log' / 'launcher-errors.txt').read_text(encoding='utf-8')
            self.assertIn('could not start run.py', text)
            self.assertEqual(len(shown), 1)


if __name__ == '__main__':
    unittest.main()
