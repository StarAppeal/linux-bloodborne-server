"""The launcher's look (Kaniel 2026-10-16: "redesign launcher to not use bloodborne native images and look
more like a game"): it shows no picture of the game (the sce_sys cover art and icon of the player's copy),
and the pages are a game-style menu down the side, all of them in the Advanced view and Play alone in the
Simple one."""
import json
import sys
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))

if sys.platform == 'win32':
    import bbport_coop
    import bbport_launcher_win as launcher

# A 1x1 PNG: a valid picture wherever one is opened.
PNG = bytes.fromhex('89504e470d0a1a0a0000000d4948445200000001000000010806000000'
                    '1f15c4890000000d49444154789c6360f8cfc0f01f0005000201a3a9b0'
                    'e20000000049454e44ae426082')


@unittest.skipUnless(sys.platform == 'win32', 'the Windows launcher')
class LookTests(unittest.TestCase):
    def setUp(self):
        import tempfile
        import tkinter
        from tkinter import filedialog, messagebox, ttk
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        scratch = Path(self.tmp.name)
        self.game = scratch / 'CUSA03173'
        (self.game / 'sce_sys').mkdir(parents=True)
        for name in ('pic0.png', 'pic1.png', 'icon0.png'):
            (self.game / 'sce_sys' / name).write_bytes(PNG)
        (scratch / 'config').mkdir()
        (scratch / 'config' / 'settings.json').write_text(
            json.dumps({'check_updates': False, 'game_dir': str(self.game)}), encoding='utf-8')
        self.opened = []
        from PIL import Image
        real_open = Image.open

        def recording_open(path, *args, **kwargs):
            self.opened.append(str(path))
            return real_open(path, *args, **kwargs)

        for patch in (mock.patch.dict(launcher.os.environ, {'BB_CONFIG': str(scratch / 'bbport.ini'),
                                                            'BB_LAUNCHER_ANIMATIONS': '0'}),
                      mock.patch.object(launcher, 'CONFIG_FILE', scratch / 'config' / 'settings.json'),
                      mock.patch.object(launcher, 'CONFIG_DIR', scratch / 'config'),
                      mock.patch.object(launcher, 'DATA_DIR', scratch),
                      mock.patch.object(launcher, 'connected_displays', return_value=[]),
                      mock.patch.object(launcher, 'gpu_tool', return_value=(scratch / 'no-such.exe', {})),
                      mock.patch.object(launcher, 'LANG', 'en'),
                      mock.patch.object(launcher, 'COOP_INSTALL', False),
                      mock.patch.object(bbport_coop, 'search_game', return_value=None),
                      mock.patch.object(launcher, 'fetch_news', return_value=None),
                      mock.patch.object(Image, 'open', side_effect=recording_open)):
            patch.start()
            self.addCleanup(patch.stop)
        for _attempt in range(5):  # Tk now and then cannot find its scripts right after another window closed
            try:
                self.root = tkinter.Tk()
                break
            except tkinter.TclError:
                time.sleep(0.5)
        else:
            self.skipTest('no display for Tk')
        self.addCleanup(self.root.destroy)
        self.app = launcher.Launcher(self.root, tkinter, ttk, filedialog, messagebox)
        self.settle()

    def settle(self, seconds=0.6):
        end = time.time() + seconds
        while time.time() < end:
            self.root.update()
            time.sleep(0.02)

    def test_no_picture_of_the_game_is_shown(self):
        self.app.start_motion()
        self.settle()
        tk_files = [self.root.tk.call(name, 'cget', '-file') for name in self.root.tk.call('image', 'names')
                    if self.root.tk.call('image', 'type', name) == 'photo']
        game = str(self.game).lower()
        for path in self.opened + [str(f) for f in tk_files]:
            self.assertFalse(str(path).lower().startswith(game), f'game art shown: {path}')

    def test_dragging_moves_the_window_without_windows_move_loop(self):
        """Kaniel 2026-10-17: the release crashed (0xc0000409) on a press on an empty place, where the drag
        handed Windows its modal move loop inside a Tk callback. The launcher moves the window itself."""
        import inspect
        self.app.start_motion()
        self.settle()
        self.assertNotIn('SendMessage', inspect.getsource(launcher.WindowChrome))
        top = self.app.caption_buttons['min'].canvas.master
        x0, y0 = self.root.winfo_x(), self.root.winfo_y()
        top.event_generate('<ButtonPress-1>', x=40, y=10, rootx=x0 + 400, rooty=y0 + 10)
        top.event_generate('<B1-Motion>', x=140, y=60, rootx=x0 + 500, rooty=y0 + 60)
        top.event_generate('<ButtonRelease-1>', x=140, y=60, rootx=x0 + 500, rooty=y0 + 60)
        self.settle()
        self.assertEqual((self.root.winfo_x(), self.root.winfo_y()), (x0 + 100, y0 + 50))

    def test_the_side_menu_is_home_and_settings(self):
        """Kaniel 2026-10-16: no Simple/Advanced views; a short menu, every other page under Settings."""
        side = self.app.side
        self.assertEqual(side.visible(), ['home', 'settings'], 'Co-op only in the co-op release')
        self.assertEqual(self.app.current_page, 'home')
        self.assertFalse(hasattr(self.app, 'set_mode'))

    def test_settings_opens_its_tabs(self):
        side = self.app.side
        side.pick(side.entry_y(side.visible().index('settings')))
        self.settle()
        self.assertEqual(self.app.current_page, 'general')
        self.assertEqual(side.active, 'settings')
        self.assertTrue(self.app.subtabs.canvas.winfo_ismapped())
        self.app.show('graphics')
        self.settle()
        self.assertEqual(self.app.subtabs.active, 'graphics')
        side.pick(side.entry_y(0))
        self.settle()
        self.assertEqual(self.app.current_page, 'home')
        self.assertFalse(self.app.subtabs.canvas.winfo_ismapped())
        side.pick(side.entry_y(1))
        self.settle()
        self.assertEqual(self.app.current_page, 'graphics', 'Settings goes back to its last tab')

    def test_the_ready_check_is_icons_with_tooltips_in_the_bottom_bar(self):
        icons = self.app.checks
        self.assertEqual(set(icons), {'game', 'saves', 'gpu', 'fsr4', 'upscaler'})
        for key in ('game', 'saves', 'gpu', 'fsr4'):
            self.assertTrue(icons[key].canvas.winfo_ismapped(), key)
            self.assertTrue(icons[key].tip.text, key)
        self.assertFalse(icons['upscaler'].canvas.winfo_ismapped(), 'only once the upscaler was changed')
        self.assertEqual(icons['game'].state, '✗', 'no eboot.bin in the test folder')

    def test_the_home_banner_shows_the_websites_news(self):
        news = launcher.parse_news({'title': 'Patch 9', 'version': '9.9', 'intro': 'Hello',
                                    'notes': ['One', 'Two', 3], 'link': {'text': 'More', 'url': 'https://x.test/'},
                                    'info': [{'title': 'Tip', 'text': 'Text'}]})
        self.assertEqual(news['notes'], ['One', 'Two'])
        self.assertIsNone(launcher.parse_news({'title': ''}), 'not the file the launcher knows')
        self.assertIsNone(launcher.parse_news([1, 2]))
        self.assertIsNone(launcher.parse_news({'title': 'x', 'link': {'url': 'http://plain'}})['link'],
                          'only https links')
        self.app.render_news(news)
        self.settle()
        texts = []

        def walk(widget):
            for child in widget.winfo_children():
                try:
                    texts.append(child.cget('text'))
                except Exception:
                    pass
                if child.winfo_class() == 'Canvas':  # the title over the painted street
                    texts.extend(child.itemcget(item, 'text') for item in child.find_all()
                                 if child.type(item) == 'text')
                walk(child)
        walk(self.app.news_holder)
        for expected in ('Patch 9', 'Hello', 'One', 'Two', 'Text'):
            self.assertIn(expected, texts)

    def test_the_website_news_file_is_one_the_launcher_reads(self):
        site = Path(__file__).resolve().parents[2] / 'website' / 'public' / 'launcher' / 'news.json'
        news = launcher.parse_news(json.loads(site.read_text(encoding='utf-8')))
        self.assertIsNotNone(news)
        self.assertTrue(news['notes'])
        self.assertTrue(launcher.NEWS_URL.endswith('/launcher/news.json'))

    def test_the_window_is_tall_enough_for_the_menu(self):
        self.root.update_idletasks()
        self.app.fit_window()
        self.settle()
        self.assertGreaterEqual(self.root.winfo_height() + 2, min(self.app.side.min_height(),
                                                                  launcher.work_area((self.root.winfo_screenwidth(),
                                                                                      self.root.winfo_screenheight()))[3]
                                                                  - self.app.frame_extra()))


    def test_the_launcher_draws_its_own_window_buttons(self):
        """Kaniel 2026-10-16: the launcher's own minimize, maximize and close instead of the Windows caption."""
        import ctypes
        self.app.start_motion()
        self.settle()
        self.assertTrue(self.root.overrideredirect(), 'no Windows caption')
        self.assertEqual(set(self.app.caption_buttons), {'min', 'max', 'close'})
        user32 = ctypes.windll.user32
        user32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
        user32.GetWindowLongPtrW.argtypes = [ctypes.c_void_p, ctypes.c_int]
        exstyle = user32.GetWindowLongPtrW(self.app.chrome.hwnd(), -20)
        self.assertTrue(exstyle & 0x00040000, 'still a taskbar button (WS_EX_APPWINDOW)')
        before = self.root.geometry()
        self.app.caption_buttons['max'].command()
        self.settle(0.3)
        self.assertTrue(self.app.chrome.zoomed())
        self.assertNotEqual(self.root.geometry(), before)
        self.app.caption_buttons['max'].command()
        self.settle(0.3)
        self.assertEqual(self.root.geometry(), before, 'restored where it was')
        self.app.caption_buttons['min'].command()
        self.settle(0.3)
        self.assertTrue(user32.IsIconic(ctypes.c_void_p(self.app.chrome.hwnd())), 'minimized')
        user32.ShowWindow(ctypes.c_void_p(self.app.chrome.hwnd()), 9)



