#!/usr/bin/env python3
"""Decoder for Ampere (SM86) shader instructions, as Switch 2 NVN 2 programs carry them.

Volta and later GPUs use 128-bit instructions. Every field position here was checked against Mesa NAK's encoder
(src/nouveau/compiler/nak/sm70_encode.rs, MIT, Copyright 2022 Collabora): written from it, no code copied. Opcodes
NAK never emits (I2FP, P2R/R2P, ...) are marked "inferred" and were checked against the Maxwell twins that Wonder's
package carries for the same shaders (WONDER_SWITCH2_PROGRESS.md, §5g).

Layout shared by most instructions:
  bits 0-11    opcode; for ALU instructions bits 0-8 are the operation and bits 9-11 the operand form
  bits 12-14   guard predicate, bit 15 its negation (P7 = PT, always)
  bits 16-23   destination register (R255 = RZ); uniform destination: bits 16-21 (UR63 = URZ)
  bits 24-31   source 0 register (neg 72, abs 73)
  form 1: src1 = register 32-39 (abs 62, neg 63), src2 = register 64-71 (abs 74, neg 75)
  form 4/5/6: src1 = immediate 32-63 / constant buffer / uniform register 32-37 (bit 91), src2 = register 64-71
  form 2/3/7: src2 = immediate / constant buffer / uniform register, src1 = register 64-71
  constant buffer: offset in bytes 38-53, bank 54-58, or bindless (bit 91) with the bank in a uniform register 32-37
  bits 105-127 scheduling (stall, barriers, yield, reuse): no meaning for a translation

decode() returns an Insn whose fields are named after NAK's IR; `unknown_bits` lists the bits (below 105) that were
set but not understood. lower.py refuses such instructions instead of guessing.

Usage: python tools/ampere/sm86.py stats <dir with manifest.tsv>...   coverage over real code (control +0x38)
       python tools/ampere/sm86.py dis <code.bin> [<ctrl.bin>]          disassembly
"""
from __future__ import annotations

import collections
import os
import struct
import sys
from dataclasses import dataclass, field

SPH_SIZE = 0x80
RZ, URZ, PT = 255, 63, 7
SCHED_LO = 105


# ---- operands ------------------------------------------------------------------------------------------------

@dataclass(frozen=True)
class Reg:
    n: int
    neg: bool = False      # FP: negate; integer: two's complement negate (or bitwise NOT for .X forms)
    abs: bool = False
    uniform: bool = False  # UR register

    def is_zero(self):
        return self.n == (URZ if self.uniform else RZ)

    def __str__(self):
        if self.uniform:
            s = "URZ" if self.n == URZ else f"UR{self.n}"
        else:
            s = "RZ" if self.n == RZ else f"R{self.n}"
        if self.abs:
            s = f"|{s}|"
        return ("-" if self.neg else "") + s


@dataclass(frozen=True)
class Imm:
    v: int

    def __str__(self):
        return f"{self.v:#x}"


@dataclass(frozen=True)
class CBuf:
    bank: int
    offset: int
    ureg: int | None = None  # bindless: bank index from a uniform register
    neg: bool = False
    abs: bool = False

    def __str__(self):
        b = f"UR{self.ureg}" if self.ureg is not None else f"{self.bank:#x}"
        s = f"c[{b}][{self.offset:#x}]"
        if self.abs:
            s = f"|{s}|"
        return ("-" if self.neg else "") + s


@dataclass(frozen=True)
class Pred:
    n: int
    neg: bool = False
    uniform: bool = False

    def is_true(self):
        return self.n == PT and not self.neg

    def __str__(self):
        name = ("UPT" if self.uniform else "PT") if self.n == PT else (f"UP{self.n}" if self.uniform else f"P{self.n}")
        return ("!" if self.neg else "") + name


@dataclass
class Insn:
    addr: int
    word: int
    op: str
    guard: Pred
    f: dict = field(default_factory=dict)
    unknown_bits: list = field(default_factory=list)

    def __str__(self):
        g = "" if self.guard.is_true() else f"@{self.guard} "
        parts = []
        for k, v in self.f.items():
            if isinstance(v, bool):
                if v:
                    parts.append(k)
            elif isinstance(v, (Reg, Imm, CBuf, Pred)):
                parts.append(f"{k}={v}")
            elif isinstance(v, int):
                parts.append(f"{k}={v:#x}" if v > 9 else f"{k}={v}")
            else:
                parts.append(f"{k}={v}")
        s = f"{g}{self.op} " + " ".join(parts)
        if self.unknown_bits:
            s += f"  ?bits{self.unknown_bits}"
        return s


class Unknown(Exception):
    pass


