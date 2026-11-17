"""RELR decoding (recomp/nso.py) on SYNTHETIC tables. Switch 2 modules keep their relative relocations in DT_RELR.

Run: private/venv/Scripts/python -m pytest tests/test_relr.py
"""
import os
import random
import struct
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from recomp import nso  # noqa: E402


def encode_relr(offsets):
    """Reference encoder (test only), the layout lld writes: an address, then bitmaps for the next 63-word windows."""
    offsets = sorted(offsets)
    out, i = [], 0
    while i < len(offsets):
        base = offsets[i]
        out.append(base)
        i += 1
        where = base + 8
        while True:
            bits = 0
            while i < len(offsets) and offsets[i] - where < 63 * 8 and (offsets[i] - where) % 8 == 0:
                bits |= 1 << ((offsets[i] - where) // 8)
                i += 1
            if not bits:
                break
            out.append((bits << 1) | 1)
            where += 63 * 8
    return out


def test_hand_vector():
    # address 0x1000, then bitmap: bits 1 and 3 -> 0x1008, 0x1018; next window starts at 0x1008 + 63*8
    table = [0x1000, (0b101 << 1) | 1, (1 << 1) | 1]
    img = bytearray(struct.pack(f"<{len(table)}Q", *table))
    assert nso.decode_relr(img, 0, len(img)) == [0x1000, 0x1008, 0x1018, 0x1008 + 63 * 8]


@pytest.mark.parametrize("seed", range(5))
def test_round_trip(seed):
    rnd = random.Random(seed)
    offs = sorted({rnd.randrange(0, 0x20000) * 8 for _ in range(rnd.randrange(1, 3000))})
    table = encode_relr(offs)
    img = bytearray(struct.pack(f"<{len(table)}Q", *table))
    assert nso.decode_relr(img, 0, len(img)) == offs


def test_bad_tables():
    img = bytearray(struct.pack("<Q", 0b11))  # bitmap with no address before it
    with pytest.raises(ValueError):
        nso.decode_relr(img, 0, 8)
    with pytest.raises(ValueError):
        nso.decode_relr(bytearray(16), 0, 16, entsize=4)


def test_module_relocs_from_relr():
    """A minimal synthetic module image with MOD0 + .dynamic carrying only DT_RELR: the parser must turn every RELR
    slot into an R_AARCH64_RELATIVE relocation whose addend is the word already in the image."""
    img = bytearray(0x400)
    struct.pack_into("<4sI", img, 0, b"\0\0\0\0", 0x10)          # MOD0 offset at +4
    mod0 = 0x10
    img[mod0:mod0 + 4] = b"MOD0"
    dyn = 0x100
    for k, v in enumerate((dyn - mod0, 0x380 - mod0, 0x380 - mod0, 0x380 - mod0, 0x380 - mod0)):
        struct.pack_into("<i", img, mod0 + 4 + k * 4, v)
    relr_at, symtab, strtab = 0x200, 0x240, 0x258
    slots = [0x300, 0x308, 0x318]
    for s in slots:
        struct.pack_into("<Q", img, s, 0x1234 + s)                 # implicit addends
    table = encode_relr(slots)
    struct.pack_into(f"<{len(table)}Q", img, relr_at, *table)
    tags = [(nso.DT_RELR, relr_at), (nso.DT_RELRSZ, len(table) * 8), (nso.DT_RELRENT, 8),
            (nso.DT_SYMTAB, symtab), (nso.DT_STRTAB, strtab), (nso.DT_SYMENT, 24), (nso.DT_NULL, 0)]
    for k, (t, v) in enumerate(tags):
        struct.pack_into("<qQ", img, dyn + k * 16, t, v)
    m = nso.Module("synthetic", img, (0, 0x100), (0, 0), (0x100, 0x280), (0x380, 0))
    nso._parse_mod0(m)
    rel = [(r.offset, r.type, r.addend) for r in m.relocs]
    assert rel == [(s, nso.R_AARCH64_RELATIVE, 0x1234 + s) for s in slots]
