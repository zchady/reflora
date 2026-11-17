"""tools/pipeline_catalog.py: the catalog format the runtime writes (nvn_pipeline_catalog.cpp, version 3; version 2
still read), read, checked and merged without the game. Synthetic records only: no captured data."""
import os
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import pipeline_catalog as pc  # noqa: E402


def key(topology=4):
    k = bytearray(pc.KEY)
    struct.pack_into("<I", k, 16, topology)
    return bytes(k)


def hashes(vs, fs):
    return vs.encode().ljust(16, b"\0") + fs.encode().ljust(16, b"\0")


def graphics(vs, fs, topology=4):
    return struct.pack("<II", pc.GRAPHICS, 0) + hashes(vs, fs) + key(topology)


def compute(cs, shared):
    return struct.pack("<II", pc.COMPUTE, shared) + hashes(cs, "") + bytes(pc.KEY)


def write(path, records, damage=(), version=3):
    rec = pc.REC if version == 3 else pc.REC_V2
    with open(path, "wb") as f:
        f.write(pc.HEADER.pack(pc.MAGIC, version, pc.KEY, rec, 2, 0, 0, 0))
        for i, r in enumerate(records):
            check = pc.fnv1a(r) ^ (1 if i in damage else 0)
            f.write(r + struct.pack("<Q", check))


def run(*args):
    return subprocess.run([sys.executable, os.path.join(REPO, "tools", "pipeline_catalog.py"), *args],
                          capture_output=True, text=True, check=True).stdout


def test_sizes_match_the_runtime():
    # nvn_pipeline_state.h: static_assert(sizeof(PipelineKey) == 704); nvn_pipeline_catalog.cpp:
    # static_assert(sizeof(Record) == 744 && offsetof(Record, key) == 40), RecordV2 736
    assert (pc.KEY, pc.REC, pc.REC_V2, pc.ENTRY, pc.HEADER.size, pc.KEY_AT) == (704, 744, 736, 752, 32, 40)
    assert pc.FIELDS[-1][1] + pc.FIELDS[-1][2] == pc.KEY


def test_fnv1a_reference_values():
    assert pc.fnv1a(b"") == 0xcbf29ce484222325
    assert pc.fnv1a(b"a") == 0xaf63dc4c8601ec8c


def test_graphics_and_compute_records(tmp_path):
    p = tmp_path / "c.bin"
    write(p, [graphics("aaaa", "bbbb"), compute("cccc", 2048)])
    recs, damaged, canon, version = pc.read(str(p))
    assert (damaged, canon, version) == (0, 2, 3)
    assert [(pc.kind(r), pc.hashes(r), pc.shared(r)) for r in recs] == [
        (pc.GRAPHICS, ("aaaa", "bbbb"), 0), (pc.COMPUTE, ("cccc", ""), 2048)]
    assert "1 graphics in 1 shader pairs, 1 compute" in run("stats", str(p))
    assert "compute cccc shared memory 2048 bytes" in run("dump", str(p))


def test_version_2_is_read_as_graphics(tmp_path):
    p = tmp_path / "v2.bin"
    write(p, [hashes("aaaa", "bbbb") + key()], version=2)
    recs, damaged, _, version = pc.read(str(p))
    assert (damaged, version) == (0, 2)
    assert recs == [graphics("aaaa", "bbbb")]


def test_damaged_entries_and_unknown_kinds_are_skipped(tmp_path):
    p = tmp_path / "c.bin"
    odd = struct.pack("<II", 7, 0) + hashes("eeee", "ffff") + key()
    write(p, [graphics("aaaa", "bbbb"), graphics("cccc", "dddd"), odd, compute("gggg", 0)], damage={1})
    recs, damaged, _, _ = pc.read(str(p))
    assert [pc.hashes(r) for r in recs] == [("aaaa", "bbbb"), ("gggg", "")]
    assert damaged == 2


def test_a_cut_short_file_keeps_its_whole_entries(tmp_path):
    p = tmp_path / "c.bin"
    write(p, [graphics("aaaa", "bbbb"), graphics("cccc", "dddd")])
    data = p.read_bytes()
    p.write_bytes(data[:-100])  # a recording ended by a crash in the middle of an entry
    assert len(pc.read(str(p))[0]) == 1


def test_another_version_is_refused(tmp_path):
    p = tmp_path / "c.bin"
    p.write_bytes(pc.HEADER.pack(pc.MAGIC, 1, pc.KEY, pc.REC_V2, 2, 0, 0, 0))
    try:
        pc.read(str(p))
    except SystemExit:
        return
    raise AssertionError("a version-1 file was read")


def test_merge_keeps_each_record_once_in_input_order_and_writes_version_3(tmp_path):
    a, b, out = tmp_path / "a.bin", tmp_path / "b.bin", tmp_path / "out.bin"
    write(a, [hashes("aaaa", "bbbb") + key(), hashes("cccc", "dddd") + key()], version=2)
    write(b, [graphics("cccc", "dddd"), graphics("aaaa", "bbbb", topology=5), compute("eeee", 512),
              compute("eeee", 1024)])
    run("merge", str(out), str(a), str(b))
    recs, damaged, _, version = pc.read(str(out))
    assert (damaged, version) == (0, 3)
    assert [(pc.kind(r), pc.hashes(r), pc.shared(r), struct.unpack_from("<I", r, 56)[0]) for r in recs] == [
        (0, ("aaaa", "bbbb"), 0, 4), (0, ("cccc", "dddd"), 0, 4), (0, ("aaaa", "bbbb"), 0, 5),
        (1, ("eeee", ""), 512, 0), (1, ("eeee", ""), 1024, 0)]


def test_diff_names_what_changed(tmp_path):
    a, b = tmp_path / "a.bin", tmp_path / "b.bin"
    write(a, [graphics("aaaa", "bbbb")])
    write(b, [graphics("aaaa", "bbbb"), graphics("aaaa", "bbbb", topology=5), graphics("eeee", "ffff"),
              compute("gggg", 0)])
    text = run("diff", str(a), str(b))
    assert "3 of 4 records" in text
    assert "topology" in text and "a shader pair the old catalog never had" in text and "a compute pipeline" in text