class _W:
    """The 128-bit instruction with a record of which bits a decoder looked at."""

    def __init__(self, v):
        self.v = v
        self.used = 0

    def bits(self, lo, hi):
        self.used |= ((1 << (hi - lo)) - 1) << lo
        return (self.v >> lo) & ((1 << (hi - lo)) - 1)

    def bit(self, n):
        return bool(self.bits(n, n + 1))

    def sbits(self, lo, hi):
        x = self.bits(lo, hi)
        return x - (1 << (hi - lo)) if x >> (hi - lo - 1) else x

    def reg(self, lo, neg=None, abs_=None):
        return Reg(self.bits(lo, lo + 8), self.bit(neg) if neg is not None else False,
                   self.bit(abs_) if abs_ is not None else False)

    def ureg(self, lo, neg=None, abs_=None):
        return Reg(self.bits(lo, lo + 6), self.bit(neg) if neg is not None else False,
                   self.bit(abs_) if abs_ is not None else False, uniform=True)

    def pred(self, lo, neg_bit=None, uniform=False):
        return Pred(self.bits(lo, lo + 3), self.bit(neg_bit) if neg_bit is not None else False, uniform)

    def cbuf(self):
        if self.bit(91):
            return CBuf(0, self.bits(38, 54), ureg=self.bits(32, 38), neg=self.bit(63), abs=self.bit(62))
        return CBuf(self.bits(54, 59), self.bits(38, 54), neg=self.bit(63), abs=self.bit(62))

    def unknown(self):
        rest = self.v & ~self.used & ((1 << SCHED_LO) - 1)
        return [b for b in range(SCHED_LO) if rest >> b & 1]


# ---- ALU operand slots ---------------------------------------------------------------------------------------

def _slots(w: _W, form: int, uniform: bool, m0=None, m2=None):
    """(A, B, C): the three operand slots of an ALU instruction.

    m0 / m2 say which modifier bits src0 (72 neg, 73 abs) and the register in bits 64..71 (75 neg, 74 abs) have for
    this operation: "fp" both, "neg" negation only, None neither. In many integer operations those bits are the
    operation's own fields (ISETP .EX/.S32, LOP3's table, SHF's type, IADD3 .X ...), as in NAK's encoder, where the
    operation sets them after encode_alu; reading them as modifiers there would be wrong."""
    def mods(kind, neg_bit, abs_bit):
        return (neg_bit if kind in ("fp", "neg") else None, abs_bit if kind == "fp" else None)

    n0, a0 = mods(m0, 72, 73)
    n2, a2 = mods(m2, 75, 74)
    if uniform:
        w.bit(91)  # all uniform ALU instructions set it
        a = w.ureg(24, n0, a0)
        c_reg = w.ureg(64, n2, a2)
        if form == 1:
            return a, w.ureg(32, 63, 62), c_reg
        if form == 4:
            return a, Imm(w.bits(32, 64)), c_reg
        if form == 2:
            return a, c_reg, Imm(w.bits(32, 64))
        raise Unknown(f"uniform ALU form {form}")
    a = w.reg(24, n0, a0)
    c_reg = w.reg(64, n2, a2)
    if form == 1:
        return a, w.reg(32, 63, 62), c_reg
    if form == 4:
        return a, Imm(w.bits(32, 64)), c_reg
    if form == 5:
        return a, w.cbuf(), c_reg
    if form == 6:
        w.bit(91)
        return a, w.ureg(32, 63, 62), c_reg
    if form == 2:
        return a, c_reg, Imm(w.bits(32, 64))
    if form == 3:
        return a, c_reg, w.cbuf()
    if form == 7:
        w.bit(91)
        return a, c_reg, w.ureg(32, 63, 62)
    raise Unknown(f"ALU form {form}")


# which modifier bits each operation has for src0 and for the register at 64..71 (see _slots)
MODS = {"FADD": ("fp", "fp"), "FMUL": ("fp", None), "FFMA": ("fp", "fp"), "FMNMX": ("fp", None),
        "FSET": ("fp", None), "FSETP": ("fp", None), "DADD": ("fp", "fp"), "DMUL": ("fp", None),
        "DFMA": ("fp", "fp"), "IADD3": ("neg", "neg"), "FSEL": ("fp", None),
        "IMAD": (None, "neg"), "IMAD.WIDE": (None, "neg"), "IMAD.HI": (None, "neg")}


SRC1_FORMS = (1, 4, 5, 6)   # the special operand (imm/cbuf/ureg) sits in src1
ROUND = {0: "RN", 1: "RM", 2: "RP", 3: "RZ"}
FCMP = {0: "F", 1: "LT", 2: "EQ", 3: "LE", 4: "GT", 5: "NE", 6: "GE", 7: "NUM", 8: "NAN", 9: "LTU", 10: "EQU",
        11: "LEU", 12: "GTU", 13: "NEU", 14: "GEU", 15: "T"}
ICMP = {0: "F", 1: "LT", 2: "EQ", 3: "LE", 4: "GT", 5: "NE", 6: "GE", 7: "T"}
BOOLOP = {0: "AND", 1: "OR", 2: "XOR"}
MUFU = {0: "COS", 1: "SIN", 2: "EX2", 3: "LG2", 4: "RCP", 5: "RSQ", 6: "RCP64H", 7: "RSQ64H", 8: "SQRT", 9: "TANH"}
MEMTYPE = {0: "U8", 1: "S8", 2: "U16", 3: "S16", 4: "32", 5: "64", 6: "128"}
TEXDIM = {0: "1D", 4: "ARRAY_1D", 1: "2D", 5: "ARRAY_2D", 2: "3D", 3: "CUBE", 7: "ARRAY_CUBE"}
LODMODE = {0: "AUTO", 1: "LZ", 2: "LB", 3: "LL", 4: "LC", 5: "LB.LC"}


