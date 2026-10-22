# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for release_src.py: the reused release checkout's emulator test decision (Kaniel
2026-10-22: one release checkout, emulator tests only when the emulator changed).

Run: py -3.12 -m unittest discover -s tools\\release\\tests
"""
import contextlib
import io
import os
import pathlib
import shutil
import subprocess
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


@unittest.skipUnless(shutil.which('jj'), 'needs jj')
class RealJjTest(unittest.TestCase):
    """run_jj with a real checkout: jj names changed files relative to the current folder, so
    from anywhere else they came out as ..\\bb\\release-src\\emulator\\... and never matched
    (v1.1.3, 2026-10-22: EMULATOR_TESTS=skip after #335 changed emulator/)."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.root = self.tmp / 'checkout'
        self.root.mkdir()
        subprocess.run(['jj', 'git', 'init', str(self.root)], check=True, capture_output=True)
        tool.run_jj(self.root, 'config', 'set', '--repo', 'user.name', 'rig')
        tool.run_jj(self.root, 'config', 'set', '--repo', 'user.email', 'rig@example.com')
        (self.root / 'bbport').mkdir()
        (self.root / 'bbport' / 'run.py').write_text('x\n')
        tool.run_jj(self.root, 'commit', '-m', 'tested')
        self.stamp = self.tmp / 'tested-commit.txt'
        tool.mark(self.root, self.stamp)
        (self.root / 'emulator').mkdir()
        (self.root / 'emulator' / 'a.cpp').write_text('y\n')

    def test_a_changed_emulator_is_seen_from_another_folder(self):
        elsewhere = self.tmp / 'elsewhere'
        elsewhere.mkdir()
        here = pathlib.Path.cwd()
        os.chdir(elsewhere)
        self.addCleanup(os.chdir, here)
        needed, why = tool.decide(self.root, self.stamp)
        self.assertTrue(needed, why)
        self.assertIn('emulator/a.cpp', why)


class BuiltRevisionTest(unittest.TestCase):
    """revision-check: the port must name the release's commit (v1.1.3's first build named itself
    0000000: jj was not on the build shell's PATH and the release checkout has no .git)."""

    COMMIT = 'f59970dbeefa8f61f017ddea1ececc91c3e1ed6c'

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.header = self.tmp / 'bbport' / 'out' / 'gpu' / 'include' / 'bbcoop_revision.h'

    def built(self, revision):
        self.header.parent.mkdir(parents=True, exist_ok=True)
        self.header.write_text(f'#pragma once\n#define BB_SCM_REV "{revision}"\n')

    def test_the_release_commit_is_fine(self):
        self.built(self.COMMIT)
        self.assertIsNone(tool.built_revision_problem(self.tmp, self.COMMIT))

    def test_no_revision_is_refused(self):
        self.built('0000000')
        problem = tool.built_revision_problem(self.tmp, self.COMMIT)
        self.assertIn('0000000', problem)
        self.assertIn('jj', problem)

    def test_another_commit_or_dirty_is_refused_unless_allowed(self):
        self.built('c0ffee' + '0' * 34)
        self.assertIsNotNone(tool.built_revision_problem(self.tmp, self.COMMIT))
        self.built(self.COMMIT + '-dirty')
        self.assertIsNotNone(tool.built_revision_problem(self.tmp, self.COMMIT))
        self.assertIsNone(tool.built_revision_problem(self.tmp, self.COMMIT, allow_other=True))

    def test_a_port_never_built_is_refused(self):
        self.assertIn('no', tool.built_revision_problem(self.tmp, self.COMMIT))

    def test_cli_exits_non_zero_on_a_problem(self):
        self.built('0000000')
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = tool.main(['revision-check', '--root', str(self.tmp), '--commit', self.COMMIT])
        self.assertEqual(code, 1)
        self.assertTrue(out.getvalue().startswith('BUILD_REVISION=wrong '))
        self.built(self.COMMIT)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(tool.main(['revision-check', '--root', str(self.tmp), '--commit', self.COMMIT]), 0)

    def test_make_release_checks_the_built_revision(self):
        script = (ROOT / 'tools' / 'make-release.ps1').read_text(encoding='utf-8-sig')
        self.assertIn('revision-check', script)


@unittest.skipUnless(shutil.which('jj') and shutil.which('cmake'), 'needs jj and cmake')
class RevisionCmakeTest(unittest.TestCase):
    """bbcoop/revision.cmake finds jj off the PATH (the MSYS2 build shell starts from its own PATH)."""

    def test_a_jj_workspace_is_named_without_jj_on_the_path(self):
        tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp, True)
        repo = tmp / 'repo'
        subprocess.run(['jj', 'git', 'init', str(repo)], check=True, capture_output=True)
        tool.run_jj(repo, 'config', 'set', '--repo', 'user.name', 'rig')
        tool.run_jj(repo, 'config', 'set', '--repo', 'user.email', 'rig@example.com')
        (repo / 'a.txt').write_text('x\n')
        tool.run_jj(repo, 'commit', '-m', 'built')
        commit = tool.run_jj(repo, 'log', '-r', '@-', '--no-graph', '-T', 'commit_id').strip()
        # A second workspace has no .git, as C:\bb\release-src.
        workspace = tmp / 'workspace'
        tool.run_jj(repo, 'workspace', 'add', '--name', 'release', '-r', commit, str(workspace))
        header = tmp / 'bbcoop_revision.h'
        jj_dir = pathlib.Path(shutil.which('jj')).parent
        path = os.pathsep.join(entry for entry in os.environ['PATH'].split(os.pathsep)
                               if entry and pathlib.Path(entry) != jj_dir)
        env = dict(os.environ, PATH=path)
        if shutil.which('jj', path=path):
            self.skipTest('jj is in more than one PATH folder')
        subprocess.run([shutil.which('cmake'), f'-DOUT={header}', f'-DSOURCE={workspace}', '-P',
                        str(ROOT / 'bbport' / 'gpu' / 'bbcoop' / 'revision.cmake')],
                       check=True, capture_output=True, env=env)
        self.assertIn(f'"{commit}"', header.read_text())


