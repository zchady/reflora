#!/usr/bin/env python3
"""Check the Ampere -> Maxwell lowering block by block against a reference interpreter of the Ampere code.

  python tools/ampere/ref_test.py <ampere dir> <lowered dir> [--seeds 6] [--limit N] [--every K] [--show 5]

<ampere dir> is what containers.py --extract wrote (manifest.tsv + code/ctrl blocks); <lowered dir> what
`lower.py --batch` made of it. The lowered blocks run through the translator front end and the harness's IR
interpreter (`spirv_harness --ir-run`, one process for all blocks); the original Ampere blocks run through
ref_interp.py; inputs are identical (see ref_interp.py). Vertex and pixel stages are checked on their own (pixel
inputs are synthetic). Compared: every output the Ampere program writes (positions, varyings, colours) and whether
the pixel was discarded. Results go to <lowered dir>/ref_test.tsv.
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
import ref_interp as ri  # noqa: E402
import sm86  # noqa: E402

HARNESS = os.path.join(REPO, "private", "build", "spirv_harness", "bin", "Release", "net10.0", "spirv_harness.exe")


def close(a, b, tol=1e-4):
    if a == b:
        return True
    fa, fb = ri.u2f(a), ri.u2f(b)
    if math.isnan(fa) and math.isnan(fb):
        return True
    if math.isnan(fa) or math.isnan(fb) or math.isinf(fa) or math.isinf(fb):
        return False
    return abs(fa - fb) / max(1.0, abs(fa), abs(fb)) <= tol


def main(argv):
    src_dir, low_dir = argv[0], argv[1]
    opt = lambda k, d: type(d)(argv[argv.index(k) + 1]) if k in argv else d  # noqa: E731
    seeds, limit, every, show = opt("--seeds", 6), opt("--limit", 0), opt("--every", 1), opt("--show", 5)
    rows = [l.rstrip("\n").split("\t") for l in open(os.path.join(src_dir, "manifest.tsv"), encoding="utf-8")][1:]
    by_hash = {}
    for r in rows:
        if r[1] in ("vertex", "pixel") and r[10] not in by_hash:
            by_hash[r[10]] = r
    low_rows = [l.rstrip("\n").split("\t") for l in open(os.path.join(low_dir, "manifest.tsv"), encoding="utf-8")][1:]
    present = sorted({r[10] for r in low_rows if r[10] in by_hash})
    present = present[::every]
    if limit:
        present = present[:limit]
    # one manifest with just the sample, for the harness
    sample_dir = low_dir
    sub = os.path.join(low_dir, "_ref_sample.tsv")
    hdr = open(os.path.join(low_dir, "manifest.tsv"), encoding="utf-8").readline()
    want = set(present)
    tmp_dir = os.path.join(low_dir, "_ref")
    os.makedirs(tmp_dir, exist_ok=True)
    with open(os.path.join(tmp_dir, "manifest.tsv"), "w", encoding="utf-8", newline="\n") as fm:
        fm.write(hdr)
        done = set()
        for r in low_rows:
            if r[10] in want and r[10] not in done:
                done.add(r[10])
                r2 = list(r)
                r2[11] = os.path.join("..", r[11])
                r2[12] = os.path.join("..", r[12])
                fm.write("\t".join(r2) + "\n")
    jl = os.path.join(tmp_dir, "ir.jsonl")
    p = subprocess.run([HARNESS, "--ir-run", tmp_dir, "--out", jl, "--seeds", str(seeds)], capture_output=True,
                       text=True, timeout=3600)
    print(p.stdout.strip())
    ir = collections.defaultdict(dict)
    for line in open(jl, encoding="utf-8"):
        d = json.loads(line)
        h = d["code"].split("code_")[-1].split(".bin")[0]
        ir[h][d.get("seed", 0)] = d
    tally, why, shown = collections.Counter(), collections.Counter(), 0
    out_rows = []
    for h in present:
        r = by_hash[h]
        code = open(os.path.join(src_dir, r[11]), "rb").read()
        ctrl = open(os.path.join(src_dir, r[12]), "rb").read()
        stage = r[1]
        status, detail = "equal", ""
        ndiff = nval = compared = 0
        for s in range(1, seeds + 1):
            d = ir.get(h, {}).get(s) or ir.get(h, {}).get(0)
            if d is None:
                status, detail = "ir-error", "missing"
                break
            try:
                m = ri.run_block(code, ctrl, stage, s)
            except ri.RefError as e:
                if str(e) == "step limit" and d.get("error") == "step limit":
                    continue          # both loop on this seed's data: inconclusive, try the next seed
                status, detail = "ref-skip", str(e)
                break
            except sm86.Unknown as e:
                status, detail = "ref-skip", f"undecoded {e}"
                break
            if "error" in d:
                status, detail = "ir-error", d["error"]
                break
            compared += 1
            if m.discard != d["discard"]:
                status, detail = "different", f"seed {s}: discard ref={m.discard} ir={d['discard']}"
                ndiff += 1
                break
            if m.discard:
                continue
            for k, v in m.out.items():
                nval += 1
                iv = d["out"].get(k)
                if iv is None or not close(v, iv):
                    ndiff += 1
                    if status == "equal":
                        status = "different"
                        detail = (f"seed {s}: {k} ref={ri.u2f(v):.6g} ir="
                                  f"{ri.u2f(iv) if iv is not None else None}")
        if status == "equal" and compared == 0:
            status, detail = "inconclusive", "every seed loops in both"
        tally[status] += 1
        if status not in ("equal",):
            why[(status, detail.split(": ", 1)[-1].split(" ref=")[0] if status == "different" else detail)] += 1
            if status == "different" and shown < show:
                shown += 1
                print(f"  {h} {stage}: {detail} ({ndiff}/{nval} values)")
        out_rows.append((h, stage, status, str(ndiff), str(nval), detail))
    with open(os.path.join(low_dir, "ref_test.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("hash\tstage\tstatus\tdifferent\tvalues\tdetail\n")
        for row in out_rows:
            f.write("\t".join(row) + "\n")
    print(f"{len(present)} blocks: " + ", ".join(f"{k} {v}" for k, v in tally.most_common()))
    for (st, d), n in why.most_common(20):
        print(f"  {n:5} {st}: {d}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