def _alu(w, op, form, uniform):
    """Operands for the 2/3-source ALU operations, named as in NAK's IR."""
    A, B, C = _slots(w, form, uniform, *MODS.get(op, (None, None)))
    two_b = B if form in SRC1_FORMS else C  # 2-source ops whose 2nd source moves to src2 for non-registers
    f = {}
    if op == "FADD":
        f.update(a=A, b=two_b, sat=w.bit(77), rnd=ROUND[w.bits(78, 80)], ftz=w.bit(80))
        if form not in SRC1_FORMS:
            w.used |= 0  # B (bits 64-71) is RZ / don't care for these forms; still mark it read
    elif op == "FMUL":
        if form not in SRC1_FORMS:
            raise Unknown("FMUL form %d" % form)
        f.update(a=A, b=B, dnz=w.bit(76), sat=w.bit(77), rnd=ROUND[w.bits(78, 80)], ftz=w.bit(80),
                 pdiv=w.bits(84, 87))
    elif op == "FFMA":
        f.update(a=A, b=B, c=C, dnz=w.bit(76), sat=w.bit(77), rnd=ROUND[w.bits(78, 80)], ftz=w.bit(80))
    elif op in ("FMNMX",):
        f.update(a=A, b=two_b, min=w.pred(87, 90, uniform), ftz=w.bit(80))
    elif op == "FSET":
        f.update(a=A, b=two_b, cmp=FCMP[w.bits(76, 80)], ftz=w.bit(80), bop=BOOLOP.get(w.bits(74, 76), "?"),
                 p=w.pred(87, 90, uniform))
    elif op == "FSETP":
        f.update(a=A, b=two_b, cmp=FCMP[w.bits(76, 80)], ftz=w.bit(80), bop=BOOLOP.get(w.bits(74, 76), "?"),
                 dst=w.pred(81, None, uniform), dst1=w.pred(84, None, uniform), p=w.pred(87, 90, uniform))
    elif op in ("SEL", "FSEL"):
        f.update(a=A, b=two_b, cond=w.pred(87, 90, uniform))
    elif op == "IADD3":
        f.update(a=A, b=B, c=C, x=w.bit(74), cin0=w.pred(87, 90, uniform), cin1=w.pred(77, 80, uniform),
                 cout0=w.pred(81, None, uniform), cout1=w.pred(84, None, uniform))
    elif op in ("IMAD", "IMAD.WIDE", "IMAD.HI"):
        f.update(a=A, b=B, c=C, signed=w.bit(73), x=w.bit(74), cin=w.pred(87, 90, uniform),
                 cout=w.pred(81, None, uniform))
    elif op == "ISETP":
        f.update(a=A, b=two_b, cmp=ICMP[w.bits(76, 79)], signed=w.bit(73), ex=w.bit(72),
                 bop=BOOLOP.get(w.bits(74, 76), "?"), low=w.pred(68, 71, uniform), p=w.pred(87, 90, uniform),
                 dst=w.pred(81, None, uniform), dst1=w.pred(84, None, uniform))
    elif op == "LEA":
        f.update(a=A, b=B, c=C, shift=w.bits(75, 80), hi=w.bit(80), neg_shifted=w.bit(72), x=w.bit(74),
                 cin=w.pred(87, 90, uniform), cout=w.pred(81, None, uniform))
    elif op == "LOP3":
        f.update(a=A, b=B, c=C, lut=w.bits(72, 80), pand=w.bit(80), pdst=w.pred(81, None, uniform),
                 p=w.pred(87, 90, uniform))
    elif op == "SHF":
        f.update(lo=A, shift=B, hi_src=C, type={0: "S64", 1: "U64", 2: "S32", 3: "U32"}[w.bits(73, 75)],
                 wrap=w.bit(75), right=w.bit(76), hi=w.bit(80))
    elif op == "IMNMX":
        f.update(a=A, b=two_b, min=w.pred(87, 90, uniform), signed=w.bit(73))
    elif op == "BMSK":
        f.update(pos=A, width=two_b, wrap=w.bit(75))
    elif op == "SGXT":
        f.update(a=A, bits=two_b, signed=w.bit(73), wrap=w.bit(75))
    elif op == "PRMT":
        f.update(a=A, sel=B, b=C, mode=w.bits(72, 75))
    elif op == "IDP4A":
        f.update(a=A, b=B, c=C, a_signed=w.bit(73), b_signed=w.bit(74))
    elif op in ("DADD",):
        f.update(a=A, b=two_b, rnd=ROUND[w.bits(78, 80)])
    elif op in ("DMUL", "DFMA"):
        f.update(a=A, b=B, c=C, rnd=ROUND[w.bits(78, 80)])
    elif op == "F2FP":
        f.update(a=A, b=B, rnd=ROUND[w.bits(79, 81)], merge=w.bit(78), h1=w.bit(72))
    else:
        raise Unknown(op)
    return f


