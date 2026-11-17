"""The block-height rule of runtime/src/nvn/nvn_upload.cpp for packaged (BNTX) textures, checked on the game's own
files when they have been extracted under private/ (skipped otherwise; no game data in the repository):
start from nvn_objects.cpp's guess (smallest power of two >= rows * 3 / 32, at most 16 GOBs) and halve while the
mip chain does not fit the texture's data (its image size, 0x200-aligned: the gap to the next texture). It must give
the file's own block height. Wonder's Switch 2 ASTC 6x5 textures of 256 px and more are the cases where halving
happens (1 GOB where the guess is 8 or 16)."""
import os
import struct
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "tools", "maxwell"))
import bntx  # noqa: E402

DIRS = [os.path.join(REPO, "private", "wonder2", d) for d in ("romfs_model", "romfs_layout")]


def bhg(rows):
    g, h = bntx.div_up(rows, 8), 1
    while h < g and h < 32:
        h <<= 1
    return h


def chain(t, bh0):  # nvn_tiling.cpp block_linear_layout().tiled_size for one layer
    total = 0
    for level in range(t.levels):
        w, h = max(1, t.width >> level), max(1, t.height >> level)
        cols, rows = bntx.div_up(w, t.block_w), bntx.div_up(h, t.block_h)
        g = min(bhg(rows), bh0)
        total += bntx.div_up(cols * t.bpb, 64) * bntx.div_up(rows, 8 * g) * 512 * g
    return total


def guess(rows):
    h = 1
    while h * 32 < rows * 3 and h < 16:
        h <<= 1
    return h


@pytest.mark.skipif(not any(os.path.isdir(d) for d in DIRS), reason="needs extracted Switch 2 BNTX data")
def test_halving_to_fit_gives_the_file_block_height():
    checked = halved = 0
    for d in DIRS:
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            for t in bntx.find_textures(os.path.join(d, f)):
                if not t.bpb or t.tile_mode or t.layers * t.depth != 1:
                    continue
                b = first = guess(bntx.div_up(t.height, t.block_h))
                gap = bntx.div_up(t.image_size, 0x200) * 0x200
                while b > 1 and chain(t, b) > gap:
                    b >>= 1
                assert b == 1 << t.block_height_log2, (f, t.width, t.height, t.levels, t.block_w, t.block_h)
                checked += 1
                halved += b != first
    assert checked > 0


NUP = os.path.join(os.path.dirname(REPO), "GameFiles", "Wonder.nup")


@pytest.mark.skipif(not os.path.isfile(NUP), reason="needs the Switch 2 package (Wonder.nup)")
def test_switch2_arrays_use_one_gob():
    """runtime/src/nvn/nvn_objects.cpp gives packaged 2D arrays 1 GOB under the NVN 2 driver: every array texture of
    the course models' BNTX files says so, and each layer's mip chain is 512-byte aligned (the layer stride is the
    image size over the layer count). Read-only, straight from the package (files named Unit*: the terrain sets)."""
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import nup
    import zstandard

    n = nup.Nup(NUP)
    model = n.subtree(n.packages[0], "romfs/Model")
    zd = zstandard.ZstdDecompressor()
    arrays = 0
    for path, fn in sorted(n.files_of(model).items()):
        if not path.startswith("Unit") or not path.endswith(".zs"):
            continue
        data = zd.decompress(n.read(fn, limit=fn.size), max_output_size=1 << 31)
        pos = data.find(b"BNTX\0\0\0\0")
        while pos >= 0:
            try:
                textures = bntx.parse_bntx(data, pos)
            except (ValueError, struct.error, IndexError):
                textures = []
            for t in textures:
                if not t.bpb or t.tile_mode or t.layers <= 1:
                    continue
                assert t.block_height_log2 == 0, (path, t.name)
                stride = bntx.div_up(chain(t, 1), 0x200) * 0x200
                assert t.image_size == stride * t.layers, (path, t.name, hex(t.image_size), hex(stride))
                arrays += 1
            pos = data.find(b"BNTX\0\0\0\0", pos + 8)
        if arrays >= 200:
            break
    assert arrays > 0
