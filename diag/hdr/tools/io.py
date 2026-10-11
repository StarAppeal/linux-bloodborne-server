import sys, pathlib, numpy as np
d = pathlib.Path(sys.argv[1])
S = np.fromfile(next(d.glob("f001_output_*_rgba16f.raw")), np.float16).reshape(1080, 1920, 4).astype(np.float32)
I = np.fromfile(next(d.glob("f001_input_*_rgba16f.raw")), np.float16).reshape(720, 1280, 4).astype(np.float32)
lu = lambda c: np.maximum(c[..., :3], 0) @ np.array([.2126, .7152, .0722], np.float32)
q = [50, 90, 99, 99.9, 99.99, 100]
print(d.name, "input720 luma", np.percentile(lu(I), q).round(3), " output1080", np.percentile(lu(S), q).round(3))