# 9-bit ALU operations with the standard forms: name -> (kind, uniform)
ALU_OPS = {
    0x002: "MOV", 0x007: "SEL", 0x008: "FSEL", 0x009: "FMNMX", 0x00A: "FSET", 0x00B: "FSETP", 0x00C: "ISETP",
    0x010: "IADD3", 0x011: "LEA", 0x012: "LOP3", 0x013: "IABS", 0x016: "PRMT", 0x017: "IMNMX", 0x019: "SHF",
    0x01A: "SGXT", 0x01B: "BMSK", 0x020: "FMUL", 0x021: "FADD", 0x023: "FFMA", 0x024: "IMAD",
    0x025: "IMAD.WIDE", 0x026: "IDP4A", 0x027: "IMAD.HI", 0x028: "DMUL", 0x029: "DADD", 0x02B: "DFMA",
    0x03E: "F2FP",
    0x045: "I2FP",  # inferred: Ampere's I2FP (not in NAK); same fields as I2F, checked on the twins
    0x100: "FLO", 0x101: "BREV", 0x104: "F2F", 0x105: "F2I", 0x106: "I2F", 0x107: "FRND", 0x108: "MUFU",
    0x109: "POPC", 0x110: "F2F.64", 0x111: "F2I.64", 0x112: "I2F.64", 0x113: "FRND.64",
}
UALU_OPS = {
    0x082: "MOV", 0x087: "SEL", 0x08C: "ISETP", 0x090: "IADD3", 0x091: "LEA", 0x092: "LOP3", 0x096: "PRMT",
    0x099: "SHF", 0x09A: "SGXT", 0x09B: "BMSK", 0x0A4: "IMAD", 0x0A5: "IMAD.WIDE", 0x0A7: "IMAD.HI", 0x0BD: "FLO",
    0x0BE: "BREV", 0x0BF: "POPC",
}
UNARY = {"MOV", "IABS", "FLO", "BREV", "POPC", "F2F", "F2I", "I2F", "I2FP", "FRND", "MUFU", "F2F.64", "F2I.64",
         "I2F.64", "FRND.64"}


def _unary(w, op, form, uniform):
    A, B, C = _slots(w, form, uniform)
    if form not in SRC1_FORMS:
        raise Unknown(f"{op} form {form}")
    f = {"src": B}
    if op == "MOV":
        if not uniform:
            f["lanes"] = w.bits(72, 76)
    elif op == "MUFU":
        f["fn"] = MUFU.get(w.bits(74, 80), w.bits(74, 80))
        f["f16"] = w.bit(73)
    elif op in ("F2F", "F2F.64", "FRND", "FRND.64"):
        f.update(dst_bytes=1 << w.bits(75, 77), src_bytes=1 << w.bits(84, 86), rnd=ROUND[w.bits(78, 80)],
                 ftz=w.bit(80))
        if op.startswith("F2F"):
            f["h1"] = w.bit(60)  # inferred: source half select for F16 -> F32
    elif op in ("F2I", "F2I.64"):
        f.update(signed=w.bit(72), dst_bytes=1 << w.bits(75, 77), ntz=w.bit(77), rnd=ROUND[w.bits(78, 80)],
                 ftz=w.bit(80), src_bytes=1 << w.bits(84, 86))
    elif op in ("I2F", "I2F.64", "I2FP"):
        f.update(signed=w.bit(74), dst_bytes=1 << w.bits(75, 77), rnd=ROUND[w.bits(78, 80)],
                 src_bytes=1 << w.bits(84, 86), byte_sel=w.bits(60, 62))
    elif op == "FLO":
        f.update(sh=w.bit(74), signed=w.bit(73), not_=w.bit(63), pdst=w.pred(81))
    elif op == "POPC":
        f["not_"] = w.bit(63)
    return f


def decode(lo: int, hi: int, addr: int = 0) -> Insn:
    w = _W(lo | (hi << 64))
    op12 = w.bits(0, 12)
    guard = w.pred(12, 15)
    base, form = op12 & 0x1FF, op12 >> 9
    i = None
    # uniform ALU: same 9-bit layout with bit 7 of the operation set
    if base in UALU_OPS and form in (1, 2, 4) and op12 not in FIXED:
        op = UALU_OPS[base]
        dst = w.ureg(16)
        if op == "MOV":
            f = {"src": _slots(w, form, True)[1]}
        elif op in ("FLO", "BREV", "POPC"):
            f = {"src": _slots(w, form, True)[1]}
            if op == "FLO":
                f.update(sh=w.bit(74), signed=w.bit(73), not_=w.bit(63), pdst=w.pred(81, None, True))
        elif op == "ISETP":
            f = _alu(w, op, form, True)
            w.bits(16, 24)
        else:
            f = _alu(w, op, form, True)
        if op != "ISETP":
            f = {"dst": dst, **f}
        else:
            f["dst"] = w.pred(81, None, True)
            f["dst1"] = w.pred(84, None, True)
        i = Insn(addr, w.v, "U" + op, guard, f)
    elif op12 in FIXED:
        i = Insn(addr, w.v, FIXED[op12][0], guard, FIXED[op12][1](w))
    elif base in ALU_OPS and form in range(1, 8):
        op = ALU_OPS[base]
        if op in UNARY:
            f = _unary(w, op, form, False)
        else:
            f = _alu(w, op, form, False)
        if op not in ("FSETP", "ISETP"):
            f = {"dst": w.reg(16), **f}
        i = Insn(addr, w.v, op, guard, f)
    else:
        raise Unknown(f"opcode {op12:#05x}")
    i.unknown_bits = w.unknown()
    return i