@unittest.skipUnless(sys.platform == 'win32', 'the Windows launcher')
class CoopSettingsTests(unittest.TestCase):
    """Kaniel 2026-10-16: the co-op settings open over the window at the first start, and the launcher
    stays behind them until the game folder, the host's IP and the enemies are set."""

    def setUp(self):
        import tempfile
        import tkinter
        from tkinter import filedialog, messagebox, ttk
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        scratch = Path(self.tmp.name)
        self.game = scratch / 'CUSA03173'
        (self.game / 'sce_sys').mkdir(parents=True)
        (self.game / 'eboot.bin').write_bytes(b'eboot')
        (scratch / 'config').mkdir()
        (scratch / 'config' / 'settings.json').write_text('{"check_updates": false}', encoding='utf-8')
        self.saved = []
        settings = dict(bbport_coop.DEFAULTS, game='', account='Lisara', password='pw')
        for patch in (mock.patch.dict(launcher.os.environ, {'BB_CONFIG': str(scratch / 'bbport.ini'),
                                                            'BB_LAUNCHER_ANIMATIONS': '0'}),
                      mock.patch.object(launcher, 'CONFIG_FILE', scratch / 'config' / 'settings.json'),
                      mock.patch.object(launcher, 'CONFIG_DIR', scratch / 'config'),
                      mock.patch.object(launcher, 'DATA_DIR', scratch),
                      mock.patch.object(launcher, 'connected_displays', return_value=[]),
                      mock.patch.object(launcher, 'gpu_tool', return_value=(scratch / 'no-such.exe', {})),
                      mock.patch.object(launcher, 'LANG', 'en'),
                      mock.patch.object(launcher, 'COOP_INSTALL', True),
                      mock.patch.object(launcher, 'fetch_news', return_value=None),
                      mock.patch.object(bbport_coop, 'load_settings', return_value=settings),
                      mock.patch.object(bbport_coop, 'save_settings',
                                        side_effect=lambda _i, s: self.saved.append(dict(s))),
                      mock.patch.object(bbport_coop, 'search_game', return_value=None),
                      mock.patch.object(bbport_coop, 'fetch_newest_release', return_value=None),
                      mock.patch.object(bbport_coop, 'find_game_folder',
                                        side_effect=lambda folder, depth=3: Path(folder) if folder and
                                        (Path(folder) / 'eboot.bin').is_file() else None)):
            patch.start()
            self.addCleanup(patch.stop)
        for _attempt in range(5):
            try:
                self.root = tkinter.Tk()
                break
            except tkinter.TclError:
                time.sleep(0.5)
        else:
            self.skipTest('no display for Tk')
        self.addCleanup(self.root.destroy)
        self.app = launcher.Launcher(self.root, tkinter, ttk, filedialog, messagebox)
        self.settle()

    def settle(self, seconds=0.6):
        end = time.time() + seconds
        while time.time() < end:
            self.root.update()
            time.sleep(0.02)

    def test_the_first_start_opens_the_co_op_settings_until_they_are_done(self):
        self.assertEqual(self.app.side.visible(), ['home', 'coop', 'settings'])
        cover = self.app.coop_cover
        self.assertTrue(cover.winfo_ismapped(), 'over the window at the first start')
        self.assertEqual(self.app.coop_done.state, 'disabled')
        self.app.close_coop()
        self.settle()
        self.assertTrue(cover.winfo_ismapped(), 'Done does nothing while settings are missing')
        self.app.coop_vars['game'].set(str(self.game))
        self.app.coop_vars['host_ip'].set('26.1.2.3')
        self.settle()
        self.assertEqual(self.app.coop_done.state, 'disabled', 'the enemies are chosen, not taken by default')
        self.assertIn('the enemies', self.app.coop_missing.cget('text'))
        self.app.coop_enemies.current(0)
        self.app.coop_enemies.event_generate('<<ComboboxSelected>>')
        self.settle()
        self.assertEqual(self.app.coop_done.state, 'normal')
        self.app.coop_done.invoke()
        self.settle()
        self.assertFalse(cover.winfo_ismapped())
        self.assertEqual(self.saved[-1]['confirmed'], '1')
        self.assertFalse(self.app.coop_needs_setup(), 'not again at the next start')
        # From the menu it opens again; the server buttons are in the bottom bar.
        side = self.app.side
        side.pick(side.entry_y(side.visible().index('coop')))
        self.settle()
        self.assertTrue(cover.winfo_ismapped())
        self.assertTrue(self.app.server_row.winfo_manager())


    def test_the_side_column_shows_the_releases_version_not_the_ports(self):
        """Kaniel 2026-10-16: the version is the co-op release's (VERSION.txt), not the upstream fork's."""
        sub = self.app.side.canvas.itemcget(self.app.side.sub, 'text')
        self.assertNotIn(launcher.VERSION, sub)
        self.assertIn('development build', sub, 'a source tree has no release version')

    def test_a_newer_release_shows_the_update_button_and_installs_with_the_switch(self):
        """Kaniel 2026-10-16: an Update button when a newer release is out; with the automatic update
        switch on, the update starts by itself at the launcher's start."""
        release = {'version': '0.3.4', 'setup': 'https://example.test/s.exe', 'sha256': 'https://example.test/s.sha256',
                   'page': '', 'notes': '', 'name': 's.exe'}
        self.assertFalse(self.app.update_button.canvas.winfo_manager(), 'nothing offered yet')
        with mock.patch.object(launcher, 'coop_release_version', return_value='0.3.3-local'),                 mock.patch.object(bbport_coop, 'fetch_newest_release', return_value=release),                 mock.patch.object(self.app, 'coop_install_update') as install:
            self.app.coop_check_update(manual=True)
            self.settle()
            self.assertTrue(self.app.update_button.canvas.winfo_manager())
            self.assertIn('0.3.4', self.app.update_button.text)
            install.assert_not_called()
            self.app.var('auto_update', 'app').set(True)
            self.app.coop_check_update(auto=True)
            self.settle()
            install.assert_called_once()
        with mock.patch.object(launcher, 'coop_release_version', return_value='0.3.4'),                 mock.patch.object(bbport_coop, 'fetch_newest_release', return_value=release):
            self.app.coop_check_update(manual=True)
            self.settle()
            self.assertIn('latest', self.app.update_line.cget('text'))

