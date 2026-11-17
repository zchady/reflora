"""tools/collect_report.py on synthetic collection sessions (the line format runtime/src/collect.cpp writes)."""
import os
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import collect_report as cr  # noqa: E402
import pipeline_catalog as pc  # noqa: E402


def rec(vs, fs, topology=4):
    key = bytearray(pc.KEY)
    struct.pack_into("<I", key, 16, topology)
    return struct.pack("<II", pc.GRAPHICS, 0) + vs.encode().ljust(16, b"\0") + fs.encode().ljust(16, b"\0") + bytes(key)


def session(path, lines):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        for wall, sec, kind, *fields in lines:
            f.write("\t".join([wall, f"{sec:.3f}", kind, *fields]) + "\n")


HITCH = "{ms} ms\tpipelines {n} ({pms} ms)\tASTC 0 (0.0 ms)\timages 1 (0.1 ms)\tupload 2.0 MB\tpackage reads 0 (0.0 MB, 0.0 ms)\tsubmits 1.0 ms\tsync waits 0.5 ms\tgame frame 3.0 ms"


def test_causes():
    h = cr.hitch_fields(HITCH.format(ms=300, n=5, pms=280).split("\t"))
    assert cr.cause(h) == "pipelines compiled"
    h = cr.hitch_fields("120 ms\tpipelines 0 (0.0 ms)\tASTC 3 (60.0 ms)\timages 2 (5.0 ms)\tupload 30 MB".split("\t"))
    assert cr.cause(h) == "textures (ASTC, images, uploads)"
    h = cr.hitch_fields("80 ms\tpipelines 0 (0.0 ms)\tASTC 0 (0.0 ms)\timages 0 (0.0 ms)\tupload 1 MB\t"
                        "package reads 2 (5.0 MB, 40.0 ms)".split("\t"))
    assert cr.cause(h) == "package reads"
    h = cr.hitch_fields("40 ms\tpipelines 0 (0.0 ms)\tASTC 0 (0.0 ms)\timages 0 (0.0 ms)\tupload 1 MB\t"
                        "sync waits 2.0 ms\tgame frame 2.0 ms\tpipeline-objects lock waits 18.0 ms".split("\t"))
    assert cr.cause(h) == "pipeline-objects lock (catalog preparation)"


def test_report_classifies_and_leaves_out_test_captures(tmp_path, capsys):
    d = tmp_path / "collect"
    session(str(d / "sessions" / "20261113-100000.tsv"), [
        ("2026-11-13 10:00:00.000", 0.0, "session", "start", "data x"),
        ("2026-11-13 10:00:05.000", 5.0, "catalog", "1231 pipelines (14 compute) from 1 file(s), mode background"),
        ("2026-11-13 10:00:30.000", 30.0, "pipeline", "graphics", "40.00 ms", "aaaa", "bbbb", "topology 4", "recorded"),
        ("2026-11-13 10:00:30.050", 30.05, "hitch", *HITCH.format(ms=300, n=1, pms=40).split("\t")),
        ("2026-11-13 10:00:40.000", 40.0, "capture", "frame dump at present 2400"),
        ("2026-11-13 10:00:40.200", 40.2, "hitch", *HITCH.format(ms=180, n=0, pms=0).split("\t")),  # the capture's
        ("2026-11-13 10:00:41.000", 41.0, "shader", "missing", "cccc", "code 0x100 bytes"),
        ("2026-11-13 10:00:50.000", 50.0, "audio", "400 of 2000 frames silent", "0 underruns", "0 dropped", "queue 3",
         "10.000 s"),
        ("2026-11-13 10:01:00.000", 60.0, "minute", "3500 frames (58.3 fps)", ">= 33 ms 3", ">= 50 ms 2",
         "worst 300.0 ms", "pipelines 1 (40.0 ms)", "ASTC 0 (0.0 ms)", "images 1", "upload 2.0 MB"),
        ("2026-11-13 10:01:00.5", 60.5, "hitch"),  # a line a crash cut short: ignored
    ])
    pc.write(str(d / "pipelines_collected.bin"), [rec("aaaa", "bbbb"), rec("dddd", "eeee")])
    official = tmp_path / "official.bin"
    pc.write(str(official), [rec("dddd", "eeee")])
    cr.report(type("A", (), {"dir": str(d), "catalog": str(official), "out": str(tmp_path / "r.md")})())
    text = (tmp_path / "r.md").read_text(encoding="utf-8")
    assert "1 slow frames right after a test's frame capture left out" in text
    assert "| pipelines compiled | 1 | 0.3 s | 300 ms |" in text
    assert "missing cccc" in text
    assert "Collected: 2 records" in text and "new to the official catalog: 1" in text
    assert "1.0 min" in text or "Play recorded:** 1 session(s)" in text


