#!/usr/bin/env python3
"""Ampere (SM86, NVN 2) -> Maxwell (SM5x, NVN 1) shader lowering, so that the Maxwell -> SPIR-V translator the
project already uses (Ryujinx's, via tools/maxwell/spirv_harness) can translate Switch 2 shaders.

Input: a code block and NVN 2 control section as tools/maxwell/containers.py --extract writes them.
Output: a Maxwell code block in NVN 1 layout (0x30-byte header, 0x50-byte SPH, instructions with a scheduling word
every 32 bytes, constant data) and a synthetic NVN 1 control section carrying only the fields the harness reads
(+0x6fc/+0x700 constant data, +0x708 program start, +0x748.. compute group size, +0x754 shared memory size).

Every Ampere instruction is rewritten as one or more Maxwell instructions with the same effect on registers,
predicates and memory, written from the two encodings (sm86.py from Mesa NAK, maxwell.py from Ryujinx's tables).
What does not exist on Maxwell is rebuilt from what does:
  - uniform registers UR0..UR63 live in general registers above the ones the program uses (URn -> R(base+n));
    uniform predicates in predicates the program leaves free;
  - 32-bit immediates that the Maxwell form cannot hold go through MOV32I into a scratch register;
  - convergence barriers (BSSY/BSYNC/BREAK/WARPSYNC) carry no single-thread meaning and become NOPs: the
    translator rebuilds structured control flow from the branches themselves.
Anything not understood raises Unsupported: a shader is lowered completely or not at all.

Usage: python tools/ampere/lower.py <code.bin> <ctrl.bin> <out_dir> [--stage vertex|pixel|compute] [--listing]
       python tools/ampere/lower.py --batch <extracted dir> <out_dir> [--limit N] [--only <hash list>]
         writes <out>/<code>.code.bin + .ctrl.bin + manifest.tsv in the layout spirv_harness --batch reads, and
         failures.tsv (one line per block not lowered, with its reason)
"""
from __future__ import annotations

import collections
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import maxwell as mx  # noqa: E402
import sm86  # noqa: E402
from sm86 import CBuf, Imm, Pred, Reg  # noqa: E402

RZ = 255
PT = 7
NVN1_HEADER = 0x30
MAXWELL_SPH = 0x50
COMPUTE_START = 0x100


class Unsupported(Exception):
    pass


# Ampere FMUL bits 84..86 (NAK: "PDIV", always 4 there) -> Maxwell MultiplyScale (NoScale 0, D2 1, D4 2, D8 3,
# M8 4, M4 5, M2 6). Inferred: symmetric around 4 = none; checked with the equivalence test on the twins (§5g).
FMUL_SCALE = {4: 0, 3: 1, 2: 2, 1: 3, 5: 6, 6: 5, 7: 4}


def f32_imm20_ok(v: int) -> bool:
    return (v & 0xFFF) == 0


def i_imm20_ok(v: int) -> bool:
    s = v - (1 << 32) if v & 0x80000000 else v
    return -(1 << 19) <= s < (1 << 19)


def imm20(v: int) -> int:
    """Signed 32-bit value -> the 20-bit Imm20 field (19 bits + sign at bit 19)."""
    return v & 0xFFFFF


# ---- emission ------------------------------------------------------------------------------------------------

class Out:
    def __init__(self):
        self.items = []          # (kind, payload): ("w", word) or ("bra", (name, fields, target_ampere_addr))
        self.label_at = {}       # ampere address -> item index

    def label(self, addr):
        self.label_at[addr] = len(self.items)

    def w(self, name, **kv):
        self.items.append(("w", (name, kv)))

    def bra(self, target, **kv):
        self.items.append(("bra", (target, kv)))