if __name__ == '__main__':
    unittest.main()


@unittest.skipUnless(sys.platform == 'win32', 'the Windows launcher')
class WheelTests(unittest.TestCase):
    """Kaniel 2026-10-18: Cross did nothing and R2 drank a vial. The wheel over the Controls page's gamepad
    dropdowns had picked another button for each one the pointer passed (pad.cross=b, pad.circle=x,
    pad.triangle=righttrigger): Tk's dropdown takes the wheel, and the page stopped scrolling over it."""
    setUp = LookTests.setUp
    settle = LookTests.settle

    def test_the_wheel_over_a_gamepad_dropdown_scrolls_the_page_not_the_button(self):
        self.app.show('controls', animate=False)
        self.settle(0.2)
        page = self.app.pages['controls']
        scrolled = []
        with mock.patch.object(page, 'scroll', side_effect=scrolled.append):
            for name in ('cross', 'circle', 'triangle'):
                box = self.app.control_rows[name]['pad']
                before = box.current()
                for delta in (-120, -120, 120):
                    box.event_generate('<MouseWheel>', delta=delta)
                self.settle(0.1)
                self.assertEqual(box.current(), before, name)
                self.assertNotIn(f'pad.{name}', self.app.ini, name)
        self.assertEqual(scrolled, [1, 1, -1] * 3, 'the page scrolls instead')
