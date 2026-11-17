"""Ampere -> Maxwell lowering (tools/ampere/lower.py) on a SYNTHETIC shader, no game data.

A small Ampere vertex program is assembled here field by field (layout from Mesa NAK, as in test_sm86.py), lowered,
and run through the translator front end with the harness's IR interpreter next to a Maxwell program written by hand
for the same computation (encoded with tools/ampere/maxwell.py). `spirv_harness --equiv-vs` compares the positions
they output over many seeds. The reference is written independently of the lowering (different instruction forms,
its own register for the immediate), so a wrong field, operand order or branch offset in the lowering shows up as a
difference. Needs the harness build and the Ryubing checkout; skipped otherwise.

Run: private/venv/Scripts/python -m pytest tests/test_lower.py
"""
import os
import struct
import subprocess
import sys

import pytest

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(REPO, "tools", "ampere"))
HARNESS = os.path.join(REPO, "private", "build", "spirv_harness", "bin", "Release", "net10.0", "spirv_harness.exe")

pytestmark = pytest.mark.skipif(not os.path.exists(HARNESS) or not os.path.isdir(
    os.path.join(REPO, "private", "third_party", "ryubing")), reason="needs the spirv_harness build")


def a_word(fields):
    v = 0
    for lo, hi, val in fields:
        assert 0 <= val < (1 << (hi - lo)), (lo, hi, val)
        v |= val << lo
    return struct.pack("<QQ", v & (2**64 - 1), v >> 64)


def alu(op, form, dst, s0, extra=(), guard=7, gneg=0):
    return a_word([(0, 9, op), (9, 12, form), (12, 15, guard), (15, 16, gneg), (16, 24, dst), (24, 32, s0)]
                  + list(extra))


def cb(bank, off):  # constant buffer operand in bits 32..63
    return [(38, 54, off), (54, 59, bank)]


# SPH (words 0..19 identical in versions 3 and 4): vertex shader, reads attribute 0 xyzw, writes the position.
def sph(version):
    w = [0] * 32
    w[0] = 1 | (version << 5) | (1 << 10)
    w[6] = 0xF                  # imap: generic attribute 0, components xyzw
    w[13] = 0xF000              # omap system values: position x, y, z, w (attribute 0x70..0x7c)
    return struct.pack("<32I", *w)


def ampere_program():
    ins = [
        a_word([(0, 12, 0x321), (12, 15, 7), (16, 24, 0), (24, 32, 255), (32, 40, 255), (40, 50, 0x80),
                (74, 76, 3)]),                                                      # ALD.128 R0, a[0x80]
        alu(0x020, 5, 4, 0, cb(3, 0x0) + [(80, 81, 1), (84, 87, 4)]),               # FMUL R4 = R0 * c[3][0]
        alu(0x021, 2, 4, 4, [(32, 64, 0x3F800000), (64, 72, 255)]),                 # FADD R4 = R4 + 1.0
        alu(0x023, 4, 5, 1, [(32, 64, 0x40490FDB), (64, 72, 2)]),                  # FFMA R5 = R1 * pi + R2
        alu(0x002, 5, 6, 0, cb(3, 0x20) + [(72, 76, 0xF)]),                        # MOV R6 = c[3][0x20]
        alu(0x00B, 1, 0, 0, [(32, 40, 2), (76, 80, 4), (81, 84, 0), (84, 87, 7), (87, 90, 7)]),  # FSETP.GT P0 = x > z
        alu(0x020, 1, 6, 6, [(32, 40, 1), (63, 64, 1), (84, 87, 4)], guard=0),     # @P0 FMUL R6 = R6 * -R1
        alu(0x002, 1, 7, 0, [(32, 40, 255), (72, 76, 0xF)]),                       # MOV R7 = RZ
        a_word([(0, 12, 0x947), (12, 15, 0), (15, 16, 1), (34, 82, 4), (87, 90, 7)]),  # @!P0 BRA +16 (skip one)
        alu(0x021, 1, 7, 0, [(32, 40, 1)]),                                        # FADD R7 = R0 + R1
        a_word([(0, 12, 0x322), (12, 15, 7), (24, 32, 255), (32, 40, 4), (40, 50, 0x70), (64, 72, 255),
                (74, 76, 3)]),                                                      # AST.128 a[0x70] = R4..R7
        a_word([(0, 12, 0x94D), (12, 15, 7), (87, 90, 7)]),                        # EXIT
        a_word([(0, 12, 0x947), (12, 15, 7), (34, 82, (-4) & (2**48 - 1)), (87, 90, 7)]),  # BRA self
    ]
    code = sph(4) + b"".join(ins)
    ctrl = bytearray(0xE0)
    struct.pack_into("<II", ctrl, 0, 0x98761234, 0xC9)
    struct.pack_into("<I", ctrl, 0x38, len(code))
    struct.pack_into("<II", ctrl, 0x40, len(code), len(code))
    struct.pack_into("<I", ctrl, 0x4C, 16)
    return code, bytes(ctrl)


