"""TEST FIXTURE: writes small synthetic NUP files (no game data) for tests/test_nup.py.

Synthetic content only: fake NSO headers, a fake NPDM and NACP built here. Nothing in it comes from a real dump.
"""
from __future__ import annotations

import hashlib
import struct


def fake_nso(module_id: bytes, body: bytes = b"") -> bytes:
    h = bytearray(0x100)
    h[:4] = b"NSO0"
    h[0x40:0x40 + len(module_id)] = module_id
    return bytes(h) + body


def fake_npdm(program_id: int) -> bytes:
    raw = bytearray(0x200)
    raw[:4] = b"META"
    raw[0x20:0x20 + 11] = b"Application"
    struct.pack_into("<I", raw, 0x70, 0x100)  # ACI0 offset
    raw[0x100:0x104] = b"ACI0"
    struct.pack_into("<Q", raw, 0x110, program_id)
    return bytes(raw)


def fake_nacp(name: str, version: str, addon_base: int, compressed: bool = False) -> bytes:
    raw = bytearray(0x4000)
    if compressed:  # u16 size + raw DEFLATE of 32 language entries
        import zlib
        titles = bytearray(0x6000)
        for lang in range(32):
            titles[lang * 0x300:lang * 0x300 + len(name.encode())] = name.encode()
            titles[lang * 0x300 + 0x200:lang * 0x300 + 0x204] = b"Test"
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        blob = c.compress(bytes(titles)) + c.flush()
        struct.pack_into("<H", raw, 0, len(blob))
        raw[2:2 + len(blob)] = blob
    else:
        raw[:len(name.encode())] = name.encode()
        raw[0x200:0x204] = b"Test"
    raw[0x3060:0x3060 + len(version)] = version.encode()
    struct.pack_into("<Q", raw, 0x3070, addon_base)
    return bytes(raw)


