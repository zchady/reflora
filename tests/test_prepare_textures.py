"""The preparer's optional textures step (tools/prepare_switch2.py) without the game: the runtime is replaced by a fake
process that writes the log lines the real one writes (nvn_astc_bc7_prepare.cpp) and leaves cache files."""
import json
import os
import sys
import types

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import prepare_switch2 as ps  # noqa: E402

DONE = ("astc bc7 prepare: done in 3.2 s: 2 BNTX, 5 textures, 3 ASTC (0 not 2D, 0 pitch-linear; 0 textures in BNTX of "
        "the first Switch, 0 unreadable entries); converted 3 (0.1 Mtexels): accepted 2, kept RGBA8 1, not written 0, "
        "could not untile 0; already in the cache 4, the same texture again 0; files not read 0")


@pytest.fixture
def ws(tmp_path):
    w = tmp_path / "ws"
    (w / "bin").mkdir(parents=True)
    (w / "work").mkdir()
    (w / "data").mkdir()
    (w / "bin" / "wonder2.exe").write_bytes(b"MZ ... " + ps.TEXTURES_MARK + b" ...")
    return str(w)


def args(**kw):
    a = dict(texture_jobs=2, textures_timeout=30)
    a.update(kw)
    return types.SimpleNamespace(**a)


def fake_runtime(monkeypatch, code=0, lines=None, hang=False):
    calls = []
    if lines is None:
        lines = ["astc bc7 prepare: 4 files (1 MB) in 1 packages, 2 threads, cache x", "astc bc7 prepare: progress 2/4",
                 "astc bc7 prepare: progress 4/4", DONE]

    class Proc:
        _handle = 0

        def __init__(self, cmd, cwd=None, env=None, **kw):
            calls.append(env)
            self.killed = False
            if not hang:
                with open(env["NX_LOG"], "w", encoding="utf-8") as f:
                    f.write("\n".join(lines) + "\n")
                cache = os.path.join(env["NX_DATA_ROOT"], ps.TEXTURES_CACHE)
                os.makedirs(cache, exist_ok=True)
                with open(os.path.join(cache, "k.bc7"), "wb") as f:
                    f.write(b"RFLBC7" + bytes(100))

        def poll(self):
            return None if hang and not self.killed else (-9 if self.killed else code)

        def wait(self):
            return -9 if self.killed else code

        def kill(self):
            self.killed = True

    monkeypatch.setattr(ps.subprocess, "Popen", Proc)
    monkeypatch.setattr(ps, "process_usage", lambda h: (1.5, 64 << 20))
    return calls


def state(ws):
    with open(os.path.join(ws, "work", ps.TEXTURES_STATE), encoding="utf-8") as f:
        return json.load(f)


def test_not_a_default_step():
    assert "textures" not in ps.STEPS and "textures" in ps.OPTIONAL_STEPS


def test_ok(ws, monkeypatch, capsys):
    calls = fake_runtime(monkeypatch)
    ps.step_textures(args(), ws)
    st = state(ws)
    assert st["status"] == "ok" and "reason" not in st
    assert (st["converted"], st["accepted"], st["kept"], st["already"]) == (3, 2, 1, 4)
    assert st["cache_files"] == 1 and st["cpu_seconds"] == 1.5 and st["peak_memory_mb"] == 64
    env = calls[0]
    assert env["NX_ASTC_BC7_PREPARE"] == "1" and env["NX_ASTC_BC7_PREPARE_THREADS"] == "2"
    assert env["NX_DATA_ROOT"] == os.path.join(ws, "data") and env["NX_HEADLESS"] == "1"
    out = capsys.readouterr().out
    assert "RESULT textures ok converted=3 accepted=2 kept=1" in out
    assert "PROGRESS 50" in out and "COUNT 4/4 files" in out


def test_measurement_switches_do_not_leak(ws, monkeypatch):
    monkeypatch.setenv("NX_ASTC_BC7_DB", "40")
    monkeypatch.setenv("NX_ASTC_BC7_CACHE", "elsewhere")
    calls = fake_runtime(monkeypatch)
    ps.step_textures(args(), ws)
    assert "NX_ASTC_BC7_DB" not in calls[0] and "NX_ASTC_BC7_CACHE" not in calls[0]


def test_partial_when_verdicts_not_written(ws, monkeypatch, capsys):
    fake_runtime(monkeypatch, code=5)
    ps.step_textures(args(), ws)
    assert state(ws)["status"] == "partial"
    assert "WARN texture preparation partial" in capsys.readouterr().out


def test_failed_without_the_end_line(ws, monkeypatch):
    fake_runtime(monkeypatch, lines=["astc bc7 prepare: 4 files (1 MB) in 1 packages, 2 threads, cache x"])
    ps.step_textures(args(), ws)
    assert state(ws)["status"] == "failed"


def test_old_executable_is_skipped(ws, monkeypatch):
    with open(os.path.join(ws, "bin", "wonder2.exe"), "wb") as f:
        f.write(b"MZ an older build")
    calls = fake_runtime(monkeypatch)
    ps.step_textures(args(), ws)
    assert state(ws)["status"] == "skipped" and not calls


def test_no_answer_is_stopped(ws, monkeypatch):
    fake_runtime(monkeypatch, hang=True)
    monkeypatch.setattr(ps, "TEXTURES_START_LIMIT", 0)
    monkeypatch.setattr(ps.time if hasattr(ps, "time") else __import__("time"), "sleep", lambda s: None)
    ps.step_textures(args(), ws)
    st = state(ws)
    assert st["status"] == "failed" and "no answer" in st["reason"]


def test_missing_executable_needs_the_user(ws):
    os.remove(os.path.join(ws, "bin", "wonder2.exe"))
    with pytest.raises(ps.NeedsUser):
        ps.step_textures(args(), ws)