def test_report_sets_silences_against_stalls(tmp_path):
    d = tmp_path / "collect"
    session(str(d / "sessions" / "20261113-110000.tsv"), [
        ("2026-11-13 11:00:00.000", 0.0, "session", "start", "data x"),
        ("2026-11-13 11:00:20.000", 20.0, "hitch", *HITCH.format(ms=900, n=9, pms=850).split("	")),
        ("2026-11-13 11:00:21.000", 21.0, "audio", "silence", "1.50 s", "game audio updates paused up to 30 ms"),
        ("2026-11-13 11:00:40.000", 40.0, "audio", "silence", "2.00 s", "game audio updates paused up to 20 ms"),
        ("2026-11-13 11:00:50.000", 50.0, "audio", "silence", "0.40 s", "game audio updates paused up to 450 ms"),
    ])
    cr.report(type("A", (), {"dir": str(d), "catalog": None, "out": str(tmp_path / "r.md")})())
    text = (tmp_path / "r.md").read_text(encoding="utf-8")
    # the first overlaps the 900 ms hitch, the third had the game's updates paused; the second is the game's own
    assert "Silences of 250 ms or more: 3, 3.9 s" in text and ": 2, 1.9 s" in text
    assert "1.50 s silent; audio updates paused up to 30 ms; hitches 900 ms" in text
    assert "0.40 s silent; audio updates paused up to 450 ms; hitches none" in text


def test_validate_keeps_only_new_records_whose_shaders_exist(tmp_path):
    d = tmp_path / "collect"
    os.makedirs(d)
    spirv = tmp_path / "spirv"
    os.makedirs(spirv)
    for h in ("aaaa", "bbbb", "dddd", "eeee"):
        (spirv / f"{h}.spv").write_bytes(b"\x03\x02\x23\x07")
    (spirv / "results.tsv").write_text("aaaa\tx\nbbbb\tx\ndddd\tx\neeee\tx\n")
    pc.write(str(d / "pipelines_collected.bin"),
             [rec("aaaa", "bbbb"), rec("dddd", "eeee"), rec("ffff", "bbbb"), rec("aaaa", "bbbb", topology=5)])
    official = tmp_path / "official.bin"
    pc.write(str(official), [rec("dddd", "eeee")])
    out = tmp_path / "cand.bin"
    subprocess.run([sys.executable, os.path.join(REPO, "tools", "collect_report.py"), "validate", str(d), "--catalog",
                    str(official), "--spirv", str(spirv), "--out", str(out)], check=True, capture_output=True)
    got = pc.read(str(out))[0]
    assert got == [rec("aaaa", "bbbb"), rec("aaaa", "bbbb", topology=5)]  # not the official one, not the one without .spv


def test_compile_check_with_no_candidates_starts_nothing(tmp_path, capsys):
    cand = tmp_path / "c.bin"
    pc.write(str(cand), [])
    assert cr.compile_check(type("A", (), {"candidates": str(cand), "workspace": str(tmp_path / "nowhere")})()) == 0
    assert "no candidates" in capsys.readouterr().out
