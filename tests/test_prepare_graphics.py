"""The preparer's graphics step (tools/prepare_switch2.py) without the game: the runtime is replaced by a fake process
that writes the log lines the real one writes (nvn_pipeline_catalog.cpp) and leaves a pipeline cache file."""
import json
import os
import struct
import sys
import types

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import pipeline_catalog as pc  # noqa: E402
import prepare_switch2 as ps  # noqa: E402

CACHE = "pipelines_1002_7550.bin"


def cache_bytes(payload=b"driver data"):
    return struct.pack("<IIQQ", 0x4356584E, 1, len(payload), 0) + payload


@pytest.fixture
def ws(tmp_path, monkeypatch):
    w = tmp_path / "ws"
    (w / "bin").mkdir(parents=True)
    (w / "work").mkdir()
    (w / "data" / "spirv").mkdir(parents=True)
    (w / "bin" / "wonder2.exe").write_bytes(b"MZ ... " + ps.RUNTIME_MARK + b" ...")
    (w / "data" / "spirv" / "results.tsv").write_text("hash\tresources\n")
    (w / "data" / "spirv" / "0123456789abcdef.spv").write_bytes(b"\x03\x02\x23\x07")
    cat = tmp_path / "catalog.bin"
    rec = struct.pack("<II", pc.COMPUTE, 0) + b"0123456789abcdef" + bytes(16) + bytes(pc.KEY)
    pc.write(str(cat), [rec])
    monkeypatch.setattr(ps, "PROJECT_CATALOG", str(cat))
    monkeypatch.setattr(ps, "gpu_signature", lambda: [["pci\\ven_1234&dev_0001", "Test GPU", "32.0", "x"]])
    return str(w)


def args(**kw):
    a = dict(graphics_jobs=2, graphics_timeout=30, graphics_salt=None)
    a.update(kw)
    return types.SimpleNamespace(**a)


def fake_runtime(monkeypatch, code=0, lines=None, cache=True, hang=False):
    """subprocess.Popen replaced: the log and cache the runtime would leave, then `code` (or never exits: `hang`)."""
    calls = []
    if lines is None:
        lines = ["nvn/vk: pipeline catalog: 1 pipelines (1 compute) from 1 file(s) (1 records, 0 with a missing "
                 "shader, 0 damaged entries); mode exit",
                 "nvn/vk: pipeline catalog: progress 1/1",
                 "nvn/vk: pipeline catalog: prepared (1 made, 0 failed), pipeline cache saved (11 bytes), stamp "
                 "written; exiting"]

    class Proc:
        _handle = 0

        def __init__(self, cmd, cwd=None, env=None, **kw):
            calls.append(env)
            self.killed = False
            if not hang:
                with open(env["NX_LOG"], "w", encoding="utf-8") as f:
                    f.write("\n".join(lines) + "\n")
                if cache:
                    with open(os.path.join(env["NX_DATA_ROOT"], CACHE), "wb") as f:
                        f.write(cache_bytes())

        def poll(self):
            return None if hang and not self.killed else (-9 if self.killed else code)

        def wait(self):
            return -9 if self.killed else code

        def kill(self):
            self.killed = True

    monkeypatch.setattr(ps.subprocess, "Popen", Proc)
    return calls


def state(ws):
    with open(os.path.join(ws, "work", ps.GRAPHICS_STATE), encoding="utf-8") as f:
        return json.load(f)


def test_a_complete_preparation(ws, monkeypatch, capsys):
    calls = fake_runtime(monkeypatch)
    monkeypatch.setenv("NX_SPIRV_SALT", "left over from a measurement")
    ps.step_graphics(args(), ws)
    out = capsys.readouterr().out
    env = calls[0]
    assert env["NX_PREWARM_MODE"] == "exit" and env["NX_PREWARM_THREADS"] == "2"
    assert "NX_SPIRV_SALT" not in env  # measurement switches never leak into a preparation
    assert env["NX_PIPELINE_PREWARM"] == os.path.join(ws, "data", ps.INSTALLED_CATALOG)
    assert open(env["NX_PIPELINE_PREWARM"], "rb").read() == open(ps.PROJECT_CATALOG, "rb").read()
    assert "PROGRESS 99" in out and "RESULT graphics ok made=1 failed=0 total=1 compute=1" in out
    assert "COUNT 0/1 pipelines" in out and "COUNT 1/1 pipelines" in out
    assert "WARN" not in out
    s = state(ws)
    assert (s["status"], s["made"], s["cache"]) == ("ok", 1, CACHE)
    assert ps.graphics_done(ws)  # recognized at once next time, without starting anything