# ---- fixed-opcode instructions -------------------------------------------------------------------------------

def _rel(w, lo, hi):
    return w.sbits(lo, hi) * 4  # in units of 4 bytes, from the next instruction


def _tex_common(w, f):
    f["dst"] = w.reg(16)
    f["dst1"] = w.reg(64)
    f["a"] = w.reg(24)
    f["b"] = w.reg(32)
    f["fault"] = w.pred(81)
    f["dim"] = TEXDIM.get(w.bits(61, 64), w.bits(61, 64))
    f["mask"] = w.bits(72, 76)
    f["nodep"] = w.bit(90)
    w.bits(84, 87)  # eviction priority
    return f


def _tex_handle(w, f, bindless):
    if bindless:
        f["bindless"] = w.bit(59)
    else:
        f["handle"] = w.bits(40, 54)      # word offset in the texture bank
        f["bank"] = w.bits(54, 59)


def _f_tex(bindless):
    def h(w):
        f = {}
        _tex_handle(w, f, bindless)
        _tex_common(w, f)
        f["scalar"] = w.bit(60)
        f["aoffi"] = w.bit(76)
        f["ndv"] = w.bit(77)
        f["dc"] = w.bit(78)
        # bit 79 (inferred; NAK never sets it): results packed as f16, two per register. Seen on a one-component
        # TEX whose result only HADD2.F32 (R.H0_H0) reads.
        f["f16"] = w.bit(79)
        f["lod"] = LODMODE.get(w.bits(87, 90), w.bits(87, 90))
        return f
    return h


def _f_hadd2_r(w):
    # HADD2, register form (0x230): NAK encode_fp16v2_alu. src0 neg 72, abs 73, swizzle 74..75; src1 (register
    # 32..39) abs 62, neg 63, swizzle 60..61; swizzle 0 = H1_H0, 1 = F32, 2 = H0_H0, 3 = H1_H1; .SAT 77, .F32
    # output 78, .FTZ 80, .BF16_V2 85 (SM90+, must be 0).
    if w.bit(85):
        raise Unknown("HADD2.BF16_V2")
    return {"dst": w.reg(16), "a": w.reg(24), "b": w.reg(32), "neg_a": w.bit(72), "abs_a": w.bit(73),
            "swz_a": w.bits(74, 76), "abs_b": w.bit(62), "neg_b": w.bit(63), "swz_b": w.bits(60, 62),
            "sat": w.bit(77), "f32": w.bit(78), "ftz": w.bit(80)}


def _f_tld(bindless):
    def h(w):
        f = {}
        _tex_handle(w, f, bindless)
        _tex_common(w, f)
        f["scalar"] = w.bit(60)
        f["aoffi"] = w.bit(76)
        f["ms"] = w.bit(78)
        f["lod"] = LODMODE.get(w.bits(87, 90), w.bits(87, 90))
        return f
    return h


def _f_tld4(bindless):
    def h(w):
        f = {}
        _tex_handle(w, f, bindless)
        _tex_common(w, f)
        f["scalar"] = w.bit(60)
        f["offset"] = w.bits(76, 78)
        f["dc"] = w.bit(78)
        f["comp"] = w.bits(87, 89)
        return f
    return h


def _f_tmml(bindless):
    def h(w):
        f = {}
        _tex_handle(w, f, bindless)
        _tex_common(w, f)
        del f["fault"]          # TMML has no fault predicate (NAK leaves 81..84 clear)
        w.used &= ~(0x7 << 81)
        f["ndv"] = w.bit(77)
        return f
    return h


def _f_txd(bindless):
    def h(w):
        f = {}
        _tex_handle(w, f, bindless)
        _tex_common(w, f)
        f["aoffi"] = w.bit(76)
        return f
    return h


def _f_txq(bindless):
    def h(w):
        f = {}
        _tex_handle(w, f, bindless)
        f["dst"] = w.reg(16)
        f["dst1"] = w.reg(64)
        f["a"] = w.reg(24)
        f["query"] = w.bits(62, 64)
        f["mask"] = w.bits(72, 76)
        f["nodep"] = w.bit(90)
        return f
    return h


def _f_ipa(w):
    return {"dst": w.reg(16), "attr": w.bits(64, 72) * 4, "loc": w.bits(76, 78),
            "freq": {0: "PASS", 1: "CONSTANT", 2: "STATE"}.get(w.bits(78, 80), w.bits(78, 80)),
            "offset": w.reg(32), "idx": w.reg(24), "sat": w.bit(77) if False else False, "pdst": w.pred(81)}


def _f_ipa_u(w):
    # 0x526 (inferred, Wonder's Switch 2 shaders): IPA whose attribute address is attr + UR(24) (bit 91: uniform
    # source). Seen reading a[0x80/0x84/0x88 + UR4] with UR4 = index << 4: an indexed varying vector.
    w.bit(91)
    f = _f_ipa(w)
    del f["idx"]
    f["idx_ureg"] = w.ureg(24)
    return f


def _f_ald(w):
    return {"dst": w.reg(16), "offset": w.reg(24), "vtx": w.reg(32), "attr": w.bits(40, 50),
            "count": w.bits(74, 76) + 1, "patch": w.bit(76), "phys": w.bit(77), "output": w.bit(79)}


