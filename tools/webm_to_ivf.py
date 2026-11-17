#!/usr/bin/env python3
"""WebM (Matroska) -> IVF: the compressed frames of one video track, for tests of the VP9 decoder
(runtime/src/movie_vpx.cpp, tests/movie_test.cpp) on the game's own movies.

Usage: python tools/webm_to_ivf.py <in.webm> <out.ivf> [--frames N]
Prints the tracks (number, codec, size) and how many frames were written. Input and output are game data: keep
them under private/.

Only what the game's files use is read: EBML elements with known sizes (unknown-size Segment/Cluster are walked as
containers), SimpleBlock and BlockGroup/Block without lacing.
"""
import struct
import sys

SEGMENT, CLUSTER, TRACKS, TRACK_ENTRY = 0x18538067, 0x1F43B675, 0x1654AE6B, 0xAE
TRACK_NUMBER, CODEC_ID, VIDEO, PIXEL_W, PIXEL_H = 0xD7, 0x86, 0xE0, 0xB0, 0xBA
TIMECODE, SIMPLE_BLOCK, BLOCK_GROUP, BLOCK = 0xE7, 0xA3, 0xA0, 0xA1
CONTAINERS = {SEGMENT, CLUSTER, TRACKS, TRACK_ENTRY, VIDEO, BLOCK_GROUP}


def vint(d, i, keep_marker):
    b = d[i]
    n = 1
    while n <= 8 and not b & (0x80 >> (n - 1)):
        n += 1
    v = b if keep_marker else b & ((0x80 >> (n - 1)) - 1)
    for k in range(1, n):
        v = (v << 8) | d[i + k]
    unknown = not keep_marker and v == (1 << (7 * n)) - 1
    return v, i + n, unknown


def walk(d, start, end, out):
    i = start
    while i < end:
        eid, i, _ = vint(d, i, True)
        size, i, unknown = vint(d, i, False)
        stop = end if unknown else min(end, i + size)
        if eid in CONTAINERS:
            out.append((eid, i, stop, None))
            walk(d, i, stop, out)
        else:
            out.append((eid, i, stop, d[i:stop]))
        i = stop


def uint(b):
    return int.from_bytes(b, "big")


def main(argv):
    src, dst = argv[0], argv[1]
    limit = int(argv[argv.index("--frames") + 1]) if "--frames" in argv else 0
    d = open(src, "rb").read()
    els = []
    walk(d, 0, len(d), els)
    tracks, cur = {}, None
    for eid, _, _, data in els:
        if eid == TRACK_ENTRY:
            cur = {}
        elif cur is not None and eid == TRACK_NUMBER:
            cur["n"] = uint(data)
            tracks[cur["n"]] = cur
        elif cur is not None and eid == CODEC_ID:
            cur["codec"] = data.decode()
        elif cur is not None and eid == PIXEL_W:
            cur["w"] = uint(data)
        elif cur is not None and eid == PIXEL_H:
            cur["h"] = uint(data)
    for t in tracks.values():
        print(f"track {t.get('n')}: {t.get('codec')} {t.get('w', '')}x{t.get('h', '')}")
    video = next(t for t in tracks.values() if t.get("codec", "").startswith("V_VP"))
    frames, cluster_tc = [], 0
    for eid, _, _, data in els:
        if eid == TIMECODE:
            cluster_tc = uint(data)
        elif eid in (SIMPLE_BLOCK, BLOCK):
            tn, j, _ = vint(data, 0, False)
            if tn != video["n"]:
                continue
            rel = struct.unpack(">h", data[j:j + 2])[0]
            flags = data[j + 2]
            if flags & 0x06:
                raise SystemExit("laced video blocks are not supported")
            frames.append((cluster_tc + rel, data[j + 3:]))
    if limit:
        frames = frames[:limit]
    fourcc = b"VP90" if video["codec"] == "V_VP9" else b"VP80"
    with open(dst, "wb") as f:
        f.write(b"DKIF" + struct.pack("<HH4sHHIIII", 0, 32, fourcc, video.get("w", 0), video.get("h", 0), 1000, 1,
                                      len(frames), 0))
        for pts, data in frames:
            f.write(struct.pack("<IQ", len(data), pts) + data)
    print(f"{len(frames)} frames -> {dst}")


if __name__ == "__main__":
    main(sys.argv[1:])
