#!/usr/bin/env python3
"""Where the game's shaders are: scan a NUP's RomFS (and optionally flat module images) for shader code, read-only.

Usage:
  python tools/maxwell/shader_sources.py --nup <file.nup> --out <dir> [--package N] [--match <dir>...]
                                         [--extra <file>...] [--only <substring>] [--skip-ext bwav,webm]

Every RomFS file is read from the package (zstd `.zs` files decompressed in memory) and searched three ways:
  1. structured: tools/maxwell/containers.py open_blob() at offset 0 (SARC nesting, FSHA, BNSH, VFXB, BAHS, BYML);
  2. embedded: every "BNSH\\0\\0\\0\\0" / "VFXB    " / "FSHA    " / "BAHS" magic anywhere in the file, parsed there
     (shader files inside formats containers.py does not open, e.g. a model or a pack it does not know);
  3. raw (only with --match): every NVN code-block header (0x12345678 graphics, 0x12345679 compute) is compared with
     the code blocks in the --match directories (code_<sha1_16>.bin, as NX_DUMP_SHADER_MISSES writes them): a block
     counts as found only when the bytes at that offset hash to the same SHA-1 over the block's full size.
Nothing is extracted. Written to --out (metadata only: paths, kinds, counts, content hashes; keep it under private/):
  sources.tsv   one line per file with shaders: path, how found, container kinds, stages, distinct blocks
  blocks.tsv    one line per distinct code block: hash, stage kind, size, number of files, platform tags of those
                files (Ounce = Nin_Ounce_NVN2, the Switch 2 build; NX = Nin_NX_NVN, the Switch 1 build; module = an
                --extra file; untagged), first file
  match.tsv     with --match: each target block, where it was found (or "not found") and by which method
  summary.txt   totals
"""
import argparse
import collections
import hashlib
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import containers as ct  # noqa: E402
import nup as nupmod  # noqa: E402

MAGICS = re.compile(rb"BNSH\0\0\0\0|VFXB    |FSHA    |BAHS")
CODE_HDR = re.compile(rb"\x78\x56\x34\x12|\x79\x56\x34\x12")
KEY = 0x40  # bytes of a code block used as the lookup key before the full hash check


def decompress(raw):
    import zstandard
    return zstandard.ZstdDecompressor().decompress(raw, max_output_size=1 << 32)


def stages_of(c):
    out = []
    for x in ct.walk(c):
        for _, ss in x.programs:
            out += ss
    return out


def kinds_of(c):
    return sorted({x.kind for x in ct.walk(c)})


