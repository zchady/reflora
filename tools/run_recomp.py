"""`python -m recomp ...` for any Python, the embeddable one included.

The embeddable distribution (Reflora's portable Python) takes sys.path only from its python312._pth: neither the
current directory nor PYTHONPATH is searched, so `-m recomp` cannot find the package. This puts the repository on
sys.path and runs recomp/__main__.py exactly as `-m` would (same arguments, run_name "__main__").
"""
import os
import runpy
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.argv[0] = "recomp"
runpy.run_module("recomp", run_name="__main__", alter_sys=True)
