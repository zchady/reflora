"""tools/maxwell/shader_sources.py without game files: the raw match of captured code blocks."""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools", "maxwell"))
import shader_sources as ss  # noqa: E402


def block(seed, size=0x200):
    head = struct.pack("<I", 0x12345678) + bytes(0x2C)
    body = bytes((seed * 31 + k) & 0xFF for k in range(size - len(head)))
    return head + body


def targets_of(*blocks):
    t, by_key = {}, {}
    for b in blocks:
        h = hashlib.sha1(b).hexdigest()[:16]
        t[h] = len(b)
        by_key.setdefault(b[:ss.KEY], set()).add(h)
    return t, by_key


def test_raw_match_finds_block_at_any_offset():
    a, b = block(1), block(2)
    data = b"\0" * 0x123 + a + b"junk" * 10 + b
    t, by_key = targets_of(a, b)
    stages, kinds, methods, hits = ss.scan_data(data, "synthetic", t, by_key)
    assert not stages and not kinds
    assert sorted(hits) == sorted([(hashlib.sha1(a).hexdigest()[:16], 0x123),
                                   (hashlib.sha1(b).hexdigest()[:16], 0x123 + len(a) + 40)])


def test_raw_match_needs_the_whole_block():
    a = block(3)
    changed = a[:-1] + bytes([a[-1] ^ 1])  # same key bytes, different tail
    t, by_key = targets_of(a)
    _, _, _, hits = ss.scan_data(b"\0" * 16 + changed, "synthetic", t, by_key)
    assert hits == []
