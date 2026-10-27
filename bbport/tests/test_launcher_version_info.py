"""BLauncher.exe's version resource (packaging/windows/version_info.py). An unsigned PyInstaller exe that
says nothing about itself was flagged by Windows Defender's machine learning as
Trojan:Win32/Bearfoos.A!ml in v1.3.0 (player report 2026-10-26); the resource names the product, its
version and its license, as a properly built Windows program does."""
import ast
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

sys.path.insert(0, str(ROOT / 'packaging' / 'windows'))
import version_info  # noqa: E402


class LauncherVersionInfoTest(unittest.TestCase):
    def test_release_versions_become_four_numbers(self):
        self.assertEqual(version_info.numbers('1.3.1'), (1, 3, 1, 0))
        self.assertEqual(version_info.numbers('1.3.1-rc.2'), (1, 3, 1, 0))
        self.assertEqual(version_info.numbers(''), (0, 0, 0, 0))

    def test_the_file_is_one_python_expression_naming_the_launcher(self):
        text = version_info.text('1.3.1')
        ast.parse(text, mode='eval')  # PyInstaller eval()s it
        for value in ('Yharnam Together', 'BLauncher.exe', '1.3.1', 'GPL', 'filevers=(1, 3, 1, 0)'):
            self.assertIn(value, text)

    def test_pyinstaller_reads_it(self):
        try:
            from PyInstaller.utils.win32 import versioninfo
        except ImportError:
            self.skipTest('PyInstaller is not installed')
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'version.txt'
            version_info.write('1.3.1', path)
            info = versioninfo.load_version_info_from_text_file(str(path))
        self.assertEqual((info.ffi.fileVersionMS, info.ffi.fileVersionLS), (1 << 16 | 3, 1 << 16))
        self.assertIn('Yharnam Together', str(info))


if __name__ == '__main__':
    unittest.main()
