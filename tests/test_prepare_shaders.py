"""tools/prepare_shaders.py without game files: group names, block assignment, fingerprints, sample lists."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import prepare_shaders as ps  # noqa: E402


def test_group_names():
    assert ps.group_name("romfs/Effect/static.Nin_Ounce_NVN2.esetb.byml.zs") == "effect_static"
    assert ps.group_name("romfs/Layout/AccountSelect01.Nin_Ounce_NVN2.blarc.zs") == "layout_AccountSelect01"
    assert ps.group_name("romfs/Lib/agl/agl_resource.Nin_Ounce_NVN2.release.sarc.zs") == "lib_agl_resource"
    assert (ps.group_name("romfs/Shader/SecredUber.Product.110.product.Nin_Ounce_NVN2.bfsha.zs")
            == "shader_SecredUber.Product.110.product")
    assert ps.group_name("extra/main.bin") == "exefs_main"


class _P(ps.Pipeline):
    def __init__(self, sample=None):  # no workspace: only assign() is used
        self.sample = sample


def test_each_block_goes_to_the_first_group_in_harness_order():
    groups = {"shader_x": {"blocks": {"a": "pixel", "b": "vertex"}},
              "effect_y": {"blocks": {"b": "vertex", "c": "pixel"}},
              "layout_z": {"blocks": {"a": "pixel"}}}
    plan = _P().assign(groups)
    # ordinal order effect_ < layout_ < shader_: shader_x adds nothing new
    assert plan == {"effect_y": ["b", "c"], "layout_z": ["a"]}


def test_sample_limits_the_plan():
    groups = {"effect_y": {"blocks": {"b": "vertex", "c": "pixel"}}, "shader_x": {"blocks": {"a": "pixel"}}}
    assert _P({"a"}).assign(groups) == {"shader_x": ["a"]}


def test_digest_is_order_independent_for_mappings_only():
    assert ps.digest({"a": 1, "b": 2}) == ps.digest({"b": 2, "a": 1})
    assert ps.digest(["a", "b"]) != ps.digest(["b", "a"])


def test_read_sample(tmp_path):
    p = tmp_path / "s.txt"
    p.write_text("# comment\n0123456789abcdef  # why\n\nfedcba9876543210\n")
    assert ps.read_sample(str(p)) == {"0123456789abcdef", "fedcba9876543210"}


def test_components_are_checked_file_by_file(tmp_path):
    import hashlib
    import json
    import pytest
    d = tmp_path / "spirv-val"
    d.mkdir()
    (d / "spirv-val.exe").write_bytes(b"tool")
    man = {"format": 1, "components": {"spirv-val": {"files": {
        "spirv-val.exe": [hashlib.sha256(b"tool").hexdigest(), 4]}}}}
    (tmp_path / "components.json").write_text(json.dumps(man))
    assert ps.load_components(str(tmp_path), ["spirv-val"]) == {"spirv-val": str(d)}
    (d / "spirv-val.exe").write_bytes(b"toox")  # same size, other content
    with pytest.raises(ps.NeedsUser):
        ps.load_components(str(tmp_path), ["spirv-val"])
    with pytest.raises(ps.NeedsUser):
        ps.load_components(str(tmp_path), ["spirv_harness"])
