#!/usr/bin/env python3
"""Reference interpreter for Ampere (SM86) shader code, one thread, written from the instruction semantics (Mesa
NAK's encoder and constant folding, MIT) - independently of the Ampere -> Maxwell lowering it is used to check.

ref_test.py runs a block here and runs the same block, lowered to Maxwell, through the translator front end and the
harness's IR interpreter (spirv_harness --ir-run), on identical inputs, and compares what the two write. A difference
is a lowering error (or a place where the translator's reading of a Maxwell instruction differs from the hardware).

The inputs mirror IrInterp.cs bit for bit: constant buffers and attributes from the same hash, c[1] = the block's own
constants, the same synthetic texture (a smooth function of the handle and of the sources in the translator's
order). That texture model is a test double, shared by both sides on purpose; it says nothing about real textures.
What is checked: arithmetic, conversions, comparisons, predicates, control flow, attribute and output mapping,
constant-buffer addressing, texture source layout as the lowering assumes it (NAK's layout; the twin tests are the
independent check of that).
"""
from __future__ import annotations

import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sm86  # noqa: E402
from sm86 import CBuf, Imm, Pred, Reg  # noqa: E402

M32 = 0xFFFFFFFF
M64 = (1 << 64) - 1
FLT_MIN = 1.1754943508222875e-38
# The translator does not emulate .FTZ (denormals are whatever the host GPU does), so the reference does not flush
# either: a difference on denormal inputs would be a translator approximation, not a lowering error.
FTZ = False


class RefError(Exception):
    pass


# ---- float helpers -------------------------------------------------------------------------------------------

def u2f(u):
    return struct.unpack("<f", struct.pack("<I", u & M32))[0]


def f2u(f):
    try:
        return struct.unpack("<I", struct.pack("<f", f))[0]
    except OverflowError:
        return 0x7F800000 if f > 0 else 0xFF800000


def r32(f):
    return u2f(f2u(f))


def _f(u):
    return u2f(u)


def _f16_bits(x):
    """f32 value -> IEEE half bits, round to nearest even; overflow -> infinity."""
    if math.isnan(x):
        return 0x7E00
    try:
        return struct.unpack("<H", struct.pack("<e", x))[0]
    except OverflowError:
        return 0x7C00 if x > 0 else 0xFC00


def _from_f16(h):
    return struct.unpack("<e", struct.pack("<H", h & 0xFFFF))[0]


def s32(u):
    u &= M32
    return u - (1 << 32) if u & 0x80000000 else u


# ---- environment (mirror of IrEnv in IrInterp.cs) ------------------------------------------------------------

def hash4(a, b, c, d):
    x = ((a * 0x9E3779B97F4A7C15) ^ (b * 0xC2B2AE3D27D4EB4F) ^ (c * 0x165667B19E3779F9) ^ d) & M64
    x ^= x >> 33
    x = (x * 0xFF51AFD7ED558CCD) & M64
    x ^= x >> 33
    x = (x * 0xC4CEB9FE1A85EC53) & M64
    x ^= x >> 33
    return x & M32


class Env:
    def __init__(self, seed, const1=b"", frag=(100.5, 50.5, 0.5, 0.8)):
        self.seed = seed
        self.const1 = const1
        self.frag = [r32(x) for x in frag]

    def cbuf(self, slot, word):
        if slot == 1:
            return struct.unpack_from("<I", self.const1, word * 4)[0] if 0 <= word * 4 <= len(self.const1) - 4 else 0
        h = hash4(self.seed & M32, slot & M32, word & M32, 0x51ED270B)
        if ((h & 3) != 3) if (self.seed & 1) == 0 else ((h & 3) == 0):
            return (h >> 8) % 9
        return f2u(((h >> 8) / 16777216.0) * 4.0 - 2.0)

    def vertex_attr(self, loc, comp):
        if comp == 3:
            return f2u(1.0)
        h = hash4(self.seed & M32, loc & M32, comp & M32, 0x2545F491)
        return f2u(((h >> 8) / 16777216.0) * 2.0 - 1.0)

    def stage_attr(self, loc, comp):
        h = hash4(self.seed & M32, loc & M32, comp & M32, 0x7F4A7C15)
        return f2u(((h >> 8) / 16777216.0) * 2.0 - 1.0)


def texel(handle, src, comp):
    s = 0.37 * handle + 0.11 * comp
    for i, v in enumerate(src):
        s += (1.3 + 0.7 * i + 0.2 * comp) * v
    return f2u(0.5 + 0.45 * math.sin(s))


