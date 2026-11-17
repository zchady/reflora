#!/usr/bin/env python3
"""Semantic test of the Ampere -> Maxwell lowering on every effect Wonder ships for both GPUs.

The game's package holds the same Vfx effects twice: `*.Nin_NX_NVN.esetb.byml.zs` (package 0, Maxwell, built by
Nintendo) and `*.Nin_Ounce_NVN2.esetb.byml.zs` (package 1, Ampere). Their BNSH variations come in the same order.
For each vertex+pixel variation present in both, this lowers the Ampere pair with lower.py and runs
`spirv_harness --equiv` against Nintendo's Maxwell pair, with the vertex inputs matched by name from the BNSH
reflection (each compiler assigns its own locations).

Usage: python tools/ampere/twin_test.py [--nup <Wonder.nup>] [--only <effect>] [--trials 64] [--limit N]
Everything it writes (extracted shaders, lowered blocks, results.tsv) goes to private/wonder2/twin/. No game data
leaves private/.
"""
from __future__ import annotations

import collections
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools", "maxwell"))
sys.path.insert(0, os.path.join(REPO, "tools"))
import containers  # noqa: E402
import lower  # noqa: E402
import nup as nupmod  # noqa: E402

OUT = os.path.join(REPO, "private", "wonder2", "twin")
HARNESS = os.path.join(REPO, "private", "build", "spirv_harness", "bin", "Release", "net10.0", "spirv_harness.exe")
NX, OUNCE = "Nin_NX_NVN", "Nin_Ounce_NVN2"
_REFL = re.compile(r"^\s+v(\d{5}) (\w+)\s+(.*)$")


def effect_pairs(nup):
    names = {}
    for pkg in (0, 1):
        for path, fn, is_dir in nup.walk(nup.subtree(nup.packages[pkg], "romfs/Effect")):
            if is_dir or not path.endswith(".esetb.byml.zs"):
                continue
            base = os.path.basename(path)
            m = re.match(r"(.+)\.(Nin_NX_NVN|Nin_Ounce_NVN2)\.esetb\.byml\.zs$", base)
            if m:
                names.setdefault(m.group(1), {})[m.group(2)] = (pkg, "romfs/Effect/" + base)
    return {n: v for n, v in sorted(names.items()) if NX in v and OUNCE in v}


def extract(nup, pkg, path, dst_dir):
    os.makedirs(dst_dir, exist_ok=True)
    dst = os.path.join(dst_dir, os.path.basename(path))
    if not os.path.exists(dst):
        data = nup.read(nupmod._single_file(nup, nup.packages[pkg], path))  # SHA-256 verified by the reader
        open(dst + ".part", "wb").write(data)
        os.replace(dst + ".part", dst)
    return dst


def reflection(blob_path):
    """{(container_kind, variation, stage): {"in": {name: loc}, ...}} from the BNSH reflection text."""
    data = containers.read_file(blob_path)
    c = containers.open_blob(data, 0, len(data), os.path.basename(blob_path), reflect=True)
    out = {}
    for node in containers.walk(c):
        kind = node.path.rsplit("/", 1)[-1] if hasattr(node, "path") else ""
        for line in getattr(node, "info", []):
            m = _REFL.match(line)
            if not m:
                continue
            var, stage, rest = int(m.group(1)), m.group(2), m.group(3)
            d = {}
            for key, body in re.findall(r"(\w+)\[([^\]]*)\]", rest):
                d[key] = {n: int(l) for n, l in re.findall(r"([\w.]+)@(-?\d+)", body)}
            out[(kind, var, stage)] = d
    return out


def manifest(d):
    rows = [l.rstrip("\n").split("\t") for l in open(os.path.join(d, "manifest.tsv"), encoding="utf-8")][1:]
    return {r[0]: r for r in rows}


def run_pair(av, ap, bv, bp, in_map, trials):
    args = [HARNESS, "--equiv", *av, *ap, *bv, *bp, "--trials", str(trials)]
    if in_map:
        args += ["--in-map", ",".join(f"{a}={b}" for a, b in sorted(in_map.items()))]
    r = subprocess.run(args, capture_output=True, text=True, timeout=600)
    out = r.stdout
    m = re.search(r"(\d+) trials, (\d+) values compared, (\d+) different, (\d+) trials discarded by both, "
                  r"worst relative error (\S+)", out)
    if "EQUIVALENT" in out and r.returncode == 0:
        return "equivalent", m.groups() if m else (), ""
    if m:
        diffs = ";".join(re.findall(r"differs: (\S+)", out)[:6])
        return "different", m.groups(), diffs
    err = (re.search(r"interpreter: (.*)", out) or re.search(r"(\w*Exception: [^\r\n]*)", out + r.stderr))
    return "error", (), err.group(1)[:200] if err else (out + r.stderr)[-200:].replace("\n", " ")


