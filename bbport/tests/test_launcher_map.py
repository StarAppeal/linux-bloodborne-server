"""Kaniel 2026-10-19 ("prerender should happen during setup/in launcher on visible place"): the launcher's
Map card counts the game's map areas and the ones whose 3D view is drawn in the saves folder, and the 3D
map setting is a bbport.ini key it writes."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))

if sys.platform == 'win32':
    import bbport_launcher_win as launcher


@unittest.skipUnless(sys.platform == 'win32', 'the Windows launcher')
class MapCardTests(unittest.TestCase):
    def test_areas_and_prepared(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            studio = root / 'dvdroot_ps4' / 'map' / 'MapStudio'
            studio.mkdir(parents=True)
            for name in ('m24_01_00_00', 'm24_01_00_01', 'm21_00_00_00', 'm33_00_00_00'):
                (studio / f'{name}.msb.dcx').write_bytes(b'')
            self.assertEqual(launcher.map_areas(root / 'dvdroot_ps4'), ['m21_00', 'm24_01', 'm33_00'])
            user = root / 'user'
            self.assertEqual(launcher.map_prepared(root / 'dvdroot_ps4', user), (0, 3))
            (user / 'map').mkdir(parents=True)
            (user / 'map' / 'm24_01_00_00_v1.bbmap').write_bytes(b'x')
            (user / 'map' / 'm21_00_00_00.bbmap').write_bytes(b'x')  # the flat picture alone is not the 3D view
            self.assertEqual(launcher.map_prepared(root / 'dvdroot_ps4', user), (1, 3))
            self.assertEqual(launcher.map_areas(root / 'missing'), [])

    def test_setting(self):
        self.assertEqual(launcher.INI_DEFAULTS['map_3d'], '0')
        # Kaniel 2026-10-21: the game's own camera over the map (BbSettings::Map3dGameCamera).
        self.assertEqual([value for value, _text in launcher.MAP_3D], ['0', '1', '2', '3'])
        self.assertEqual(launcher.MAP_3D[3][1], ('Game camera', 'Камера игры'))
        self.assertNotEqual(launcher.INI_DEFAULTS['map_3d'], '3')  # opt-in only (Kaniel 2026-10-21)


if __name__ == '__main__':
    unittest.main()