def src_float(v):
    """IrMachine.Floats: integer-looking bit patterns are used as their integer value."""
    f = u2f(v)
    if math.isnan(f) or abs(f) > 1e6 or (v != 0 and abs(f) < 1e-30):
        return float(s32(v))
    return f


# ---- the machine ----------------------------------------------------------------------------------------------

class Machine:
    def __init__(self, insns, stage, env, sph=b""):
        self.insns = {i.addr: i for i in insns}
        self.order = [i.addr for i in insns]
        self.stage = stage
        self.env = env
        self.R = [0] * 256
        self.UR = [0] * 64
        self.P = [False] * 8
        self.UP = [False] * 8
        self.P[7] = self.UP[7] = True
        self.out = {}
        self.local = {}
        self.discard = False
        self.steps = 0
        self.notes = set()
        w = list(struct.unpack_from("<32I", sph, 0)) if len(sph) >= 128 else [0] * 32
        self.sph = w

    # -- operands
    def g(self, x):
        if isinstance(x, Reg):
            if x.uniform:
                return 0 if x.n == 63 else self.UR[x.n]
            return 0 if x.n == 255 else self.R[x.n]
        if isinstance(x, Imm):
            return x.v & M32
        if isinstance(x, CBuf):
            if x.ureg is not None:
                raise RefError("bindless cbuf")
            if x.offset % 4:
                raise RefError("unaligned cbuf")
            return self.env.cbuf(x.bank, x.offset // 4)
        raise RefError(f"operand {x}")

    def gf(self, x, ftz=False):
        f = u2f(self.g(x))
        if getattr(x, "abs", False):
            f = abs(f)
        if getattr(x, "neg", False):
            f = -f
        if FTZ and ftz and f != 0 and abs(f) < FLT_MIN:
            f = math.copysign(0.0, f)
        return f

    def gi(self, x):
        v = self.g(x)
        if getattr(x, "neg", False):
            v = (-v) & M32
        return v

    def s(self, x, v):
        if x.uniform:
            if x.n != 63:
                self.UR[x.n] = v & M32
        elif x.n != 255:
            self.R[x.n] = v & M32

    def sf(self, x, f, ftz=False, sat=False):
        if sat:
            f = 0.0 if math.isnan(f) else min(max(f, 0.0), 1.0)
        f = r32(f)
        if FTZ and ftz and f != 0 and abs(f) < FLT_MIN:
            f = math.copysign(0.0, f)
        self.s(x, f2u(f))

    def pv(self, p):
        v = (self.UP if p.uniform else self.P)[p.n] if p.n != 7 else True
        return (not v) if p.neg else v

    def ps(self, p, v):
        if p is None or p.n == 7:
            return
        (self.UP if p.uniform else self.P)[p.n] = bool(v)

    # -- run
    def run(self, max_steps=2_000_000):
        pc = self.order[0] if self.order else 0
        while True:
            if pc not in self.insns:
                self.finish()
                return
            i = self.insns[pc]
            self.steps += 1
            if self.steps > max_steps:
                raise RefError("step limit")
            nxt = pc + 16
            if self.pv(i.guard):
                h = getattr(self, "x_" + i.op.replace(".", "_"), None)
                if h is None:
                    raise RefError(f"no reference for {i.op}")
                r = h(i, i.f)
                if r == "exit":
                    self.finish()
                    return
                if r == "kill":
                    self.discard = True
                    return
                if isinstance(r, int):
                    nxt = r
            pc = nxt

    def finish(self):
        if self.stage == "pixel":
            omap = self.sph[18]
            base = 0
            for rt in range(8):
                m = (omap >> (rt * 4)) & 0xF
                for c in range(4):
                    if m >> c & 1:
                        self.out[f"FragmentOutputColor[{rt},{c}]"] = self.R[base + c]
                if m:
                    base += 4

    # -- no-ops
    def x_NOP(self, i, f):
        pass

    x_DEPBAR = x_BSSY = x_BSYNC = x_BREAK = x_WARPSYNC = x_MEMBAR = x_BAR = x_NOP

    # -- control flow
    def x_BRA(self, i, f):
        if self.pv(f["p"]):
            return i.addr + 16 + f["target_rel"]

    def x_CALL(self, i, f):
        if self.pv(f["p"]):
            return i.addr + 16 + f["target_rel"]

    def x_RET(self, i, f):
        if self.pv(f["p"]):
            return (self.g(f["src"]) + i.addr + 16 + f["target_rel"]) & M32

    def x_BRX(self, i, f):
        if self.pv(f["p"]):
            return (self.g(f["src"]) + i.addr + 16 + f["target_rel"]) & M32

    def x_IMAD_WIDE(self, i, f):
        if f["x"]:
            raise RefError("IMAD.WIDE.X")
        a, b = self.g(f["a"]), self.g(f["b"])
        if f["signed"]:
            a, b = s32(a), s32(b)
        c = f["c"]
        cv = 0 if c.is_zero() else (self.R[c.n] | (self.R[c.n + 1] << 32)) if not c.uniform else             (self.UR[c.n] | (self.UR[c.n + 1] << 32))
        v = (a * b + cv) & M64
        d = f["dst"]
        self.s(d, v & M32)
        if not d.is_zero():
            self.s(Reg(d.n + 1, uniform=d.uniform), v >> 32)

    def x_EXIT(self, i, f):
        if self.pv(f["p"]):
            return "exit"

    def x_KILL(self, i, f):
        if self.pv(f["p"]):
            return "kill"

    # -- moves
    def x_MOV(self, i, f):
        self.s(f["dst"], self.g(f["src"]))

    x_UMOV = x_R2UR = x_MOV

    def x_CS2R(self, i, f):
        self.s(f["dst"], 0)
        if f["wide"]:
            self.s(Reg(f["dst"].n + 1), 0)

    def x_S2R(self, i, f):
        self.notes.add(f"S2R {f['sr']:#x} = 0")
        self.s(f["dst"], 0)

    x_S2UR = x_S2R

    def _cb(self, bank, off, typ, dst):
        if typ in ("32", "64", "128"):
            off &= ~3  # the translator reads word offset >> 2 (unaligned wide loads are undefined on the hardware)
        if typ in ("32", "64", "128"):
            for k in range({"32": 1, "64": 2, "128": 4}[typ]):
                self.s(Reg(dst.n + k, uniform=dst.uniform), self.env.cbuf(bank, off // 4 + k))
            return
        w = self.env.cbuf(bank, off // 4)
        sh = (off % 4) * 8
        v = {"U8": (w >> sh) & 0xFF, "S8": s32(((w >> sh) & 0xFF) << 24) >> 24 if True else 0,
             "U16": (w >> (sh & 16)) & 0xFFFF, "S16": s32(((w >> (sh & 16)) & 0xFFFF) << 16) >> 16}[typ]
        self.s(dst, v & M32)

    def x_LDC(self, i, f):
        if "handle_ureg" in f or f.get("mode", 0):
            raise RefError("LDC mode")
        self._cb(f["bank"], (f["offset"] + self.g(f["offset_reg"])) & M32, f["type"], f["dst"])

    def x_ULDC(self, i, f):
        a = self.g(f["offset_reg"]) if "offset_reg" in f else 0
        self._cb(f["bank"], (f["offset"] + a) & M32, f["type"], f["dst"])

    # -- floating point
    _FMUL_SCALE = {4: 1.0, 3: 0.5, 2: 0.25, 1: 0.125, 5: 2.0, 6: 4.0, 7: 8.0}  # same inference as lower.py

    def x_FMUL(self, i, f):
        a, b = self.gf(f["a"], f["ftz"]), self.gf(f["b"], f["ftz"])
        if f["dnz"] and (a == 0 or b == 0):
            r = 0.0
        else:
            r = r32(a * b) * self._FMUL_SCALE[f["pdiv"]]
        self.sf(f["dst"], r, f["ftz"], f["sat"])

    def x_FADD(self, i, f):
        self.sf(f["dst"], self.gf(f["a"], f["ftz"]) + self.gf(f["b"], f["ftz"]), f["ftz"], f["sat"])

    def x_FFMA(self, i, f):
        a, b, c = self.gf(f["a"], f["ftz"]), self.gf(f["b"], f["ftz"]), self.gf(f["c"], f["ftz"])
        if f["dnz"] and (a == 0 or b == 0):
            r = c + 0.0
        else:
            try:
                r = a * b + c
            except OverflowError:
                r = math.inf
        self.sf(f["dst"], r, f["ftz"], f["sat"])

    def x_FMNMX(self, i, f):
        a, b = self.gf(f["a"], f["ftz"]), self.gf(f["b"], f["ftz"])
        if math.isnan(a):
            r = b
        elif math.isnan(b):
            r = a
        else:
            r = min(a, b) if self.pv(f["min"]) else max(a, b)
        self.sf(f["dst"], r, f["ftz"])

    @staticmethod
    def _fcmp(c, a, b):
        un = math.isnan(a) or math.isnan(b)
        base = {"F": False, "T": True, "NUM": not un, "NAN": un}
        if c in base:
            return base[c]
        o = c.rstrip("U")
        r = {"LT": a < b, "EQ": a == b, "LE": a <= b, "GT": a > b, "NE": a != b, "GE": a >= b}[o] if not un else False
        return r or (un and c.endswith("U"))

    @staticmethod
    def _bop(op, x, y):
        return {"AND": x and y, "OR": x or y, "XOR": x != y}[op]

    def x_FSETP(self, i, f):
        r = self._fcmp(f["cmp"], self.gf(f["a"], f["ftz"]), self.gf(f["b"], f["ftz"]))
        p = self.pv(f["p"])
        self.ps(f["dst"], self._bop(f["bop"], r, p))
        self.ps(f["dst1"], self._bop(f["bop"], not r, p))

    def x_FSET(self, i, f):
        r = self._fcmp(f["cmp"], self.gf(f["a"], f["ftz"]), self.gf(f["b"], f["ftz"]))
        self.s(f["dst"], f2u(1.0) if self._bop(f["bop"], r, self.pv(f["p"])) else 0)

    def x_FSEL(self, i, f):
        x = f["a"] if self.pv(f["cond"]) else f["b"]
        self.s(f["dst"], f2u(self.gf(x)))

    def x_SEL(self, i, f):
        self.s(f["dst"], self.g(f["a"] if self.pv(f["cond"]) else f["b"]))

    x_USEL = x_SEL

    def x_MUFU(self, i, f):
        x = self.gf(f["src"])
        fn = f["fn"]
        try:
            if fn == "RCP":
                r = math.copysign(math.inf, x) if x == 0 else 1.0 / x
            elif fn == "RSQ":
                r = math.inf if x == 0 else (math.nan if x < 0 else 1.0 / math.sqrt(x))
            elif fn == "SQRT":
                r = math.nan if x < 0 else math.sqrt(x)
            elif fn == "EX2":
                r = math.nan if math.isnan(x) else (2.0 ** x if x < 200 else math.inf)
            elif fn == "LG2":
                r = -math.inf if x == 0 else (math.nan if x < 0 else math.log2(x))
            elif fn in ("SIN", "COS"):
                t = r32(x * r32(2 * math.pi))       # revolutions (Volta+), same rounding as the lowering's FMUL
                r = math.nan if math.isinf(t) else (math.sin(t) if fn == "SIN" else math.cos(t))
            else:
                raise RefError(f"MUFU.{fn}")
        except (OverflowError, ValueError):
            r = math.nan
        self.sf(f["dst"], r)

    def x_PRMT(self, i, f):
        if f["mode"] != 0:
            raise RefError("PRMT mode")
        a, b, sel = self.g(f["a"]), self.g(f["b"]), self.g(f["sel"])
        x = a | (b << 32)
        r = 0
        for k in range(4):
            n = (sel >> (4 * k)) & 0xF
            byte = (x >> (8 * (n & 7))) & 0xFF
            if n & 8:
                byte = 0xFF if byte & 0x80 else 0
            r |= byte << (8 * k)
        self.s(f["dst"], r)

    x_UPRMT = x_PRMT

    def x_F2FP(self, i, f):
        if f["merge"] or f["h1"]:
            raise RefError("F2FP merge")
        def h(x):
            try:
                return struct.unpack("<H", struct.pack("<e", self.gf(x)))[0]
            except OverflowError:
                return 0x7C00 if self.gf(x) > 0 else 0xFC00
        self.s(f["dst"], (h(f["a"]) << 16) | h(f["b"]))

    def x_FRND(self, i, f):
        x = self.gf(f["src"], f["ftz"])
        if math.isnan(x) or math.isinf(x):
            r = x
        else:
            r = {"RN": round(x), "RM": math.floor(x), "RP": math.ceil(x), "RZ": math.trunc(x)}[f["rnd"]]
            r = math.copysign(float(r), x) if r == 0 else float(r)
        self.sf(f["dst"], r, f["ftz"])

    def x_F2I(self, i, f):
        x = self.gf(f["src"], f["ftz"])
        bits = f["dst_bytes"] * 8
        lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if f["signed"] else (0, (1 << bits) - 1)
        if math.isnan(x):
            v = 0
        elif math.isinf(x):
            v = hi if x > 0 else lo
        else:
            v = {"RN": round(x), "RM": math.floor(x), "RP": math.ceil(x), "RZ": math.trunc(x)}[f["rnd"]]
            v = min(max(int(v), lo), hi)
        self.s(f["dst"], v & M32)

    def x_I2F(self, i, f):
        v = self.g(f["src"])
        sb = f["src_bytes"]
        if sb < 4:
            v = (v >> (8 * f["byte_sel"])) & ((1 << (8 * sb)) - 1)
            if f["signed"] and v >> (8 * sb - 1):
                v -= 1 << (8 * sb)
        elif f["signed"]:
            v = s32(v)
        self.sf(f["dst"], float(v))

    x_I2FP = x_I2F

    # -- integer
    def x_IADD3(self, i, f):
        a, b, c = f["a"], f["b"], f["c"]
        if f["x"]:
            vals = [(~self.g(x)) & M32 if getattr(x, "neg", False) else self.g(x) for x in (a, b, c)]
            total = sum(vals) + int(self.pv(f["cin0"])) + int(self.pv(f["cin1"]))
        else:
            total = sum(((~self.g(x)) & M32) + 1 if getattr(x, "neg", False) else self.g(x) for x in (a, b, c))
        self.s(f["dst"], total & M32)
        self.ps(f["cout0"], total >= (1 << 32))
        self.ps(f["cout1"], total >= (2 << 32))

    x_UIADD3 = x_IADD3

    def x_IMAD(self, i, f):
        a, b = self.g(f["a"]), self.g(f["b"])
        if f["x"]:  # a * b + c + carry in; a "negated" c is a bitwise NOT, as in IADD3.X
            c = self.g(f["c"])
            c = (~c) & M32 if getattr(f["c"], "neg", False) else c
            self.s(f["dst"], (a * b + c + int(self.pv(f["cin"]))) & M32)
            return
        c = self.gi(f["c"])
        self.s(f["dst"], (a * b + c) & M32)

    x_UIMAD = x_IMAD

    def x_LEA(self, i, f):
        if f["hi"] or f["x"] or f["neg_shifted"]:
            raise RefError("LEA.HI/X")
        sh = (self.g(f["a"]) << f["shift"]) & M32
        b = self.gi(f["b"])
        total = sh + (b if not getattr(f["b"], "neg", False) else b)
        if getattr(f["b"], "neg", False):
            total = sh + (((~self.g(f["b"])) & M32) + 1)
        self.s(f["dst"], total & M32)
        self.ps(f["cout"], total >= (1 << 32))

    x_ULEA = x_LEA

    def x_LOP3(self, i, f):
        a, b, c = self.g(f["a"]), self.g(f["b"]), self.g(f["c"])
        lut = f["lut"]
        r = 0
        for k in range(8):
            if lut >> k & 1:
                r |= (a if k & 4 else ~a) & (b if k & 2 else ~b) & (c if k & 1 else ~c)
        r &= M32
        self.s(f["dst"], r)
        self.ps(f["pdst"], r != 0)

    x_ULOP3 = x_LOP3

    def x_SHF(self, i, f):
        lo, sh, hi_src = self.g(f["lo"]), self.g(f["shift"]), self.g(f["hi_src"])
        typ = f["type"]
        bits = 64 if typ in ("S64", "U64") else 32
        sh = sh & (bits - 1) if f["wrap"] else min(sh, bits)
        x = lo | (hi_src << 32)
        if typ in ("S64", "S32"):
            xs = x - (1 << 64) if x >> 63 else x
            shifted = (xs >> sh) & M64 if f["right"] else (x << sh) & M64
        else:
            shifted = (x >> sh) if f["right"] else (x << sh) & M64
        self.s(f["dst"], (shifted >> 32) if f["hi"] else shifted)

    x_USHF = x_SHF

    def x_ISETP(self, i, f):
        a, b = self.g(f["a"]), self.g(f["b"])
        if f["signed"]:
            a, b = s32(a), s32(b)
        c = f["cmp"]
        cmp = {"F": False, "T": True, "LT": a < b, "EQ": a == b, "LE": a <= b, "GT": a > b, "NE": a != b,
               "GE": a >= b}
        if f["ex"]:
            low = self.pv(f["low"])
            eq = a == b
            r = {"EQ": eq and low, "NE": (not eq) or low, "LT": a < b or (eq and low), "LE": a < b or (eq and low),
                 "GT": a > b or (eq and low), "GE": a > b or (eq and low), "F": False, "T": True}[c]
        else:
            r = cmp[c]
        p = self.pv(f["p"])
        self.ps(f["dst"], self._bop(f["bop"], r, p))
        self.ps(f["dst1"], self._bop(f["bop"], not r, p))

    x_UISETP = x_ISETP

    def x_IMNMX(self, i, f):
        a, b = self.g(f["a"]), self.g(f["b"])
        ka, kb = (s32(a), s32(b)) if f["signed"] else (a, b)
        mn = self.pv(f["min"])
        self.s(f["dst"], a if (ka < kb) == mn or ka == kb else b)

    def x_IABS(self, i, f):
        v = s32(self.g(f["src"]))
        self.s(f["dst"], abs(v) & M32)

    def x_PLOP3(self, i, f):
        a, b, c = self.pv(f["a"]), self.pv(f["b"]), self.pv(f["c"])
        k = (a << 2) | (b << 1) | int(c)
        self.ps(f["dst"], f["lut0"] >> k & 1)
        self.ps(f["dst1"], f["lut1"] >> k & 1)

    x_UPLOP3 = x_PLOP3

    def x_P2R(self, i, f):
        m = self.g(f["mask"]) & 0xFF
        sh = f["byte"] * 8
        preds = sum(1 << b for b in range(7) if self.P[b])
        src = self.g(f["src"])
        self.s(f["dst"], (src & ~(m << sh)) | ((preds & m) << sh))

    def x_R2P(self, i, f):
        m = self.g(f["mask"])
        v = self.g(f["src"]) >> (f["byte"] * 8)
        for b in range(7):
            if m >> b & 1:
                self.P[b] = bool(v >> b & 1)

    def x_POPC(self, i, f):
        v = self.g(f["src"])
        self.s(f["dst"], bin((~v if f["not_"] else v) & M32).count("1"))

    def x_FLO(self, i, f):
        v = self.g(f["src"])
        if f["not_"]:
            v = ~v & M32
        if f["signed"] and v >> 31:
            v = ~v & M32
        if v == 0:
            r = M32
        else:
            r = v.bit_length() - 1
            if f["sh"]:
                r = 31 - r
        self.s(f["dst"], r)

    def x_BREV(self, i, f):
        self.s(f["dst"], int(f"{self.g(f['src']):032b}"[::-1], 2))

    x_UBREV = x_BREV

    def x_UFLO(self, i, f):
        return self.x_FLO(i, f)



    # -- attributes
    def x_ALD(self, i, f):
        if f["patch"] or f["phys"] or not f["offset"].is_zero():
            raise RefError("ALD indexed")
        for k in range(f["count"]):
            a = f["attr"] + 4 * k
            if f["output"]:
                v = self.out.get(_out_key(a), 0)
            elif 0x80 <= a < 0x280:
                v = self.env.vertex_attr((a - 0x80) // 16, (a % 16) // 4)
            else:
                self.notes.add(f"ALD a[{a:#x}] = 0")
                v = 0
            self.s(Reg(f["dst"].n + k), v)

    def x_AST(self, i, f):
        if f["patch"] or f["phys"] or not f["offset"].is_zero():
            raise RefError("AST indexed")
        for k in range(f["count"]):
            key = _out_key(f["attr"] + 4 * k)
            if key:
                self.out[key] = self.g(Reg(f["data"].n + k) if not f["data"].is_zero() else f["data"])

    def x_IPA(self, i, f):
        a = f["attr"]
        if f.get("idx_ureg") is not None:
            a = (a + self.g(f["idx_ureg"])) & M32
        fc = self.env.frag
        if a in (0x70, 0x74, 0x78, 0x7C):
            v = f2u(fc[(a - 0x70) // 4])
        elif 0x80 <= a < 0x280:
            loc, comp = (a - 0x80) // 16, (a % 16) // 4
            v = self.env.stage_attr(loc, comp)
            # the translator multiplies by FragCoord.w when the SPH says the vector is perspective (hardware IPA
            # returns attribute/w interpolated; programs then multiply by 1/IPA(a[0x7c]))
            types = [(self._imap(loc * 4 + c)) for c in range(4)]
            first = next((t for t in types if t), 0)
            if first == 2 and f["freq"] == "PASS":
                v = f2u(r32(u2f(v) * fc[3]))
        else:
            self.notes.add(f"IPA a[{a:#x}] = 0")
            v = 0
        self.s(f["dst"], v)

    def _imap(self, index):
        bit = 192 + index * 2
        return (self.sph[bit // 32] >> (bit % 32)) & 3

    # -- local memory
    def x_STL(self, i, f):
        if f["type"] not in ("32", "64", "128"):
            raise RefError("STL size")
        base = (self.g(f["addr"]) + f["offset"]) & M32
        for k in range({"32": 1, "64": 2, "128": 4}[f["type"]]):
            self.local[base + 4 * k] = self.g(Reg(f["data"].n + k)) if not f["data"].is_zero() else 0

    def x_LDL(self, i, f):
        if f["type"] not in ("32", "64", "128"):
            raise RefError("LDL size")
        base = (self.g(f["addr"]) + f["offset"]) & M32
        for k in range({"32": 1, "64": 2, "128": 4}[f["type"]]):
            self.s(Reg(f["dst"].n + k), self.local.get(base + 4 * k, 0))

    # -- textures (sources in the translator's order: what both sides feed the shared texture model)
    NCOORD = {"1D": 1, "2D": 2, "3D": 3, "CUBE": 3, "ARRAY_1D": 1, "ARRAY_2D": 2, "ARRAY_CUBE": 3}

    def _tex_src(self, f, n0, n1):
        a, b = f["a"], f["b"]
        if f.get("scalar"):
            n = n0 + n1
            k = 1 if n <= 2 else 2
            regs = [a.n + x if not a.is_zero() else 255 for x in range(k)] + \
                   [b.n + x if not b.is_zero() else 255 for x in range(n - k)]
        else:
            regs = [a.n + x if not a.is_zero() else 255 for x in range(n0)] + \
                   [b.n + x if not b.is_zero() else 255 for x in range(n1)]
        vals = [0 if r == 255 else self.R[r] for r in regs]
        return vals[:n0], vals[n0:]

    def _handle(self, f):
        if f.get("bank", 2) != 2:
            return 2000 + (self.env.cbuf(f["bank"], f["handle"]) & 0xFFFF)
        return f["handle"]

    def _tex_write(self, f, comps):
        dsts = []
        d0, d1 = f["dst"], f["dst1"]
        if not d0.is_zero():
            dsts += [d0.n, d0.n + 1]
        if not d1.is_zero():
            dsts += [d1.n, d1.n + 1]
        for k, v in enumerate(comps):
            if k < len(dsts) and (k < 2 or not d1.is_zero() or d0.n + k == dsts[k]):
                self.R[dsts[k]] = v & M32

    def _order(self, f, s0, s1, dims, lod_mode, dc, aoffi):
        """Ryujinx EmitTex: coords, (1D: 0.0), array index, dref, lod (LZ: 0.0), offsets, bias."""
        arr = f["dim"].startswith("ARRAY")
        arrv = s0[0] if arr else None
        coords = s0[1:] if arr else s0
        rb = list(s1)
        lod = rb.pop(0) if lod_mode in ("LB", "LL") else None
        offs = rb.pop(0) if aoffi else None
        dref = rb.pop(0) if dc else None
        src = [src_float(v) for v in coords]
        if dims == 1:
            src.append(0.0)
        if arr:
            src.append(src_float(arrv))
        if dc:
            src.append(src_float(dref))
        if lod_mode == "LZ":
            src.append(0.0)
        elif lod_mode == "LL":
            src.append(src_float(lod))
        if aoffi:
            for k in range(dims):
                x = (offs >> (k * 4)) & 0xF
                src.append(float(x - 16 if x & 8 else x))
            if dims == 1:
                src.append(0.0)
        if lod_mode == "LB":
            src.append(src_float(lod))
        return src

    def x_TEX(self, i, f):
        if f["lod"] in ("LC", "LB.LC"):
            raise RefError("TEX LC")
        dims = self.NCOORD[f["dim"]]
        n0 = dims + (1 if f["dim"].startswith("ARRAY") else 0)
        n1 = (1 if f["lod"] in ("LB", "LL") else 0) + (1 if f["aoffi"] else 0) + (1 if f["dc"] else 0)
        s0, s1 = self._tex_src(f, n0, n1)
        src = self._order(f, s0, s1, dims, f["lod"], f["dc"], f["aoffi"])
        h = self._handle(f)
        comps = [texel(h, src, c) for c in range(4) if f["mask"] >> c & 1]
        if f.get("f16"):  # two f16 per register, low half first: pair 0 -> dst, pair 1 -> dst1
            hs = [_f16_bits(_f(c)) for c in comps] + [0] * (len(comps) % 2)
            for p, d in enumerate((f["dst"], f["dst1"])):
                if 2 * p < len(hs) and not d.is_zero():
                    self.R[d.n] = hs[2 * p] | (hs[2 * p + 1] << 16)
            return
        self._tex_write(f, comps)

    def x_HADD2(self, i, f):
        def half(r, swz, neg, ab):
            v = self.g(r)
            if swz == 1:
                pair = (_f(v), _f(v))
            else:
                lo, hi = _from_f16(v & 0xFFFF), _from_f16(v >> 16)
                pair = {0: (lo, hi), 2: (lo, lo), 3: (hi, hi)}[swz]
            pair = tuple(abs(x) if ab else x for x in pair)
            return tuple(-x if neg else x for x in pair)
        a = half(f["a"], f["swz_a"], f["neg_a"], f["abs_a"])
        b = half(f["b"], f["swz_b"], f["neg_b"], f["abs_b"])
        r = [_from_f16(_f16_bits(a[k] + b[k])) for k in range(2)]  # rounded to f16, as the hardware adds
        if f["sat"]:
            r = [min(max(x, 0.0), 1.0) if x == x else 0.0 for x in r]
        if f["f32"]:
            self.sf(f["dst"], r[0])
        else:
            self.s(f["dst"], _f16_bits(r[0]) | (_f16_bits(r[1]) << 16))

    def x_TLD(self, i, f):
        if f["aoffi"] or f["ms"]:
            raise RefError("TLD offsets/ms")
        dims = self.NCOORD[f["dim"]]
        n0 = dims + (1 if f["dim"].startswith("ARRAY") else 0)
        lod = "LL" if f["lod"] == "LL" else "LZ"
        s0, s1 = self._tex_src(f, n0, 1 if lod == "LL" else 0)
        src = self._order(f, s0, s1, dims, lod, False, False)
        h = self._handle(f)
        self._tex_write(f, [texel(h, src, c) for c in range(4) if f["mask"] >> c & 1])

    def x_TLD4(self, i, f):
        if f["offset"]:
            raise RefError("TLD4 offsets")
        dims = self.NCOORD[f["dim"]]
        arr = f["dim"].startswith("ARRAY")
        n0 = dims + (1 if arr else 0)
        s0, s1 = self._tex_src(f, n0, 1 if f["dc"] else 0)
        coords = s0[1:] if arr else s0
        src = [src_float(v) for v in coords]
        if dims == 1:
            src.append(0.0)
        if arr:
            src.append(src_float(s0[0]))
        if f["dc"]:
            src.append(src_float(s1[0]))
        else:
            src.append(float(f["comp"]))
        h = self._handle(f)
        self._tex_write(f, [texel(h, src, c) for c in range(4) if f["mask"] >> c & 1])

    def x_TMML(self, i, f):
        dims = self.NCOORD[f["dim"]]
        arr = f["dim"].startswith("ARRAY")
        s0, _ = self._tex_src(f, dims + (1 if arr else 0), 0)
        coords = s0[1:] if arr else s0
        src = [src_float(v) for v in coords]
        if dims == 1:
            src.append(0.0)
        if arr:
            src.append(src_float(s0[0]))
        h = self._handle(f)
        t = u2f(texel(h + 50, src, 0))
        vals = []
        for c in range(4):
            if not f["mask"] >> c & 1:
                continue
            if c >= 2:
                vals.append(0)
                continue
            lod = min(max(r32(t * 3), 0.0), 2.5) if (c ^ 1) == 0 else r32(t * 3)
            x = r32(lod * 256.0)
            vals.append(int(x) & M32 if not math.isnan(x) else 0)
        self._tex_write(f, vals)


def _out_key(a):
    if 0x70 <= a < 0x80:
        return f"Position[{(a - 0x70) // 4}]"
    if 0x80 <= a < 0x280:
        return f"UserDefined[{(a - 0x80) // 16},{(a % 16) // 4}]"
    if a == 0x6C:
        return "PointSize[]"
    return None


def run_block(code: bytes, ctrl: bytes, stage: str, seed: int):
    start, end = sm86.code_range(ctrl, stage)
    insns = [sm86.decode(lo, hi, addr) for addr, lo, hi in sm86.instructions(code, start, end)]
    c_lo, c_hi = struct.unpack_from("<II", ctrl, 0x40)
    env = Env(seed, code[c_lo:c_hi] if c_hi > c_lo else b"")
    m = Machine(insns, stage, env, code[:0x80])
    m.run()
    return m