class ReleaseSrcScriptTest(unittest.TestCase):
    def test_the_checkout_is_reused_not_made_again(self):
        script = (ROOT / 'tools' / 'release-src.ps1').read_text(encoding='utf-8-sig')
        self.assertIn('release-src', script)
        self.assertIn('workspace add', script)  # only the first time
        self.assertIn('jj -R $Root new', script)  # every later release: in place, so builds stay incremental
        self.assertIn('emulator-check', script)
        self.assertIn('emulator-mark', script)
        # It never deletes the checkout or its build output.
        self.assertNotRegex(script, r'Remove-Item[^\n]*\$(Root|out)\b')

    def test_readme_tells_release_builders_to_use_it(self):
        readme = (ROOT / 'README.md').read_text(encoding='utf-8')
        self.assertIn('tools\\release-src.ps1', readme)


class StaleCacheTest(unittest.TestCase):
    """A build folder configured from another checkout (rel112-src): CMake refuses it, so its cache
    goes; one configured from the release checkout stays, so its build stays incremental."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)

    def build_dir(self, name, source):
        folder = self.tmp / name
        (folder / 'CMakeFiles' / 'x.dir').mkdir(parents=True)
        (folder / 'CMakeCache.txt').write_text(
            f'# This is the CMakeCache file.\nCMAKE_HOME_DIRECTORY:INTERNAL={source}\n', encoding='utf-8')
        (folder / 'shadnet.exe').write_text('built', encoding='utf-8')
        return folder

    def test_another_checkouts_cache_is_cleared(self):
        folder = self.build_dir('release-server', 'C:/bb/rel112-src/server')
        cleared = tool.clear_stale_caches('C:\\bb\\release-src', [folder])
        self.assertEqual(cleared, [folder])
        self.assertFalse((folder / 'CMakeCache.txt').exists())
        self.assertFalse((folder / 'CMakeFiles').exists())
        self.assertTrue((folder / 'shadnet.exe').exists())  # the build output itself stays

    def test_the_release_checkouts_own_cache_stays(self):
        folder = self.build_dir('release-server', 'C:/bb/release-src/server')
        self.assertEqual(tool.clear_stale_caches('C:\\bb\\release-src', [folder]), [])
        self.assertTrue((folder / 'CMakeCache.txt').exists())

    def test_case_and_a_folder_that_only_starts_alike(self):
        same = self.build_dir('a', 'c:/BB/Release-Src/server/clientsample')
        other = self.build_dir('b', 'C:/bb/release-src-pr/server')
        self.assertEqual(tool.clear_stale_caches('C:\\bb\\release-src', [same, other]), [other])

    def test_cli_takes_the_folders_after_root_as_the_script_passes_them(self):
        folder = self.build_dir('release-server', 'C:/bb/rel112-src/server')
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = tool.main(['clear-stale-caches', '--root', 'C:\\bb\\release-src', str(folder), str(self.tmp / 'none')])
        self.assertEqual(code, 0)
        self.assertFalse((folder / 'CMakeCache.txt').exists())
        self.assertIn('release-server', out.getvalue())

    def test_missing_folders_are_fine(self):
        self.assertEqual(tool.clear_stale_caches('C:\\bb\\release-src', [self.tmp / 'none']), [])


class PinnedCloneTest(unittest.TestCase):
    def test_the_script_clones_the_patched_submodule_instead_of_linking_it(self):
        script = (ROOT / 'tools' / 'release-src.ps1').read_text(encoding='utf-8-sig')
        self.assertIn('fsr-vulkan', script)
        self.assertIn('ls-tree', script)  # the commit main pins
        self.assertIn('clear-stale-caches', script)

    def test_another_root_gets_its_own_workspace_and_leaves_the_kept_caches(self):
        script = (ROOT / 'tools' / 'release-src.ps1').read_text(encoding='utf-8-sig')
        self.assertIn('--name (Split-Path $Root -Leaf)', script)
        self.assertNotIn('--name release-src', script)
        # Clearing runs only for the default checkout: the kept build folders belong to it.
        clear = script.index('clear-stale-caches')
        self.assertIn('$Root -eq $default', script[script.rindex('if', 0, clear):clear])


if __name__ == '__main__':
    unittest.main()
