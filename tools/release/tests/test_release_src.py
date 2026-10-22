# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for release_src.py: the reused release checkout's emulator test decision (Kaniel
2026-10-22: one release checkout, emulator tests only when the emulator changed).

Run: py -3.12 -m unittest discover -s tools\\release\\tests
"""
import contextlib
import io
import pathlib
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import release_src as tool  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[3]


class EmulatorChangedTest(unittest.TestCase):
    def test_a_change_under_emulator_needs_the_tests(self):
        self.assertTrue(tool.emulator_changed(['bbport/run.py', 'emulator/src/core/linker.cpp']))

    def test_changes_elsewhere_do_not(self):
        self.assertFalse(tool.emulator_changed(['bbport/run.py', 'server/src/main.cpp', 'docs/x.md']))
        self.assertFalse(tool.emulator_changed([]))

    def test_the_emulator_build_script_counts_as_the_emulator(self):
        self.assertTrue(tool.emulator_changed(['tools/build-emu.ps1']))

    def test_windows_separators_and_a_name_that_only_starts_alike(self):
        self.assertTrue(tool.emulator_changed(['emulator\\CMakeLists.txt']))
        self.assertFalse(tool.emulator_changed(['emulator-notes.md']))


class DecisionTest(unittest.TestCase):
    """decide(): reads the stamp of the last tested commit and asks jj what changed since."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.stamp = self.tmp / 'out' / 'release-emu' / 'tested-commit.txt'
        self.asked = []

    def jj(self, changed=(), commit='c0ffee'):
        def run(root, *args):
            self.asked.append(args)
            if args[0] == 'log':
                return commit + '\n'
            if args[0] == 'diff':
                return ''.join(f'{path}\n' for path in changed)
            raise AssertionError(args)
        return run

    def test_no_stamp_means_the_tests_run(self):
        needed, why = tool.decide(self.tmp, self.stamp, self.jj())
        self.assertTrue(needed)
        self.assertIn('never', why)

    def test_unchanged_emulator_skips_them(self):
        tool.write_stamp(self.stamp, 'abc123')
        needed, why = tool.decide(self.tmp, self.stamp, self.jj(changed=['bbport/run.py']))
        self.assertFalse(needed)
        self.assertIn('abc123', why)
        diff = next(args for args in self.asked if args[0] == 'diff')
        self.assertIn('abc123', diff)
        self.assertIn('@', diff)

    def test_a_changed_emulator_runs_them_and_names_the_files(self):
        tool.write_stamp(self.stamp, 'abc123')
        needed, why = tool.decide(self.tmp, self.stamp, self.jj(changed=['emulator/a.cpp', 'bbport/b.py']))
        self.assertTrue(needed)
        self.assertIn('emulator/a.cpp', why)
        self.assertNotIn('bbport/b.py', why)

    def test_a_stamp_jj_cannot_diff_against_runs_them(self):
        tool.write_stamp(self.stamp, 'gone')

        def broken(root, *args):
            if args[0] == 'diff':
                raise tool.JjError('Revision "gone" doesn\'t exist')
            return 'c0ffee\n'
        needed, why = tool.decide(self.tmp, self.stamp, broken)
        self.assertTrue(needed)
        self.assertIn('gone', why)

    def test_mark_stores_the_checked_out_commit(self):
        commit = tool.mark(self.tmp, self.stamp, self.jj(commit='feedbeef'))
        self.assertEqual(commit, 'feedbeef')
        self.assertEqual(tool.read_stamp(self.stamp), 'feedbeef')
        log = next(args for args in self.asked if args[0] == 'log')
        self.assertIn('@-', log)

    def test_cli_prints_one_line_the_scripts_can_read(self):
        tool.write_stamp(self.stamp, 'abc123')
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = tool.main(['emulator-check', '--root', str(self.tmp), '--stamp', str(self.stamp)],
                             run_jj=self.jj(changed=[]))
        self.assertEqual(code, 0)
        self.assertTrue(out.getvalue().startswith('EMULATOR_TESTS=skip '))


class ReleaseSrcScriptTest(unittest.TestCase):
    def test_the_checkout_is_reused_not_made_again(self):
        script = (ROOT / 'tools' / 'release-src.ps1').read_text(encoding='utf-8-sig')
        self.assertIn('release-src', script)
        self.assertIn('workspace add', script)  # only the first time
        self.assertIn('jj -R $Root new', script)  # every later release: in place, so builds stay incremental
        self.assertIn('emulator-check', script)
        self.assertIn('emulator-mark', script)
        # It never deletes the checkout or its build output.
        self.assertNotIn('Remove-Item', script)

    def test_readme_tells_release_builders_to_use_it(self):
        readme = (ROOT / 'README.md').read_text(encoding='utf-8')
        self.assertIn('tools\\release-src.ps1', readme)


if __name__ == '__main__':
    unittest.main()
