#!/usr/bin/env python3
"""Maxwell (SM5x) instruction encoder/decoder generated from Ryujinx's own decoder tables.

Ryujinx's shader translator (MIT) describes every Maxwell instruction twice: Decoders/InstTable.cs gives the fixed
opcode bits of each instruction form (a 64-character pattern, bit 63 first, 'x' = operand bit) and
Decoders/InstDecoders.cs gives each operand field as a C# accessor over the 64-bit word. This module reads both
files from the local Ryubing checkout at run time (nothing is copied) and inverts the accessors, so an instruction
encoded here is by construction what that translator decodes. tests/test_maxwell_enc.py checks the round trip.

  enc("FaddR", Dest=1, SrcA=2, SrcB=3)   -> 64-bit word (Pred defaults to PT, every other field to 0)
  dec(word)                               -> ("FaddR", {field: value})

Maxwell code layout (Decoder.cs): addresses are relative to the program start; every address with (addr & 0x1f) == 0
holds a scheduling word, which the translator skips; BRA's target is addr + 8 + sext24(bits 20..43).
"""
from __future__ import annotations

import functools
import os
import re

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
# Ryujinx's shader decoder sources (MIT), read for the Maxwell encodings: Decoders/InstTable.cs and InstDecoders.cs.
# NX_RYUJINX_SHADER_SRC: a folder holding Decoders/ (the preparer points it at the translator component's copy, or at
# its own verified Ryubing checkout); else the development checkout in private/third_party.
RYU_SHADER = os.environ.get("NX_RYUJINX_SHADER_SRC") or os.path.join(
    REPO, "private", "third_party", "ryubing", "src", "Ryujinx.Graphics.Shader")

# The form is named after its emitter (InstEmit.FaddR), which is also the name of its InstDecoders struct.
_ADD = re.compile(r'Add\("([01x]{64})",\s*InstName\.\w+,\s*InstEmit\.(\w+)')
_STRUCT = re.compile(r"readonly struct Inst(\w+)\s*\{(.*?)\n    \}", re.S)
_PROP = re.compile(r"public (\w+) (\w+) => (.*?);")
# One term of an accessor: (int)((_opcode >> S) & M), (_opcode & M), (int)(_opcode >> S) (open-ended, 32 bits)
_TERM_SHIFT_MASK = re.compile(r"\(_opcode >> (\d+)\) & 0x([0-9A-Fa-f]+)")
_TERM_MASK = re.compile(r"^\(?\(?\w*\)?\(?_opcode & 0x([0-9A-Fa-f]+)\)")
_TERM_OPEN = re.compile(r"\(int\)\(_opcode >> (\d+)\)$")


class Field:
    """A field as a list of (value_mask, shift): word |= (value & value_mask) << shift."""

    def __init__(self, name, terms, is_bool=False):
        self.name, self.terms, self.is_bool = name, terms, is_bool

    def put(self, v: int) -> int:
        if self.is_bool:
            return self.terms[0][0] << self.terms[0][1] if v else 0
        full = 0
        for m, s in self.terms:
            full |= m
        if v & ~full and v >= 0:
            raise ValueError(f"field {self.name}: value {v:#x} does not fit mask {full:#x}")
        if v < 0:
            v &= full
        w = 0
        for m, s in self.terms:
            w |= (v & m) << s
        return w

    def get(self, word: int):
        v = 0
        for m, s in self.terms:
            v |= (word >> s) & m
        return bool(v) if self.is_bool else v


def _parse_accessor(expr: str):
    expr = expr.strip()
    if expr.endswith("!= 0"):
        m = re.search(r"_opcode & 0x([0-9A-Fa-f]+)", expr)
        mask = int(m.group(1), 16)
        return [(mask, 0)], True
    m = _TERM_OPEN.search(expr)
    if m and "&" not in expr:
        return [(0xFFFFFFFF, int(m.group(1)))], False
    terms = []
    for s, msk in _TERM_SHIFT_MASK.findall(expr):
        s, msk = int(s), int(msk, 16)
        # (_opcode >> s) & msk: value bits = msk positions, word bits = msk << s
        terms.append((msk, s))
    rest = _TERM_SHIFT_MASK.sub("", expr)
    for msk in re.findall(r"\(_opcode & 0x([0-9A-Fa-f]+)\)", rest):
        terms.append((int(msk, 16), 0))
    return (terms, False) if terms else (None, False)


@functools.lru_cache(maxsize=1)
def tables():
    table_src = open(os.path.join(RYU_SHADER, "Decoders", "InstTable.cs"), encoding="utf-8").read()
    dec_src = open(os.path.join(RYU_SHADER, "Decoders", "InstDecoders.cs"), encoding="utf-8").read()
    forms = {}
    for pat, name in _ADD.findall(table_src):
        val = msk = 0
        for i, ch in enumerate(pat):
            b = 63 - i
            if ch != "x":
                msk |= 1 << b
                if ch == "1":
                    val |= 1 << b
        forms.setdefault(name, []).append((val, msk))
    fields = {}
    for sname, body in _STRUCT.findall(dec_src):
        fs = {}
        for typ, pname, expr in _PROP.findall(body):
            if "_opcode" not in expr:
                continue
            terms, is_bool = _parse_accessor(expr)
            if terms:
                fs[pname] = Field(pname, terms, is_bool or typ == "bool")
        fields[sname] = fs
    return forms, fields


def enc(name: str, **kv) -> int:
    forms, fields = tables()
    if name not in forms:
        raise KeyError(f"unknown Maxwell instruction {name}")
    val, msk = forms[name][0]
    fs = fields.get(name, {})
    kv.setdefault("Pred", 7)
    w = val
    for k, v in kv.items():
        if k not in fs:
            if k == "Pred" and "Pred" not in fs:
                continue
            raise KeyError(f"{name} has no field {k} (has {sorted(fs)})")
        w |= fs[k].put(v)
    # the operand fields must not touch the fixed opcode bits
    if (w & msk) != val:
        raise ValueError(f"{name}: operands overwrite opcode bits ({w:#018x})")
    return w


@functools.lru_cache(maxsize=1)
def _sorted_forms():
    forms, _ = tables()
    out = [(bin(msk).count("1"), val, msk, name) for name, fl in forms.items() for val, msk in fl]
    out.sort(reverse=True)  # most specific pattern first
    return out


def dec(word: int):
    for _, val, msk, name in _sorted_forms():
        if word & msk == val:
            fs = tables()[1].get(name, {})
            return name, {k: f.get(word) for k, f in fs.items()}
    return None, {}


def sched_word() -> int:
    """A scheduling word for three instructions. The translator ignores its content; this one asks for the most
    conservative wait (stall 15, wait on all barriers) in case anything ever executes it."""
    one = (0x3F << 11) | 0xF  # wait mask all, stall 15
    return one | (one << 21) | (one << 42)
