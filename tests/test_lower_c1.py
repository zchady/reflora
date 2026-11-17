"""Constant-offset loads of c[1] (the block's inline constants) are folded into immediates by the lowering
(tools/ampere/lower.py, Lowerer._ldc). Wonder's Switch 2 effect compute shaders add such a constant (a byte offset)
to a buffer base read from c[0]; loaded, the translator took the constant as the storage-buffer pointer and the
runtime refused the shader."""
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools", "ampere"))
import lower  # noqa: E402


def lowerer(c1):
    lw = lower.Lowerer([], "compute", 32, c1)
    lw.out = lower.Out() if hasattr(lower, "Out") else lw.out
    lw.guard = (lower.PT, False)
    return lw


def words(lw):
    return [p for k, p in lw.out.items if k == "w"]


def test_c1_loads_become_immediates():
    c1 = struct.pack("<4I", 0x11, 0x160, 0x0, 0xDEADBEEF)
    lw = lowerer(c1)
    lw._ldc(5, lower.RZ, 1, 4, "64")  # 64-bit load of c[1][4]: two words
    w = words(lw)
    assert [n for n, _ in w] == ["Mov32i", "Mov32i"]
    assert (w[0][1]["Dest"], w[0][1]["Imm32"]) == (5, 0x160)
    assert (w[1][1]["Dest"], w[1][1]["Imm32"]) == (6, 0x0)


def test_other_banks_and_indexed_loads_are_not_folded():
    c1 = struct.pack("<4I", 1, 2, 3, 4)
    lw = lowerer(c1)
    lw._ldc(5, lower.RZ, 3, 0, "32")      # c[3]: the game's data, read at run time
    lw._ldc(6, 7, 1, 0, "32")             # c[1] indexed by a register: not a constant offset
    lw._ldc(8, lower.RZ, 1, 0x40, "32")   # past the block's constants: left to the runtime
    names = [n for n, _ in words(lw)]
    assert "Mov32i" not in names
