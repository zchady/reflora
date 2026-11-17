#!/usr/bin/env python3
"""Instruction-level differential test of the Ampere -> Maxwell lowering, on instruction words taken from the
game's own shaders (one per distinct form, a few of each).

Each word is wrapped in a synthetic vertex program:
  prologue  every register the word can touch = c[3][4n] (MOV), UR k = c[3][0x300 + 4k] (MOV + R2UR),
            P0..P6 = bit 0 of c[3][0x400 + 4k] (LOP3 + ISETP)
  the word  unchanged (guard included)
  epilogue  every register, uniform register and predicate written to an output attribute (AST)
The program runs through the reference interpreter (ref_interp.py) and, lowered, through the translator front end
and the harness's IR interpreter; the outputs must agree for every seed. A difference names one instruction form.
The programs and results stay under private/ (they contain the game's instruction words).

Usage: python tools/ampere/insn_test.py <ampere dirs...> [--per 3] [--seeds 8] [--ops FMUL,IADD3]
"""
from __future__ import annotations

import collections
import json
import math
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import lower  # noqa: E402
import ref_interp as ri  # noqa: E402
import sm86  # noqa: E402
from sm86 import Pred, Reg  # noqa: E402

HARNESS = os.path.join(REPO, "private", "build", "spirv_harness", "bin", "Release", "net10.0", "spirv_harness.exe")
OUT = os.path.join(REPO, "private", "wonder2", "insn_test")
SKIP = {"BRA", "BRA.U", "BSSY", "BSYNC", "BREAK", "EXIT", "KILL", "WARPSYNC", "NOP", "DEPBAR", "IPA", "ALD", "AST",
        "LDG", "STG", "LDS", "STS", "S2R", "S2UR", "VOTE", "UVOTE", "SHFL", "FSWZADD", "MEMBAR", "BAR", "CS2R",
        "LDL", "STL", "TXQ", "TEX.B", "TLD.B", "TLD4.B", "TMML.B"}
T0, T1, T2 = 120, 121, 122   # prologue/epilogue scratch registers (words using them are skipped)


def w128(fields):
    v = 0
    for lo, hi, val in fields:
        v |= (val & ((1 << (hi - lo)) - 1)) << lo
    return struct.pack("<QQ", v & (2**64 - 1), v >> 64)


def mov_cb(dst, bank, off):
    return w128([(0, 9, 0x002), (9, 12, 5), (12, 15, 7), (16, 24, dst), (24, 32, 255), (38, 54, off), (54, 59, bank),
                 (64, 72, 255), (72, 76, 0xF)])


def mov_ur(dst, ur):
    return w128([(0, 9, 0x002), (9, 12, 6), (12, 15, 7), (16, 24, dst), (24, 32, 255), (32, 38, ur), (64, 72, 255),
                 (72, 76, 0xF), (91, 92, 1)])


def r2ur(ur, src):
    return w128([(0, 12, 0x3C2), (12, 15, 7), (16, 22, ur), (24, 32, src), (81, 84, 7)])


def lop3_and1(dst, src):
    return w128([(0, 9, 0x012), (9, 12, 4), (12, 15, 7), (16, 24, dst), (24, 32, src), (32, 64, 1), (64, 72, 255),
                 (72, 80, 0xC0), (81, 84, 7), (87, 90, 7), (90, 91, 1)])


def isetp_ne(p, src):
    return w128([(0, 9, 0x00C), (9, 12, 1), (12, 15, 7), (24, 32, src), (32, 40, 255), (76, 79, 5), (81, 84, p),
                 (84, 87, 7), (87, 90, 7), (68, 71, 7)])


def sel_p(dst, p):
    # SEL dst = P ? 1 : 0  (form 4: a = RZ... we want P ? 1 : RZ -> a = imm needs form 2: src2 imm)
    return w128([(0, 9, 0x007), (9, 12, 2), (12, 15, 7), (16, 24, dst), (24, 32, 255), (32, 64, 1), (64, 72, 255),
                 (87, 90, p), (90, 91, 1)])  # !P ? RZ... -> cond !P selects a=RZ, else 1


def ast(attr, reg, count=1):
    return w128([(0, 12, 0x322), (12, 15, 7), (24, 32, 255), (32, 40, reg), (40, 50, attr), (64, 72, 255),
                 (74, 76, count - 1)])


