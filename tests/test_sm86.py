"""tools/ampere/sm86.py on SYNTHETIC instruction words, built field by field here (no shader code from a game).

Field positions follow Mesa NAK's encoder (sm70_encode.rs); see the module docstring.
Run: private/venv/Scripts/python -m pytest tests/test_sm86.py
"""
import os
import struct
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "ampere"))
import sm86  # noqa: E402


def word(fields):
    """fields: [(lo_bit, hi_bit, value)] -> (lo, hi) of a 128-bit instruction. Constant-buffer offsets are in bytes."""
    v = 0
    for lo, hi, val in fields:
        assert 0 <= val < (1 << (hi - lo))
        v |= val << lo
    return v & (2**64 - 1), v >> 64


def alu(op, form, dst=0, s0=0, extra=(), guard=7, gneg=0):
    return word([(0, 9, op), (9, 12, form), (12, 15, guard), (15, 16, gneg), (16, 24, dst), (24, 32, s0)] + list(extra))


def test_ffma_forms():
    i = sm86.decode(*alu(0x023, 1, dst=3, s0=1, extra=[(32, 40, 2), (64, 72, 4), (80, 81, 1)]))
    assert i.op == "FFMA" and (str(i.f["a"]), str(i.f["b"]), str(i.f["c"])) == ("R1", "R2", "R4") and i.f["ftz"]
    i = sm86.decode(*alu(0x023, 5, dst=3, s0=1, extra=[(38, 54, 0x50), (54, 59, 5), (64, 72, 4)]))
    assert str(i.f["b"]) == "c[0x5][0x50]" and str(i.f["c"]) == "R4"
    i = sm86.decode(*alu(0x023, 2, dst=3, s0=1, extra=[(32, 64, 0x3F800000), (64, 72, 4)]))
    assert str(i.f["b"]) == "R4" and str(i.f["c"]) == "0x3f800000"  # form 2: immediate in src2, src1 at 64
    i = sm86.decode(*alu(0x023, 3, dst=3, s0=1, extra=[(38, 54, 4), (54, 59, 2), (64, 72, 4)]))
    assert str(i.f["c"]) == "c[0x2][0x4]"
    i = sm86.decode(*alu(0x023, 6, dst=3, s0=1, extra=[(32, 38, 9), (64, 72, 4), (91, 92, 1)]))
    assert str(i.f["b"]) == "UR9"
    assert not i.unknown_bits


def test_fadd_second_source_slot():
    # FADD's second source is in src1 for registers and in src2 for immediates / constant buffers (NAK OpFAdd)
    i = sm86.decode(*alu(0x021, 1, dst=0, s0=1, extra=[(32, 40, 2), (63, 64, 1), (72, 73, 1)], guard=3, gneg=1))
    assert str(i.f["a"]) == "-R1" and str(i.f["b"]) == "-R2" and str(i.guard) == "!P3"
    i = sm86.decode(*alu(0x021, 3, dst=0, s0=1, extra=[(38, 54, 0x8C), (54, 59, 0xA), (63, 64, 1)]))
    assert str(i.f["b"]) == "-c[0xa][0x8c]"


def test_integer_fields_are_not_modifiers():
    # ISETP: bits 72/73 are .EX and .S32, not src0 negate/abs
    i = sm86.decode(*alu(0x00C, 1, s0=7, extra=[(32, 40, 1), (73, 74, 1), (76, 79, 6), (81, 84, 0), (84, 87, 7),
                                                 (87, 90, 7), (68, 71, 7)]))
    assert i.op == "ISETP" and i.f["signed"] and not i.f["ex"] and not i.f["a"].neg and i.f["cmp"] == "GE"
    # LOP3: bits 72..79 are the truth table
    i = sm86.decode(*alu(0x012, 4, dst=2, s0=1, extra=[(32, 64, 0x3FF), (64, 72, 255), (72, 80, 0xC0),
                                                        (81, 84, 7), (87, 90, 7), (90, 91, 1)]))
    assert i.f["lut"] == 0xC0 and not i.f["a"].neg and not i.unknown_bits


def test_mov_mufu_setp():
    i = sm86.decode(*alu(0x002, 5, dst=7, extra=[(38, 54, 0x48), (54, 59, 5), (72, 76, 0xF)]))
    assert i.op == "MOV" and str(i.f["src"]) == "c[0x5][0x48]" and i.f["lanes"] == 0xF
    i = sm86.decode(*alu(0x108, 1, dst=2, extra=[(32, 40, 3), (74, 78, 4)]))
    assert i.op == "MUFU" and i.f["fn"] == "RCP"
    i = sm86.decode(*alu(0x00B, 1, s0=1, extra=[(32, 40, 2), (76, 80, 1), (74, 76, 0), (81, 84, 2), (84, 87, 7),
                                                 (87, 90, 7)]))
    assert i.op == "FSETP" and i.f["cmp"] == "LT" and str(i.f["dst"]) == "P2"