def scan_data(data, name, targets, by_key):
    """(found stages, container kinds, methods, raw hits) for one file's bytes."""
    stages, kinds, methods, hits = [], set(), set(), []
    covered = []  # (start, end) of containers already parsed
    try:
        c = ct.open_blob(data, 0, len(data), name)
    except Exception as e:  # noqa: BLE001 - a format we only half understand: fall back to the magic scan
        c = None
        methods.add(f"open_blob error: {type(e).__name__}")
    def cover(x):
        # the shader files parsed (not a whole SARC: a member it could not open is still searched by magic)
        covered.extend((y.off, y.off + max(y.size, 8)) for y in ct.walk(x) if y.kind in ("BNSH", "VFXB", "FSHA", "BAHS"))

    if c:
        stages += stages_of(c)
        kinds.update(kinds_of(c))
        methods.add("structured")
        cover(c)
    for m in MAGICS.finditer(data):
        pos = m.start()
        if any(a <= pos < b for a, b in covered):
            continue
        try:
            sub = ct.open_blob(data, pos, len(data) - pos, f"{name}@{pos:#x}")
        except Exception:  # noqa: BLE001 - a false magic match
            sub = None
        if sub:
            stages += stages_of(sub)
            kinds.update(kinds_of(sub))
            methods.add("embedded")
            cover(sub)
    if targets:
        for m in CODE_HDR.finditer(data):
            pos = m.start()
            cand = by_key.get(bytes(data[pos:pos + KEY]))
            if not cand:
                continue
            for h in cand:
                size = targets[h]
                if hashlib.sha1(bytes(data[pos:pos + size])).hexdigest()[:16] == h:
                    hits.append((h, pos))
    return stages, kinds, methods, hits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nup", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--package", type=int, default=0)
    ap.add_argument("--match", nargs="*", default=[])
    ap.add_argument("--extra", nargs="*", default=[], help="other files to scan (e.g. flat module images)")
    ap.add_argument("--only", default="", help="scan only RomFS paths containing this text")
    ap.add_argument("--skip-ext", default="bwav,webm", help="extensions never scanned (audio, video)")
    a = ap.parse_args()

    targets, captured_in = {}, collections.defaultdict(list)
    by_key = collections.defaultdict(set)
    for d in a.match:
        for f in sorted(os.listdir(d)):
            mm = re.fullmatch(r"code_([0-9a-f]{16})\.bin", f)
            if mm:
                b = open(os.path.join(d, f), "rb").read()
                targets[mm.group(1)] = len(b)
                captured_in[mm.group(1)].append(os.path.basename(os.path.normpath(d)))
                by_key[b[:KEY]].add(mm.group(1))

    os.makedirs(a.out, exist_ok=True)
    skip = {e.strip().lower() for e in a.skip_ext.split(",") if e.strip()}
    n = nupmod.Nup(a.nup)
    pkg = n.packages[a.package]
    files = n.files_of(n.subtree(pkg, "romfs"))
    blocks = {}                     # hash -> [kind, size, n files, first file]
    hits = collections.defaultdict(list)
    t0 = time.time()
    scanned = skipped = 0
    raw_bytes = 0
    with open(os.path.join(a.out, "sources.tsv"), "w", encoding="utf-8", newline="\n") as src:
        src.write("path\tmethods\tcontainers\tstages\tdistinct_blocks\tbytes\n")

        def tag(path):
            return ("Ounce" if "Nin_Ounce_NVN2" in path else "NX" if "Nin_NX_NVN" in path
                    else "module" if path.startswith("extra/") else "untagged")

        def record(path, data):
            stages, kinds, methods, h = scan_data(data, os.path.basename(path), targets, by_key)
            for t, pos in h:
                hits[t].append((path, pos, "raw"))
            seen = set()
            for s in stages:
                b = blocks.get(s.hash)
                if not b:
                    blocks[s.hash] = [s.kind, s.code_size, 0, path, set()]
                    b = blocks[s.hash]
                if s.hash not in seen:
                    seen.add(s.hash)
                    b[2] += 1
                    b[4].add(tag(path))
                    if s.hash in targets:
                        hits[s.hash].append((path, s.code_off, "parsed"))
            if stages or any(m.startswith("open_blob error") for m in methods):
                src.write(f"{path}\t{','.join(sorted(methods))}\t{','.join(kinds)}\t{len(stages)}\t{len(seen)}\t"
                          f"{len(data)}\n")

        for path, fn in sorted(files.items()):
            ext = path.rsplit(".", 2)
            e = (ext[-2] if path.endswith(".zs") and len(ext) > 2 else ext[-1]).lower()
            if e in skip or (a.only and a.only not in path):
                skipped += 1
                continue
            data = n.read(fn)
            if path.endswith(".zs"):
                data = decompress(data)
            raw_bytes += len(data)
            record("romfs/" + path, data)
            scanned += 1
            if scanned % 2000 == 0:
                print(f"{scanned} files, {raw_bytes / 1e9:.1f} GB, {time.time() - t0:.0f} s", flush=True)
        for f in a.extra:
            data = open(f, "rb").read()
            raw_bytes += len(data)
            record("extra/" + os.path.basename(f), data)
            scanned += 1

    with open(os.path.join(a.out, "blocks.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("hash\tstage\tcode_size\tfiles\ttags\tfirst_file\n")
        for h, (k, size, nf, first, tags) in sorted(blocks.items()):
            f.write(f"{h}\t{k}\t{size:#x}\t{nf}\t{','.join(sorted(tags))}\t{first}\n")
    found = 0
    if targets:
        with open(os.path.join(a.out, "match.tsv"), "w", encoding="utf-8", newline="\n") as f:
            f.write("hash\tcaptured_in\tcode_size\tfound\tmethod\tfiles\tfirst_file\tfirst_offset\n")
            for h in sorted(targets):
                hs = hits.get(h, [])
                files_h = sorted({p for p, _, _ in hs})
                methods = sorted({m for _, _, m in hs})
                found += bool(hs)
                first = min(hs) if hs else ("", 0, "")
                f.write(f"{h}\t{','.join(captured_in[h])}\t{targets[h]:#x}\t{'yes' if hs else 'no'}\t"
                        f"{','.join(methods)}\t{len(files_h)}\t{first[0]}\t{first[1]:#x}\n")
    kinds = collections.Counter(k for k, *_ in blocks.values())
    lines = [f"package {a.package}: {len(files)} RomFS files, {scanned} scanned ({raw_bytes / 1e9:.2f} GB "
             f"decompressed), {skipped} skipped ({','.join(sorted(skip))}{', --only' if a.only else ''})",
             f"distinct code blocks: {len(blocks)} (" + ", ".join(f"{k} {v}" for k, v in kinds.most_common()) + ")",
             f"time: {time.time() - t0:.0f} s"]
    if targets:
        lines.append(f"match: {found} of {len(targets)} target blocks found in the scanned files")
    open(os.path.join(a.out, "summary.txt"), "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