def main(argv):
    nup_path = argv[argv.index("--nup") + 1] if "--nup" in argv else open(
        os.path.join(REPO, "private", "wonder2", "nup.txt")).read().strip()
    only = argv[argv.index("--only") + 1] if "--only" in argv else None
    trials = int(argv[argv.index("--trials") + 1]) if "--trials" in argv else 64
    limit = int(argv[argv.index("--limit") + 1]) if "--limit" in argv else 0
    nup = nupmod.Nup(nup_path)
    pairs = effect_pairs(nup)
    print(f"{len(pairs)} effects in both builds")
    results = []
    tally = collections.Counter()
    why = collections.Counter()
    for name, v in pairs.items():
        if only and name != only:
            continue
        root = os.path.join(OUT, name)
        blobs = {k: extract(nup, pkg, path, os.path.join(root, "raw")) for k, (pkg, path) in v.items()}
        dirs = {}
        for k, blob in blobs.items():
            d = os.path.join(root, k)
            if not os.path.exists(os.path.join(d, "manifest.tsv")):
                data = containers.read_file(blob)
                c = containers.open_blob(data, 0, len(data), os.path.basename(blob))
                containers.extract(c, d, data)
            dirs[k] = d
        man = {k: manifest(d) for k, d in dirs.items()}
        refl = {k: reflection(b) for k, b in blobs.items()}
        # variations with a vertex and a pixel stage in both builds
        by_var = collections.defaultdict(dict)
        for sid, r in man[OUNCE].items():
            parts = sid.split("/")
            kind, var, stage = parts[-3], parts[-2], parts[-1]
            twin = sid.replace(OUNCE, NX)
            if twin in man[NX]:
                by_var[(kind, var)][stage] = (r, man[NX][twin])
        low_dir = os.path.join(root, "low")
        for (kind, var), stages in sorted(by_var.items()):
            if "vertex" not in stages or "pixel" not in stages:
                continue
            if limit and len(results) >= limit:
                break
            key = f"{name}/{kind}/{var}"
            try:
                files = {}
                for st in ("vertex", "pixel"):
                    ra, rb = stages[st]
                    code = open(os.path.join(dirs[OUNCE], ra[11]), "rb").read()
                    ctrl = open(os.path.join(dirs[OUNCE], ra[12]), "rb").read()
                    blk, nctrl, _ = lower.lower_block(code, ctrl, st)
                    os.makedirs(low_dir, exist_ok=True)
                    pa = os.path.join(low_dir, f"{kind}_{var}_{st}.code.bin")
                    open(pa, "wb").write(blk)
                    open(pa[:-9] + ".ctrl.bin", "wb").write(nctrl)
                    files[st] = ((pa, pa[:-9] + ".ctrl.bin"),
                                 (os.path.join(dirs[NX], rb[11]), os.path.join(dirs[NX], rb[12])))
            except lower.Unsupported as e:
                status, detail = "unsupported", str(e).split(" at ")[0]
                results.append((key, status, "", detail))
                tally[status] += 1
                why[detail] += 1
                continue
            vi = int(var[1:])
            ra_in = refl[OUNCE].get((kind, vi, "vertex"), {}).get("in", {})
            rb_in = refl[NX].get((kind, vi, "vertex"), {}).get("in", {})
            in_map = {ra_in[n]: rb_in[n] for n in ra_in if n in rb_in and ra_in[n] >= 0 and rb_in[n] >= 0}
            status, nums, detail = run_pair(files["vertex"][0], files["pixel"][0], files["vertex"][1],
                                            files["pixel"][1], in_map, trials)
            mp = ",".join(f"{a}={b}" for a, b in sorted(in_map.items()))
            results.append((key, status, " ".join(nums), detail + (f" in-map {mp}" if status != "equivalent" else "")))
            tally[status] += 1
            if status != "equivalent":
                why[f"{status}: {detail}"] += 1
            print(f"  {key:70} {status:12} {' '.join(nums)} {detail}" + (f" in-map {mp}" if status != "equivalent" else ""))
    with open(os.path.join(OUT, "results.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("pair\tstatus\ttrials values different discarded worst\tdetail\n")
        for r in results:
            f.write("\t".join(r) + "\n")
    print(f"\n{len(results)} vertex+pixel pairs: " + ", ".join(f"{k} {v}" for k, v in tally.most_common()))
    for k, v in why.most_common(25):
        print(f"  {v:5} {k}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