class Lowerer:
    def __init__(self, insns, stage, nregs_hint=0, c1=b"", sph=b""):
        self.insns = insns
        self.stage = stage
        self.sph = sph  # the Ampere SPH (input map of indexed IPA)
        self.c1 = c1  # the block's inline constants (jump tables of BRX)
        used_r, used_ur, used_p = set(), set(), set()
        for i in insns:
            for v in i.f.values():
                if isinstance(v, Reg) and not v.is_zero():
                    (used_ur if v.uniform else used_r).add(v.n)
                elif isinstance(v, Pred) and v.n != PT and not v.uniform:
                    used_p.add(v.n)
            if i.guard.n != PT:
                used_p.add(i.guard.n)
            if i.op in ("P2R", "R2P"):  # the mask names predicates implicitly
                m = i.f["mask"].v if isinstance(i.f["mask"], Imm) else 0x7F
                used_p.update(b for b in range(7) if m >> b & 1)
        # vector operands reach past the register they name: leave 8 registers of headroom
        top = max([nregs_hint] + [r + 8 for r in used_r]) if used_r else max(nregs_hint, 8)
        self.ur_base = min(top, 254)
        self.tmp_base = self.ur_base + (64 if used_ur else 0)
        self.tmp_next = 0
        if self.tmp_base + 17 > 254:
            raise Unsupported(f"no room for uniform registers / temporaries (top R{top})")
        self.borrowed = None
        self.cur = None
        free_p = [p for p in range(7) if p not in used_p]
        self.upred = {}
        upreds = sorted({v.n for i in insns for v in list(i.f.values()) + [i.guard]
                         if isinstance(v, Pred) and v.uniform and v.n != PT})
        # Uniform predicates go to predicates the program leaves free; when there are not enough, they live in
        # general registers (0/1) instead, and each instruction that uses one borrows a predicate around itself
        # (_one_spill: P2R save, ISETP load, the instruction, SEL store, R2P restore).
        self.up_spill = None
        self.up_override = {}
        if len(upreds) > len(free_p):
            self.up_spill = {u: self.tmp_base + k for k, u in enumerate(upreds)}
            self.up_save = self.tmp_base + 8
            self.tmp_base += 16
            if self.tmp_base + 17 > 254:
                raise Unsupported("no room for uniform predicate registers")
        else:
            for u, p in zip(upreds, free_p):
                self.upred[u] = p
        self.free_p = free_p[len(upreds):] if self.up_spill is None else []
        self.borrow_reg = self.tmp_base + 16
        self.out = Out()
        self.guard = (PT, False)

    # -- registers and predicates
    def r(self, x: Reg) -> int:
        if x.is_zero():
            return RZ
        return self.ur_base + x.n if x.uniform else x.n

    def p(self, x: Pred):
        if x.uniform and x.n != PT:
            if self.up_spill is not None:
                if x.n not in self.up_override:
                    raise Unsupported("uniform predicate outside an instruction that loads it")
                return self.up_override[x.n], x.neg
            return self.upred[x.n], x.neg
        return x.n, x.neg

    def tmp(self) -> int:
        t = self.tmp_base + self.tmp_next
        self.tmp_next += 1
        if t > 254 or self.tmp_next > 16:
            raise Unsupported("out of temporaries")
        return t

    def tmp_pred(self) -> int:
        if self.free_p:
            return self.free_p[0]
        # every predicate is in use: borrow one this instruction does not name, saved (P2R) now and restored (R2P)
        # once the instruction's Maxwell code is done (straight-line uses only; branches use condition codes)
        if self.borrowed is not None:
            return self.borrowed
        refs = {v.n for v in list(self.cur.f.values()) + [self.cur.guard] if isinstance(v, Pred) and not v.uniform}
        refs |= set(self.up_override.values())
        victims = [q for q in range(7) if q not in refs]
        if not victims:
            raise Unsupported("no predicate to borrow")
        self.borrowed = victims[0]
        self.emit("P2rI", Dest=self.borrow_reg, SrcA=RZ, Imm20=1 << self.borrowed, ByteSel=0, Ccpr=False, Pred=PT)
        return self.borrowed

    def emit(self, name, **kv):
        # Maxwell's LOP3 register form also writes a predicate (DestPred, bits 48..50, with PredicateOp): left at 0
        # it wrote P0 = false after every lowered LOP3 (PredicateOp F), and a later @!P0 EXIT ran always (the map's
        # bushes: a pixel shader returned black right after its alpha test). PT unless asked for.
        if name == "Lop3R":
            kv.setdefault("DestPred", PT)
        if "Pred" not in kv:
            g, inv = self.guard
            kv["Pred"] = g
            if inv:
                kv.setdefault("PredInv", True)
        self.out.w(name, **kv)

    # -- operand materialisation
    def to_reg(self, x, allow_mods=False) -> int:
        """Any operand -> a register holding its value (modifiers applied unless allow_mods)."""
        if isinstance(x, Reg):
            if (x.neg or x.abs) and not allow_mods:
                t = self.tmp()
                self.emit("FaddR", Dest=t, SrcA=self.r(x), SrcB=RZ, NegA=x.neg, AbsA=x.abs)
                return t
            return self.r(x)
        t = self.tmp()
        if isinstance(x, Imm):
            self.emit("Mov32i", Dest=t, Imm32=x.v, QuadMask=0xF)
        elif isinstance(x, CBuf):
            self._cbuf_check(x)
            self.emit("MovC", Dest=t, CbufSlot=x.bank, CbufOffset=x.offset // 4, QuadMask=0xF)
            if x.neg or x.abs:
                self.emit("FaddR", Dest=t, SrcA=t, SrcB=RZ, NegA=x.neg, AbsA=x.abs)
        else:
            raise Unsupported(f"operand {x}")
        return t

    def to_ireg(self, x) -> int:
        """Integer operand -> register; integer negation is two's complement."""
        if isinstance(x, Reg) and (x.neg or x.abs):
            raise Unsupported("integer source modifier outside IADD3/IMAD")
        if isinstance(x, CBuf) and (x.neg or x.abs):
            raise Unsupported("integer cbuf modifier")
        return self.to_reg(x, allow_mods=True)

    def _cbuf_check(self, c: CBuf):
        if c.ureg is not None:
            raise Unsupported("bindless constant buffer")
        if c.bank > 17:
            raise Unsupported(f"constant bank c{c.bank}")
        if c.offset % 4:
            raise Unsupported("unaligned constant offset")

    def fp_form(self, base, b, extra, b_mods=True):
        """Emit <base>R/I/C/32i for a 2-operand FP op whose second operand is b (a is in extra['SrcA'])."""
        if isinstance(b, Reg):
            mods = {"NegB": b.neg, "AbsB": b.abs} if b_mods else {}
            if (b.neg or b.abs) and not b_mods:
                self.emit(base + "R", SrcB=self.to_reg(b), **extra)
            else:
                self.emit(base + "R", SrcB=self.r(b), **mods, **extra)
        elif isinstance(b, Imm):
            if f32_imm20_ok(b.v):
                self.emit(base + "I", Imm20=b.v >> 12, **extra)
            else:
                self.emit(base + "R", SrcB=self.to_reg(b), **extra)
        elif isinstance(b, CBuf):
            self._cbuf_check(b)
            mods = {"NegB": b.neg, "AbsB": b.abs} if b_mods else {}
            if (b.neg or b.abs) and not b_mods:
                self.emit(base + "R", SrcB=self.to_reg(b), **extra)
            else:
                self.emit(base + "C", CbufSlot=b.bank, CbufOffset=b.offset // 4, **mods, **extra)
        else:
            raise Unsupported(f"operand {b}")

    def int_form(self, base, b, extra, imm32=None):
        """<base>R/I/C for an integer op; immediates that need 32 bits use <imm32> or a scratch register."""
        if isinstance(b, Reg):
            if b.neg or b.abs:
                raise Unsupported(f"{base}: modifier on integer source")
            self.emit(base + "R", SrcB=self.r(b), **extra)
        elif isinstance(b, Imm):
            if i_imm20_ok(b.v):
                self.emit(base + "I", Imm20=imm20(b.v), **extra)
            elif imm32:
                self.emit(imm32, Imm32=b.v, **{k: v for k, v in extra.items()})
            else:
                self.emit(base + "R", SrcB=self.to_reg(b), **extra)
        elif isinstance(b, CBuf):
            self._cbuf_check(b)
            self.emit(base + "C", CbufSlot=b.bank, CbufOffset=b.offset // 4, **extra)
        else:
            raise Unsupported(f"operand {b}")

    # -- the instructions
    def _one(self, i, guard):
        self.tmp_next = 0
        self.cur = i
        self.borrowed = None
        if i.unknown_bits:
            raise Unsupported(f"{i.op}: unexplained bits {i.unknown_bits}")
        self.guard = guard
        h = getattr(self, "op_" + i.op.replace(".", "_"), None)
        if h is None:
            raise Unsupported(i.op)
        h(i, i.f)
        if self.borrowed is not None:
            self.emit("R2pI", SrcA=self.borrow_reg, Imm20=1 << self.borrowed, ByteSel=0, Ccpr=False, Pred=PT)
            self.borrowed = None

    _PRED_OUT = ("dst", "dst1", "pdst", "cout", "cout0", "cout1", "fault")

    def _one_spill(self, i):
        preds = [v for v in list(i.f.values()) + [i.guard] if isinstance(v, Pred) and v.n != PT]
        ups = sorted({v.n for v in preds if v.uniform}) if self.up_spill is not None else []
        if not ups:
            return self._one(i, self.p(i.guard))
        if i.op in ("P2R", "R2P", "RET"):
            raise Unsupported(f"{i.op} with a spilled uniform predicate")
        used = {v.n for v in preds if not v.uniform}
        scratch = [q for q in range(7) if q not in used][:len(ups)]
        if len(scratch) < len(ups):
            raise Unsupported("no predicate to borrow")
        for k, (u, q) in enumerate(zip(ups, scratch)):
            self.emit("P2rI", Dest=self.up_save + k, SrcA=RZ, Imm20=1 << q, ByteSel=0, Ccpr=False, Pred=PT)
            self.emit("IsetpR", SrcA=self.up_spill[u], SrcB=RZ, IComp=5, Signed=False, Bop=0, SrcPred=PT,
                      DestPred=q, DestPredInv=PT, Pred=PT)
            self.up_override[u] = q
        self._one(i, self.p(i.guard))
        written = {i.f[k].n for k in self._PRED_OUT if isinstance(i.f.get(k), Pred) and i.f[k].uniform
                   and i.f[k].n != PT}
        for u in written:
            self.emit("SelI", Dest=self.up_spill[u], SrcA=RZ, Imm20=1, SrcPred=self.up_override[u], SrcPredInv=True,
                      Pred=PT)
        for k, (u, q) in enumerate(zip(ups, scratch)):
            self.emit("R2pI", SrcA=self.up_save + k, Imm20=1 << q, ByteSel=0, Ccpr=False, Pred=PT)
        self.up_override = {}

    def _writes_pred(self, i, pn):
        if i.op in ("R2P",):
            m = i.f["mask"].v if isinstance(i.f["mask"], Imm) else 0x7F
            return bool(m >> pn & 1)
        # A spilled uniform predicate lives in a register (_one_spill restores the predicate it borrows), so writing it
        # never changes a regular predicate.
        return any(isinstance(i.f.get(k), Pred) and not (i.f[k].uniform and self.up_spill is not None)
                   and self.p(i.f[k])[0] == pn for k in self._PRED_OUT)

    def _windows(self):
        """Runs of instructions predicated on one predicate (P or !P) that contain a global memory access, write
        no predicate of that register, contain no control flow and are not branched into. The translator emits
        every predicated instruction as its own conditional block, so an address computed under @!P and used by an
        @!P LDG reaches it through phi nodes, and its search for the storage buffer behind the address fails
        ("Failed to find storage buffer"). Emitted instead as if/else regions: since exactly one of the two groups
        runs, putting each group's instructions together keeps the meaning."""
        targets = {i.addr + 16 + i.f["target_rel"] for i in self.insns if i.op in ("BRA",)}
        targets |= {c.addr + 16 for c in self.insns if c.op == "CALL"}
        for b in self.insns:
            if b.op == "BRX":
                targets |= set(self.brx_targets(b))
        wins, k, n = {}, 0, len(self.insns)
        cf = ("BRA", "BRA.U", "EXIT", "KILL", "CALL", "RET", "BRX", "BSSY", "BSYNC", "BREAK", "WARPSYNC")
        while k < n:
            g = self.insns[k].guard
            if g.n == PT or g.uniform or self.insns[k].op in cf:
                k += 1
                continue
            pn = self.p(g)[0]
            j = k
            while (j < n and self.insns[j].guard.n == g.n and self.insns[j].guard.uniform == g.uniform
                   and self.insns[j].op not in cf and (j == k or self.insns[j].addr not in targets)):
                if self._writes_pred(self.insns[j], pn):
                    break
                j += 1
            if j - k >= 2 and any(self.insns[x].op in ("LDG", "STG") for x in range(k, j)):
                wins[k] = j
                k = j
            else:
                k += 1
        return wins

    def run(self):
        wins = self._windows()
        k, n, syn = 0, len(self.insns), 0
        while k < n:
            i = self.insns[k]
            if k not in wins:
                self.out.label(i.addr)
                self._one_spill(i)
                k += 1
                continue
            j = wins[k]
            pn, _ = self.p(i.guard)
            run = self.insns[k:j]
            when_false = [x for x in run if self.p(x.guard)[1]]        # @!P: run when P is false
            when_true = [x for x in run if not self.p(x.guard)[1]]     # @P
            l_else, l_end = ("syn", syn), ("syn", syn + 1)
            syn += 2
            self.out.label(i.addr)
            self.out.bra(l_else, Pred=pn, PredInv=False, Ccc=15)      # P true -> skip the @!P group
            for x in when_false:
                self._one(x, (PT, False))
            self.out.bra(l_end, Pred=PT, PredInv=False, Ccc=15)
            self.out.label(l_else)
            for x in when_true:
                self._one(x, (PT, False))
            self.out.label(l_end)
            k = j
        self.guard = (PT, False)
        self.out.label(self.insns[-1].addr + 16 if self.insns else 0)
        return self.out

    # moves / conversions --------------------------------------------------------------------------------------
    def op_NOP(self, i, f):
        pass

    op_DEPBAR = op_BSSY = op_BSYNC = op_BREAK = op_WARPSYNC = op_NOP

    def op_MOV(self, i, f):
        s = f["src"]
        if f.get("lanes", 0xF) != 0xF:
            raise Unsupported("MOV with lane mask")
        d = self.r(f["dst"])
        if isinstance(s, Imm):
            self.emit("Mov32i", Dest=d, Imm32=s.v, QuadMask=0xF)
        elif isinstance(s, CBuf):
            self._cbuf_check(s)
            self.emit("MovC", Dest=d, CbufSlot=s.bank, CbufOffset=s.offset // 4, QuadMask=0xF)
        else:
            self.emit("MovR", Dest=d, SrcA=self.r(s), QuadMask=0xF)

    op_UMOV = op_MOV

    def op_R2UR(self, i, f):
        self.emit("MovR", Dest=self.r(f["dst"]), SrcA=self.r(f["src"]), QuadMask=0xF)

    def op_ULDC(self, i, f):
        a = self.r(f["offset_reg"]) if "offset_reg" in f else RZ
        self._ldc(self.r(f["dst"]), a, f["bank"], f["offset"], f["type"])

    def op_LDC(self, i, f):
        if "handle_ureg" in f:
            raise Unsupported("bindless LDC")
        if f["mode"] != 0:
            raise Unsupported("LDC addressing mode")
        self._ldc(self.r(f["dst"]), self.r(f["offset_reg"]), f["bank"], f["offset"], f["type"])

    def _ldc(self, d, a, bank, off, typ):
        if bank > 17:
            raise Unsupported(f"constant bank c{bank}")
        size = {"U8": 0, "S8": 1, "U16": 2, "S16": 3, "32": 4, "64": 5, "128": 6}[typ]
        nwords = {4: 1, 5: 2, 6: 4}.get(size, 0)
        if (bank == 1 and a == RZ and nwords and off % 4 == 0 and d != RZ and self.c1
                and off + 4 * nwords <= len(self.c1)):
            # c[1] is the block's own inline constants (fixed data, served as they are by the runtime): a load at
            # a constant offset is folded into immediates. The translator then sees a plain value; loaded, it
            # took such a value as a storage-buffer pointer when it was added to one (UIMAD.WIDE 1 * c0[0xe0] +
            # c1[0]: a buffer base plus a byte offset, in Wonder's Switch 2 effect compute shaders), and the
            # shader was refused.
            for k in range(nwords):
                v = struct.unpack_from("<I", self.c1, off + 4 * k)[0]
                self.emit("Mov32i", Dest=d + k, Imm32=v, QuadMask=0xF)
            return
        if a == RZ and size in (4, 5, 6) and off % 4 == 0 and d != RZ:
            # constant offset: MOV from the constant buffer, one word at a time. The translator sees constant-buffer
            # operands (as for a c[] source), which its storage-buffer and bindless analyses follow; an LDC becomes
            # an indexed load they do not look through.
            for k in range({4: 1, 5: 2, 6: 4}[size]):
                self.emit("MovC", Dest=d + k, CbufSlot=bank, CbufOffset=off // 4 + k, QuadMask=0xF)
            return
        if size == 6:
            raise Unsupported("LDC.128")
        off16 = off - 0x10000 if off & 0x8000 else off
        self.emit("Ldc", Dest=d, SrcA=a, CbufSlot=bank, CbufOffset=off16 & 0xFFFF, LsSize=size)

    def op_S2R(self, i, f):
        self.emit("S2r", Dest=self.r(f["dst"]), SReg=f["sr"])

    op_S2UR = op_S2R

    def op_CS2R(self, i, f):
        if f["sr"] != 0xFF:
            raise Unsupported(f"CS2R SR {f['sr']:#x}")
        d = self.r(f["dst"])
        self.emit("MovR", Dest=d, SrcA=RZ, QuadMask=0xF)  # CS2R SRZ: zero (a register pair when .64)
        if f["wide"]:
            self.emit("MovR", Dest=d + 1, SrcA=RZ, QuadMask=0xF)

    # floating point -------------------------------------------------------------------------------------------
    @staticmethod
    def _fmz(f):
        return 2 if f.get("dnz") else (1 if f.get("ftz") else 0)

    def op_FMUL(self, i, f):
        scale = FMUL_SCALE.get(f["pdiv"])
        if scale is None:
            raise Unsupported(f"FMUL scale {f['pdiv']}")
        a, b = f["a"], f["b"]
        neg = a.neg ^ (getattr(b, "neg", False))
        ra = self.to_reg(Reg(a.n, False, a.abs, a.uniform)) if a.abs else self.r(a)
        if isinstance(b, (Reg, CBuf)) and b.abs:
            b = Reg(self.to_reg(b), False, False) if False else b
            rb = self.to_reg(b)
            b = Reg(rb)
        elif isinstance(b, Reg):
            b = Reg(b.n, False, False, b.uniform)
        elif isinstance(b, CBuf):
            b = CBuf(b.bank, b.offset, b.ureg)
        extra = dict(Dest=self.r(f["dst"]), SrcA=ra, NegA=neg, Sat=f["sat"], RoundMode=_rnd(f["rnd"]),
                     Fmz=self._fmz(f), Scale=scale)
        if isinstance(b, Imm) and not f32_imm20_ok(b.v):
            if not f["sat"] and f["rnd"] == "RN" and scale == 0:
                v = b.v ^ (0x80000000 if neg else 0)
                self.emit("Fmul32i", Dest=extra["Dest"], SrcA=ra, Imm32=v, Fmz=extra["Fmz"], Sat=False)
                return
        if isinstance(b, Imm):
            if f32_imm20_ok(b.v):
                self.emit("FmulI", Imm20=b.v >> 12, **extra)
            else:
                self.emit("FmulR", SrcB=self.to_reg(b), **extra)
        elif isinstance(b, CBuf):
            self._cbuf_check(b)
            self.emit("FmulC", CbufSlot=b.bank, CbufOffset=b.offset // 4, **extra)
        else:
            self.emit("FmulR", SrcB=self.r(b), **extra)

    def op_FADD(self, i, f):
        a, b = f["a"], f["b"]
        extra = dict(Dest=self.r(f["dst"]), SrcA=self.r(a), NegA=a.neg, AbsA=a.abs, Sat=f["sat"],
                     RoundMode=_rnd(f["rnd"]), Ftz=f["ftz"])
        if isinstance(b, Imm) and not f32_imm20_ok(b.v) and not f["sat"] and f["rnd"] == "RN":
            self.emit("Fadd32i", Dest=extra["Dest"], SrcA=extra["SrcA"], NegA=a.neg, AbsA=a.abs, Imm32=b.v,
                      Ftz=f["ftz"])
            return
        self.fp_form("Fadd", b, extra)

    def op_FFMA(self, i, f):
        a, b, c = f["a"], f["b"], f["c"]
        # |x| has no Maxwell FFMA form: such an operand is materialised with its negation applied (to_reg), and
        # then must not count again in the sign of the product
        neg_a = a.neg and not a.abs
        ra = self.to_reg(a) if a.abs else self.r(a)
        neg_b = getattr(b, "neg", False) and not getattr(b, "abs", False)
        neg_ab = neg_a ^ neg_b
        if getattr(b, "abs", False):
            b = Reg(self.to_reg(b))
        elif isinstance(b, Reg):
            b = Reg(b.n, uniform=b.uniform)
        elif isinstance(b, CBuf):
            b = CBuf(b.bank, b.offset, b.ureg)
        neg_c = getattr(c, "neg", False)
        if getattr(c, "abs", False):
            c = Reg(self.to_reg(c))
            neg_c = False
        extra = dict(Dest=self.r(f["dst"]), SrcA=ra, NegA=neg_ab, NegC=neg_c, Sat=f["sat"],
                     RoundMode=_rnd(f["rnd"]), Fmz=self._fmz(f))
        if isinstance(c, Reg):
            rc = self.r(c)
            if isinstance(b, Imm):
                if f32_imm20_ok(b.v):
                    self.emit("FfmaI", Imm20=b.v >> 12, SrcC=rc, **extra)
                else:
                    self.emit("FfmaR", SrcB=self.to_reg(b), SrcC=rc, **extra)
            elif isinstance(b, CBuf):
                self._cbuf_check(b)
                self.emit("FfmaC", CbufSlot=b.bank, CbufOffset=b.offset // 4, SrcC=rc, **extra)
            else:
                self.emit("FfmaR", SrcB=self.r(b), SrcC=rc, **extra)
        elif isinstance(c, CBuf) and isinstance(b, Reg):
            self._cbuf_check(c)
            # RC form: the register field SrcC holds b, the constant buffer is c (Ryujinx FfmaRc)
            self.emit("FfmaRc", SrcC=self.r(b), CbufSlot=c.bank, CbufOffset=c.offset // 4, **extra)
        else:
            rc = self.to_reg(CBuf(c.bank, c.offset, c.ureg) if isinstance(c, CBuf) else c)
            rb = self.r(b) if isinstance(b, Reg) else self.to_reg(b)
            self.emit("FfmaR", SrcB=rb, SrcC=rc, **extra)

    def op_FMNMX(self, i, f):
        a, b = f["a"], f["b"]
        p, inv = self.p(f["min"])
        extra = dict(Dest=self.r(f["dst"]), SrcA=self.r(a), NegA=a.neg, AbsA=a.abs, SrcPred=p, SrcPredInv=inv,
                     Ftz=f["ftz"])
        self.fp_form("Fmnmx", b, extra)

    def _fsetp(self, f, dst_kind):
        a, b = f["a"], f["b"]
        p, pinv = self.p(f["p"])
        extra = dict(SrcA=self.r(a), NegA=a.neg, AbsA=a.abs, FComp=_fcmp(f["cmp"]), Bop=_bop(f["bop"]),
                     SrcPred=p, SrcPredInv=pinv, Ftz=f["ftz"])
        extra.update(dst_kind)
        return a, b, extra

    def op_FSETP(self, i, f):
        d, dinv = self.p(f["dst"])
        d1, _ = self.p(f["dst1"])
        a, b, extra = self._fsetp(f, dict(DestPred=d, DestPredInv=d1))
        self.fp_form("Fsetp", b, extra)

    def op_FSET(self, i, f):
        a, b, extra = self._fsetp(f, dict(Dest=self.r(f["dst"]), BVal=True))
        self.fp_form("Fset", b, extra)

    def op_MUFU(self, i, f):
        if f["f16"]:
            raise Unsupported("MUFU.F16")
        s = f["src"]
        fn = {"COS": 0, "SIN": 1, "EX2": 2, "LG2": 3, "RCP": 4, "RSQ": 5, "RCP64H": 6, "RSQ64H": 7, "SQRT": 8}
        if f["fn"] not in fn:
            raise Unsupported(f"MUFU.{f['fn']}")
        if isinstance(s, Reg):
            ra, neg, ab = self.r(s), s.neg, s.abs
        else:
            ra, neg, ab = self.to_reg(s), False, False
        if f["fn"] in ("SIN", "COS"):
            # Volta+ MUFU.SIN/COS take the angle in revolutions (sin(2*pi*x); the compiler multiplies by 1/(2*pi)
            # first, as NAK does). Maxwell had RRO before MUFU, which the translator reads as a move and then
            # takes MUFU.SIN/COS in radians: convert back.
            t = self.tmp()
            self.emit("Fmul32i", Dest=t, SrcA=ra, Imm32=0x40C90FDB, Fmz=0)  # 2*pi
            if neg or ab:
                self.emit("FaddR", Dest=t, SrcA=t, SrcB=RZ, NegA=neg, AbsA=ab)
            ra, neg, ab = t, False, False
        self.emit("Mufu", Dest=self.r(f["dst"]), SrcA=ra, MufuOp=fn[f["fn"]], NegA=neg, AbsA=ab)

    def op_FRND(self, i, f):
        if f["dst_bytes"] != 4 or f["src_bytes"] != 4:
            raise Unsupported("FRND not F32")
        s = f["src"]
        # Maxwell FRND is F2F.F32.F32 with integer rounding (IntegerRound: Round 4, Floor 5, Ceil 6, Trunc 7)
        extra = dict(Dest=self.r(f["dst"]), RoundMode={"RN": 4, "RM": 5, "RP": 6, "RZ": 7}[f["rnd"]], Ftz=f["ftz"],
                     DstFmt=2, SrcFmt=2)
        if isinstance(s, Reg):
            self.emit("F2fR", SrcB=self.r(s), NegB=s.neg, AbsB=s.abs, **extra)
        else:
            self.emit("F2fR", SrcB=self.to_reg(s), **extra)

    def op_F2I(self, i, f):
        if f["src_bytes"] != 4:
            raise Unsupported("F2I from non-F32")
        fmt = {(2, False): 1, (4, False): 2, (2, True): 5, (4, True): 6}.get((f["dst_bytes"], f["signed"]))
        if fmt is None:
            raise Unsupported("F2I destination size")
        s = f["src"]
        extra = dict(Dest=self.r(f["dst"]), IDstFmt=fmt, SrcFmt=2, RoundMode=_rnd2(f["rnd"]), Ftz=f["ftz"])
        if isinstance(s, Reg):
            self.emit("F2iR", SrcB=self.r(s), NegB=s.neg, AbsB=s.abs, **extra)
        else:
            self.emit("F2iR", SrcB=self.to_reg(s), **extra)

    def op_I2F(self, i, f):
        if f["dst_bytes"] != 4:
            raise Unsupported("I2F to non-F32")
        sz = {1: 0, 2: 1, 4: 2}.get(f["src_bytes"])
        if sz is None:
            raise Unsupported("I2F source size")
        fmt = sz + (4 if f["signed"] else 0)
        s = f["src"]
        rs = self.r(s) if isinstance(s, Reg) else self.to_reg(s)
        if isinstance(s, Reg) and (s.neg or s.abs):
            raise Unsupported("I2F source modifier")
        self.emit("I2fR", Dest=self.r(f["dst"]), SrcB=rs, ISrcFmt=fmt, DstFmt=2, RoundMode=_rnd(f["rnd"]),
                  ByteSel=f["byte_sel"])

    op_I2FP = op_I2F

    # integer ----------------------------------------------------------------------------------------------------
    def op_SEL(self, i, f):
        p, inv = self.p(f["cond"])
        a, b = f["a"], f["b"]
        if a.neg or a.abs or getattr(b, "neg", False) or getattr(b, "abs", False):
            if i.op != "FSEL":
                raise Unsupported("SEL modifiers")
        ra = self.to_reg(a) if (a.neg or a.abs) else self.r(a)
        if isinstance(b, (Reg, CBuf)) and (b.neg or b.abs):
            b = Reg(self.to_reg(b))
        self.int_form("Sel", b, dict(Dest=self.r(f["dst"]), SrcA=ra, SrcPred=p, SrcPredInv=inv))

    op_FSEL = op_USEL = op_SEL

    def op_ISETP(self, i, f):
        a, b = f["a"], f["b"]
        d, _ = self.p(f["dst"])
        d1, _ = self.p(f["dst1"])
        p, pinv = self.p(f["p"])
        if f["ex"]:
            # (a_hi:a_lo) cmp (b_hi:b_lo) given low = (a_lo cmp' b_lo): for EQ low = lo equal, for NE lo differ,
            # for the orderings low = the unsigned lo comparison with the same strictness (as nvcc emits it).
            if d1 != PT or (p, pinv) != (PT, False) or f["bop"] != "AND":
                raise Unsupported("ISETP.EX with combine")
            low, linv = self.p(f["low"])
            cmp = f["cmp"]
            ra = self._ireg(a)
            rb = b
            tp = self.tmp_pred()
            if cmp in ("EQ", "NE"):
                # EQ: hi == && low ; NE: hi != || low
                self.int_form("Isetp", rb, dict(SrcA=ra, IComp=_icmp(cmp), Signed=False, Bop=0 if cmp == "EQ" else 1,
                                                 SrcPred=low, SrcPredInv=linv, DestPred=d, DestPredInv=PT))
                return
            strict = {"LT": "LT", "LE": "LT", "GT": "GT", "GE": "GT"}[cmp]
            # d = (hi strict) || (hi == && low)
            self.int_form("Isetp", rb, dict(SrcA=ra, IComp=_icmp("EQ"), Signed=False, Bop=0, SrcPred=low,
                                             SrcPredInv=linv, DestPred=tp, DestPredInv=PT))
            self.int_form("Isetp", rb, dict(SrcA=ra, IComp=_icmp(strict), Signed=f["signed"], Bop=1, SrcPred=tp,
                                             SrcPredInv=False, DestPred=d, DestPredInv=PT))
            return
        if self.p(f["low"]) != (PT, False):
            raise Unsupported("ISETP low predicate without .EX")
        extra = dict(SrcA=self._ireg(a), IComp=_icmp(f["cmp"]), Signed=f["signed"], Bop=_bop(f["bop"]),
                     SrcPred=p, SrcPredInv=pinv, DestPred=d, DestPredInv=d1)
        self.int_form("Isetp", b, extra)

    op_UISETP = op_ISETP

    def op_LOP3(self, i, f):
        # predicate output: every instance in the corpus has p = !PT and no .PAND, i.e. P = (result != 0)
        pd, _ = self.p(f["pdst"])
        p_false = f["p"].n == PT and f["p"].neg      # !PT or !UPT
        if pd != PT and (f["pand"] or not p_false):
            raise Unsupported("LOP3 predicate output with an input predicate")
        if pd == PT and not (f["p"].n == PT):
            raise Unsupported("LOP3 predicate input")
        a, b, c = f["a"], f["b"], f["c"]
        for x in (a, b, c):
            if isinstance(x, (Reg, CBuf)) and (x.neg or x.abs):
                raise Unsupported("LOP3 modifiers")
        rc = self.to_ireg(c)
        d = self.r(f["dst"])
        if pd != PT:
            d = self.tmp()  # result first, then the predicate, then the destination (which may be a source)
        extra = dict(Dest=d, SrcA=self.r(a), SrcC=rc)
        if isinstance(b, Imm) and not i_imm20_ok(b.v):
            b = Reg(self.to_reg(b))
        if isinstance(b, Reg):
            self.emit("Lop3R", SrcB=self.r(b), Imm=f["lut"], **extra)
        elif isinstance(b, Imm):
            self.emit("Lop3I", Imm20=imm20(b.v), Imm=f["lut"], **extra)
        else:
            self._cbuf_check(b)
            self.emit("Lop3C", CbufSlot=b.bank, CbufOffset=b.offset // 4, Imm=f["lut"], **extra)
        if pd != PT:
            self.emit("IsetpR", SrcA=d, SrcB=RZ, IComp=5, Signed=False, Bop=0, SrcPred=PT, DestPred=pd,
                      DestPredInv=PT)
            if self.r(f["dst"]) != RZ:
                self.emit("MovR", Dest=self.r(f["dst"]), SrcA=d, QuadMask=0xF)

    op_ULOP3 = op_LOP3

    # -- integer helpers: carries without touching the program's predicates
    def _carry_reg(self, s, x):
        """Register = -1 if the unsigned add that produced s from x carried (s < x), else 0 (Maxwell ISET)."""
        t = self.tmp()
        self.emit("IsetR", Dest=t, SrcA=s, SrcB=x, IComp=1, Signed=False, Bop=0, SrcPred=PT, BVal=False)
        return t

    def _set_pred_from_reg(self, pd, r):
        """Predicate pd = (r != 0)."""
        self.emit("IsetpR", SrcA=r, SrcB=RZ, IComp=5, Signed=False, Bop=0, SrcPred=PT, DestPred=pd,
                  DestPredInv=PT)

    def _ireg(self, x, bnot=False):
        """Integer operand -> register (value, no modifier); bnot applies a bitwise NOT (for .X forms)."""
        if isinstance(x, Reg) and not x.neg and not x.abs and not bnot:
            return self.r(x)
        if isinstance(x, (CBuf,)) and (x.neg or x.abs):
            x = CBuf(x.bank, x.offset, x.ureg)
        r = self.to_reg(Reg(x.n, uniform=x.uniform) if isinstance(x, Reg) else x, allow_mods=True)
        if bnot or (isinstance(x, Reg) and x.neg and bnot):
            t = self.tmp()
            self.emit("Lop3R", Dest=t, SrcA=r, SrcB=RZ, SrcC=RZ, Imm=0x0F)  # ~a
            return t
        return r

    def op_IADD3(self, i, f):
        a, b, c = f["a"], f["b"], f["c"]
        d = self.r(f["dst"])
        c0, _ = self.p(f["cout0"])
        c1, _ = self.p(f["cout1"])
        if c1 != PT:
            raise Unsupported("IADD3 second carry out")
        if f["x"]:
            # .X: a + b + c + cin0 + cin1, where a source "negation" is a bitwise NOT (NAK fold of OpIAdd3X)
            ops = [self._ireg(Reg(x.n, uniform=x.uniform) if isinstance(x, Reg) else x,
                              bnot=getattr(x, "neg", False)) for x in (a, b, c)]
            t = self.tmp()
            self.emit("Iadd3R", Dest=t, SrcA=ops[0], SrcB=ops[1], SrcC=ops[2])
            for cin in (f["cin0"], f["cin1"]):
                pn, pinv = self.p(cin)
                if (pn, pinv) == (PT, True):
                    continue                      # !PT: no carry in
                if (pn, pinv) == (PT, False):
                    self.emit("Iadd3I", Dest=t, SrcA=t, Imm20=1, SrcC=RZ)
                    continue
                one = self.tmp()
                self.emit("SelI", Dest=one, SrcA=RZ, Imm20=1, SrcPred=pn, SrcPredInv=not pinv)  # P ? 1 : 0
                self.emit("Iadd3R", Dest=t, SrcA=t, SrcB=one, SrcC=RZ)
            if c0 != PT:
                raise Unsupported("IADD3.X carry out")
            self.emit("MovR", Dest=d, SrcA=t, QuadMask=0xF)
            return
        for cin in (f["cin0"], f["cin1"]):
            if self.p(cin) != (PT, True):
                raise Unsupported("IADD3 carry in without .X")
        if c0 == PT:
            rc = self.r(c) if isinstance(c, Reg) else self.to_ireg(c)
            extra = dict(Dest=d, SrcA=self.r(a), NegA=a.neg, SrcC=rc, NegC=c.neg if isinstance(c, Reg) else False)
            if isinstance(b, Reg):
                self.emit("Iadd3R", SrcB=self.r(b), NegB=b.neg, **extra)
            elif isinstance(b, Imm) and i_imm20_ok(b.v):
                self.emit("Iadd3I", Imm20=imm20(b.v), **extra)
            elif isinstance(b, Imm):
                self.emit("Iadd3R", SrcB=self.to_reg(b), **extra)
            else:
                self._cbuf_check(b)
                self.emit("Iadd3C", CbufSlot=b.bank, CbufOffset=b.offset // 4, NegB=b.neg, **extra)
            return
        # carry out (64-bit arithmetic). Supported shapes, with their exact carry (NAK fold of OpIAdd3):
        #   x + y (+ RZ):  carry = sum < x             -x + y / x - y:  carry = (x - y does not borrow) = y <= x
        terms = [x for x in (a, b, c) if not (isinstance(x, Reg) and x.is_zero())]
        negs = [getattr(x, "neg", False) for x in terms]
        if len(terms) != 2 or all(negs):
            raise Unsupported("IADD3 carry out with this operand shape")
        if negs[0]:
            terms, negs = [terms[1], terms[0]], [False, True]
        x = self._ireg(Reg(terms[0].n, uniform=terms[0].uniform) if isinstance(terms[0], Reg) else terms[0])
        y = self._ireg(Reg(terms[1].n, uniform=terms[1].uniform) if isinstance(terms[1], Reg) else terms[1])
        t = self.tmp()
        self.emit("Iadd3R", Dest=t, SrcA=x, SrcB=y, NegB=negs[1], SrcC=RZ)
        if negs[1]:
            # x - y carries (no borrow) when y <= x, including y == 0 (~0 + 1 = 2^32)
            self.emit("IsetpR", SrcA=y, SrcB=x, IComp=3, Signed=False, Bop=0, SrcPred=PT, DestPred=c0,
                      DestPredInv=PT)
        else:
            self.emit("IsetpR", SrcA=t, SrcB=x, IComp=1, Signed=False, Bop=0, SrcPred=PT, DestPred=c0,
                      DestPredInv=PT)
        self.emit("MovR", Dest=d, SrcA=t, QuadMask=0xF)

    op_UIADD3 = op_IADD3

    def op_SHF(self, i, f):
        lo, sh, hi_src = f["lo"], f["shift"], f["hi_src"]
        d = self.r(f["dst"])
        typ, right, hi, wrap = f["type"], f["right"], f["hi"], f["wrap"]
        z = lambda x: isinstance(x, Reg) and x.is_zero()  # noqa: E731
        if typ in ("U32", "S32") and not right and not hi and z(hi_src):
            src, signed, op = lo, False, "Shl"          # low word of (0:lo) << s
        elif typ in ("U32", "S32") and right and hi and z(lo):
            src, signed, op = hi_src, typ == "S32", "Shr"  # high word of (hi:0) >> s
        elif typ in ("U32", "S32") and not right and hi and z(lo):
            src, signed, op = hi_src, False, "Shl"      # high word of (hi:0) << s == hi << s
        elif typ in ("U32", "S32") and right and not hi and z(hi_src):
            src, signed, op = lo, False, "Shr"          # low word of (0:lo) >> s == lo >> s (logical: hi is 0)
        else:
            raise Unsupported(f"SHF {typ} {'R' if right else 'L'} {'HI' if hi else 'LO'} funnel")
        rs = self._ireg(src)
        kw = dict(Dest=d, SrcA=rs, M=wrap)
        if op == "Shr":
            kw["Signed"] = signed
        if isinstance(sh, Imm):
            v = sh.v & 31 if wrap else sh.v
            if v >= 32:
                if op == "Shr" and signed:
                    self.emit("ShrI", Imm20=31, **{**kw, "M": False})
                else:
                    self.emit("MovR", Dest=d, SrcA=RZ, QuadMask=0xF)
                return
            self.emit(op + "I", Imm20=v, **kw)
        else:
            self.emit(op + "R", SrcB=self._ireg(sh), **kw)

    op_USHF = op_SHF

    def op_IMAD(self, i, f):
        if f["x"]:
            # IMAD.X: a * b + c + carry. The compiler uses it with a zero product for the high word of a 64-bit
            # address (RZ * RZ + c[0][hi] + P): that is IADD3.X c + RZ + RZ with the same carry in.
            zero = lambda x: (isinstance(x, Reg) and x.is_zero()) or (isinstance(x, Imm) and x.v == 0)  # noqa: E731
            if not (zero(f["a"]) or zero(f["b"])):
                raise Unsupported("IMAD.X with a product")
            if self.p(f["cout"])[0] != PT:
                raise Unsupported("IMAD.X carry out")
            rz = Reg(RZ)
            g = dict(dst=f["dst"], a=f["c"], b=rz, c=rz, x=True, cin0=f["cin"], cin1=Pred(PT, True),
                     cout0=Pred(PT), cout1=Pred(PT))
            return self.op_IADD3(i, g)
        if self.p(f["cout"])[0] != PT:
            raise Unsupported("IMAD carry out")
        a, b, c = f["a"], f["b"], f["c"]
        d = self.r(f["dst"])
        negc = getattr(c, "neg", False)
        if isinstance(c, Reg):
            c = Reg(c.n, uniform=c.uniform)
        elif isinstance(c, CBuf):
            c = CBuf(c.bank, c.offset, c.ureg)
        ra = self._ireg(a)
        dst = self.tmp() if negc else d
        cz = RZ if negc else None
        kw = dict(Dest=dst, SrcA=ra, ASigned=f["signed"], BSigned=f["signed"])
        if cz is None and isinstance(c, CBuf) and isinstance(b, Reg):
            self._cbuf_check(c)
            self.emit("ImadRc", SrcC=self.r(b), CbufSlot=c.bank, CbufOffset=c.offset // 4, **kw)
        else:
            rc = RZ if cz is not None else self._ireg(c)
            if isinstance(b, Imm) and i_imm20_ok(b.v):
                self.emit("ImadI", Imm20=imm20(b.v), SrcC=rc, **kw)
            elif isinstance(b, CBuf):
                self._cbuf_check(b)
                self.emit("ImadC", CbufSlot=b.bank, CbufOffset=b.offset // 4, SrcC=rc, **kw)
            else:
                self.emit("ImadR", SrcB=self._ireg(b), SrcC=rc, **kw)
        if negc:
            self.emit("Iadd3R", Dest=d, SrcA=dst, SrcB=self._ireg(c), NegB=True, SrcC=RZ)

    op_UIMAD = op_IMAD

    def op_IMAD_WIDE(self, i, f):
        # (dst+1:dst) = a * b (64-bit product, signed per .S32) + (c+1:c)
        if f["x"] or self.p(f["cout"])[0] != PT:
            raise Unsupported("IMAD.WIDE.X / carry out")
        a, b, c = f["a"], f["b"], f["c"]
        if getattr(c, "neg", False) or not isinstance(c, Reg):
            raise Unsupported("IMAD.WIDE c form")
        d = self.r(f["dst"])
        ra = self._ireg(a)
        rb = self._ireg(b) if not isinstance(b, Imm) else self.to_reg(b)
        cl = self.r(c)
        ch = cl + 1 if cl != RZ else RZ
        sg = dict(ASigned=f["signed"], BSigned=f["signed"])
        lo, hi = self.tmp(), self.tmp()
        self.emit("ImadR", Dest=lo, SrcA=ra, SrcB=rb, SrcC=RZ, **sg)                 # low 32 bits of a*b
        self.emit("ImadR", Dest=hi, SrcA=ra, SrcB=rb, SrcC=ch, Hilo=True, **sg)      # high 32 bits + c_hi
        s_lo = self.tmp()
        self.emit("Iadd3R", Dest=s_lo, SrcA=lo, SrcB=cl, SrcC=RZ)
        carry = self._carry_reg(s_lo, lo)
        self.emit("Iadd3R", Dest=hi, SrcA=hi, SrcB=RZ, SrcC=carry, NegC=True)
        self.emit("MovR", Dest=d, SrcA=s_lo, QuadMask=0xF)
        if d != RZ:
            self.emit("MovR", Dest=d + 1, SrcA=hi, QuadMask=0xF)

    op_UIMAD_WIDE = op_IMAD_WIDE

    def op_LEA(self, i, f):
        if f["x"] or f["hi"] or f["neg_shifted"]:
            raise Unsupported("LEA.HI/.X/negated")
        if self.p(f["cin"]) != (PT, True):
            raise Unsupported("LEA carry in")
        a, b = f["a"], f["b"]
        d = self.r(f["dst"])
        co, _ = self.p(f["cout"])
        t = self.tmp()
        self.emit("ShlI", Dest=t, SrcA=self._ireg(a), Imm20=f["shift"], M=False)
        rb = self._ireg(Reg(b.n, uniform=b.uniform) if isinstance(b, Reg) else b)
        s2 = self.tmp()
        self.emit("Iadd3R", Dest=s2, SrcA=t, SrcB=rb, NegB=getattr(b, "neg", False), SrcC=RZ)
        if co != PT:
            if getattr(b, "neg", False):
                raise Unsupported("LEA carry out with negated b")
            self.emit("IsetpR", SrcA=s2, SrcB=t, IComp=1, Signed=False, Bop=0, SrcPred=PT, DestPred=co,
                      DestPredInv=PT)
        self.emit("MovR", Dest=d, SrcA=s2, QuadMask=0xF)

    op_ULEA = op_LEA

    def op_IMNMX(self, i, f):
        p, inv = self.p(f["min"])
        self.int_form("Imnmx", f["b"], dict(Dest=self.r(f["dst"]), SrcA=self._ireg(f["a"]), Signed=f["signed"],
                                            SrcPred=p, SrcPredInv=inv))

    def op_PLOP3(self, i, f):
        # Maxwell has PSETP: d = (a op1 b) op2 c. A truth table that is one of those shapes is rewritten; the corpus
        # uses a handful of tables (see WONDER_SWITCH2_PROGRESS.md).
        lut, lut1 = f["lut0"], f["lut1"]
        d0, _ = self.p(f["dst"])
        d1, _ = self.p(f["dst1"])
        if d1 != PT and lut1 not in (0x00,):
            raise Unsupported("PLOP3 second output")
        srcs = [self.p(f["a"]), self.p(f["b"]), self.p(f["c"])]
        shape = _psetp_shape(lut)
        if shape is None:
            raise Unsupported(f"PLOP3 table {lut:#x}")
        (ia, na), (ib, nb), op_ab, (ic, nc), op_c = shape
        pa, pb, pc = srcs[ia], srcs[ib], srcs[ic]
        self.emit("Psetp", DestPred=d0, DestPredInv=PT, Src1Pred=pa[0], Src1PredInv=pa[1] ^ na,
                  Src2Pred=pb[0], Src2PredInv=pb[1] ^ nb, BoolOpAB=op_ab, SrcPred=pc[0], SrcPredInv=pc[1] ^ nc,
                  BoolOpC=op_c)

    op_UPLOP3 = op_PLOP3

    def op_FLO(self, i, f):
        if self.p(f["pdst"])[0] != PT:
            raise Unsupported("FLO predicate output")
        self.emit("FloR", Dest=self.r(f["dst"]), SrcB=self._ireg(f["src"]), Sh=f["sh"], Signed=f["signed"],
                  NegB=f["not_"])

    op_UFLO = op_FLO

    def op_POPC(self, i, f):
        self.emit("PopcR", Dest=self.r(f["dst"]), SrcB=self._ireg(f["src"]), NegB=f["not_"])

    op_UPOPC = op_POPC

    def op_BREV(self, i, f):
        # Maxwell has no BREV; BFE.BREV with position 0, width 32 reverses (Ryujinx BfeR: Brev reverses the source)
        t = self.tmp()
        self.emit("Mov32i", Dest=t, Imm32=0x2000, QuadMask=0xF)  # width 32 at bits 8..15, position 0
        self.emit("BfeR", Dest=self.r(f["dst"]), SrcA=self._ireg(f["src"]), SrcB=t, Brev=True, Signed=False)

    op_UBREV = op_BREV

    def op_F2F(self, i, f):
        sb, db = f["src_bytes"], f["dst_bytes"]
        fmt = {2: 1, 4: 2}
        if sb not in fmt or db not in fmt:
            raise Unsupported("F2F size")
        s = f["src"]
        rs = self.r(s) if isinstance(s, Reg) else self.to_reg(s)
        self.emit("F2fR", Dest=self.r(f["dst"]), SrcB=rs, NegB=getattr(s, "neg", False),
                  AbsB=getattr(s, "abs", False), SrcFmt=fmt[sb], DstFmt=fmt[db], RoundMode=_rnd(f["rnd"]),
                  Ftz=f["ftz"], Sh=f.get("h1", False))

    def op_HADD2(self, i, f):
        # Maxwell HADD2 has the same operand swizzles (HalfSwizzle: 0 H1_H0, 1 F32, 2 H0_H0, 3 H1_H1) and an output
        # format (OFmt: 0 F16 pair, 1 F32)
        self.emit("Hadd2R", Dest=self.r(f["dst"]), SrcA=self.r(f["a"]), SrcB=self.r(f["b"]), ASwizzle=f["swz_a"],
                  BSwizzle=f["swz_b"], NegA=f["neg_a"], AbsA=f["abs_a"], NegB=f["neg_b"], AbsB=f["abs_b"],
                  Sat=f["sat"], Ftz=f["ftz"], OFmt=1 if f["f32"] else 0)

    def op_PRMT(self, i, f):
        # byte permute, index mode: output byte k = byte (n & 7) of b:a (0..3 from a, 4..7 from b), n = selector
        # nibble k; with n & 8 the byte is filled with that byte's sign bit. The translator has no Maxwell PRMT, so
        # with an immediate selector the bytes are gathered with BFE/SHL/LOP.
        sel, mode = f["sel"], f["mode"]
        if mode != 0 or not isinstance(sel, Imm):
            raise Unsupported("PRMT with a register selector or a special mode")
        srcs = [self._ireg(f["a"]), self._ireg(f["b"])]
        acc = self.tmp()
        t = self.tmp()
        self.emit("MovR", Dest=acc, SrcA=RZ, QuadMask=0xF)
        for k in range(4):
            n = (sel.v >> (4 * k)) & 0xF
            r = srcs[(n & 7) >> 2]
            byte = n & 3
            if r == RZ:
                continue
            if n & 8:
                self.emit("BfeI", Dest=t, SrcA=r, Imm20=(1 << 8) | (8 * byte + 7), Signed=True)  # 0 or -1
            else:
                self.emit("BfeI", Dest=t, SrcA=r, Imm20=(8 << 8) | (8 * byte), Signed=False)
            self.emit("ShlI", Dest=t, SrcA=t, Imm20=8 * k, M=False)
            if n & 8:  # 0 or -1 shifted: keep only this byte (an unsigned BFE is already 0..255)
                self._and_mask(t, 0xFF << (8 * k))
            self.emit("Lop3R", Dest=acc, SrcA=acc, SrcB=t, SrcC=RZ, Imm=0xFC)
        self.emit("MovR", Dest=self.r(f["dst"]), SrcA=acc, QuadMask=0xF)

    op_UPRMT = op_PRMT

    def _and_mask(self, t, mask):
        m = self.tmp()
        self.emit("Mov32i", Dest=m, Imm32=mask & 0xFFFFFFFF, QuadMask=0xF)
        self.emit("Lop3R", Dest=t, SrcA=t, SrcB=m, SrcC=RZ, Imm=0xC0)

    def op_F2FP(self, i, f):
        # F2FP.PACK_AB d = (f16(a) << 16) | f16(b) (NAK: pack_half_2x16_split(x, y) -> F2FP [y, x])
        if f["merge"] or f["h1"]:
            raise Unsupported("F2FP.MERGE_C")
        lo, hi = self.tmp(), self.tmp()
        for dst, x in ((lo, f["b"]), (hi, f["a"])):
            rx = self.to_reg(x)
            self.emit("F2fR", Dest=dst, SrcB=rx, SrcFmt=2, DstFmt=1, RoundMode=_rnd(f["rnd"]))
        self.emit("ShlI", Dest=hi, SrcA=hi, Imm20=16, M=False)
        self.emit("Lop3R", Dest=self.r(f["dst"]), SrcA=lo, SrcB=hi, SrcC=RZ, Imm=0xFC)  # a | b

    def op_VOTE(self, i, f):
        pd, _ = self.p(f["pdst"])
        p, pinv = self.p(f["p"])
        self.emit("Vote", Dest=self.r(f["dst"]), VpDest=pd, SrcPred=p, SrcPredInv=pinv,
                  VoteMode={"ALL": 0, "ANY": 1, "EQ": 2}[f["mode"]])

    op_UVOTE = op_VOTE

    def op_SHFL(self, i, f):
        # Same instruction on Maxwell (Ryujinx InstEmitWarp: BFixShfl/CFixShfl say the lane / clamp-segment operand is an
        # immediate). Seen in fragment shaders as quad exchanges (mode IDX, c 0x1c03) for hand-made derivatives.
        kv = dict(Dest=self.r(f["dst"]), SrcA=self.r(f["src"]), DestPred=self.p(f["pdst"])[0],
                  ShflMode={"IDX": 0, "UP": 1, "DOWN": 2, "BFLY": 3}[f["mode"]])
        if isinstance(f["lane"], Imm):
            kv.update(BFixShfl=True, SrcBImm=f["lane"].v & 31)
        else:
            kv.update(BFixShfl=False, SrcB=self.r(f["lane"]))
        if isinstance(f["c"], Imm):
            kv.update(CFixShfl=True, SrcCImm=f["c"].v & 0x1FFF)
        else:
            kv.update(CFixShfl=False, SrcC=self.r(f["c"]))
        self.emit("Shfl", **kv)

    def op_FSWZADD(self, i, f):
        self.emit("Fswzadd", Dest=self.r(f["dst"]), SrcA=self.r(f["a"]), SrcB=self.r(f["b"]), PnWord=f["ops"],
                  Ndv=f["ndv"], RoundMode=_rnd(f["rnd"]), Ftz=f["ftz"])

    def op_MEMBAR(self, i, f):
        self.emit("Membar", Membar=min(f["scope"], 2) if f["scope"] != 5 else 1)

    def op_ERRBAR(self, i, f):
        pass

    # memory ---------------------------------------------------------------------------------------------------
    def _addr(self, f):
        """Maxwell address register (pair when 64-bit) for an Ampere address = reg (+ uniform reg)."""
        a = f["addr"]
        u = f.get("uaddr")
        if u is None or u.is_zero():
            return self.r(a), f.get("addr64", False)
        ua = self.r(u)
        if a.is_zero():
            return ua, f.get("uaddr64", False)
        lo = self.tmp()
        hi = lo + 1
        self.tmp()
        self.emit("Iadd3R", Dest=lo, SrcA=self.r(a), SrcB=ua, SrcC=RZ)
        if not f.get("uaddr64", False) and not f.get("addr64", False):
            return lo, False
        carry = self._carry_reg(lo, ua)
        ahi = self.r(a) + 1 if f.get("addr64", False) else RZ
        uhi = ua + 1 if f.get("uaddr64", False) else RZ
        self.emit("Iadd3R", Dest=hi, SrcA=ahi, SrcB=uhi, SrcC=carry, NegC=True)
        return lo, True

    _LS = {"U8": 0, "S8": 1, "U16": 2, "S16": 3, "32": 4, "64": 5, "128": 6}

    def op_LDG(self, i, f):
        if "p" in f and f["p"] is not None and not f["p"].is_true():
            raise Unsupported("LDG with a predicate")
        if self.p(f["pdst"])[0] != PT:
            raise Unsupported("LDG predicate output")
        r, wide = self._addr(f)
        self.emit("Ldg", Dest=self.r(f["dst"]), SrcA=r, Imm24=f["offset"] & 0xFFFFFF, LsSize=self._LS[f["type"]],
                  E=wide)

    def op_STG(self, i, f):
        r, wide = self._addr(f)
        self.emit("Stg", Dest=self.r(f["data"]), SrcA=r, Imm24=f["offset"] & 0xFFFFFF, LsSize=self._LS[f["type"]],
                  E=wide)

    def _local(self, f):
        u = f.get("uaddr")
        if u is not None and not u.is_zero():
            t = self.tmp()
            self.emit("Iadd3R", Dest=t, SrcA=self.r(f["addr"]), SrcB=self.r(u), SrcC=RZ)
            return t
        return self.r(f["addr"])

    def op_LDL(self, i, f):
        self.emit("Ldl", Dest=self.r(f["dst"]), SrcA=self._local(f), Imm24=f["offset"] & 0xFFFFFF,
                  LsSize=self._LS[f["type"]])

    def op_STL(self, i, f):
        self.emit("Stl", Dest=self.r(f["data"]), SrcA=self._local(f), Imm24=f["offset"] & 0xFFFFFF,
                  LsSize=self._LS[f["type"]])

    def op_LDS(self, i, f):
        if f.get("stride", 0):
            raise Unsupported("LDS with stride")
        self.emit("Lds", Dest=self.r(f["dst"]), SrcA=self._local(f), Imm24=f["offset"] & 0xFFFFFF,
                  LsSize=self._LS[f["type"]])

    def op_STS(self, i, f):
        if f.get("stride", 0):
            raise Unsupported("STS with stride")
        self.emit("Sts", Dest=self.r(f["data"]), SrcA=self._local(f), Imm24=f["offset"] & 0xFFFFFF,
                  LsSize=self._LS[f["type"]])

    def op_IABS(self, i, f):
        s = f["src"]
        rs = self.r(s) if isinstance(s, Reg) else self.to_reg(s)
        self.emit("I2iR", Dest=self.r(f["dst"]), SrcB=rs, AbsB=True, ISrcFmt=6, IDstFmt=6)

    def op_P2R(self, i, f):
        m = f["mask"]
        if not isinstance(m, Imm) or not i_imm20_ok(m.v):
            raise Unsupported("P2R mask")
        if any(m.v >> b & 1 for b in self.upred.values()):
            raise Unsupported("P2R over a remapped uniform predicate")
        self.emit("P2rI", Dest=self.r(f["dst"]), SrcA=self.r(f["src"]), Imm20=m.v, ByteSel=f["byte"], Ccpr=False)

    def op_R2P(self, i, f):
        m = f["mask"]
        if not isinstance(m, Imm) or not i_imm20_ok(m.v):
            raise Unsupported("R2P mask")
        self.emit("R2pI", SrcA=self.r(f["src"]), Imm20=m.v, ByteSel=f["byte"], Ccpr=False)

    # control flow -----------------------------------------------------------------------------------------------
    def _cond(self, f):
        """Branch-type instructions run when guard AND their own predicate (87..90) hold. Returns the single
        predicate to use, emitting "@!guard BRA past" first when both are real (no predicate register needed)."""
        bp = self.p(f["p"])
        g = self.guard
        if bp == (PT, False):
            return g, None
        if g == (PT, False):
            return bp, None
        skip = ("syn-skip", len(self.out.items))
        self.out.bra(skip, Pred=g[0], PredInv=not g[1], Ccc=15)
        return bp, skip

    def _end_cond(self, skip):
        if skip is not None:
            self.out.label(skip)

    def op_BRA(self, i, f):
        (pn, inv), skip = self._cond(f)
        self.out.bra(i.addr + 16 + f["target_rel"], Pred=pn, PredInv=inv, Ccc=15)
        self._end_cond(skip)


    def op_CALL(self, i, f):
        # no call stack (.NOINC): the program keeps its own return address, so the call is a plain branch
        if not f["noinc"]:
            raise Unsupported("CALL with a call stack")
        self.op_BRA(i, f)

    def op_RET(self, i, f):
        # RET.NODEC Rn: jump to the address in Rn. The addresses a program returns to are its call sites + 16
        # (the compiler loads them into the register before the CALL); compare and branch to each.
        if not f["nodec"] or i.addr + 16 + f["target_rel"] != 0:
            raise Unsupported("RET with a stack or a relative base")
        (pn, inv), skip = self._cond(f)
        if (pn, inv) != (PT, False):
            raise Unsupported("predicated RET")
        # compare through the condition codes (IADD32I.CC, BRA.EQ on the zero flag): no predicate register needed
        r = self.r(f["src"])
        t = self.tmp()
        for c in self.insns:
            if c.op == "CALL":
                ret = c.addr + 16
                self.emit("Iadd32i", Dest=t, SrcA=r, Imm32=(-ret) & 0xFFFFFFFF, WriteCC=True)
                self.out.bra(ret, Pred=PT, PredInv=False, Ccc=2)  # Ccc.Eq
        self.emit("Exit", Ccc=15)  # no call site matched: undefined on the hardware
        self._end_cond(skip)

    def brx_targets(self, i):
        """Where BRX Rn can go: next + offset + v for every 32-bit word v of c[1] that lands on an instruction of the
        program. The compiler puts the jump table there (LDC Rn, c[1][index * 4], index clamped to the table) and
        sign-extends it into the pair; the other words of c[1] are data, and any that happen to land on an
        instruction only add a comparison that never matches."""
        base = i.addr + 16 + i.f["target_rel"]
        addrs = {x.addr for x in self.insns}
        out = []
        for k in range(len(self.c1) // 4):
            v = struct.unpack_from("<i", self.c1, 4 * k)[0]
            t = base + v
            if t in addrs and t not in out:
                out.append(t)
        return out

    def op_BRX(self, i, f):
        # compared and branched like RET: IADD32I.CC + BRA.EQ per possible target, no predicate register needed
        (pn, inv), skip = self._cond(f)
        if (pn, inv) != (PT, False):
            raise Unsupported("predicated BRX")
        targets = self.brx_targets(i)
        if not targets:
            raise Unsupported("BRX without a jump table in c[1]")
        base = i.addr + 16 + f["target_rel"]
        r = self.r(f["src"])
        t = self.tmp()
        for tgt in targets:
            self.emit("Iadd32i", Dest=t, SrcA=r, Imm32=(base - tgt) & 0xFFFFFFFF, WriteCC=True)
            self.out.bra(tgt, Pred=PT, PredInv=False, Ccc=2)  # Ccc.Eq
        self.emit("Exit", Ccc=15)  # no table entry matched: undefined on the hardware
        self._end_cond(skip)

    def op_BRA_U(self, i, f):
        self.op_BRA(i, f)

    def op_EXIT(self, i, f):
        (pn, inv), skip = self._cond(f)
        self.emit("Exit", Ccc=15, Pred=pn, PredInv=inv)
        self._end_cond(skip)

    def op_KILL(self, i, f):
        (pn, inv), skip = self._cond(f)
        self.emit("Kil", Ccc=15, Pred=pn, PredInv=inv)
        self._end_cond(skip)

    # attributes -------------------------------------------------------------------------------------------------
    def op_IPA(self, i, f):
        if not f["offset"].is_zero() or f["loc"] != 0:
            raise Unsupported("IPA with centroid/offset")
        mode = {"PASS": 0, "CONSTANT": 2, "STATE": 3}[f["freq"]]
        pd, _ = self.p(f["pdst"])
        if pd != PT:
            raise Unsupported("IPA predicate output")
        idx = f.get("idx_ureg")
        if idx is not None and not idx.is_zero():
            return self._ipa_indexed(f, mode)
        self.emit("Ipa", Dest=self.r(f["dst"]), SrcA=RZ, SrcB=RZ, SrcC=RZ, Imm10=f["attr"], IpaOp=mode,
                  SrcPred=PT)

    def _ipa_indexed(self, f, mode):
        """IPA a[attr + UR]: compared and branched over the generic input components the SPH declares (byte
        0x18 + slot, 2 bits per component), each branch a fixed-address IPA. The translator's own indexed IPA
        (IPA.IDX) always scales by FragCoord.w, unlike a fixed address of a non-perspective input, so it is not
        used. An address no declared input has reads 0."""
        if len(self.sph) < 0x38:
            raise Unsupported("indexed IPA without an SPH")
        cands = [0x80 + 16 * k + 4 * c for k in range(32) for c in range(4) if (self.sph[0x18 + k] >> (2 * c)) & 3]
        d, t, r = self.r(f["dst"]), self.tmp(), self.r(f["idx_ureg"])
        n = len(self.out.items)
        end = ("syn-ipa-end", n)
        self.emit("MovR", Dest=d, SrcA=RZ, QuadMask=0xF)
        for a in cands:
            self.emit("Iadd32i", Dest=t, SrcA=r, Imm32=(f["attr"] - a) & 0xFFFFFFFF, WriteCC=True)
            self.out.bra(("syn-ipa", n, a), Pred=PT, PredInv=False, Ccc=2)  # Ccc.Eq
        self.out.bra(end, Pred=PT, PredInv=False, Ccc=15)
        for a in cands:
            self.out.label(("syn-ipa", n, a))
            self.emit("Ipa", Dest=d, SrcA=RZ, SrcB=RZ, SrcC=RZ, Imm10=a, IpaOp=mode, SrcPred=PT)
            self.out.bra(end, Pred=PT, PredInv=False, Ccc=15)
        self.out.label(end)

    def op_ALD(self, i, f):
        if f["patch"] or f["phys"] or not f["offset"].is_zero():
            raise Unsupported("ALD patch/phys/indexed")
        self.emit("Ald", Dest=self.r(f["dst"]), SrcA=RZ, SrcB=self.r(f["vtx"]), Imm11=f["attr"],
                  AlSize=f["count"] - 1, O=f["output"], P=False)

    def op_AST(self, i, f):
        if f["patch"] or f["phys"] or not f["offset"].is_zero():
            raise Unsupported("AST patch/phys/indexed")
        # Maxwell AST (Ryujinx): SrcB = first data register, SrcC = vertex, SrcA = indirect offset
        self.emit("Ast", SrcA=RZ, SrcB=self.r(f["data"]), SrcC=self.r(f["vtx"]), Imm11=f["attr"],
                  AlSize=f["count"] - 1, P=False)

    # textures ---------------------------------------------------------------------------------------------------
    def _tex_dst(self, f, ncomp):
        """Maxwell writes the components to consecutive registers; Ampere to two register pairs."""
        d0, d1 = self.r(f["dst"]), self.r(f["dst1"])
        if d0 == RZ:
            return RZ, []
        if ncomp <= 2 or d1 == d0 + 2:
            return d0, []
        base = self.tmp()
        for _ in range(3):
            self.tmp()
        moves = [(d0, base), (d0 + 1, base + 1)] + ([(d1, base + 2), (d1 + 1, base + 3)] if d1 != RZ else [])
        return base, moves[:ncomp]

    _NCOORD = {"1D": 1, "2D": 2, "3D": 3, "CUBE": 3, "ARRAY_1D": 1, "ARRAY_2D": 2, "ARRAY_CUBE": 3}

    def _tex_sources(self, f, n0, n1):
        """Ampere texture sources -> (Ra, Rb) in Maxwell TEX layout: Ra = src0 = [array index, coordinates...],
        Rb = src1 = [lod/bias, offset, depth reference, ms index] (Mesa NAK nak_nir_lower_tex.c builds the same two
        vectors for SM50 and SM70+). With .scalar (SM70+), the n0 + n1 <= 4 values are rebalanced over the two
        operands instead: 1+1 for two, 2+1 for three, 2+2 for four (NAK try_scalarize_tex)."""
        a, b = self.r(f["a"]), self.r(f["b"])
        if not f.get("scalar"):
            return a, b, n1
        n = n0 + n1
        if n > 4:
            raise Unsupported("scalar texture with more than 4 sources")
        k = 1 if n <= 2 else 2
        regs = [a + x if a != RZ else RZ for x in range(k)] + [b + x if b != RZ else RZ for x in range(n - k)]
        src0, src1 = regs[:n0], regs[n0:]

        def block(rs):
            if not rs:
                return RZ
            if all(r == rs[0] + x for x, r in enumerate(rs)) and rs[0] != RZ:
                return rs[0]
            t = self.tmp()
            for _ in rs[1:]:
                self.tmp()
            for x, r in enumerate(rs):
                self.emit("MovR", Dest=t + x, SrcA=r, QuadMask=0xF)
            return t
        return block(src0), block(src1), n1

    def _tex(self, i, f, name, n0, n1, **kv):
        if "fault" in f and f["fault"].n != PT:
            raise Unsupported("texture fault predicate")
        ncomp = bin(f["mask"]).count("1")
        ra, rb, nb = self._tex_sources(f, n0, n1)
        packed = None
        if f.get("f16"):
            # .F16 results: two per register, low half first; the first pair goes to dst, the second to dst1 (seen:
            # mask 0xF into R4, R6, alpha read as R6.H1). Maxwell TEX returns f32, so sample into temporaries and
            # pack (F2F to f16 lands in the low half; the odd component is shifted up and OR-ed in).
            if ncomp > 2 and not f.get("scalar"):
                raise Unsupported("f16 vector texture result with more than 2 components")
            packed = [self.r(f["dst"]), self.r(f["dst1"])]
            dst, moves = self.tmp(), []
            for _ in range(3):
                self.tmp()
        else:
            dst, moves = self._tex_dst(f, ncomp)
        bank = f.get("bank", 2)
        if bank != 2:
            # Maxwell bound textures read their handle from the texture bank (c2 in NVN); a handle in another bank
            # becomes a bindless access whose handle (first register of Rb) is loaded from that bank. The translator
            # turns it back into a bound texture with that constant buffer slot.
            if bank > 17:
                raise Unsupported(f"texture bank c{bank}")
            h = self.tmp()
            for k in range(nb):
                self.tmp()
            self.emit("Ldc", Dest=h, SrcA=RZ, CbufSlot=bank, CbufOffset=f["handle"] * 4, LsSize=4)
            for k in range(nb):
                self.emit("MovR", Dest=h + 1 + k, SrcA=rb + k if rb != RZ else RZ, QuadMask=0xF)
            kv = {k: v for k, v in kv.items() if k != "TidB"}
            fields_b = mx.tables()[1].get(name + "B", {})
            renamed = {}
            for k, v in kv.items():
                alt = {"Lod": "Lodb", "Lc": "Lcb", "Aoffi": "Aoffib"}.get(k, k)
                if k in fields_b:
                    renamed[k] = v
                elif alt in fields_b:
                    renamed[alt] = v
                elif v:
                    raise Unsupported(f"{name}B has no field for {k}")
            kv = renamed
            name, rb = name + "B", h
        self.emit(name, Dest=dst, SrcA=ra, SrcB=rb, WMask=f["mask"], Nodep=f["nodep"], Dim=_dim(f["dim"]), **kv)
        for to, frm in moves:
            self.emit("MovR", Dest=to, SrcA=frm, QuadMask=0xF)
        if packed is not None:
            for k in range(ncomp):
                self.emit("F2fR", Dest=dst + k, SrcB=dst + k, SrcFmt=2, DstFmt=1, RoundMode=0)
            for p in range((ncomp + 1) // 2):
                lo = dst + 2 * p
                if 2 * p + 1 < ncomp:
                    self.emit("ShlI", Dest=lo + 1, SrcA=lo + 1, Imm20=16, M=False)
                    self.emit("Lop3R", Dest=lo, SrcA=lo, SrcB=lo + 1, SrcC=RZ, Imm=0xFC)
                if packed[p] != RZ:
                    self.emit("MovR", Dest=packed[p], SrcA=lo, QuadMask=0xF)

    def _n0(self, f):
        return self._NCOORD[f["dim"]] + (1 if f["dim"].startswith("ARRAY") else 0)

    def op_TEX(self, i, f):
        if f["lod"] in ("LC", "LB.LC"):
            raise Unsupported("TEX with LOD clamp")  # min_lod is packed with the array index (NAK PRMT 0x1054)
        lod = {"AUTO": 0, "LZ": 1, "LB": 2, "LL": 3}[f["lod"]]
        n1 = (1 if lod in (2, 3) else 0) + (1 if f["aoffi"] else 0) + (1 if f["dc"] else 0)
        self._tex(i, f, "Tex", self._n0(f), n1, TidB=f["handle"], Lod=lod, Lc=False, Dc=f["dc"],
                  Aoffi=f["aoffi"], Ndv=f["ndv"], DestPred=PT)

    def op_TMML(self, i, f):
        self._tex(i, f, "Tmml", self._n0(f), 0, TidB=f["handle"], Ndv=f["ndv"])

    def op_TLD(self, i, f):
        lod = {"LZ": 0, "LL": 1}.get(f["lod"])
        if lod is None:
            raise Unsupported(f"TLD lod {f['lod']}")
        if f["aoffi"]:
            raise Unsupported("TLD with offsets")
        self._tex(i, f, "Tld", self._n0(f), lod + (1 if f["ms"] else 0), TidB=f["handle"], Lod=bool(lod),
                  Ms=f["ms"], DestPred=PT)

    def op_TLD4(self, i, f):
        if f["offset"] == 2:
            raise Unsupported("TLD4 per-pixel offsets")
        self._tex(i, f, "Tld4", self._n0(f), (1 if f["offset"] else 0) + (1 if f["dc"] else 0), TidB=f["handle"],
                  Dc=f["dc"], TexComp=f["comp"], Toff=f["offset"], DestPred=PT)

    def op_TXQ(self, i, f):
        if f.get("bank", 2) != 2:
            raise Unsupported("TXQ outside bank 2")
        q = {0: 1, 1: 2, 2: 5}.get(f["query"])  # Maxwell TexQuery: Dimension 1, TextureType 2, SamplerPos 5
        if q is None:
            raise Unsupported("TXQ query")
        ncomp = bin(f["mask"]).count("1")
        dst, moves = self._tex_dst(f, ncomp)
        self.emit("Txq", Dest=dst, SrcA=self.r(f["a"]), TidB=f["handle"], WMask=f["mask"], TexQuery=q,
                  Nodep=f["nodep"])
        for to, frm in moves:
            self.emit("MovR", Dest=to, SrcA=frm, QuadMask=0xF)


def _psetp_shape(lut):
    """PLOP3 truth table (inputs a = 0xF0, b = 0xCC, c = 0xAA) -> PSETP shape ((ia, na), (ib, nb), op_ab, (ic, nc),
    op_c) computing ((x op_ab y) op_c z) with optional negations, or None. Ops: AND 0, OR 1, XOR 2."""
    import itertools
    vals = (0xF0, 0xCC, 0xAA)
    ops = (lambda x, y: x & y, lambda x, y: x | y, lambda x, y: x ^ y)
    for perm in itertools.permutations(range(3)):
        for na, nb, nc in itertools.product((False, True), repeat=3):
            for oab in range(3):
                for oc in range(3):
                    x = vals[perm[0]] ^ (0xFF if na else 0)
                    y = vals[perm[1]] ^ (0xFF if nb else 0)
                    z = vals[perm[2]] ^ (0xFF if nc else 0)
                    if ops[oc](ops[oab](x, y), z) & 0xFF == lut:
                        return (perm[0], na), (perm[1], nb), oab, (perm[2], nc), oc
    return None


def _rnd(r):
    return {"RN": 0, "RM": 1, "RP": 2, "RZ": 3}[r]


def _rnd2(r):
    return {"RN": 0, "RM": 1, "RP": 2, "RZ": 3}[r]  # RoundMode2: Round, Floor, Ceil, Trunc


def _fcmp(c):
    return list(sm86.FCMP.values()).index(c)


def _icmp(c):
    return list(sm86.ICMP.values()).index(c)


def _bop(b):
    return {"AND": 0, "OR": 1, "XOR": 2}[b]


def _dim(d):
    return {"1D": 0, "ARRAY_1D": 1, "2D": 2, "ARRAY_2D": 3, "3D": 4, "CUBE": 6, "ARRAY_CUBE": 7}[d]


# ---- layout ----------------------------------------------------------------------------------------------------

def layout(out: Out, first_addr: int, prog_start: int):
    """Maxwell addresses (relative to the program start) for every item. Scheduling words sit where the offset in
    the code block (prog_start + address) is a multiple of 32: that is where Nintendo's Maxwell blocks have them
    (graphics programs start at block offset 0x30, so relative 0x50, 0x70, ...)."""
    addrs = []
    a = first_addr
    for _ in out.items:
        if (prog_start + a) & 0x1F == 0:
            a += 8
        addrs.append(a)
        a += 8
    return addrs, a


def assemble(out: Out, first_addr: int, prog_start: int):
    """Code bytes from first_addr (relative) to a 32-byte boundary of the block."""
    # close with EXITs so the translator's decoder never runs past the end (it reads ahead)
    for _ in range(3):
        out.w("Exit", Ccc=15, Pred=PT)
    addrs, end = layout(out, first_addr, prog_start)
    words = {}
    for k, (kind, payload) in enumerate(out.items):
        if kind == "w":
            name, kv = payload
            kv.setdefault("Pred", PT)
            words[addrs[k]] = mx.enc(name, **kv)
        else:
            target, kv = payload
            if target not in out.label_at:
                raise Unsupported(f"branch to {target:#x}, not an instruction")
            ti = out.label_at[target]
            ta = addrs[ti] if ti < len(addrs) else end
            rel = ta - (addrs[k] + 8)
            words[addrs[k]] = mx.enc("Bra", Imm24=rel & 0xFFFFFF, **kv)
    end_abs = (prog_start + end + 0x1F) & ~0x1F
    code = bytearray()
    for a in range(first_addr, end_abs - prog_start, 8):
        if (prog_start + a) & 0x1F == 0:
            code += struct.pack("<Q", mx.sched_word())
        else:
            code += struct.pack("<Q", words.get(a, mx.enc("Nop", Ccc=15)))
    return bytes(code)


def convert_sph(sph: bytes) -> bytes:
    """Ampere SPH (version 4, 32 words) -> Maxwell SPH (version 3, 20 words). Words 0..19 share one layout
    (Mesa NAK sph.rs uses the same field table for both); only the version in word 0 bits 5..9 changes."""
    w = list(struct.unpack_from("<20I", sph, 0))
    w[0] = (w[0] & ~(0x1F << 5)) | (3 << 5)
    return struct.pack("<20I", *w)


def sampler_hints(insns):
    """{texture handle: Ryujinx SamplerType} from the sampling instructions (bound textures, bank 2)."""
    hints = {}
    for i in insns:
        if i.op in ("TEX", "TLD", "TLD4", "TMML") and i.f.get("bank", 2) == 2 and "handle" in i.f:
            d = i.f["dim"]
            t = {"1D": 1, "2D": 3, "3D": 4, "CUBE": 5}[d.replace("ARRAY_", "")] | (0x100 if d.startswith("ARRAY") else 0)
            hints.setdefault(i.f["handle"], t)
    return hints


def block_hints(code: bytes, ctrl: bytes, stage: str):
    try:
        start, end = sm86.code_range(ctrl, stage)
        return sampler_hints([sm86.decode(lo, hi, a) for a, lo, hi in sm86.instructions(code, start, end)])
    except (sm86.Unknown, struct.error):
        return {}


def lower_block(code: bytes, ctrl: bytes, stage: str, extra_hints=None):
    """Returns (maxwell_code_block, nvn1_ctrl, stats)."""
    if struct.unpack_from("<I", ctrl, 0)[0] != 0x98761234 or struct.unpack_from("<I", ctrl, 4)[0] != 0xC9:
        raise Unsupported("not an NVN 2 control section")
    start, end = sm86.code_range(ctrl, stage)
    if end <= start or len(code) < end:
        raise Unsupported("empty code block")
    insns = []
    for addr, lo, hi in sm86.instructions(code, start, end):
        try:
            insns.append(sm86.decode(lo, hi, addr))
        except sm86.Unknown as e:
            raise Unsupported(f"undecoded {e} at {addr:#x}")
    nregs = struct.unpack_from("<I", ctrl, 0x4C)[0] if len(ctrl) >= 0x50 else 0
    c_lo, c_hi = struct.unpack_from("<II", ctrl, 0x40)
    lw = Lowerer(insns, stage, nregs, code[c_lo:c_hi] if c_hi > c_lo else b"", code[:sm86.SPH_SIZE])
    out = lw.run()
    compute = stage == "compute"
    # Maxwell program: graphics = header + SPH + code; compute = code at 0x100
    prog_start = COMPUTE_START if compute else NVN1_HEADER
    first = 0 if compute else MAXWELL_SPH
    body = assemble(out, first, prog_start)
    blk = bytearray(prog_start)
    struct.pack_into("<I", blk, 0, 0x12345679 if compute else 0x12345678)
    if not compute:
        blk += convert_sph(code[:sm86.SPH_SIZE])
    blk += body
    # constant data (c1 inline constants): NVN 2 control +0x40..+0x44 are offsets in the Ampere block
    c_lo, c_hi = struct.unpack_from("<II", ctrl, 0x40)
    const = code[c_lo:c_hi] if c_hi > c_lo else b""
    blk += bytes((-len(blk)) % 0x100)
    const_off = len(blk)
    blk += const
    nctrl = bytearray(0x880)
    struct.pack_into("<I", nctrl, 0, 0x98761234)
    struct.pack_into("<I", nctrl, 4, 1)
    struct.pack_into("<III", nctrl, 0x6FC, len(const), const_off, 0)
    struct.pack_into("<I", nctrl, 0x708, prog_start)
    # Sampler type hints: what the program's own sampling instructions say each bound texture is. The translator
    # asks the GPU for a texture's type only where the instruction does not carry it (TXQ); without a hint the
    # harness answers 2D, and a TXQ of the depth/layer component of an array or 3D texture made invalid SPIR-V.
    # Layout (inside the synthetic control, past every NVN 1 field the harness reads): +0x800 "HINT", +0x804 count,
    # then (handle, Ryujinx SamplerType) pairs. Real NVN 1 controls have other data there: the tag tells them apart.
    hints = dict(extra_hints or {})
    hints.update(sampler_hints(insns))
    hints = sorted(hints.items())[:14]
    struct.pack_into("<4sI", nctrl, 0x800, b"HINT", len(hints))
    for k, (h, t) in enumerate(hints):
        struct.pack_into("<II", nctrl, 0x808 + 8 * k, h, t)
    if compute:
        # NVN 2 compute: work-group size at +0x58/+0x5c/+0x60 (checked against the BNSH reflection, e.g. 32,1,1),
        # shared memory bytes at +0x64; NVN 1 keeps them at +0x748.. and +0x754
        struct.pack_into("<IIII", nctrl, 0x748, *struct.unpack_from("<IIII", ctrl, 0x58))
    stats = {"ampere_insns": len(insns), "maxwell_items": len(out.items), "ur_base": lw.ur_base,
             "tmp_base": lw.tmp_base}
    return bytes(blk), bytes(nctrl), stats


def listing(blk: bytes, prog_start: int, graphics: bool):
    a = MAXWELL_SPH if graphics else 0
    lines, exits = [], 0
    while prog_start + a + 8 <= len(blk) and exits < 3:
        if (prog_start + a) & 0x1F:
            w = struct.unpack_from("<Q", blk, prog_start + a)[0]
            name, fields = mx.dec(w)
            nz = {k: v for k, v in fields.items() if v and not (k == "Pred" and v == 7)}
            lines.append(f"  {a:05x}: {name} {nz}")
            exits = exits + 1 if name == "Exit" and fields.get("Pred") == 7 else 0
        a += 8
    return "\n".join(lines)


# ---- CLI ---------------------------------------------------------------------------------------------------------

def _stage_of(code: bytes, ctrl: bytes):
    w0 = struct.unpack_from("<I", code, 0)[0]
    t = (w0 >> 10) & 0xF
    return {1: "vertex", 5: "pixel"}.get(t, "compute" if (w0 & 0x1F) not in (1, 2) else f"type{t}")


def cmd_batch(src_dir, out_dir, limit=0, only=None):
    """Lower every distinct block of <src_dir>/manifest.tsv. only: lower just these code hashes (the other rows
    still give sampler hints to their variation). Also writes failures.tsv: one line per block that could not be
    lowered (hash, stage, first stage id, reason, full message)."""
    os.makedirs(out_dir, exist_ok=True)
    rows = [l.rstrip("\n").split("\t") for l in open(os.path.join(src_dir, "manifest.tsv"), encoding="utf-8")]
    hdr, rows = rows[0], rows[1:]
    done, why = {}, collections.Counter()
    # the stages of one variation share their texture bindings: a stage that only queries a texture (TXQ) takes
    # its type from a sibling stage that samples it
    group_hints = collections.defaultdict(dict)
    for r in rows:
        g = r[0].rsplit("/", 1)[0]
        code = open(os.path.join(src_dir, r[11]), "rb").read()
        ctrl = open(os.path.join(src_dir, r[12]), "rb").read()
        for h, t in block_hints(code, ctrl, r[1]).items():
            group_hints[g].setdefault(h, t)
    man = open(os.path.join(out_dir, "manifest.tsv"), "w", encoding="utf-8", newline="\n")
    man.write("\t".join(hdr) + "\n")
    fails = open(os.path.join(out_dir, "failures.tsv"), "w", encoding="utf-8", newline="\n")
    fails.write("hash\tstage\tid\treason\tmessage\n")
    ok = bad = 0
    for r in rows:
        code_f, ctrl_f = r[11], r[12]
        if only is not None and r[10] not in only:
            continue
        if code_f not in done:
            if limit and len(done) >= limit:
                continue
            code = open(os.path.join(src_dir, code_f), "rb").read()
            ctrl = open(os.path.join(src_dir, ctrl_f), "rb").read()
            try:
                blk, nctrl, _ = lower_block(code, ctrl, r[1], group_hints[r[0].rsplit("/", 1)[0]])
                h = r[10]
                cf, tf = f"code_{h}.bin", f"ctrl_{h}.bin"
                open(os.path.join(out_dir, cf), "wb").write(blk)
                open(os.path.join(out_dir, tf), "wb").write(nctrl)
                done[code_f] = (cf, tf)
                ok += 1
            except Unsupported as e:
                done[code_f] = None
                reason = str(e).split(" at ")[0]
                why[reason] += 1
                bad += 1
                msg = str(e).replace("\t", " ").replace("\n", " ")
                fails.write(f"{r[10]}\t{r[1]}\t{r[0]}\t{reason}\t{msg}\n")
        if done.get(code_f):
            cf, tf = done[code_f]
            r2 = list(r)
            r2[11], r2[12] = cf, tf
            man.write("\t".join(r2) + "\n")
    man.close()
    fails.close()
    print(f"lowered {ok} of {ok + bad} distinct blocks")
    for k, v in why.most_common(40):
        print(f"  {v:6} {k}")
    return 0


def main(argv):
    if len(argv) >= 3 and argv[0] == "--batch":
        limit = int(argv[argv.index("--limit") + 1]) if "--limit" in argv else 0
        only = None
        if "--only" in argv:  # a file with one code hash per line
            only = {l.split()[0] for l in open(argv[argv.index("--only") + 1], encoding="utf-8") if l.strip()}
        return cmd_batch(argv[1], argv[2], limit, only)
    if len(argv) < 3:
        sys.exit(__doc__)
    code, ctrl = open(argv[0], "rb").read(), open(argv[1], "rb").read()
    stage = argv[argv.index("--stage") + 1] if "--stage" in argv else _stage_of(code, ctrl)
    blk, nctrl, st = lower_block(code, ctrl, stage)
    os.makedirs(argv[2], exist_ok=True)
    name = os.path.basename(argv[0]).replace(".code.bin", "").replace(".bin", "")
    open(os.path.join(argv[2], name + ".code.bin"), "wb").write(blk)
    open(os.path.join(argv[2], name + ".ctrl.bin"), "wb").write(nctrl)
    print(stage, st)
    if "--listing" in argv:
        print(listing(blk, COMPUTE_START if stage == "compute" else NVN1_HEADER, stage != "compute"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
