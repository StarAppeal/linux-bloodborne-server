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
    import bbport_upnp
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
                      mock.patch.object(launcher, 'fetch_patch_notes', return_value=None),
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
        self.app.patch_notes = []
        self.app.render_news(news)
        self.settle()
        texts = self.home_texts()
        for expected in ('Patch 9', 'Hello', 'One', 'Two'):
            self.assertIn(expected, texts)
        self.assertNotIn('Text', texts, 'Kaniel 2026-10-20: no info cards under the banner')

    def home_texts(self):
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
        return texts

    def test_home_lists_every_releases_patch_notes(self):
        # Kaniel 2026-10-20: the latest release's notes in the banner, then every earlier one in a row
        news = launcher.parse_news({'title': 'Old patch', 'version': '0.3.4', 'intro': 'Old intro', 'notes': ['Old']})
        self.app.patch_notes = [
            {'version': '1.0.0', 'date': '20 Oct 2026', 'notes': ['Chalices together'], 'known': ['Root chalices']},
            {'version': '0.3.18-local', 'date': '19 Oct 2026', 'notes': ['Crash reports'], 'known': []},
            {'version': '0.1.0', 'date': '13 Oct 2026', 'notes': ['First co-op'], 'known': []}]
        self.app.render_news(news)
        self.settle()
        texts = self.home_texts()
        joined = '\n'.join(t for t in texts if isinstance(t, str))
        for expected in ("What's new in v1.0.0", 'Chalices together', 'Known issue: Root chalices', 'Crash reports',
                         'First co-op'):
            self.assertIn(expected, joined)
        for stale in ('Old patch', 'Old intro', 'Old'):
            self.assertNotIn(stale, texts, 'the website text is about another release')
        self.assertLess(joined.index('Crash reports'), joined.index('First co-op'), 'newest first')
        news = launcher.parse_news({'title': 'Together 1.0', 'version': '1.0.0', 'intro': 'Intro 1.0'})
        self.app.render_news(news)
        self.settle()
        texts = self.home_texts()
        self.assertIn('Together 1.0', texts)
        self.assertIn('Intro 1.0', texts)

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
                      mock.patch.object(launcher, 'fetch_patch_notes', return_value=None),
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
        self.assertEqual(self.app.coop_next.state, 'disabled')
        self.app.close_coop()
        self.settle()
        self.assertTrue(cover.winfo_ismapped(), 'Done does nothing while settings are missing')
        self.app.coop_vars['game'].set(str(self.game))
        self.app.coop_vars['host_ip'].set('26.1.2.3')
        self.settle()
        self.app.coop_next.invoke()
        self.settle()
        self.assertEqual(self.app.coop_step, 1)
        self.assertEqual(self.app.coop_next.state, 'disabled', 'the enemies are chosen, not taken by default')
        self.assertIn('the enemies', self.app.coop_missing.cget('text'))
        self.app.coop_enemies.current(0)
        self.app.coop_enemies.event_generate('<<ComboboxSelected>>')
        self.settle()
        self.app.coop_next.invoke()
        self.settle()
        self.assertTrue(self.app.coop_done.canvas.winfo_ismapped(), 'Done on the last step')
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


    def test_play_never_starts_the_server(self):
        """Kaniel 2026-10-21: one server, started only with Start server; PLAY on the host's PC no longer
        starts it (players did not know it ran)."""
        scratch = Path(self.tmp.name)
        settings = dict(bbport_coop.DEFAULTS, host_ip='26.1.2.3', account='Lisara', password='pw')
        with mock.patch.object(launcher, 'PORT_DIR', scratch), \
                mock.patch.object(bbport_coop, 'is_this_pc', return_value=True), \
                mock.patch.object(bbport_coop, 'servers_running', return_value=False), \
                mock.patch.object(bbport_coop, 'start_servers') as start, \
                mock.patch.object(bbport_coop, 'needs_prepare', return_value=False), \
                mock.patch.object(bbport_coop, 'view_root', return_value=scratch), \
                mock.patch.object(bbport_coop, 'copy_events'), \
                mock.patch.object(self.app, 'coop_firewall', return_value=True), \
                mock.patch.object(self.app, 'coop_start_game') as start_game:
            self.app.coop_prepare_and_play(settings)
            self.settle()
        start.assert_not_called()
        start_game.assert_called_once()
        self.assertIsNone(start_game.call_args[0][2], 'the game still starts')

    def test_the_server_line_says_whether_the_server_runs(self):
        """Reddit 2026-10-21: the line beside Start server stayed empty while the server ran."""
        # The check the launcher started on its own asks the real port (0.3 s on Windows when nothing
        # listens); its answer must land before the mocked ones, not after.
        end = time.time() + 5
        while self.app.server_checking and time.time() < end:
            self.settle(0.05)
        self.settle()
        self.app.coop['host_ip'] = '26.1.2.3'
        with mock.patch.object(bbport_coop, 'is_this_pc', return_value=True):
            with mock.patch.object(bbport_coop, 'server_state', return_value='running'):
                self.app.coop_check_server()
                self.settle()
            self.assertEqual(self.app.coop_server_line.cget('text'), 'Running')
            # 2026-10-22: the server ran while nothing answered the game's port 20443, and the host's
            # game failed to acquire the server information under a green "Running".
            with mock.patch.object(bbport_coop, 'server_state', return_value='no_backend'):
                self.app.coop_check_server()
                self.settle()
            self.assertEqual(self.app.coop_server_line.cget('text'), bbport_coop.NO_BACKEND_TEXT)
            with mock.patch.object(bbport_coop, 'server_state', return_value='stopped'):
                self.app.coop_check_server()
                self.settle()
            self.assertEqual(self.app.coop_server_line.cget('text'), 'Not running: press Start server to host')
        with mock.patch.object(bbport_coop, 'is_this_pc', return_value=False), \
                mock.patch.object(bbport_coop, 'server_state', return_value='stopped'):
            self.app.coop_check_server()
            self.settle()
        self.assertEqual(self.app.coop_server_line.cget('text'), '', "a guest's server is the host's")

    def test_start_server_opens_the_router_and_the_help_shows_the_internet_address(self):
        """UPnP (Kaniel 2026-10-21): without a VPN, Start server asks the router to open the ports and says
        what came of it; the help shows the internet address to give the friends, and Stop closes them."""
        self.app.coop_vars['host_ip'].set('192.168.4.25')
        self.settle()
        opened = {'state': 'open', 'public': '203.0.113.7', 'local': '192.168.4.25', 'failed': []}
        with mock.patch.object(bbport_coop, 'is_this_pc', return_value=True), \
                mock.patch.object(bbport_coop, 'start_servers', return_value=[]), \
                mock.patch.object(bbport_coop, 'servers_running', return_value=True), \
                mock.patch.object(bbport_upnp, 'open_ports', return_value=opened) as open_ports, \
                mock.patch.object(self.app, 'coop_firewall', return_value=True):
            self.app.coop_start_servers()
            self.settle(1.0)
        open_ports.assert_called_once()
        self.assertIn('203.0.113.7', self.app.status.cget('text'))
        with mock.patch.object(bbport_coop, 'vpn_addresses', return_value=[]):
            self.app.open_coop_help()
            self.settle()
        self.assertEqual(self.app.coop_help_public.cget('text'),
                         'Without a VPN, friends type your internet address: 203.0.113.7')
        self.assertTrue(self.app.coop_help_copy.canvas.winfo_ismapped())
        self.app.coop_help_copy.invoke()
        self.assertEqual(self.root.clipboard_get(), '203.0.113.7')
        bottom = self.app.coop_help.winfo_rooty() + self.app.coop_help.winfo_height()
        self.assertLessEqual(bottom, self.root.winfo_rooty() + self.root.winfo_height(), 'fits in the window')
        with mock.patch.object(bbport_upnp, 'close_ports', return_value=4) as close_ports, \
                mock.patch.object(bbport_coop, 'stop_servers', return_value=1):
            self.app.coop_stop_servers()
            self.settle(1.0)
        close_ports.assert_called_once()
        self.assertFalse(self.app.coop_help_copy.canvas.winfo_ismapped(), 'no address once the ports are closed')
        # Behind carrier-grade NAT the host is told the friends need the VPN.
        cgnat = {'state': 'cgnat', 'public': '100.72.1.9', 'local': '192.168.4.25', 'failed': []}
        with mock.patch.object(bbport_coop, 'is_this_pc', return_value=True), \
                mock.patch.object(bbport_coop, 'start_servers', return_value=[]), \
                mock.patch.object(bbport_coop, 'servers_running', return_value=True), \
                mock.patch.object(bbport_upnp, 'open_ports', return_value=cgnat), \
                mock.patch.object(self.app, 'coop_firewall', return_value=True):
            self.app.coop_start_servers()
            self.settle(1.0)
        self.assertIn('Radmin VPN', self.app.status.cget('text'))
        # On a VPN address the router is left alone.
        self.app.coop_vars['host_ip'].set('26.12.34.56')
        self.settle()
        with mock.patch.object(bbport_coop, 'is_this_pc', return_value=True), \
                mock.patch.object(bbport_coop, 'start_servers', return_value=[]), \
                mock.patch.object(bbport_coop, 'servers_running', return_value=True), \
                mock.patch.object(bbport_upnp, 'open_ports') as open_ports, \
                mock.patch.object(self.app, 'coop_firewall', return_value=True):
            self.app.coop_start_servers()
            self.settle(1.0)
        open_ports.assert_not_called()

    def test_the_setup_is_three_steps(self):
        """Kaniel 2026-10-25: steps instead of switches: the game and the host's IP, then how the enemies get
        harder (Auto or Fixed, and Rot), then how the team travels; a step opens only once the ones before it
        lack nothing, and the help buttons stay on the first."""
        app = self.app
        self.assertEqual(app.coop_step, 0)
        self.assertTrue(app.coop_steps[0].winfo_ismapped())
        self.assertFalse(app.coop_back.canvas.winfo_ismapped(), 'no Back on the first step')
        app.coop_go(1)
        self.settle()
        self.assertEqual(app.coop_step, 0, 'no game folder or IP yet')
        app.coop_vars['game'].set(str(self.game))
        app.coop_vars['host_ip'].set('26.1.2.3')
        self.settle()
        app.coop_next.invoke()
        self.settle()
        self.assertEqual(app.coop_step, 1)
        self.assertTrue(app.coop_steps[1].winfo_ismapped() and not app.coop_steps[0].winfo_ismapped())
        self.assertEqual(app.coop_enemy_mode.get(), 'fixed', 'Auto is a host rule, off by default')
        app.coop_go(2)
        self.settle()
        self.assertEqual(app.coop_step, 1, 'the enemies are still to be chosen')
        app.coop_enemy_mode.set('auto')
        self.settle(0.5)
        self.assertEqual(app.coop['rule_scale'], '1', 'Auto is the scaling rule')
        self.assertEqual(str(app.coop_enemies.cget('state')), 'disabled', 'Auto takes its own levels')
        self.assertEqual(app.coop_next.state, 'normal', 'Auto needs no level')
        app.coop_next.invoke()
        self.settle()
        self.assertEqual(app.coop_step, 2)
        self.assertEqual(app.coop_travel_mode.get(), 'seamless', 'travelling together is off by default')
        app.coop_travel_mode.set('together')
        app.coop_vars['rule_vote'].set('1')
        self.settle(0.5)
        self.assertEqual(bbport_coop.host_rules_value(app.coop), 'travel,vote,scale')
        app.coop_back.invoke()
        app.coop_back.invoke()
        self.settle()
        self.assertEqual(app.coop_step, 0)
        app.open_coop()
        self.settle()
        self.assertEqual(app.coop_step, 0, 'opens at the first step')

    def test_the_upnp_button_asks_the_router_and_shows_the_answer(self):
        """Kaniel 2026-10-21 asked for a UPnP button in the help; Stop server closes what it opened."""
        with mock.patch.object(bbport_coop, 'vpn_addresses', return_value=[]):
            self.app.open_coop_help()
            self.settle()
        self.assertTrue(self.app.coop_help_router.canvas.winfo_ismapped())
        refused = {'state': 'none', 'public': '', 'local': '', 'failed': []}
        with mock.patch.object(bbport_upnp, 'open_ports', return_value=refused) as open_ports:
            self.app.coop_help_router.invoke()
            self.settle(1.0)
        open_ports.assert_called_once()
        self.assertIn('No router answered', self.app.coop_help_public.cget('text'))
        self.assertTrue(self.app.coop_help_router.canvas.winfo_ismapped(), 'it can be tried again')
        opened = {'state': 'open', 'public': '203.0.113.7', 'local': '192.168.4.25', 'failed': []}
        with mock.patch.object(bbport_upnp, 'open_ports', return_value=opened):
            self.app.coop_help_router.invoke()
            self.settle(1.0)
        self.assertIn('203.0.113.7', self.app.coop_help_public.cget('text'))
        self.assertTrue(self.app.coop_help_copy.canvas.winfo_ismapped())
        self.assertFalse(self.app.coop_help_router.canvas.winfo_ismapped())
        self.app.coop_vars['host_ip'].set('26.12.34.56')
        self.settle()
        with mock.patch.object(bbport_upnp, 'close_ports', return_value=3) as close_ports, \
                mock.patch.object(bbport_coop, 'stop_servers', return_value=0):
            self.app.coop_stop_servers()
            self.settle(1.0)
        close_ports.assert_called_once()

    def test_custom_enemies_take_the_players_own_multipliers(self):
        """Kaniel 2026-10-21: the presets were too specific; Custom shows HP, damage and poise fields."""
        self.app.coop_vars['game'].set(str(self.game))
        self.app.coop_vars['host_ip'].set('26.1.2.3')
        self.settle()
        self.app.coop_go(1)
        self.settle()
        box = self.app.coop_enemies
        custom = [v for v, _t in launcher.HARDER_CHOICES].index('custom')
        self.assertFalse(self.app.coop_custom.winfo_ismapped())
        box.current(custom)
        box.event_generate('<<ComboboxSelected>>')
        self.settle()
        self.assertTrue(self.app.coop_custom.winfo_ismapped())
        for v, rate in zip(self.app.coop_custom_vars, ('2', '1.25', '4')):
            v.set(rate)
        self.settle(0.5)
        self.assertEqual(self.app.coop_vars['harder'].get(), '2,1.25,4')
        self.assertEqual(self.app.coop['harder'], '2,1.25,4', 'saved')
        box.current(0)
        box.event_generate('<<ComboboxSelected>>')
        self.settle(0.5)
        self.assertFalse(self.app.coop_custom.winfo_ismapped())
        self.assertEqual(self.app.coop['harder'], 'off')

    def test_custom_enemies_are_kept_as_typed(self):
        """Kaniel 2026-10-26: players said Custom was not saved between sessions: a 1,5 typed became 1, a 15
        became 10 without the fields saying so, and a change typed right before Done or closing was lost."""
        self.app.coop_vars['game'].set(str(self.game))
        self.app.coop_vars['host_ip'].set('26.1.2.3')
        self.settle()
        self.app.coop_go(1)
        self.settle()
        box = self.app.coop_enemies
        box.current([v for v, _t in launcher.HARDER_CHOICES].index('custom'))
        box.event_generate('<<ComboboxSelected>>')
        self.settle()
        for v, rate in zip(self.app.coop_custom_vars, ('1,5', '15', '2')):
            v.set(rate)
        self.app.coop_flush()
        self.assertEqual(self.saved[-1]['harder'], '1.5,10,2', 'saved at once')
        self.app.coop_custom_tidy()
        self.assertEqual([v.get() for v in self.app.coop_custom_vars], ['1.5', '10', '2'], 'the fields show it')
        self.app.coop_custom_vars[0].set('4')
        with mock.patch.object(self.app, 'collect'), mock.patch.object(self.app.root, 'destroy'):
            self.app.close()
        self.assertEqual(self.saved[-1]['harder'], '4,10,2', 'closing right after typing keeps it')

    def test_host_rules_are_switches_off_by_default(self):
        """Kaniel 2026-10-25: the host rules in the co-op card, each off at first; the vote only goes with
        travelling together; what is switched on is saved."""
        for key in ('rule_rot', 'rule_travel', 'rule_vote', 'rule_scale'):
            self.assertEqual(self.app.coop_vars[key].get(), '', key)
        self.app.coop_vars['rule_scale'].set('1')
        self.settle(0.5)
        self.assertEqual(self.app.coop['rule_scale'], '1', 'saved')
        self.assertEqual(bbport_coop.host_rules_value(self.app.coop), 'scale')
        self.app.coop_vars['rule_vote'].set('1')
        self.settle(0.5)
        self.assertEqual(bbport_coop.host_rules_value(self.app.coop), 'scale', 'no vote without the trip')
        self.app.coop_vars['rule_travel'].set('1')
        self.settle(0.5)
        self.assertEqual(bbport_coop.host_rules_value(self.app.coop), 'travel,vote,scale')

    def test_boss_tempo_is_a_pick_vanilla_by_default(self):
        """Kaniel 2026-10-28: the boss tempo host rule, Vanilla until the player picks Aggressive or
        Relentless; the pick is saved."""
        self.assertEqual(self.app.coop_vars['rule_tempo'].get(), '')
        self.assertEqual(self.app.coop_tempo.get(), 'Vanilla: the game\'s own')
        self.app.coop_tempo.current(2)
        self.app.coop_tempo.event_generate('<<ComboboxSelected>>')
        self.settle(0.5)
        self.assertEqual(self.app.coop['rule_tempo'], 'relentless', 'saved')
        self.assertEqual(bbport_coop.host_rules_value(self.app.coop), 'relentless')
        self.app.coop_tempo.current(0)
        self.app.coop_tempo.event_generate('<<ComboboxSelected>>')
        self.settle(0.5)
        self.assertEqual(self.app.coop['rule_tempo'], '')

    def test_how_to_connect_shows_the_steps_and_gives_the_host_their_vpn_address(self):
        """Kaniel 2026-10-21: players got lost at the VPN step; the co-op card has a help button with the
        Radmin VPN steps, and the host can take this PC's Radmin address with one press."""
        card = self.app.coop_card
        self.assertTrue(card.winfo_ismapped())
        with mock.patch.object(bbport_coop, 'vpn_addresses', return_value=[('Radmin VPN', '26.12.34.56')]):
            self.app.open_coop_help()
            self.settle()
            self.assertFalse(card.winfo_ismapped(), 'the help takes the card\'s place')
            self.assertTrue(self.app.coop_help.winfo_ismapped())
            self.assertEqual(self.app.coop_help_address.cget('text'), "This PC's Radmin VPN address: 26.12.34.56")
            self.assertTrue(self.app.coop_help_use.canvas.winfo_ismapped())
            self.app.coop_help_use.invoke()
            self.settle()
            self.assertEqual(self.app.coop_vars['host_ip'].get(), '26.12.34.56')
            self.assertFalse(self.app.coop_help_use.canvas.winfo_ismapped(), 'nothing left to take')
        bottom = self.app.coop_help.winfo_rooty() + self.app.coop_help.winfo_height()
        self.assertLessEqual(bottom, self.root.winfo_rooty() + self.root.winfo_height(), 'fits in the window')
        with mock.patch.object(bbport_coop, 'vpn_addresses', return_value=[]):
            self.app.coop_help_refresh()
        self.assertIn('No Radmin VPN address', self.app.coop_help_address.cget('text'))
        self.app.close_coop_help()
        self.settle()
        self.assertTrue(card.winfo_ismapped())
        self.assertFalse(self.app.coop_help.winfo_ismapped())

    def test_the_side_column_shows_the_releases_version_not_the_ports(self):
        """Kaniel 2026-10-16: the version is the co-op release's (VERSION.txt), not the upstream fork's."""
        sub = self.app.side.canvas.itemcget(self.app.side.sub, 'text')
        self.assertNotIn(launcher.VERSION, sub)
        self.assertIn('development build', sub, 'a source tree has no release version')

    def test_check_for_updates_sits_in_the_bottom_bar_until_an_update_is_out(self):
        """Kaniel 2026-10-20: a Check for updates button; it says what it found, and the Update button takes
        its place once a newer release is out."""
        self.assertTrue(self.app.check_button.canvas.winfo_manager())
        with mock.patch.object(launcher, 'coop_release_version', return_value='1.0.0'), \
                mock.patch.object(bbport_coop, 'fetch_newest_release', return_value=None):
            self.app.coop_check_now()
            deadline = time.monotonic() + 5
            while self.app.check_button.state != 'normal' and time.monotonic() < deadline:
                self.settle()
        self.assertEqual(self.app.status.cget('text'), 'You have the latest version.')
        release = {'version': '1.0.1', 'setup': 'https://example.test/s.exe', 'sha256': 'https://example.test/s.sha256',
                   'page': '', 'notes': '', 'name': 's.exe'}
        with mock.patch.object(launcher, 'coop_release_version', return_value='1.0.0'), \
                mock.patch.object(bbport_coop, 'fetch_newest_release', return_value=release):
            self.app.coop_check_update(manual=True)
            self.settle()
        self.assertTrue(self.app.update_button.canvas.winfo_manager())
        self.assertFalse(self.app.check_button.canvas.winfo_manager())
        self.assertEqual(self.app.status.cget('text'), 'Version 1.0.1 is available.', 'not left on Checking…')

    def test_the_launcher_has_the_new_name(self):
        self.assertEqual(self.root.title(), 'Yharnam Together: Bloodborne Seamless Co-op')
        self.assertIn('Yharnam Together', self.app.side.canvas.itemcget(self.app.side.sub, 'text'))

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