def _f_ast(w):
    return {"data": w.reg(32), "vtx": w.reg(64), "offset": w.reg(24), "attr": w.bits(40, 50),
            "count": w.bits(74, 76) + 1, "patch": w.bit(76), "phys": w.bit(77)}


def _f_ldc(w):
    f = {"dst": w.reg(16), "type": MEMTYPE[w.bits(73, 76)]}
    if w.bit(91):
        f.update(offset_reg=w.reg(64), handle_ureg=w.ureg(24), offset=w.bits(38, 54))
    else:
        f.update(offset_reg=w.reg(24), bank=w.bits(54, 59), offset=w.bits(38, 54), mode=w.bits(78, 80))
    return f


def _f_uldc(w):
    w.bit(91)
    return {"dst": w.ureg(16), "bank": w.bits(54, 59), "offset": w.bits(38, 54), "type": MEMTYPE[w.bits(73, 76)]}


def _f_uldc_ur(w):
    # 0xABB (inferred, Wonder's Switch 2 shaders): ULDC with a uniform offset register, c[bank][UR(24) + offset].
    # NAK only emits 0xAB9 (no offset register) on SM86; seen loading a c[1] jump table entry for BRXU.
    w.bit(91)
    return {"dst": w.ureg(16), "offset_reg": w.ureg(24), "bank": w.bits(54, 59), "offset": w.bits(38, 54),
            "type": MEMTYPE[w.bits(73, 76)]}


def _f_brxu(w):
    # 0x958 (inferred): BRX through a uniform register pair (bit 91 set, as for other uniform-source forms)
    w.bit(91)
    return {"src": w.ureg(24), "target_rel": _rel(w, 34, 82), "p": w.pred(87, 90)}


def _mem_order(w):
    w.bits(77, 81)
    w.bits(84, 87)


def _f_ld(space, ugpr):
    def h(w):
        f = {"dst": w.reg(16), "type": MEMTYPE[w.bits(73, 76)], "offset": w.sbits(40, 64)}
        if space == "G":
            f["addr"] = w.reg(24)
            f["addr64"] = w.bit(90 if ugpr else 72)
            if ugpr:
                f["uaddr"] = w.ureg(32)
                f["uaddr64"] = w.bit(72)
                p = w.bits(64, 67)
                f["p"] = Pred(7 - p, w.bit(67))  # reversed encoding (NAK set_rev_pred_src)
            f["pdst"] = w.pred(81)
            _mem_order(w)
        else:
            f["addr"] = w.reg(24)
            if ugpr:
                f["uaddr"] = w.ureg(32)
            if space == "S":
                f["stride"] = w.bits(78, 80)
                w.bit(87)
            else:
                w.bits(84, 87)
        if ugpr:
            w.bit(91)
        return f
    return h


def _f_st(space, ugpr):
    def h(w):
        f = {"data": w.reg(32), "type": MEMTYPE[w.bits(73, 76)], "offset": w.sbits(40, 64), "addr": w.reg(24)}
        if space == "G":
            f["addr64"] = w.bit(90 if ugpr else 72)
            if ugpr:
                f["uaddr"] = w.ureg(64)
                f["uaddr64"] = w.bit(72)
            _mem_order(w)
        else:
            if ugpr:
                f["uaddr"] = w.ureg(64)
            if space == "S":
                f["stride"] = w.bits(78, 80)
            else:
                w.bits(84, 87)
        if ugpr:
            w.bit(91)
        return f
    return h


def _f_bra(w):
    return {"target_rel": _rel(w, 34, 82), "p": w.pred(87, 90), "u": w.bit(32)}


def _f_bra_u(w):
    w.bits(87, 90)
    w.bit(91)
    return {"target_rel": _rel(w, 34, 82), "p": w.pred(24, 27, uniform=True), "u": w.bit(32)}


def _f_call(w):
    # CALL.REL.NOINC (bit 86): no call stack; the compiler keeps the return address in a register itself
    return {"target_rel": _rel(w, 34, 82), "p": w.pred(87, 90), "noinc": w.bit(86)}


def _f_ret(w):
    # RET.REL.NODEC Rn: to the address in Rn (relative to next + offset; the compiler encodes an offset that makes it
    # absolute, i.e. next + rel == 0)
    return {"src": w.reg(24), "target_rel": _rel(w, 34, 82), "p": w.pred(87, 90), "nodec": w.bit(86)}


def _f_brx(w):
    # BRX Rn (register pair): to next + offset + Rn. Seen in Wonder only with next + offset == 0 (as RET), so Rn holds
    # a program-relative address, loaded from a jump table in the inline constants (c[1]).
    return {"src": w.reg(24), "target_rel": _rel(w, 34, 82), "p": w.pred(87, 90)}


def _f_bssy(w):
    return {"bar": w.bits(16, 20), "target_rel": _rel(w, 34, 64), "p": w.pred(87, 90)}


def _f_barsrc(w):
    return {"bar": w.bits(16, 20), "p": w.pred(87, 90)}


def _f_exit(w):
    return {"p": w.pred(87, 90), "keep_refcount": w.bit(84), "no_atexit": w.bit(85)}


def _f_kill(w):
    return {"p": w.pred(87, 90)}


def _f_none(w):
    return {}