def exit_():
    return w128([(0, 12, 0x94D), (12, 15, 7), (87, 90, 7)])


def bra_self():
    return w128([(0, 12, 0x947), (12, 15, 7), (34, 82, (-4) & (2**48 - 1)), (87, 90, 7)])


def sph_vs():
    w = [0] * 32
    w[0] = 1 | (4 << 5) | (1 << 10)
    w[13] = 0xFFFF0000 | 0xF000   # position + generic outputs 0..3
    for k in range(14, 18):
        w[k] = 0xFFFFFFFF          # generic outputs 4..31
    return struct.pack("<32I", *w)


def regs_of(i):
    rs, urs, ps = set(), set(), set()
    for k, v in i.f.items():
        if isinstance(v, Reg) and not v.is_zero():
            (urs if v.uniform else rs).add(v.n)
        elif isinstance(v, Pred) and v.n != 7:
            if v.uniform:
                return None
            ps.add(v.n)
    if i.guard.n != 7:
        if i.guard.uniform:
            return None
        ps.add(i.guard.n)
    return rs, urs, ps


def build(word_lo, word_hi):
    i = sm86.decode(word_lo, word_hi, 0)
    got = regs_of(i)
    if got is None:
        return None, "uniform predicate"
    rs, urs, ps = got
    top = max(rs) + 4 if rs else 4   # vector operands reach a few registers past the one they name
    if top >= 116 or rs & {T0, T1, T2}:
        return None, "registers out of range"
    ins = []
    for r in range(0, top + 1):
        ins.append(mov_cb(r, 3, r * 4))
    for u in sorted(urs | {u + 1 for u in urs}):
        if u < 63:
            ins.append(mov_cb(T0, 3, 0x300 + u * 4))
            ins.append(r2ur(u, T0))
    for p in range(7):
        ins.append(mov_cb(T0, 3, 0x400 + p * 4))
        ins.append(lop3_and1(T1, T0))
        ins.append(isetp_ne(p, T1))
    ins.append(struct.pack("<QQ", word_lo, word_hi))
    nout = 0
    for r in range(0, top + 1, 4):
        ins.append(ast(0x80 + 16 * (r // 4), r, min(4, top + 1 - r)))
        nout = r // 4 + 1
    attr = 0x80 + 16 * nout
    for u in sorted(urs | {u + 1 for u in urs}):
        if u < 63:
            ins.append(mov_ur(T2, u))
            ins.append(ast(attr, T2))
            attr += 4
    for p in range(7):
        ins.append(sel_p(T2, p))
        ins.append(ast(attr, T2))
        attr += 4
    if attr > 0x80 + 16 * 32:
        return None, "too many outputs"
    ins += [exit_(), bra_self()]
    code = sph_vs() + b"".join(ins)
    ctrl = bytearray(0xE0)
    struct.pack_into("<II", ctrl, 0, 0x98761234, 0xC9)
    struct.pack_into("<I", ctrl, 0x38, len(code))
    struct.pack_into("<II", ctrl, 0x40, len(code), len(code))
    struct.pack_into("<I", ctrl, 0x4C, top + 1)
    return (code, bytes(ctrl)), None


def signature(i):
    keep = []
    for k, v in i.f.items():
        if isinstance(v, (bool, str)) or (isinstance(v, int) and k not in ("offset", "handle", "attr", "lut",
                                                                            "lut0", "lut1", "target_rel", "shift")):
            keep.append((k, v))
        elif isinstance(v, (Reg,)):
            keep.append((k, ("U" if v.uniform else "R") + ("-" if v.neg else "") + ("|" if v.abs else "")
                         + ("Z" if v.is_zero() else "")))
        elif isinstance(v, sm86.Imm):
            keep.append((k, "imm"))
        elif isinstance(v, sm86.CBuf):
            keep.append((k, "cb" + ("-" if v.neg else "") + ("|" if v.abs else "")))
    return (i.op, tuple(keep))


def main(argv):
    dirs = [a for a in argv if not a.startswith("--") and os.path.isdir(a)]
    per = int(argv[argv.index("--per") + 1]) if "--per" in argv else 3
    seeds = int(argv[argv.index("--seeds") + 1]) if "--seeds" in argv else 8
    only = set(argv[argv.index("--ops") + 1].split(",")) if "--ops" in argv else None
    sigs = collections.defaultdict(list)
    for stage, cp, tp in sm86._blocks(dirs):
        code, ctrl = open(cp, "rb").read(), open(tp, "rb").read()
        s, e = sm86.code_range(ctrl, stage)
        for a, lo, hi in sm86.instructions(code, s, e):
            try:
                i = sm86.decode(lo, hi, a)
            except sm86.Unknown:
                continue
            if i.op in SKIP or (only and i.op not in only):
                continue
            sg = signature(i)
            if len(sigs[sg]) < per and (lo, hi) not in sigs[sg]:
                sigs[sg].append((lo, hi))
    os.makedirs(OUT, exist_ok=True)
    man = open(os.path.join(OUT, "manifest.tsv"), "w", encoding="utf-8", newline="\n")
    man.write("id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\t"
              "const_size\tcode_sha1_16\tcode_file\tctrl_file\n")
    cases, skipped = {}, collections.Counter()
    for sg, words in sigs.items():
        for k, (lo, hi) in enumerate(words):
            built, why = build(lo, hi)
            if built is None:
                skipped[why] += 1
                continue
            code, ctrl = built
            name = f"{len(cases):05d}"
            try:
                blk, nctrl, _ = lower.lower_block(code, ctrl, "vertex")
            except lower.Unsupported as e:
                skipped[f"lowering: {str(e).split(' at ')[0]}"] += 1
                continue
            open(os.path.join(OUT, f"code_{name}.bin"), "wb").write(blk)
            open(os.path.join(OUT, f"ctrl_{name}.bin"), "wb").write(nctrl)
            man.write(f"{name}\tvertex\t0\t0\t0\t0\t0\t0\t0\t0\t{name}\tcode_{name}.bin\tctrl_{name}.bin\n")
            cases[name] = (sg, lo, hi, code, ctrl)
    man.close()
    jl = os.path.join(OUT, "ir.jsonl")
    p = subprocess.run([HARNESS, "--ir-run", OUT, "--out", jl, "--seeds", str(seeds)], capture_output=True,
                       text=True, timeout=3600)
    print(p.stdout.strip(), p.stderr.strip()[-300:])
    ir = collections.defaultdict(dict)
    for line in open(jl, encoding="utf-8"):
        d = json.loads(line)
        ir[d["code"][5:10]][d.get("seed", 0)] = d
    bad = collections.defaultdict(list)
    ok = 0
    for name, (sg, lo, hi, code, ctrl) in cases.items():
        fails = []
        for s in range(1, seeds + 1):
            d = ir.get(name, {}).get(s)
            if d is None or "error" in d:
                fails.append(f"ir error {(d or {}).get('error')}")
                break
            try:
                m = ri.run_block(code, ctrl, "vertex", s)
            except ri.RefError as e:
                fails.append(f"ref: {e}")
                break
            for k, v in m.out.items():
                iv = d["out"].get(k)
                if iv is None or not _close(v, iv):
                    fails.append(f"seed {s} {k} ref={v:#010x}({ri.u2f(v):.6g}) ir="
                                 f"{iv if iv is None else hex(iv)}({ri.u2f(iv) if iv is not None else None})")
                    break
            if fails:
                break
        if fails:
            bad[sg[0]].append((sg, f"{hi:016x}{lo:016x}", fails[0]))
        else:
            ok += 1
    print(f"{len(cases)} instruction cases from {len(sigs)} forms: {ok} agree, "
          f"{sum(len(v) for v in bad.values())} differ; skipped {dict(skipped)}")
    for op, items in sorted(bad.items(), key=lambda kv: -len(kv[1])):
        print(f"  {op}: {len(items)}")
        for sg, w, why in items[:3]:
            print(f"     {w} {dict(sg[1])}\n        {why}")
    return 0


def _close(a, b, tol=1e-4):
    if a == b:
        return True
    fa, fb = ri.u2f(a), ri.u2f(b)
    if math.isnan(fa) and math.isnan(fb):
        return True
    if any(math.isnan(x) or math.isinf(x) for x in (fa, fb)):
        return False
    return abs(fa - fb) / max(1.0, abs(fa), abs(fb)) <= tol


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
