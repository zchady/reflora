"""Unit tests for tools/nup.py and tools/nxcontent.py on SYNTHETIC packages (tests/nup_synth.py, no game data).

Run: private/venv/Scripts/python -m pytest tests/test_nup.py
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)

import nup  # noqa: E402
import nup_synth as synth  # noqa: E402
import nxcontent as nxc  # noqa: E402

S1_APP = 0x010015100B514000
S2_PROG1 = 0x040015100B514001
DLC1 = 0x010015100B515001


# ---- title IDs ---------------------------------------------------------------------------------------------------

def test_title_kinds():
    assert nxc.TitleId(S1_APP).kind == "program" and nxc.TitleId(S1_APP).program_index == 0
    t = nxc.TitleId(S2_PROG1)
    assert (t.kind, t.program_index, t.platform, t.application) == ("program", 1, "switch2", 0x040015100B514000)
    assert nxc.TitleId(S1_APP | 0x800).kind == "patch"
    d = nxc.TitleId(DLC1)
    assert (d.kind, d.addon_index, d.application, d.platform) == ("addon", 1, S1_APP, "switch")
    assert nxc.TitleId(S1_APP + 0x1000).kind == "unknown"  # add-on numbers start at 1


def test_family_and_addon_link():
    assert nxc.same_family(S2_PROG1, S1_APP)
    assert nxc.addon_belongs_to(DLC1, S2_PROG1)                     # by family
    assert nxc.addon_belongs_to(DLC1, S2_PROG1, S1_APP + 0x1000)    # by NACP add-on base
    assert not nxc.addon_belongs_to(DLC1, S2_PROG1, 0x0100AAAA00001000)
    assert not nxc.addon_belongs_to(0x0100AAAA00001001, S2_PROG1)
    assert not nxc.addon_belongs_to(S1_APP, S2_PROG1)               # a program is not an add-on


def test_select_program():
    p0 = nxc.Program(S1_APP, ("main", "sdk"))
    p1 = nxc.Program(S2_PROG1, ("main", "sdk", "subsdk0"))
    assert nxc.select_program([p0, p1]).title_id == S2_PROG1
    assert nxc.select_program([p0, p1], prefer="switch").title_id == S1_APP
    assert nxc.select_program([p0, p1], index=0).title_id == S1_APP
    with pytest.raises(LookupError):
        nxc.select_program([p0], index=1)                          # program 1 absent: say so, never substitute
    with pytest.raises(LookupError):
        nxc.select_program([nxc.Program(S2_PROG1, ("sdk",))])      # no main module


def test_layering():
    base = ("base", 0, S1_APP, {"a": 1, "b": 2})
    upd2 = ("update", 0x20000, S1_APP | 0x800, {"b": 4})
    upd1 = ("update", 0x10000, S1_APP | 0x800, {"b": 3, "c": 3})
    dlc = ("addon", 0, DLC1, {"a": 9, "d": 9})
    view, addons = nxc.layer_romfs([dlc, upd2, base, upd1])
    assert {k: v[1] for k, v in view.items()} == {"a": 1, "b": 4, "c": 3}  # newest update wins, add-on never shadows
    assert addons == {DLC1: {"a": 9, "d": 9}}


def test_nacp_npdm_nso():
    n = nxc.parse_nacp(synth.fake_nacp("Game", "1.2.3", S1_APP + 0x1000))
    assert (n.name, n.publisher, n.display_version, n.addon_base_id) == ("Game", "Test", "1.2.3", S1_APP + 0x1000)
    c = nxc.parse_nacp(synth.fake_nacp("Gâme – S2", "1.2.1", S1_APP + 0x1000, compressed=True))
    assert (c.name, c.publisher, c.display_version) == ("Gâme – S2", "Test", "1.2.1")
    assert nxc.npdm_program_id(synth.fake_npdm(S2_PROG1)) == S2_PROG1
    assert nxc.npdm_program_id(b"NOPE" + bytes(0x100)) is None
    assert nxc.nso_module_id(synth.fake_nso(b"\xab" * 32)) == b"\xab" * 32
    with pytest.raises(ValueError):
        nxc.parse_nacp(b"\0" * 0x100)


# ---- container ---------------------------------------------------------------------------------------------------

def sample(tmp_path, **kw):
    pkgs = [
        dict(title_id=S2_PROG1, kind=0, tree={
            "control.nacp": synth.fake_nacp("Synthetic", "3.0.0", S1_APP + 0x1000),
            "exefs/main": synth.fake_nso(b"\x11" * 20, b"x" * 5000),
            "exefs/main.npdm": synth.fake_npdm(S2_PROG1),
            "exefs/sdk": synth.fake_nso(b"\x22" * 20),
            "exefs/subsdk0": synth.fake_nso(b"\x33" * 20),
            "romfs/System/a.bin": b"A" * 100,
            "romfs/Stage/b.bin": b"B" * 100,
            "romfs/empty": b"",
        }, meta=[(0, synth.fake_npdm(S2_PROG1)), (4, "Synthetic Game – S2".encode()), (6, b"\x11" * 20 + bytes(12))]),
        dict(title_id=DLC1, kind=2, tree={"romfs/System/a.bin": b"A" * 100, "romfs/Stage/new.bin": b"N" * 64}),
    ]
    p = tmp_path / "t.nup"
    p.write_bytes(synth.build(pkgs, **kw))
    return p


@pytest.mark.parametrize("compress", [False, True])
def test_read_tree_and_files(tmp_path, compress):
    with nup.Nup(str(sample(tmp_path, compress=compress))) as n:
        assert [p.kind_name for p in n.packages] == ["base", "addon"]
        base = n.packages[0]
        ex = n.files_of(base.exefs_dir)
        assert sorted(ex) == ["main", "main.npdm", "sdk", "subsdk0"]
        assert n.read(ex["main"]).endswith(b"x" * 5000)
        assert n.read(ex["main"], 0x60)[:4] == b"NSO0"
        rom = n.files_of(base.romfs_dir)
        assert sorted(rom) == ["Stage/b.bin", "System/a.bin", "empty"]
        assert n.read(rom["empty"]) == b""
        labels = [lab for _t, lab, _p in n.meta(base)]
        assert labels == ["npdm", "text", "build_id"]
        prog, pid, nacp, mods = nup.program_of(n, base)
        assert pid == S2_PROG1 and nacp.display_version == "3.0.0"
        assert list(mods) == ["main", "sdk", "subsdk0"]
        assert nup.program_of(n, n.packages[1]) is None  # add-on: no ExeFS


def test_extract_and_verify(tmp_path, capsys):
    p = sample(tmp_path, compress=True)
    out = tmp_path / "out"
    assert nup.main(["extract", str(p), str(out), "--package", "0", "--subtree", "exefs"]) == 0
    assert sorted(os.listdir(out)) == ["main", "main.npdm", "sdk", "subsdk0"]
    assert nup.main(["verify", str(p)]) == 0
    assert nup.main(["info", str(p)]) == 0
    text = capsys.readouterr().out
    assert "selected (prefer switch2): 040015100B514001" in text
    assert "010015100B515001 -> program 040015100B514001: belongs" in text
    assert "2 files: 1 identical to the program's, 0 same path different content, 1 only in the add-on" in text


def test_rejects_corruption(tmp_path):
    with pytest.raises(nup.NupError, match="TOC SHA-256"):
        nup.Nup(str(sample(tmp_path, corrupt_toc=True)))
    p = sample(tmp_path)
    raw = bytearray(p.read_bytes())
    p.write_bytes(raw[:-1])
    with pytest.raises(nup.NupError, match="file_size"):
        nup.Nup(str(p))
    raw[0] = ord("X")
    p.write_bytes(raw)
    with pytest.raises(nup.NupError, match="magic"):
        nup.Nup(str(p))


def test_detects_content_corruption(tmp_path):
    p = sample(tmp_path)
    raw = bytearray(p.read_bytes())
    k = raw.rfind(b"x" * 5000)          # inside exefs/main (raw codec)
    raw[k] ^= 0xFF
    p.write_bytes(raw)
    with nup.Nup(str(p)) as n:
        with pytest.raises(nup.NupError, match="SHA-256 mismatch"):
            n.read(n.files_of(n.packages[0].exefs_dir)["main"])
    assert nup.main(["verify", str(p), "--package", "0", "--subtree", "exefs"]) == 1
