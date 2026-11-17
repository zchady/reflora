#!/usr/bin/env python3
"""Read-only NUP reader (NX package: unencrypted Switch / Switch 2 dumps in one file).

Written from the public format description (github.com/a55uka/nup, SPEC.md, MIT); no code is taken from the
reference implementation. Only the index (superblock + TOC) is read up front; file contents are read on demand, so a
multi-GB package is inspected without extracting it.

Usage:
  python tools/nup.py info <file.nup>                       packages, programs, modules, DLC links, layering
  python tools/nup.py ls <file.nup> --package N [--subtree exefs] [--depth 2]
  python tools/nup.py extract <file.nup> <out dir> --package N [--subtree exefs]   verified (SHA-256) extraction
  python tools/nup.py verify <file.nup> [--package N] [--subtree exefs]           recompute per-file SHA-256

Everything extracted is game data: write it under private/ only (gitignored), never commit it.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nxcontent as nxc  # noqa: E402

MAGIC = b"NUP\x01"
TOC_MAGIC = b"TOC\x01"
SB_SIZE, PKG_SIZE, DIR_SIZE, FILE_SIZE = 0x80, 0x40, 0x18, 0x50
NONE = 0xFFFFFFFF
KINDS = {0: "base", 1: "update", 2: "addon", 3: "addon_update", 4: "system"}
CODECS = {0: "raw", 1: "zstd"}


class NupError(Exception):
    pass


@dataclass(frozen=True)
class Package:
    index: int
    title_id: int
    root_dir: int
    exefs_dir: int
    romfs_dir: int
    version: int
    kind: int
    flags: int
    meta_offset: int
    meta_size: int

    @property
    def kind_name(self) -> str:
        return KINDS.get(self.kind, f"kind{self.kind}")


@dataclass(frozen=True)
class FileNode:
    index: int
    name: str
    size: int
    extent_offset: int
    extent_size: int
    sha256: bytes
    codec: int


class Nup:
    def __init__(self, path: str):
        self.path = path
        self.f = open(path, "rb")
        try:
            self._load()
        except BaseException:
            self.f.close()
            raise

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ---- index -----------------------------------------------------------------------------------------------

    def _load(self):
        real_size = os.fstat(self.f.fileno()).st_size
        sb = self.f.read(SB_SIZE)
        if len(sb) < SB_SIZE or sb[:4] != MAGIC:
            raise NupError("not a NUP v1 file (bad magic)")
        hsz, _hver, toc_off, toc_size = struct.unpack_from("<HHQQ", sb, 4)
        toc_sha = sb[0x18:0x38]
        blob_off, file_size, pkg_count, flags = struct.unpack_from("<QQII", sb, 0x38)
        self.uuid = sb[0x50:0x60]
        if hsz < SB_SIZE or toc_off != hsz:
            raise NupError("bad header_size / toc_offset")
        if flags:
            raise NupError(f"unknown superblock flags {flags:#x}")
        if file_size != real_size:
            raise NupError(f"file_size {file_size} != actual {real_size} (truncated download?)")
        if blob_off % 16 or blob_off < toc_off + toc_size or blob_off > file_size:
            raise NupError("bad blob_offset")
        if pkg_count < 1:
            raise NupError("no packages")
        self.f.seek(toc_off)
        toc = self.f.read(toc_size)
        if len(toc) != toc_size or hashlib.sha256(toc).digest() != toc_sha:
            raise NupError("TOC SHA-256 mismatch")
        self.toc_sha256 = toc_sha
        self.blob_offset, self.file_size = blob_off, file_size

        magic, pc, dc, fc, pool_off, pool_size, _ = struct.unpack_from("<4sIQQQQQ", toc, 0)
        if magic != TOC_MAGIC or pc != pkg_count:
            raise NupError("bad TOC header")
        dbase = 0x30 + pc * PKG_SIZE
        fbase = dbase + dc * DIR_SIZE
        cbase = fbase + fc * FILE_SIZE
        if cbase > len(toc) or pool_off + pool_size > len(toc):
            raise NupError("TOC truncated")
        pool = toc[pool_off:pool_off + pool_size]

        def name(off, ln):
            if off + ln > len(pool):
                raise NupError("name out of string pool")
            return pool[off:off + ln].decode("utf-8")

        self.packages = []
        for i in range(pc):
            tid, root, exd, rod, ver, kind, pflags, _r, _r2, moff, msz = struct.unpack_from(
                "<QIIIIBBHIQI", toc, 0x30 + i * PKG_SIZE)
            if kind >= 5:
                raise NupError(f"package {i}: unknown kind {kind}")
            if moff and (moff + msz > file_size):
                raise NupError(f"package {i}: meta block out of file")
            self.packages.append(Package(i, tid, root, exd, rod, ver, kind, pflags, moff, msz))

        self.dirs = []
        nchild = 0
        for i in range(dc):
            no, nl, first, count = struct.unpack_from("<IIII", toc, dbase + i * DIR_SIZE)
            self.dirs.append((name(no, nl), first, count))
            nchild = max(nchild, first + count)
        if cbase + nchild * 4 > len(toc):
            raise NupError("children array truncated")
        self.children = struct.unpack_from(f"<{nchild}I", toc, cbase)

        self.files = []
        for i in range(fc):
            o = fbase + i * FILE_SIZE
            no, nl, size, eo, es = struct.unpack_from("<IIQQQ", toc, o)
            codec = toc[o + 0x40]
            if codec not in CODECS:
                raise NupError(f"file {i}: unknown codec {codec}")
            if es and (eo < blob_off or eo + es > file_size or eo % 16):
                raise NupError(f"file {i}: extent out of blob region")
            if codec == 0 and es != size:
                raise NupError(f"file {i}: raw extent size != size")
            self.files.append(FileNode(i, name(no, nl), size, eo, es, toc[o + 0x20:o + 0x40], codec))
        for p in self.packages:
            for d in (p.root_dir, p.exefs_dir, p.romfs_dir):
                if d != NONE and d >= dc:
                    raise NupError(f"package {p.index}: directory index out of range")

    # ---- tree ------------------------------------------------------------------------------------------------

    def walk(self, d: int, prefix: str = "", depth: int | None = None):
        """Yield (path, FileNode | None, is_dir) under directory index d."""
        _, first, count = self.dirs[d]
        dc = len(self.dirs)
        for nid in self.children[first:first + count]:
            if nid < dc:
                p = prefix + self.dirs[nid][0]
                yield p, None, True
                if depth is None or depth > 1:
                    yield from self.walk(nid, p + "/", None if depth is None else depth - 1)
            else:
                fn = self.files[nid - dc]
                yield prefix + fn.name, fn, False

    def files_of(self, d: int) -> dict:
        return {p: fn for p, fn, is_dir in self.walk(d) if not is_dir}

    def subtree(self, pkg: Package, sub: str | None) -> int:
        if not sub:
            return pkg.root_dir
        if sub == "exefs" and pkg.exefs_dir != NONE:
            return pkg.exefs_dir
        if sub == "romfs" and pkg.romfs_dir != NONE:
            return pkg.romfs_dir
        d = pkg.root_dir
        for part in sub.strip("/").split("/"):
            _, first, count = self.dirs[d]
            nxt = [n for n in self.children[first:first + count] if n < len(self.dirs) and self.dirs[n][0] == part]
            if not nxt:
                raise NupError(f"package {pkg.index}: no directory {sub}")
            d = nxt[0]
        return d

    # ---- contents --------------------------------------------------------------------------------------------

    def read(self, fn: FileNode, limit: int | None = None) -> bytes:
        """Uncompressed bytes of a file (only the first `limit` bytes when given, without hashing)."""
        want = fn.size if limit is None else min(limit, fn.size)
        if fn.size == 0:
            return b""
        self.f.seek(fn.extent_offset)
        if fn.codec == 0:
            data = self.f.read(want)
        else:
            import zstandard
            d = zstandard.ZstdDecompressor()
            with d.stream_reader(self.f, read_size=1 << 20, read_across_frames=False, closefd=False) as r:
                parts, got = [], 0
                while got < want:
                    chunk = r.read(min(1 << 24, want - got))
                    if not chunk:
                        break
                    parts.append(chunk)
                    got += len(chunk)
                data = b"".join(parts)
        if len(data) != want:
            raise NupError(f"{fn.name}: short read {len(data)} of {want}")
        if limit is None and hashlib.sha256(data).digest() != fn.sha256:
            raise NupError(f"{fn.name}: SHA-256 mismatch")
        return data

    def meta(self, pkg: Package):
        """Package meta TLVs, classified by content: [(type, label, payload)].

        SPEC 1.0-draft numbers NPDM=1, NACP=2, title=5, build ID=7, but packers in the wild use other numbers
        (the Wonder package seen here has NPDM at 0, title at 4, build ID at 6), so the payload decides the label.
        """
        if not pkg.meta_offset:
            return []
        self.f.seek(pkg.meta_offset)
        raw = self.f.read(pkg.meta_size)
        out, o = [], 0
        while o + 8 <= len(raw):
            t, _r, s = struct.unpack_from("<HHI", raw, o)
            if o + 8 + s > len(raw):
                break
            pl = raw[o + 8:o + 8 + s]
            if pl[:4] == b"META":
                label = "npdm"
            elif s == nxc.NACP_SIZE:
                label = "nacp"
            elif pl[:2] == b"\xff\xd8" or pl[:4] == b"\x89PNG":
                label = "icon"
            elif s == 32:
                label = "build_id"
            else:
                try:
                    pl.decode("utf-8")
                    label = "text"
                except UnicodeDecodeError:
                    label = "text?" if all(b >= 0x20 or b in (9, 10) for b in pl) else "binary"
            out.append((t, label, pl))
            o += 8 + s
        return out


# ---- reports ---------------------------------------------------------------------------------------------------


def _txt(b: bytes) -> str:
    return b.decode("utf-8", "replace")


def program_of(nup: Nup, pkg: Package):
    """(Program, npdm program id, NACP | None, {module: (size, module_id hex)}) for a package with an ExeFS."""
    if pkg.exefs_dir == NONE:
        return None
    ex = nup.files_of(pkg.exefs_dir)
    mods = {}
    for name, fn in sorted(ex.items()):
        head = nup.read(fn, 0x100)
        mid = nxc.nso_module_id(head)
        if mid is not None:
            mods[name] = (fn.size, mid.hex())
    npdm = nup.read(ex["main.npdm"]) if "main.npdm" in ex else None
    pid = nxc.npdm_program_id(npdm) if npdm else None
    nacp = None
    root = nup.files_of(pkg.root_dir)
    if "control.nacp" in root:
        nacp = nxc.parse_nacp(nup.read(root["control.nacp"]))
    else:
        for _t, label, pl in nup.meta(pkg):
            if label == "nacp":
                nacp = nxc.parse_nacp(pl)
    return nxc.Program(pid or pkg.title_id, tuple(mods), f"package {pkg.index}"), pid, nacp, mods


def cmd_info(nup: Nup, args):
    print(f"{nup.path}\n  size {nup.file_size} B, {len(nup.packages)} packages, {len(nup.dirs)} dirs, "
          f"{len(nup.files)} files, uuid {nup.uuid.hex()}, TOC SHA-256 ok ({nup.toc_sha256.hex()[:16]}...)")
    programs, layers, nacps = [], [], {}
    by_extent = {}
    for pkg in nup.packages:
        tid = nxc.TitleId(pkg.title_id)
        print(f"\npackage {pkg.index}: {pkg.kind_name}, version {pkg.version}, flags {pkg.flags:#x}")
        print(f"  title  {tid.describe()}")
        for t, label, pl in nup.meta(pkg):
            if label in ("text", "text?"):
                shown = _txt(pl)
            elif label == "build_id":
                shown = pl.hex()
            elif label == "npdm":
                pid = nxc.npdm_program_id(pl)
                shown = f"program id {pid:016X}" if pid is not None else "no ACI0"
            else:
                shown = f"{len(pl)} bytes"
            print(f"  meta   tlv {t:#06x} {label:8} {shown}")
        prog = program_of(nup, pkg)
        if prog:
            p, pid, nacp, mods = prog
            programs.append(p)
            if pid is not None and pid != pkg.title_id:
                print(f"  WARNING main.npdm program id {pid:016X} != package title id")
            for name, (size, mid) in mods.items():
                print(f"  exefs  {name:9} {size:>10} B  module id {mid[:40]}")
            if nacp:
                nacps[pkg.title_id] = nacp
                print(f"  nacp   '{nacp.name}' / '{nacp.publisher}' version '{nacp.display_version}', "
                      f"add-on base {nacp.addon_base_id:016X}, save owner {nacp.save_owner_id:016X}")
        if pkg.romfs_dir != NONE:
            files = nup.files_of(pkg.romfs_dir)
            size = sum(fn.size for fn in files.values())
            print(f"  romfs  {len(files)} files, {size / 2**30:.3f} GiB")
            kind = "addon" if pkg.kind == 2 else ("update" if pkg.kind == 1 else "base")
            layers.append((kind, pkg.version, pkg.title_id, {k: v.sha256 for k, v in files.items()}))
            by_extent[pkg.index] = files

    print("\nprograms")
    for p in programs:
        print(f"  {nxc.TitleId(p.title_id).describe()} [{', '.join(p.modules)}]")
    for pref in ("switch2", "switch"):
        try:
            sel = nxc.select_program(programs, prefer=pref)
            print(f"  selected (prefer {pref}): {sel.title_id:016X} from {sel.source}")
        except LookupError as e:
            print(f"  selected (prefer {pref}): none - {e}")

    print("\nadd-ons")
    for pkg in nup.packages:
        if pkg.kind != 2:
            continue
        for p in programs:
            nacp = nacps.get(p.title_id)
            base_id = nacp.addon_base_id if nacp else None
            ok = nxc.addon_belongs_to(pkg.title_id, p.title_id, base_id)
            how = f"NACP add-on base {base_id:016X}" if base_id else "title family"
            print(f"  {pkg.title_id:016X} -> program {p.title_id:016X}: {'belongs' if ok else 'unrelated'} (by {how})")

    if layers:
        view, addons = nxc.layer_romfs(layers)
        print(f"\nromfs layering: program view {len(view)} files")
        for tid, files in addons.items():
            same = sum(1 for k, h in files.items() if k in view and view[k][1] == h)
            diff = sum(1 for k, h in files.items() if k in view and view[k][1] != h)
            only = sum(1 for k in files if k not in view)
            print(f"  add-on {tid:016X}: {len(files)} files: {same} identical to the program's, "
                  f"{diff} same path different content, {only} only in the add-on")


def cmd_ls(nup: Nup, args):
    pkg = nup.packages[args.package]
    for path, fn, is_dir in nup.walk(nup.subtree(pkg, args.subtree), depth=args.depth):
        if is_dir:
            print(f"{'':>12}  {'':4}  {path}/")
        else:
            print(f"{fn.size:>12}  {CODECS[fn.codec]:4}  {path}  {fn.sha256.hex()[:16]}")


def _single_file(nup: Nup, pkg: Package, sub: str | None):
    """The file node when `sub` names a file (dir/name), else None."""
    if not sub or "/" not in sub.strip("/"):
        return None
    parent, name = sub.strip("/").rsplit("/", 1)
    try:
        d = nup.subtree(pkg, parent)
    except NupError:
        return None
    return nup.files_of(d).get(name) if name in {n for n, _f, isd in nup.walk(d, depth=1) if not isd} else None


def cmd_extract(nup: Nup, args):
    pkg = nup.packages[args.package]
    out = os.path.abspath(args.out)
    n = total = 0
    one = _single_file(nup, pkg, args.subtree)
    if one is not None:  # a single file: written as <out>/<its name>
        os.makedirs(out, exist_ok=True)
        dst = os.path.join(out, one.name)
        data = nup.read(one)
        with open(dst + ".part", "wb") as f:
            f.write(data)
        os.replace(dst + ".part", dst)
        print(f"extracted 1 file, {one.size} B to {dst}")
        return
    for path, fn, is_dir in nup.walk(nup.subtree(pkg, args.subtree)):
        parts = path.split("/")
        if any(p in ("", ".", "..") or "\\" in p or ":" in p for p in parts):
            raise NupError(f"unsafe path in package: {path!r}")  # names are labels, never trusted as paths
        dst = os.path.join(out, *parts)
        if is_dir:
            os.makedirs(dst, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.exists(dst) and os.path.getsize(dst) == fn.size and not args.force:
            with open(dst, "rb") as f:
                if hashlib.sha256(f.read()).digest() == fn.sha256:
                    continue
        data = nup.read(fn)  # SHA-256 verified
        with open(dst + ".part", "wb") as f:
            f.write(data)
        os.replace(dst + ".part", dst)
        n += 1
        total += fn.size
    print(f"extracted {n} files, {total} B to {out}")


def cmd_verify(nup: Nup, args):
    pkgs = nup.packages if args.package is None else [nup.packages[args.package]]
    bad = n = 0
    for pkg in pkgs:
        for path, fn, is_dir in nup.walk(nup.subtree(pkg, args.subtree)):
            if is_dir:
                continue
            n += 1
            try:
                nup.read(fn)
            except NupError as e:
                bad += 1
                print(f"FAIL package {pkg.index} {path}: {e}")
    print(f"verified {n} files, {bad} failures")
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("info"); a.add_argument("nup")
    a = sub.add_parser("ls"); a.add_argument("nup"); a.add_argument("--package", type=int, default=0)
    a.add_argument("--subtree"); a.add_argument("--depth", type=int)
    a = sub.add_parser("extract"); a.add_argument("nup"); a.add_argument("out")
    a.add_argument("--package", type=int, required=True); a.add_argument("--subtree"); a.add_argument("--force", action="store_true")
    a = sub.add_parser("verify"); a.add_argument("nup"); a.add_argument("--package", type=int); a.add_argument("--subtree")
    args = ap.parse_args(argv)
    try:
        with Nup(args.nup) as nup:
            return {"info": cmd_info, "ls": cmd_ls, "extract": cmd_extract, "verify": cmd_verify}[args.cmd](nup, args) or 0
    except NupError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
