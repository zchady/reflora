"""Read, check, compare and merge REFLORA pipeline catalogs (runtime/src/nvn/nvn_pipeline_catalog.cpp).

  python tools/pipeline_catalog.py stats <catalog>...          records (graphics, compute), stages, damaged entries
  python tools/pipeline_catalog.py dump <catalog> [n]          what a record holds, field by field (composition review)
  python tools/pipeline_catalog.py diff <old> <new>            the records of <new> not in <old>, and how they differ
  python tools/pipeline_catalog.py merge <out> <catalog>...    one catalog with every distinct record, in input order

A record is a kind (graphics or compute), shader hashes (16 hex characters: the .spv file names) and plain numbers:
a graphics pipeline's PipelineKey (NVN enums and VkFormat values), a compute pipeline's shared memory size. No game
code or data, no driver output. Format version 3: a 32-byte header ("NXPC", version, key size 704, record size 744,
canonical-rules version), then entries of a record and its FNV-1a check. Version 2 files (graphics records without
the kind, 736 bytes) are read as graphics; merge always writes version 3.
"""
import collections
import struct
import sys

HEADER = struct.Struct("<8I")
MAGIC, VERSION, KEY = 0x4350584E, 3, 704
REC = 8 + 32 + KEY  # kind, shared memory size, two hashes, the key
REC_V2 = 32 + KEY
ENTRY = REC + 8
GRAPHICS, COMPUTE = 0, 1
# PipelineKey (runtime/src/nvn/nvn_pipeline_state.h): name, offset, size (after the kind, the size and the hashes)
FIELDS = [("vs/fs pointers (0)", 0, 16), ("topology", 16, 4), ("attribs", 20, 192), ("streams", 212, 128),
          ("attrib/stream count", 340, 8), ("blend", 348, 224), ("color", 572, 16), ("mask", 588, 8),
          ("depth", 596, 40), ("polygon", 636, 16), ("color_formats", 652, 32), ("color_count", 684, 4),
          ("depth_format", 688, 4), ("depth_clamp", 692, 4), ("ufloat", 696, 4), ("alpha_to_coverage", 700, 4)]
KEY_AT = 40


def fnv1a(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def read(path):
    """(records as version-3 bytes, damaged entries, canonical-rules version, file format version)"""
    data = open(path, "rb").read()
    if len(data) < HEADER.size:
        raise SystemExit(f"{path}: not a pipeline catalog")
    magic, version, key, rec, canon = HEADER.unpack_from(data)[:5]
    if (magic, key) != (MAGIC, KEY) or (version, rec) not in ((3, REC), (2, REC_V2)):
        raise SystemExit(f"{path}: not a version-2 or version-3 catalog")
    records, damaged = [], 0
    for at in range(HEADER.size, len(data) - (rec + 8) + 1, rec + 8):
        r = data[at:at + rec]
        if fnv1a(r) != struct.unpack_from("<Q", data, at + rec)[0]:
            damaged += 1
            continue
        if version == 2:
            r = struct.pack("<II", GRAPHICS, 0) + r
        elif kind(r) not in (GRAPHICS, COMPUTE):
            damaged += 1
            continue
        records.append(r)
    return records, damaged, canon, version


def kind(r):
    return struct.unpack_from("<I", r)[0]


def shared(r):
    return struct.unpack_from("<I", r, 4)[0]


def hashes(r):
    return r[8:24].rstrip(b"\0").decode(), r[24:40].rstrip(b"\0").decode()


def field(r, name):
    for n, off, size in FIELDS:
        if n == name:
            return r[KEY_AT + off:KEY_AT + off + size]


def write(path, records, canon=2):
    import os
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(HEADER.pack(MAGIC, VERSION, KEY, REC, canon, 0, 0, 0))
        for r in records:
            f.write(r + struct.pack("<Q", fnv1a(r)))
    os.replace(tmp, path)


def main():
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "stats":
        for p in args:
            recs, damaged, canon, version = read(p)
            graphics = [r for r in recs if kind(r) == GRAPHICS]
            pairs = {r[8:40] for r in graphics}
            stages = {h for r in recs for h in hashes(r) if h}
            print(f"{p}: {len(recs)} records ({len(graphics)} graphics in {len(pairs)} shader pairs, "
                  f"{len(recs) - len(graphics)} compute), {len(stages)} shaders, {damaged} damaged, format "
                  f"v{version}, canonical rules v{canon}")
    elif cmd == "dump":
        recs = read(args[0])[0]
        for r in recs[:int(args[1]) if len(args) > 1 else 3]:
            vs, fs = hashes(r)
            if kind(r) == COMPUTE:
                print(f"compute {vs} shared memory {shared(r)} bytes")
                continue
            print(f"graphics vs {vs} fs {fs}")
            for n, off, size in FIELDS[1:]:
                words = struct.unpack_from(f"<{size // 4}I", r, KEY_AT + off)
                print(f"  {n:20s} {' '.join(f'{w:x}' for w in words)}")
    elif cmd == "diff":
        old = read(args[0])[0]
        new = read(args[1])[0]
        old_set, by_pair = set(old), collections.defaultdict(list)
        for r in old:
            by_pair[r[:40]].append(r)
        fresh = [r for r in new if r not in old_set]
        kinds = collections.Counter()
        for r in fresh:
            if kind(r) == COMPUTE:
                kinds[("a compute pipeline",)] += 1
                continue
            cands = by_pair.get(r[:40])
            if not cands:
                kinds[("a shader pair the old catalog never had",)] += 1
                continue
            best = min(([n for n, off, size in FIELDS if field(r, n) != field(c, n)] for c in cands), key=len)
            kinds[tuple(best)] += 1
        print(f"{len(fresh)} of {len(new)} records not in {args[0]}")
        for k, n in kinds.most_common():
            print(f"  {n:5d}  {', '.join(k)}")
    elif cmd == "merge":
        out, seen, recs = args[0], set(), []
        for p in args[1:]:
            for r in read(p)[0]:
                if r not in seen:
                    seen.add(r)
                    recs.append(r)
        write(out, recs)
        print(f"{out}: {len(recs)} records")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
