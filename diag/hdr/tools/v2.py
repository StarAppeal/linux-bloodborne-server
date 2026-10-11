import sys, pathlib, numpy as np
from PIL import Image
d = pathlib.Path(sys.argv[1]); W, H = 1920, 1080
g = np.fromfile(next(d.glob("hdr_gain_*.raw")), np.float16).reshape(H, W, 2).astype(np.float32)
D = np.fromfile(next(d.glob("display_*.raw")), np.uint8).reshape(H, W, 4)
Image.fromarray(D[..., :3]).save(d / "display.png")
Image.fromarray((np.clip(g[..., 1], 0, 1) * 255).astype(np.uint8)).save(d / "ui_mask.png")
gain, ui = g[..., 0], g[..., 1]
print(d.name, f"gain max {gain.max():.2f}  >1.05 {np.mean(gain>1.05)*100:.2f}%  >2 {np.mean(gain>2)*100:.3f}%  UI(>0.5) {np.mean(ui>0.5)*100:.2f}%")
lum = D[..., :3].astype(np.float32).mean(-1) / 255
for name, (x0, y0, x1, y1) in {"HUD top-left (bars, items)": (100, 55, 340, 220), "HUD top-right (counters)": (1570, 60, 1790, 170)}.items():
    gi, ui_i, li = gain[y0:y1, x0:x1], ui[y0:y1, x0:x1], lum[y0:y1, x0:x1]
    bright = li > 0.5
    print(f"  {name}: UI {np.mean(ui_i>0.5)*100:.1f}% of box; bright px {bright.sum()}: gain max {gi[bright].max() if bright.any() else 0:.3f}, >1.05 {np.mean(gi[bright]>1.05)*100 if bright.any() else 0:.1f}%; whole box gain max {gi.max():.3f}")
ys, xs = np.where(gain > 2)
if len(xs): print(f"  gain>2 bbox x {xs.min()}-{xs.max()} y {ys.min()}-{ys.max()} ({len(xs)} px)")
