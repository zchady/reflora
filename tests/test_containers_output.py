"""containers.check_output_dir: where extracted shaders may be written (no game files needed)."""
import os
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools", "maxwell"))
import containers as ct  # noqa: E402


def workspace(tmp_path, name="Reflora work space"):
    ws = tmp_path / name
    ws.mkdir()
    (ws / ct.WORKSPACE_MARK).write_text("test\n")
    return ws


def test_marked_workspace_with_spaces_is_accepted(tmp_path):
    ws = workspace(tmp_path)
    out = ws / "work" / "shaders" / "sh" / "effect static"
    assert ct.check_output_dir(str(out)) == os.path.realpath(str(out))


def test_unmarked_folder_outside_the_repository_is_refused(tmp_path):
    with pytest.raises(ct.OutputRefused):
        ct.check_output_dir(str(tmp_path / "games" / "sh"))


def test_repository_sources_are_refused_also_by_relative_path(tmp_path, monkeypatch):
    with pytest.raises(ct.OutputRefused):
        ct.check_output_dir(os.path.join(REPO, "tools", "out"))
    monkeypatch.chdir(os.path.join(REPO, "tools"))  # private/ may not exist in a fresh clone
    with pytest.raises(ct.OutputRefused):
        ct.check_output_dir(os.path.join("..", "runtime", "sh"))  # tools/../runtime
    with pytest.raises(ct.OutputRefused):
        ct.check_output_dir(os.path.join(REPO, "private"))       # private/ itself


def test_private_subfolder_is_accepted_without_writing():
    p = os.path.join(REPO, "private", "_never_created_by_this_test", "sh")
    assert ct.check_output_dir(p) == os.path.realpath(p)
    assert not os.path.exists(p)


def test_existing_folder_with_other_files_is_refused(tmp_path):
    ws = workspace(tmp_path)
    d = ws / "data"
    d.mkdir()
    (d / "Wonder.nup").write_bytes(b"x")
    with pytest.raises(ct.OutputRefused):
        ct.check_output_dir(str(d))
    e = ws / "sh"
    e.mkdir()
    (e / "manifest.tsv").write_text("id\n")
    (e / "code_0.bin").write_bytes(b"")
    assert ct.check_output_dir(str(e))  # an earlier extraction may be redone


def _link(link, target):
    """A directory symlink, or a junction on Windows when symlinks need a privilege."""
    try:
        os.symlink(target, link, target_is_directory=True)
    except OSError:
        if sys.platform != "win32":
            pytest.skip("cannot create a symbolic link here")
        import _winapi
        _winapi.CreateJunction(str(target), str(link))


def test_link_from_a_workspace_into_the_repository_is_refused(tmp_path):
    ws = workspace(tmp_path)
    _link(ws / "escape", os.path.join(REPO, "runtime"))
    with pytest.raises(ct.OutputRefused):
        ct.check_output_dir(str(ws / "escape" / "sh"))


def test_link_from_a_workspace_to_an_unmarked_folder_is_refused(tmp_path):
    ws = workspace(tmp_path)
    other = tmp_path / "games"
    other.mkdir()
    _link(ws / "games", other)
    with pytest.raises(ct.OutputRefused):
        ct.check_output_dir(str(ws / "games" / "sh"))
