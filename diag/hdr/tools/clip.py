import sys, pathlib, numpy as np
d = pathlib.Path(sys.argv[1]); W, H = 1920, 1080
D = np.fromfile(next(d.glob("display_*.raw")), np.uint8).reshape(H, W, 4).astype(np.float32) / 255
S = np.fromfile(next(d.glob("f001_output_*_rgba16f.raw")), np.float16).reshape(H, W, 4).astype(np.float32)
I = np.fromfile(next(d.glob("f001_input_*_rgba16f.raw")), np.float16).reshape(720, 1280, 4).astype(np.float32)
lin = lambda c: np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
lu = lambda c: c[..., 0] * .2126 + c[..., 1] * .7152 + c[..., 2] * .0722
ld = lu(lin(D[..., :3])); ls = lu(np.maximum(S[..., :3], 0)); li = lu(np.maximum(I[..., :3], 0))
m = ld > 0.9
print(d.name, f"sdr>0.9: {m.mean()*100:.3f}% px", "scene luma there p10/p50/p90/max", np.percentile(ls[m], [10, 50, 90, 100]).round(3) if m.any() else "-",
      "| input720 max", round(float(li.max()), 3), "frac>1", round(float((li > 1).mean() * 100), 3), "%")
