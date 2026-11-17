"""Title IDs, program selection and content layering for Switch / Switch 2 dumps.

Pure functions, no game data: used by tools/nup.py and covered by tests/test_nxcontent.py.

Title ID layout (Switchbrew, "Title list"): an application ID is aligned to 0x2000. Relative to it:
  +0x000..+0x00F  program index of a multi-program application (program 0 is the usual one)
  +0x800..+0x80F  the patch (update) of that program
  +0x1000 + n     add-on content (DLC) number n, n >= 1
The top byte says the platform family: 0x01 for Switch titles; Switch 2 native titles seen so far use 0x04
(Super Mario Bros. Wonder: Switch 1 application 010015100B514000, Switch 2 Edition program 040015100B514001).
Both share the low 56 bits, which is how a Switch 2 Edition program is tied to the Switch 1 game and its DLC.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

APP_ALIGN = 0x2000
FAMILY_MASK = 0x00FF_FFFF_FFFF_E000  # the title "family" without platform byte and without the low 13 bits

PLATFORMS = {0x01: "switch", 0x04: "switch2"}


@dataclass(frozen=True)
class TitleId:
    value: int

    @property
    def application(self) -> int:
        """The application ID this title belongs to (program 0, same platform byte)."""
        return self.value & ~(APP_ALIGN - 1)

    @property
    def rel(self) -> int:
        return self.value & (APP_ALIGN - 1)

    @property
    def kind(self) -> str:
        r = self.rel
        if r < 0x800:
            return "program" if r < 0x10 else "unknown"
        if r < 0x1000:
            return "patch" if (r - 0x800) < 0x10 else "unknown"
        return "addon" if r > 0x1000 else "unknown"

    @property
    def program_index(self) -> int | None:
        if self.kind == "program":
            return self.rel
        if self.kind == "patch":
            return self.rel - 0x800
        return None

    @property
    def addon_index(self) -> int | None:
        return self.rel - 0x1000 if self.kind == "addon" else None

    @property
    def platform(self) -> str:
        return PLATFORMS.get(self.value >> 56, f"unknown({self.value >> 56:#04x})")

    @property
    def family(self) -> int:
        return self.value & FAMILY_MASK

    def __str__(self) -> str:
        return f"{self.value:016X}"

    def describe(self) -> str:
        k = self.kind
        extra = ""
        if k in ("program", "patch"):
            extra = f" program {self.program_index}"
        elif k == "addon":
            extra = f" #{self.addon_index}"
        return f"{self} {self.platform} {k}{extra} of application {self.application:016X}"


def same_family(a: int, b: int) -> bool:
    """True when two title IDs belong to the same game across platforms (Switch 1 vs Switch 2 Edition)."""
    return TitleId(a).family == TitleId(b).family


def addon_belongs_to(addon: int, program: int, addon_base_id: int | None = None) -> bool:
    """Does add-on `addon` belong to the game whose program is `program`?

    With the program's NACP AddOnContentBaseId (the authority on console), the add-on must be base + n. Without it,
    fall back to the title family (the platform byte may differ: Switch 1 DLC for a Switch 2 Edition program).
    """
    a = TitleId(addon)
    if a.kind != "addon":
        return False
    if addon_base_id:
        return (addon & ~(APP_ALIGN - 1)) + 0x1000 == addon_base_id or addon - a.addon_index == addon_base_id
    return same_family(addon, program)


# ---- NACP and NPDM -----------------------------------------------------------------------------------------------

NACP_SIZE = 0x4000


@dataclass(frozen=True)
class Nacp:
    name: str
    publisher: str
    display_version: str
    addon_base_id: int
    save_owner_id: int
    presence_group_id: int


def parse_nacp(raw: bytes) -> Nacp:
    """ApplicationControlProperty (switchbrew "NACP"): the fields used for selection. Raises on a short blob."""
    if len(raw) < 0x3080:
        raise ValueError(f"NACP too short: {len(raw):#x}")
    titles = _nacp_titles(raw)
    name, publisher = "", ""
    for lang in range(len(titles) // 0x300):  # first non-empty language entry (0x300 each: name 0x200, publisher 0x100)
        e = titles[lang * 0x300:(lang + 1) * 0x300]
        n = e[:0x200].split(b"\0")[0].decode("utf-8", "replace")
        if n:
            name, publisher = n, e[0x200:].split(b"\0")[0].decode("utf-8", "replace")
            break
    dv = raw[0x3060:0x3070].split(b"\0")[0].decode("ascii", "replace")
    presence, = struct.unpack_from("<Q", raw, 0x3038)
    addon_base, save_owner = struct.unpack_from("<QQ", raw, 0x3070)
    return Nacp(name, publisher, dv, addon_base, save_owner, presence)


def _nacp_titles(raw: bytes) -> bytes:
    """The 0x3000-byte title area, or its compressed form: newer NACPs (32 languages, the Wonder Switch 2 Edition
    one included) store u16 size + a raw DEFLATE stream that inflates to 32 * 0x300 bytes. Measured on our dump;
    the flag that announces it is not identified yet, so the layout itself is checked."""
    import zlib
    size, = struct.unpack_from("<H", raw, 0)
    if 0 < size <= 0x3000 - 2:
        try:
            d = zlib.decompressobj(-15)
            out = d.decompress(raw[2:2 + size], 0x6000 + 1)
            if d.eof and len(out) == 0x6000:
                return out
        except zlib.error:
            pass
    return raw[:0x3000]


def npdm_program_id(raw: bytes) -> int | None:
    """Program ID from a main.npdm: the ACI0 block (offset at META+0x70) holds it at +0x10."""
    if raw[:4] != b"META" or len(raw) < 0x80:
        return None
    aci_off, = struct.unpack_from("<I", raw, 0x70)
    if raw[aci_off:aci_off + 4] != b"ACI0":
        return None
    return struct.unpack_from("<Q", raw, aci_off + 0x10)[0]


def nso_module_id(header: bytes) -> bytes | None:
    """The 32-byte ModuleId (build ID) of an NSO, from its first 0x60 bytes."""
    return header[0x40:0x60] if header[:4] == b"NSO0" and len(header) >= 0x60 else None


# ---- program selection -------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Program:
    """A runnable program found in a dump: its title ID and the modules of its ExeFS."""
    title_id: int
    modules: tuple  # names present in exefs, e.g. ("main", "rtld", "sdk", "subsdk0")
    source: str = ""


def select_program(programs, prefer: str = "switch2", index: int | None = None) -> Program:
    """Choose the program to run. `index` forces a program index; else prefer the platform asked for.

    Raises LookupError listing what exists when nothing matches (never guesses a different game).
    """
    cands = [p for p in programs if TitleId(p.title_id).kind == "program" and "main" in p.modules]
    if index is not None:
        cands = [p for p in cands if TitleId(p.title_id).program_index == index]
    if not cands:
        have = ", ".join(TitleId(p.title_id).describe() for p in programs) or "nothing"
        raise LookupError(f"no runnable program (index={index}); found: {have}")
    cands.sort(key=lambda p: (TitleId(p.title_id).platform != prefer, TitleId(p.title_id).program_index))
    return cands[0]


# ---- content layering --------------------------------------------------------------------------------------------

LAYER_ORDER = {"base": 0, "update": 1, "addon": 2}


def layer_romfs(layers):
    """Merge RomFS file maps the way the game sees them: base, then updates (oldest first), then add-ons.

    `layers` is an iterable of (kind, version, title_id, {path: ref}). A later layer shadows a path of an earlier one
    (updates replace base files). Add-ons are separate mounts on console (aoc:/); they are returned apart and never
    shadow the program's files. Returns (program_view {path: (layer_index, ref)}, addons {title_id: {path: ref}}).
    """
    ordered = sorted(enumerate(layers), key=lambda kv: (LAYER_ORDER[kv[1][0]], kv[1][1], kv[0]))
    view, addons = {}, {}
    for idx, (kind, _version, tid, files) in ordered:
        if kind == "addon":
            addons.setdefault(tid, {}).update(files)
            continue
        for path, ref in files.items():
            view[path] = (idx, ref)
    return view, addons