def _f_depbar(w):
    w.bits(32, 48)
    w.bits(38, 48)
    return {}


def _f_s2r(uniform):
    def h(w):
        return {"dst": w.ureg(16) if uniform else w.reg(16), "sr": w.bits(72, 80)}
    return h


def _f_cs2r(w):
    return {"dst": w.reg(16), "sr": w.bits(72, 80), "wide": w.bit(80)}


def _f_r2ur(w):
    return {"dst": w.ureg(16), "src": w.reg(24), "pdst": w.pred(81)}


def _f_umov(w):
    form = w.bits(9, 12)
    f = {"dst": w.ureg(16)}
    if form == 6:
        w.bit(91)
        f["src"] = w.ureg(32)
    else:
        f["src"] = Imm(w.bits(32, 64))
    return f


def _f_vote(uniform):
    def h(w):
        return {"dst": w.ureg(16) if uniform else w.reg(16), "mode": {0: "ALL", 1: "ANY", 2: "EQ"}[w.bits(72, 74)],
                "pdst": w.pred(81), "p": w.pred(87, 90)}
    return h


def _f_fswzadd(w):
    return {"dst": w.reg(16), "a": w.reg(24), "b": w.reg(64), "ops": w.bits(32, 40), "ndv": w.bit(77),
            "rnd": ROUND[w.bits(78, 80)], "ftz": w.bit(80)}


def _f_plop3(uniform):
    def h(w):
        f = {"a": w.pred(87, 90, uniform), "b": w.pred(77, 80, uniform)}
        c_uniform = uniform or w.bit(67)
        f["c"] = w.pred(68, 71, c_uniform)
        f["lut0"] = w.bits(64, 67) | (w.bits(72, 77) << 3)
        f["lut1"] = w.bits(16, 24)
        f["dst"] = w.pred(81, None, uniform)
        f["dst1"] = w.pred(84, None, uniform)
        return f
    return h


def _f_shfl(lane_imm, c_imm):
    def h(w):
        f = {"dst": w.reg(16), "src": w.reg(24), "pdst": w.pred(81),
             "mode": {0: "IDX", 1: "UP", 2: "DOWN", 3: "BFLY"}[w.bits(58, 60)]}
        f["lane"] = Imm(w.bits(53, 58)) if lane_imm else w.reg(32)
        f["c"] = Imm(w.bits(40, 53)) if c_imm else w.reg(64)
        return f
    return h


def _f_membar(w):
    return {"scope": w.bits(76, 79), "mmio": w.bit(72), "sc": w.bit(80)}


def _f_bar(w):
    w.bits(16, 105)
    return {"raw": hex(w.v & ((1 << 105) - 1))}


def _f_p2r(is_r2p):
    """P2R (0x803) / R2P (0x804): inferred, not in NAK. Same model as Maxwell's P2R/R2P (checked on the twins and
    against every instance in the corpus: always the immediate form, byte select at 76..77)."""
    def h(w):
        form = w.bits(9, 12)
        A, B, C = _slots(w, form, False)
        f = {"src": A, "mask": B, "byte": w.bits(76, 78)}
        if not is_r2p:
            f = {"dst": w.reg(16), **f}
        return f
    return h


def _f_warpsync(w):
    A, B, C = _slots(w, w.bits(9, 12), False)
    return {"mask": B, "p": w.pred(87, 90)}


FIXED = {
    0x918: ("NOP", _f_none), 0x91A: ("DEPBAR", _f_depbar), 0x919: ("S2R", _f_s2r(False)),
    0x9C3: ("S2UR", _f_s2r(True)), 0x805: ("CS2R", _f_cs2r), 0x3C2: ("R2UR", _f_r2ur),
    0xC82: ("UMOV", _f_umov), 0x882: ("UMOV", _f_umov),
    0x947: ("BRA", _f_bra), 0x547: ("BRA.U", _f_bra_u), 0x945: ("BSSY", _f_bssy), 0x941: ("BSYNC", _f_barsrc),
    0x942: ("BREAK", _f_barsrc), 0x94D: ("EXIT", _f_exit), 0x95B: ("KILL", _f_kill),
    0x148: ("WARPSYNC", _f_warpsync), 0x948: ("WARPSYNC", _f_warpsync),
    0x326: ("IPA", _f_ipa), 0x321: ("ALD", _f_ald), 0x322: ("AST", _f_ast),
    0xB82: ("LDC", _f_ldc), 0x582: ("LDC", _f_ldc), 0xAB9: ("ULDC", _f_uldc),
    0x981: ("LDG", _f_ld("G", True)), 0x381: ("LDG", _f_ld("G", False)), 0x983: ("LDL", _f_ld("L", True)),
    0x984: ("LDS", _f_ld("S", True)),
    0x986: ("STG", _f_st("G", True)), 0x386: ("STG", _f_st("G", False)), 0x987: ("STL", _f_st("L", True)),
    0x387: ("STL", _f_st("L", False)), 0x988: ("STS", _f_st("S", True)), 0x388: ("STS", _f_st("S", False)),
    0xB60: ("TEX", _f_tex(False)), 0x361: ("TEX.B", _f_tex(True)),
    0xB66: ("TLD", _f_tld(False)), 0x367: ("TLD.B", _f_tld(True)),
    0xB63: ("TLD4", _f_tld4(False)), 0x364: ("TLD4.B", _f_tld4(True)),
    0xB69: ("TMML", _f_tmml(False)), 0x36A: ("TMML.B", _f_tmml(True)),
    0xB6C: ("TXD", _f_txd(False)), 0x36D: ("TXD.B", _f_txd(True)),
    0xB6F: ("TXQ", _f_txq(False)), 0x370: ("TXQ.B", _f_txq(True)),
    0x806: ("VOTE", _f_vote(False)), 0x886: ("UVOTE", _f_vote(True)), 0x822: ("FSWZADD", _f_fswzadd),
    0x81C: ("PLOP3", _f_plop3(False)), 0x89C: ("UPLOP3", _f_plop3(True)),
    0x389: ("SHFL", _f_shfl(False, False)), 0x589: ("SHFL", _f_shfl(False, True)),
    0x989: ("SHFL", _f_shfl(True, False)), 0xF89: ("SHFL", _f_shfl(True, True)),
    0x992: ("MEMBAR", _f_membar), 0xB1D: ("BAR", _f_bar),
    0x803: ("P2R", _f_p2r(False)), 0x804: ("R2P", _f_p2r(True)),
    0x230: ("HADD2", _f_hadd2_r), 0x526: ("IPA", _f_ipa_u), 0x944: ("CALL", _f_call), 0x950: ("RET", _f_ret), 0x949: ("BRX", _f_brx), 0x958: ("BRX", _f_brxu),
    0xABB: ("ULDC", _f_uldc_ur),
}