def test_what_makes_it_run_again(ws, monkeypatch):
    fake_runtime(monkeypatch)
    ps.step_graphics(args(), ws)
    assert ps.graphics_done(ws)
    cache = os.path.join(ws, "data", CACHE)
    open(cache, "ab").write(b"!")  # damaged
    assert not ps.graphics_done(ws)
    open(cache, "wb").write(cache_bytes())
    assert ps.graphics_done(ws)
    os.remove(cache)  # deleted
    assert not ps.graphics_done(ws)
    ps.step_graphics(args(), ws)
    monkeypatch.setattr(ps, "gpu_signature", lambda: [["pci\\ven_10de&dev_2507", "NVIDIA", "1.0", "y"]])
    assert not ps.graphics_done(ws)  # another GPU or driver
    monkeypatch.setattr(ps, "gpu_signature", lambda: None)
    assert not ps.graphics_done(ws)  # unknown: ask the runtime
    monkeypatch.setattr(ps, "gpu_signature", lambda: [["pci\\ven_1234&dev_0001", "Test GPU", "32.0", "x"]])
    assert ps.graphics_done(ws)
    open(ps.PROJECT_CATALOG, "ab").write(b"x")  # a new catalog
    assert not ps.graphics_done(ws)


def test_new_spirv_or_executable_runs_it_again(ws, monkeypatch):
    fake_runtime(monkeypatch)
    ps.step_graphics(args(), ws)
    open(os.path.join(ws, "data", "spirv", "fedcba9876543210.spv"), "wb").write(b"\x03\x02\x23\x07")
    assert not ps.graphics_done(ws)
    ps.step_graphics(args(), ws)
    open(os.path.join(ws, "bin", "wonder2.exe"), "ab").write(b" rebuilt")
    assert not ps.graphics_done(ws)


def test_refused_pipelines_are_a_warning_not_a_failure(ws, monkeypatch, capsys):
    fake_runtime(monkeypatch, code=4, lines=[
        "nvn/vk: pipeline catalog: 1 pipelines (1 compute) from 1 file(s) (1 records, 0 with a missing shader, 0 "
        "damaged entries); mode exit",
        "nvn/vk: pipeline catalog: prepared (0 made, 1 failed), pipeline cache saved (11 bytes), stamp not written; "
        "exiting"])
    ps.step_graphics(args(), ws)
    out = capsys.readouterr().out
    assert "RESULT graphics partial made=0 failed=1" in out and "WARN graphics preparation partial" in out
    assert not ps.graphics_done(ws)  # retried next time


def test_a_runtime_that_crashes(ws, monkeypatch, capsys):
    fake_runtime(monkeypatch, code=0xC0000005, lines=["nvn/vk: something"], cache=False)
    ps.step_graphics(args(), ws)
    out = capsys.readouterr().out
    assert "RESULT graphics failed" in out and "before reading the catalog" in state(ws)["reason"]


def test_a_runtime_that_never_answers_is_stopped(ws, monkeypatch, capsys):
    monkeypatch.setattr(ps, "GRAPHICS_START_LIMIT", 0.3)
    fake_runtime(monkeypatch, hang=True)
    ps.step_graphics(args(), ws)
    assert "no answer from the runtime" in state(ws)["reason"]
    assert "RESULT graphics failed" in capsys.readouterr().out


def test_an_old_executable_is_skipped_without_starting_it(ws, monkeypatch, capsys):
    calls = fake_runtime(monkeypatch)
    open(os.path.join(ws, "bin", "wonder2.exe"), "wb").write(b"MZ an executable from before")
    ps.step_graphics(args(), ws)
    assert not calls
    assert "RESULT graphics skipped" in capsys.readouterr().out


def test_a_damaged_project_catalog_is_skipped(ws, monkeypatch, capsys):
    calls = fake_runtime(monkeypatch)
    open(ps.PROJECT_CATALOG, "wb").write(b"not a catalog")
    ps.step_graphics(args(), ws)
    assert not calls and state(ws)["status"] == "skipped"


def test_no_executable_needs_the_build(ws):
    os.remove(os.path.join(ws, "bin", "wonder2.exe"))
    with pytest.raises(ps.NeedsUser, match="build step"):
        ps.step_graphics(args(), ws)


def test_cache_file_check(tmp_path):
    p = tmp_path / "c.bin"
    p.write_bytes(cache_bytes())
    assert ps.cache_file_ok(str(p))
    p.write_bytes(cache_bytes()[:-1])
    assert not ps.cache_file_ok(str(p))
    p.write_bytes(b"raw driver data without the header")
    assert not ps.cache_file_ok(str(p))


def test_graphics_is_the_last_step():
    assert ps.STEPS[-2:] == ["check", "graphics"]


def test_this_machine_gpu_signature_reads_quickly():
    import time
    t = time.time()
    sig = ps.gpu_signature()
    assert time.time() - t < 1.0
    if sys.platform == "win32":
        assert sig and all(len(x) == 4 for x in sig)