def test_branches():
    # BRA: 48-bit signed offset at bits 34..81 in units of 4 bytes from the next instruction (NAK get_rel_offset)
    lo, hi = word([(0, 12, 0x947), (12, 15, 7), (34, 82, (-0x10) & (2**48 - 1)), (87, 90, 7)])
    assert sm86.decode(lo, hi, addr=0x100).f["target_rel"] == -0x40
    lo, hi = word([(0, 12, 0x947), (12, 15, 7), (34, 82, (-4) & (2**48 - 1)), (87, 90, 7)])
    assert sm86.decode(lo, hi).f["target_rel"] == -16  # the "BRA to itself" end marker
    lo, hi = word([(0, 12, 0x945), (12, 15, 7), (16, 20, 1), (34, 64, 0x20), (87, 90, 7)])
    i = sm86.decode(lo, hi, addr=0x20)
    assert i.op == "BSSY" and i.f["target_rel"] == 0x80 and i.f["bar"] == 1


def test_p2r_r2p_and_tmml():
    i = sm86.decode(*alu(0x004, 4, s0=1, extra=[(32, 64, 0x70), (76, 78, 3)]))
    assert i.op == "R2P" and i.f["mask"].v == 0x70 and i.f["byte"] == 3
    i = sm86.decode(*word([(0, 12, 0xB69), (12, 15, 7), (16, 24, 2), (24, 32, 0), (32, 40, 255), (40, 54, 8),
                           (54, 59, 2), (61, 64, 1), (64, 72, 255), (72, 76, 2)]))
    assert i.op == "TMML" and "fault" not in i.f and i.f["handle"] == 8 and i.f["dim"] == "2D"


def test_unknown_and_instructions():
    with pytest.raises(sm86.Unknown):
        sm86.decode(*word([(0, 12, 0x7FF)]))
    block = bytes(0x80) + struct.pack("<QQ", *alu(0x021, 1)) + bytes(16)
    got = list(sm86.instructions(block))
    assert [a for a, _, _ in got] == [0, 0x10]
    i = sm86.decode(*alu(0x021, 1, extra=[(100, 101, 1)]))
    assert i.unknown_bits == [100]  # set but not understood: reported, never silently ignored


def test_brx_jump_through_register():
    # BRX R2 with offset -(next): the register holds a program-relative address (a c[1] jump table entry)
    i = sm86.decode(0xFFFFFCB002007949, 0x000FEA000383FFFF, 0x340)
    assert i.op == "BRX" and i.f["src"].n == 2 and i.addr + 16 + i.f["target_rel"] == 0
    assert not i.unknown_bits


def test_switch2_forms_seen_in_wonder():
    # HADD2.F32 R8 = -RZ + R4.H0_H0 (f16 -> f32 of a packed TEX result)
    i = sm86.decode(0x20000004FF087230, 0x020FCA0000004100, 0)
    assert i.op == "HADD2" and i.f["f32"] and i.f["neg_a"] and i.f["swz_b"] == 2 and not i.unknown_bits
    # TEX ... f16 (bit 79): results packed two per register
    i = sm86.decode(0xB080080406047B60, 0x101F4400001E88FF, 0)
    assert i.op == "TEX" and i.f["f16"] and i.f["mask"] == 8 and not i.unknown_bits
    # IPA R41, a[UR4 + 0x84]
    i = sm86.decode(0x000000FF04297526, 0x000E6200080E0021, 0)
    assert i.op == "IPA" and i.f["attr"] == 0x84 and i.f["idx_ureg"].n == 4 and i.f["idx_ureg"].uniform
    # ULDC UR4, c[1][UR4]; BRX UR4 (uniform pair)
    i = sm86.decode(0x0040000004047ABB, 0x000FE40008000800, 0)
    assert i.op == "ULDC" and i.f["bank"] == 1 and i.f["offset_reg"].n == 4 and not i.unknown_bits
    i = sm86.decode(0xFFFFFF6004007958, 0x000FEA000B83FFFF, 0x90)
    assert i.op == "BRX" and i.f["src"].uniform and i.addr + 16 + i.f["target_rel"] == 0
