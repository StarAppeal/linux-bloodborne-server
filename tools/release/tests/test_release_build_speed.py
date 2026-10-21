# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""The release build's time savers (make-release.ps1, 2026-10-22): FSR 4 assets come from a
download cache shared by the checkouts and download in parallel, the release packages the port
without the zip and the playable copy it does not use, and Inno Setup compresses on several
threads. None of them changes what the release holds.

Run: py -3.12 -m unittest discover -s tools\\release\\tests
"""
import filecmp
import os
import pathlib
import shutil
import subprocess
import tempfile
import textwrap
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
FETCH = ROOT / 'bbport' / 'tools' / 'fetch_fsr4_assets.sh'
PACKAGE = ROOT / 'bbport' / 'packaging' / 'windows' / 'package.sh'
MAKE_RELEASE = ROOT / 'tools' / 'make-release.ps1'
SETUP_ISS = ROOT / 'tools' / 'release' / 'setup.iss'
ASSET_COUNT = 189  # 3 + 6 models x (3 + 2 tiers x 14 passes)

# A stand-in for curl: writes the URL's file name into the -o file, logs the call, and fails for
# the file named in FAKE_CURL_FAIL. It waits a little so parallel downloads overlap.
FAKE_CURL = textwrap.dedent('''\
    #!/usr/bin/env bash
    out=; url=
    while [[ $# -gt 0 ]]; do
        case $1 in
            -o) out=$2; shift 2 ;;
            -*) shift ;;
            *) url=$1; shift ;;
        esac
    done
    name=${url##*/}
    echo "$name" >> "$FAKE_CURL_LOG"
    touch "$FAKE_CURL_RUNNING/$name"
    sleep 0.05
    ls "$FAKE_CURL_RUNNING" | wc -l >> "$FAKE_CURL_LOG.together"
    rm -f "$FAKE_CURL_RUNNING/$name"
    [[ $name == "${FAKE_CURL_FAIL:-}" ]] && exit 22
    printf 'asset %s\\n' "$name" > "$out"
''')


@unittest.skipIf(os.name == 'nt' or not shutil.which('bash'), 'runs the shell script with a POSIX bash')
class FetchFsr4AssetsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        bin_dir = self.tmp / 'bin'
        bin_dir.mkdir()
        (bin_dir / 'curl').write_text(FAKE_CURL, encoding='utf-8')
        (bin_dir / 'curl').chmod(0o755)
        (self.tmp / 'running').mkdir()
        self.log = self.tmp / 'curl.log'
        # No spirv-cross or glslang: the optimized passes are fsr4_optimize.sh's own business.
        self.env = {'PATH': f'{bin_dir}:/usr/bin:/bin', 'HOME': str(self.tmp),
                    'FAKE_CURL_LOG': str(self.log), 'FAKE_CURL_RUNNING': str(self.tmp / 'running')}

    def fetch(self, dest, cache=None, **extra):
        env = dict(self.env, BB_FSR4_DIR=str(dest), **extra)
        if cache:
            env['BB_FSR4_CACHE'] = str(cache)
        return subprocess.run(['bash', str(FETCH)], env=env, capture_output=True, text=True)

    def downloads(self):
        return self.log.read_text().split() if self.log.exists() else []

    def test_fills_the_cache_and_the_folder(self):
        dest, cache = self.tmp / 'a', self.tmp / 'cache'
        run = self.fetch(dest, cache)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(len(self.downloads()), ASSET_COUNT)
        self.assertEqual(len(list(dest.iterdir())), ASSET_COUNT)
        stored = next(cache.iterdir())  # one folder per pinned Q2RTX commit
        self.assertEqual(len(list(stored.iterdir())), ASSET_COUNT)
        names = sorted(p.name for p in dest.iterdir())
        _match, mismatch, errors = filecmp.cmpfiles(dest, stored, names, shallow=False)
        self.assertEqual((mismatch, errors), ([], []))
        self.assertIn(f'{ASSET_COUNT} files', run.stdout)
        self.assertIn(f'({ASSET_COUNT} downloaded)', run.stdout)

    def test_another_checkout_takes_them_from_the_cache(self):
        cache = self.tmp / 'cache'
        self.assertEqual(self.fetch(self.tmp / 'a', cache).returncode, 0)
        self.log.unlink()
        run = self.fetch(self.tmp / 'b', cache)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(self.downloads(), [])
        self.assertEqual(len(list((self.tmp / 'b').iterdir())), ASSET_COUNT)
        self.assertIn('(0 downloaded)', run.stdout)

    def test_without_a_cache_it_downloads_into_the_folder(self):
        dest = self.tmp / 'a'
        run = self.fetch(dest)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(len(self.downloads()), ASSET_COUNT)
        self.assertEqual(len(list(dest.iterdir())), ASSET_COUNT)
        self.assertEqual(sorted(p.name for p in self.tmp.iterdir()), ['a', 'bin', 'curl.log', 'curl.log.together', 'running'])

    def test_downloads_run_in_parallel(self):
        self.assertEqual(self.fetch(self.tmp / 'a', self.tmp / 'cache').returncode, 0)
        together = [int(n) for n in (self.tmp / 'curl.log.together').read_text().split()]
        self.assertGreater(max(together), 1)

    def test_a_failed_download_fails_and_stays_out_of_the_cache(self):
        bad = 'fsr4_model_v07_i8_balanced_1080_pass3.spv'
        dest, cache = self.tmp / 'a', self.tmp / 'cache'
        run = self.fetch(dest, cache, FAKE_CURL_FAIL=bad)
        self.assertNotEqual(run.returncode, 0)
        stored = next(cache.iterdir())
        self.assertFalse((stored / bad).exists())
        self.assertFalse((dest / bad).exists())
        self.assertEqual([p.name for p in stored.iterdir() if p.name.endswith('.part')], [])
        # The next run fetches only what is missing.
        self.log.unlink()
        run = self.fetch(dest, cache)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(self.downloads(), [bad])


class ReleasePackagingTest(unittest.TestCase):
    def test_make_release_shares_the_fsr4_download_cache(self):
        release = MAKE_RELEASE.read_text(encoding='utf-8-sig')
        self.assertIn('BB_FSR4_CACHE', release)
        self.assertIn('cache\\fsr4', release)

    def test_make_release_packages_without_the_zip(self):
        release = MAKE_RELEASE.read_text(encoding='utf-8-sig')
        self.assertIn('BB_PACKAGE_STAGE_ONLY=1', release)

    def test_package_stops_after_staging_when_asked(self):
        package = PACKAGE.read_text(encoding='utf-8')
        stop = package.index('BB_PACKAGE_STAGE_ONLY')
        # After the fsr4vk check (the package must still be checked) and before the zip.
        self.assertLess(package.index('an fsr4vk file ended up in the package'), stop)
        self.assertLess(stop, package.index('make_zip.py'))
        self.assertLess(stop, package.index('Refresh dist/bbport-windows'))

    def test_setup_compresses_on_several_threads(self):
        iss = SETUP_ISS.read_text(encoding='utf-8-sig')
        self.assertIn('Compression=lzma2', iss)
        self.assertIn('SolidCompression=yes', iss)
        self.assertIn('LZMAUseSeparateProcess=yes', iss)
        self.assertRegex(iss, r'(?m)^LZMANumBlockThreads=([2-9]|\d\d)\s*$')


if __name__ == '__main__':
    unittest.main()
