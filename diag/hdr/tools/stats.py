import sys, pathlib, numpy as np
d = pathlib.Path(sys.argv[1])
W, H = 1920, 1080
def raw8(p):
    a = np.fromfile(p, np.uint8).reshape(H, W, 4).astype(np.float32) / 255
    return a
def f16(p, w, h):
    return np.fromfile(p, np.float16).reshape(h, w, 4).astype(np.float32)
def lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
def luma(c):
    return c[..., 0] * 0.2126 + c[..., 1] * 0.7152 + c[..., 2] * 0.0722
disp = next(d.glob("display_*.raw")); outp = next(d.glob("output_*.raw")); gainp = next(d.glob("hdr_gain_*.raw"))
g = np.fromfile(gainp, np.float16).reshape(H, W, 2).astype(np.float32)
D = raw8(disp); O = raw8(outp)
S = f16(next(d.glob("f001_output_*_rgba16f.raw")), W, H)
print(d.name)
for name, img in (("display", D), ("output", O)):
    print(f"  {name}: mean rgb {img[...,:3].mean((0,1)).round(3)} alpha mean {img[...,3].mean():.3f}")
ui = g[..., 1]
ld = luma(lin(D[..., :3]))
for lo, hi in ((0, .002), (.002, .05), (.05, .25), (.25, .8), (.8, 1.01)):
    m = (ld >= lo) & (ld < hi)
    if m.any(): print(f"  sdr luma {lo}-{hi}: {m.mean()*100:5.1f}% of px, ui>0.5 in {np.mean(ui[m]>0.5)*100:5.1f}%")
ls = luma(np.maximum(S[..., :3], 0))
p = lambda x: np.percentile(x, [50, 90, 99, 100]).round(4)
print("  scene16F luma p50/p90/p99/max", p(ls))
print("  sdr(display) linear luma p50/p90/p99/max", p(ld))
m = ld > 0.25
if m.any():
    r = ls[m] / np.maximum(ld[m], 1e-4)
    print(f"  px sdr>0.25: {m.mean()*100:.2f}%  scene/sdr median {np.median(r):.4f} p99 {np.percentile(r,99):.4f}")
mid = (ld > .03) & (ld < .3) & (ls > 1e-5)
if mid.any():
    k = np.exp(np.mean(np.log(ld[mid] / ls[mid])))
    print(f"  midtone exposure sdr/scene {k:.4f}; scene*k / sdr for sdr>0.25: median {np.median(ls[m]*k/np.maximum(ld[m],1e-4)) if m.any() else 0:.3f} p99 {np.percentile(ls[m]*k/np.maximum(ld[m],1e-4),99) if m.any() else 0:.3f}")
print(f"  gain max {g[...,0].max():.3f} ui mean {ui.mean():.3f}")
