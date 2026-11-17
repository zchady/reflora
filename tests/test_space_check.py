"""tools/space_check.py: the free-space reserve and the folder budget."""
import os
import shutil
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import space_check as sc  # noqa: E402


def test_reserve_and_budget(tmp_path):
    free = shutil.disk_usage(tmp_path).free / sc.GB
    assert sc.check(str(tmp_path), 0.0, reserve_gb=0.0)[0]
    assert not sc.check(str(tmp_path), free + 1.0, reserve_gb=0.0)[0]  # more than the drive has
    assert not sc.check(str(tmp_path / "not" / "yet"), 0.0, reserve_gb=free + 1.0)[0]  # a destination not made yet
    d = tmp_path / "work"
    d.mkdir()
    (d / "a.bin").write_bytes(b"\0" * 4096)
    assert sc.folder_bytes(str(d)) == 4096
    assert sc.check(str(d), 0.0, 0.0, str(d), budget_gb=1.0)[0]
    ok, lines = sc.check(str(d), 1.0, 0.0, str(d), budget_gb=1.0)  # 4 KB used + 1 GB > 1 GB
    assert not ok and "REFUSED" in lines[-1]
    assert sc.main(["--path", str(d), "--need-gb", "0", "--reserve-gb", "0"]) == 0