def code_range(ctrl: bytes, stage: str):
    """[start, end) of the instructions in a code block, from the NVN 2 control section (+0x38 = code size)."""
    end = struct.unpack_from("<I", ctrl, 0x38)[0]
    return (0 if stage == "compute" else SPH_SIZE), end


def instructions(block: bytes, start: int = SPH_SIZE, end: int | None = None):
    """(addr, lo, hi) of every instruction; addresses are relative to the first instruction."""
    end = len(block) if end is None else min(end, len(block))
    for o in range(start, end - 15, 16):
        lo, hi = struct.unpack_from("<QQ", block, o)
        yield o - start, lo, hi


# ---- CLI -------------------------------------------------------------------------------------------------------

def _blocks(dirs):
    for d in dirs:
        man = os.path.join(d, "manifest.tsv")
        seen = set()
        for line in list(open(man, encoding="utf-8"))[1:]:
            r = line.rstrip("\n").split("\t")
            if r[11] in seen:
                continue
            seen.add(r[11])
            yield r[1], os.path.join(d, r[11]), os.path.join(d, r[12])


def cmd_stats(dirs):
    seen, unknown, badbits = collections.Counter(), collections.Counter(), collections.Counter()
    blocks = total = 0
    for stage, code_p, ctrl_p in _blocks(dirs):
        code, ctrl = open(code_p, "rb").read(), open(ctrl_p, "rb").read()
        start, end = code_range(ctrl, stage)
        blocks += 1
        for addr, lo, hi in instructions(code, start, end):
            total += 1
            try:
                i = decode(lo, hi, addr)
                seen[i.op] += 1
                if i.unknown_bits:
                    badbits[f"{i.op} {i.unknown_bits}"] += 1
            except Unknown as e:
                unknown[str(e)] += 1
    known = sum(seen.values())
    print(f"{blocks} blocks, {total} instructions, decoded {known} ({100 * known / max(total, 1):.3f}%), "
          f"{len(seen)} kinds, {sum(badbits.values())} with unexplained bits")
    print("  " + " ".join(f"{n}:{c}" for n, c in seen.most_common()))
    print("undecoded: " + " ".join(f"{n}:{c}" for n, c in unknown.most_common(40)))
    print("unexplained bits:")
    for n, c in badbits.most_common(40):
        print(f"  {c:8} {n}")
    return 0


def cmd_dis(code_p, ctrl_p=None, stage=None):
    # The stage decides where the instructions start (graphics: after the 0x80-byte SPH; compute: at 0). Without
    # --stage it is guessed from the SPH's first word, which a compute block does not have: pass it for compute.
    code = open(code_p, "rb").read()
    start, end = SPH_SIZE, len(code)
    if ctrl_p:
        if stage is None:
            t = (struct.unpack_from("<I", code, 0)[0] >> 10) & 0xF
            stage = "graphics" if struct.unpack_from("<I", code, 0)[0] & 0x1F in (1, 2) and t in (1, 5) else "compute"
        start, end = code_range(open(ctrl_p, "rb").read(), stage)
    for addr, lo, hi in instructions(code, start, end):
        try:
            text = str(decode(lo, hi, addr))
        except Unknown as e:
            text = f"?? {e}"
        print(f"  /*{addr:04x}*/ {text:90} /* {hi:016x}{lo:016x} */")


def main(argv):
    if len(argv) < 2 or argv[0] not in ("stats", "dis"):
        sys.exit(__doc__)
    if argv[0] == "stats":
        return cmd_stats(argv[1:])
    stage = argv[argv.index("--stage") + 1] if "--stage" in argv else None
    return cmd_dis(*[a for a in argv[1:3] if not a.startswith("--")], stage=stage)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
