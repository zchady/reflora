"""tools/prepare_switch2.py preflight (AVX2 and disk) without game files."""
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import prepare_switch2 as ps  # noqa: E402


def test_avx2_missing_needs_the_user(tmp_path):
    with pytest.raises(ps.NeedsUser, match="AVX2"):
        ps.preflight(str(tmp_path), avx2=False, free_gb=100, windows_build=22631)


def test_avx2_unknown_on_old_windows_only_warns(tmp_path, capsys):
    ps.preflight(str(tmp_path), avx2=None, free_gb=100, windows_build=18363)
    assert capsys.readouterr().out.startswith("WARN AVX2 cannot be confirmed")


def test_disk_new_workspace_needs_the_whole_budget(tmp_path):
    with pytest.raises(ps.NeedsUser, match="more are needed"):
        ps.preflight(str(tmp_path), avx2=True, free_gb=3.0, used_gb=0.0)
    ps.preflight(str(tmp_path), avx2=True, free_gb=ps.WORKSPACE_GB, used_gb=0.0)


def test_disk_resumed_workspace_needs_only_the_rest(tmp_path):
    ps.preflight(str(tmp_path), avx2=True, free_gb=1.0, used_gb=4.8)  # most of it already written
    with pytest.raises(ps.NeedsUser):
        ps.preflight(str(tmp_path), avx2=True, free_gb=1.0, used_gb=2.0)


def test_allocated_size_rounds_to_clusters(tmp_path):
    (tmp_path / "a").write_bytes(b"x")
    (tmp_path / "sub").mkdir()
    (tmp_path / "sub" / "b").write_bytes(b"y" * 4097)
    assert ps.allocated_gb(str(tmp_path)) == pytest.approx(3 * 4096 / 2**30)


def _fake_run(monkeypatch, tmp_path, steps=None, fail=None):
    nup = tmp_path / "Wonder.nup"
    nup.write_bytes(b"x")
    argv = ["prepare_switch2.py", "--nup", str(nup), "--workspace", str(tmp_path / "ws")]
    if steps:
        argv += ["--steps", steps]
    monkeypatch.setattr(sys, "argv", argv)
    monkeypatch.setattr(ps, "done", lambda *a: False)
    for s in ps.STEPS:
        def step(a, ws, s=s):
            if s == fail:
                raise RuntimeError("boom")
        monkeypatch.setattr(ps, "step_" + s, step)
    return ps.main(), tmp_path / "ws" / "work" / ps.PREPARED_MARKER


def test_marker_only_after_a_whole_preparation_reaches_done(monkeypatch, tmp_path, capsys):
    code, marker = _fake_run(monkeypatch, tmp_path)
    out = capsys.readouterr().out.splitlines()
    assert code == 0 and marker.is_file() and out[-1] == "DONE"
    # PROGRESS is progress inside a step: each step ends with 100, never an overall value
    assert [l for l in out if l.startswith("PROGRESS")] == ["PROGRESS 100"] * len(ps.STEPS)
    code, marker = _fake_run(monkeypatch, tmp_path, fail="check")  # a new run removes it; failing leaves none
    assert code == 1 and not marker.exists()
    code, marker = _fake_run(monkeypatch, tmp_path, steps="verify,config")  # part of the steps: no marker
    assert code == 0 and not marker.exists()


def test_a_failure_after_check_keeps_the_installation_ready(monkeypatch, tmp_path):
    # the graphics step never raises (it reports), but whatever stops a whole run after check leaves it playable
    code, marker = _fake_run(monkeypatch, tmp_path, fail="graphics")
    assert code == 1 and marker.is_file()
    code, marker = _fake_run(monkeypatch, tmp_path, fail="build")
    assert code == 1 and not marker.exists()


def test_this_machine_passes_the_cpu_check(tmp_path):
    if sys.platform != "win32" or sys.getwindowsversion().build < 19041:
        pytest.skip("needs Windows 10 2004 or later")
    ps.preflight(str(tmp_path), free_gb=100)  # the test machine has AVX2
