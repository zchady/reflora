"""tools/prepare_build.py without game files or downloads: import allowlist, job limit, fingerprints, pins."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import prepare_build as pb  # noqa: E402


def test_imports_of_the_tested_executable_are_allowed():
    # the import list of the llvm-mingw wonder2.exe checked in WONDER_SWITCH2_PROGRESS.md §5za4
    dlls = ["GDI32.dll", "KERNEL32.dll", "SDL3.dll", "SHELL32.dll", "USER32.dll", "XAudio2_9.dll", "XINPUT1_4.dll",
            "api-ms-win-crt-heap-l1-1-0.dll", "api-ms-win-crt-runtime-l1-1-0.dll", "dwmapi.dll", "ole32.dll"]
    assert pb.disallowed_imports(dlls) == []


def test_development_and_redistributable_dlls_are_refused():
    for d in ("libiconv-2.dll", "libc++.dll", "libunwind.dll", "libwinpthread-1.dll", "VCRUNTIME140.dll",
              "MSVCP140.dll", "msys-2.0.dll", "xinput1_3.dll"):
        assert pb.disallowed_imports(["KERNEL32.dll", d]) == [d]


def test_jobs_stay_within_cpu_memory_and_eight(monkeypatch):
    monkeypatch.setattr(pb, "memory_gb", lambda: (32.0, 14.0))
    monkeypatch.setattr(pb.os, "cpu_count", lambda: 12)
    assert pb.safe_jobs(0) == 6          # (14 - 6) / 1.2 = 6.6 -> 6
    monkeypatch.setattr(pb, "memory_gb", lambda: (64.0, 50.0))
    assert pb.safe_jobs(0) == 8          # capped at 8 by default
    assert pb.safe_jobs(16) == 10        # asked for more: still at most cores - 2
    monkeypatch.setattr(pb, "memory_gb", lambda: (8.0, 5.0))
    assert pb.safe_jobs(0) == 1          # little memory: one at a time


def test_digest_and_tree_digest(tmp_path):
    assert pb.digest({"a": "1", "b": "2"}) == pb.digest({"b": "2", "a": "1"})
    (tmp_path / "x.py").write_text("a")
    (tmp_path / "sub").mkdir()
    (tmp_path / "sub" / "y.txt").write_text("b")
    d1, n1 = pb.tree_digest(str(tmp_path))
    d2, n2 = pb.tree_digest(str(tmp_path), (".py",))
    assert (n1, n2) == (2, 1) and d1 != d2
    (tmp_path / "sub" / "y.txt").write_text("c")
    assert pb.tree_digest(str(tmp_path))[0] != d1


def test_a_build_directory_made_for_other_sources_is_detected(tmp_path):
    # CMake refuses a cache configured for another source folder (REFLORA moved or updated in a new folder)
    assert pb.cmake_cache_source(str(tmp_path)) is None
    assert pb.same_source(None, r"C:\Reflora")
    (tmp_path / "CMakeCache.txt").write_text(
        "# cache\nCMAKE_HOME_DIRECTORY:INTERNAL=C:/Games/old/reflora\nOTHER:STRING=x\n")
    cached = pb.cmake_cache_source(str(tmp_path))
    assert cached == "C:/Games/old/reflora"
    assert pb.same_source(cached, r"C:\Games\OLD\reflora")  # same folder, other spelling
    assert not pb.same_source(cached, r"C:\Games\reflora")


def test_every_tool_is_pinned():
    for name, t in pb.TOOLS.items():
        assert t["url"].startswith("https://github.com/"), name
        assert len(t["sha256"]) == 64 and t["probe"], name