def maxwell_reference():
    """The same computation, written directly for Maxwell."""
    import maxwell as mx
    e = mx.enc
    words = [
        e("Ald", Dest=0, SrcA=255, SrcB=255, Imm11=0x80, AlSize=3),
        e("FmulC", Dest=4, SrcA=0, CbufSlot=3, CbufOffset=0, Fmz=1),
        e("Fadd32i", Dest=4, SrcA=4, Imm32=0x3F800000),
        e("Mov32i", Dest=10, Imm32=0x40490FDB, QuadMask=0xF),
        e("FfmaR", Dest=5, SrcA=1, SrcB=10, SrcC=2),
        e("MovC", Dest=6, CbufSlot=3, CbufOffset=8, QuadMask=0xF),
        e("FsetpR", SrcA=0, SrcB=2, FComp=4, Bop=0, SrcPred=7, DestPred=0, DestPredInv=7),
        e("FmulR", Dest=6, SrcA=6, SrcB=1, NegA=True, Pred=0),
        e("MovR", Dest=7, SrcA=255, QuadMask=0xF),
        None,  # @!P0 BRA over the FADD (filled below)
        e("FaddR", Dest=7, SrcA=0, SrcB=1),
        e("Ast", SrcA=255, SrcB=4, SrcC=255, Imm11=0x70, AlSize=3),
        e("Exit", Ccc=15), e("Exit", Ccc=15), e("Exit", Ccc=15),
    ]
    # lay out with a scheduling word wherever the block offset (0x30 + address) is a multiple of 32
    out, addr, addrs = [], 0x50, []
    for w in words:
        if (0x30 + addr) % 32 == 0:
            out.append(mx.sched_word())
            addr += 8
        addrs.append(addr)
        out.append(w)
        addr += 8
    bra_i = 9
    rel = addrs[bra_i + 2] - (addrs[bra_i] + 8)
    out[out.index(None)] = e("Bra", Imm24=rel & 0xFFFFFF, Pred=0, PredInv=True, Ccc=15)
    while (0x30 + addr) % 32:
        out.append(mx.sched_word() if (0x30 + addr) % 32 == 0 else e("Nop", Ccc=15))
        addr += 8
    import lower
    body = struct.pack(f"<{len(out)}Q", *out)
    blk = struct.pack("<I", 0x12345678) + bytes(0x2C) + lower.convert_sph(sph(4)) + body
    ctrl = bytearray(0x880)
    struct.pack_into("<I", ctrl, 0x708, 0x30)
    return blk, bytes(ctrl)


def test_synthetic_vertex_shader_is_equivalent(tmp_path):
    import lower
    code, ctrl = ampere_program()
    blk, nctrl, stats = lower.lower_block(code, ctrl, "vertex")
    ref, rctrl = maxwell_reference()
    paths = {}
    for name, data in (("a.code", blk), ("a.ctrl", nctrl), ("b.code", ref), ("b.ctrl", rctrl)):
        paths[name] = tmp_path / name
        paths[name].write_bytes(data)
    r = subprocess.run([HARNESS, "--equiv-vs", str(paths["a.code"]), str(paths["a.ctrl"]), str(paths["b.code"]),
                        str(paths["b.ctrl"]), "--trials", "64"], capture_output=True, text=True, timeout=120)
    assert r.returncode == 0 and "EQUIVALENT" in r.stdout, r.stdout + r.stderr


def test_lowering_refuses_what_it_does_not_understand():
    import lower
    code, ctrl = ampere_program()
    bad = bytearray(code)
    bad[0x80 + 16 * 1 + 12] |= 0x10  # set bit 100 of the FMUL: unexplained -> refused, never guessed
    with pytest.raises(lower.Unsupported):
        lower.lower_block(bytes(bad), ctrl, "vertex")
