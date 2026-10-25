"""Point the frozen Windows launcher at its explicitly bundled Tcl/Tk data."""

import os
import sys


bundle = getattr(sys, "_MEIPASS", os.path.dirname(sys.executable))
os.environ.setdefault("TCL_LIBRARY", os.path.join(bundle, "_tcl_data"))
os.environ.setdefault("TK_LIBRARY", os.path.join(bundle, "_tk_data"))