def build(packages, compress=False, corrupt_toc=False, tlv_numbers=None):
    """packages: list of dict(title_id, kind, version, tree={path: bytes}, meta=[(type, payload)]).

    Returns the bytes of a NUP v1 file following SPEC.md (directories sorted bytewise, 16-byte aligned extents).
    """
    dirs = []      # [name, children(list of node keys)]
    files = []     # [name, data]
    pkg_recs = []

    def mkdir(name):
        dirs.append([name, []])
        return len(dirs) - 1

    for p in packages:
        root = mkdir("")
        index = {"": root}
        for path in sorted(p["tree"]):
            parts = path.split("/")
            cur = ""
            for part in parts[:-1]:
                nxt = f"{cur}/{part}" if cur else part
                if nxt not in index:
                    index[nxt] = mkdir(part)
                    dirs[index[cur]][1].append(("d", index[nxt]))
                cur = nxt
            files.append([parts[-1], p["tree"][path]])
            dirs[index[cur]][1].append(("f", len(files) - 1))
        pkg_recs.append((p, root, index.get("exefs", 0xFFFFFFFF), index.get("romfs", 0xFFFFFFFF)))

    dc, fc = len(dirs), len(files)
    pool = bytearray()
    interned = {}

    def s(name):
        b = name.encode()
        if b not in interned:
            interned[b] = len(pool)
            pool.extend(b)
        return interned[b], len(b)

    def node_name(key):
        kind, i = key
        return (dirs[i][0] if kind == "d" else files[i][0]).encode()

    children = []
    dir_first = []
    for d in dirs:
        d[1].sort(key=node_name)
        dir_first.append(len(children))
        children.extend(i if k == "d" else dc + i for k, i in d[1])

    toc_head = 0x30
    size_wo_pool = toc_head + len(pkg_recs) * 0x40 + dc * 0x18 + fc * 0x50 + len(children) * 4
    # names first, so the pool size is known
    dnames = [s(d[0]) for d in dirs]
    fnames = [s(f[0]) for f in files]
    toc_size = size_wo_pool + len(pool)
    blob_off = (0x80 + toc_size + 15) & ~15

    blob = bytearray()
    extents = []
    for name, data in files:
        stored, codec = data, 0
        if compress and data:
            import zstandard
            c = zstandard.ZstdCompressor(level=3, write_content_size=True).compress(data)
            if len(c) < len(data):
                stored, codec = c, 1
        if not data:
            extents.append((0, 0, codec))
            continue
        while (blob_off + len(blob)) % 16:
            blob.append(0)
        extents.append((blob_off + len(blob), len(stored), codec))
        blob.extend(stored)
    meta_blobs = []
    for p, *_ in pkg_recs:
        m = b"".join(struct.pack("<HHI", t, 0, len(pl)) + pl for t, pl in p.get("meta", []))
        while (blob_off + len(blob)) % 16:
            blob.append(0)
        meta_blobs.append((blob_off + len(blob) if m else 0, len(m)))
        blob.extend(m)

    toc = bytearray(struct.pack("<4sIQQQQQ", b"TOC\x01", len(pkg_recs), dc, fc, size_wo_pool, len(pool), 0))
    for (p, root, exd, rod), (moff, msz) in zip(pkg_recs, meta_blobs):
        toc += struct.pack("<QIIIIBBHIQI", p["title_id"], root, exd, rod, p.get("version", 0), p["kind"], 0, 0, 0, moff, msz)
        toc += bytes(20)
    for (no, nl), first, d in zip(dnames, dir_first, dirs):
        toc += struct.pack("<IIIIQ", no, nl, first, len(d[1]), 0)
    for (no, nl), (name, data), (eo, es, codec) in zip(fnames, files, extents):
        toc += struct.pack("<IIQQQ", no, nl, len(data), eo, es) + hashlib.sha256(data).digest()
        toc += bytes([codec, 0]) + bytes(14)
    toc += struct.pack(f"<{len(children)}I", *children)
    toc += pool
    assert len(toc) == toc_size

    file_size = blob_off + len(blob)
    sha = hashlib.sha256(toc).digest()
    if corrupt_toc:
        sha = bytes(32)
    uuid = hashlib.sha256(b"".join(struct.pack("<QIB", p["title_id"], p.get("version", 0), p["kind"])
                                   for p, *_ in pkg_recs)).digest()[:16]
    sb = struct.pack("<4sHHQQ", b"NUP\x01", 0x80, 1, 0x80, toc_size) + sha
    sb += struct.pack("<QQII", blob_off, file_size, len(pkg_recs), 0) + uuid + bytes(32)
    assert len(sb) == 0x80
    return sb + bytes(toc) + bytes(blob_off - 0x80 - toc_size) + bytes(blob)


def test_vector() -> bytes:
    """The synthetic package tests/data/synthetic.nup holds (deterministic: zstd level 3 on fixed content).
    Regenerate: python -c "import sys; sys.path.insert(0, 'tests'); import nup_synth as s; open('tests/data/synthetic.nup', 'wb').write(s.test_vector())"
    """
    big = bytes((k * 7 + k // 13) & 0xFF for k in range(200_000))  # compresses, spans several zstd blocks
    return build([
        dict(title_id=0x040015100B514001, kind=0, meta=[(2, fake_nacp("Synthetic", "9.8.7", 0x010015100B515000))], tree={
            "exefs/main": fake_nso(b"\x11" * 20, b"code" * 64),
            "exefs/main.npdm": fake_npdm(0x040015100B514001),
            "romfs/System/AddressTable/a.bin": b"raw-file-content",
            "romfs/System/big.bin": big,
            "romfs/Stage/b.txt": b"hello " * 100,
            "romfs/empty": b"",
        }),
        dict(title_id=0x010015100B515001, kind=2, tree={"romfs/System/AocUpgradeVersion.txt": b"1"}),
    ], compress=True)
