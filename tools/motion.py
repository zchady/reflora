"""Motion as the screen shows it, from tools/motion_capture.ps1 (grey frames of a band of the game window, one per
desktop update, with their times).

usage: motion.py <capture dir> [--from S] [--to S] [--list N]

For each pair of consecutive captured frames: the time between them and the image's shift (phase correlation over
the band, sub-pixel, in captured pixels). While the player runs at a constant speed the camera scrolls at a constant
speed, so the shift per 1/60 s is constant; a stutter is a frame that repeats (shift 0, then a double step), a frame
held for 2 updates, or a shift that does not match the time it was on screen. Prints the update intervals, the
speed per update (pixels per 1/60 s) and its spread, and the irregular updates.
"""
import argparse
import math
import os
import re
import statistics

import numpy as np


def load(d):
    raw_info = open(os.path.join(d, "capture.txt"), "rb").read()
    bom = raw_info[:2] in (bytes([0xFF, 0xFE]), bytes([0xFE, 0xFF]))
    info = raw_info.decode("utf-16") if bom else raw_info.decode("utf-8", "replace")
    if "size" not in info:  # the band's output size from the first line
        m = re.search(r"-> (\d+)x(\d+)", info)
        info += f" size {m.group(1)}x{m.group(2)}"
    w, h = map(int, re.search(r"size (\d+)x(\d+)", info).group(1, 2))
    times = [float(m) for m in re.findall(r"pts_time:([0-9.]+)", open(os.path.join(d, "ffmpeg.log"), errors="replace").read())]
    raw = np.fromfile(os.path.join(d, "frames.gray"), dtype=np.uint8)
    n = min(len(times), raw.size // (w * h))
    return np.asarray(times[:n]), raw[: n * w * h].reshape(n, h, w).astype(np.float32)


def shift(a, b, win):
    """(dx, dy, peak) such that b is a moved by (dx, dy)."""
    fa, fb = np.fft.rfft2((a - a.mean()) * win), np.fft.rfft2((b - b.mean()) * win)
    r = fb * np.conj(fa)
    r /= np.abs(r) + 1e-6
    c = np.fft.irfft2(r, s=a.shape)
    h, w = c.shape
    y, x = np.unravel_index(np.argmax(c), c.shape)

    def sub(cm, c0, cp):
        d = cm - 2 * c0 + cp
        return 0.0 if d == 0 else 0.5 * (cm - cp) / d

    dx = x + sub(c[y, (x - 1) % w], c[y, x], c[y, (x + 1) % w])
    dy = y + sub(c[(y - 1) % h, x], c[y, x], c[(y + 1) % h, x])
    if dx > w / 2:
        dx -= w
    if dy > h / 2:
        dy -= h
    return dx, dy, float(c[y, x])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--from", dest="t0", type=float, default=0)
    ap.add_argument("--to", dest="t1", type=float, default=1e9)
    ap.add_argument("--list", type=int, default=40)
    a = ap.parse_args()
    t, f = load(a.dir)
    keep = (t >= t[0] + a.t0) & (t <= t[0] + a.t1)
    t, f = t[keep], f[keep]
    h, w = f.shape[1:]
    win = np.outer(np.hanning(h), np.hanning(w)).astype(np.float32)
    rows = []
    for k in range(1, len(f)):
        same = float(np.mean(np.abs(f[k] - f[k - 1])))
        dx, dy, pk = shift(f[k - 1], f[k], win)
        rows.append((t[k] - t[0], (t[k] - t[k - 1]) * 1000, dx, dy, pk, same))
    print(f"{len(f)} frames of {w}x{h} over {t[-1] - t[0]:.2f} s ({(len(f) - 1) / (t[-1] - t[0]):.1f} updates/s)")
    dts = [r[1] for r in rows]
    print("update interval ms: p1 %.2f p50 %.2f p99 %.2f; histogram (1/60 s units): %s" % (
        np.percentile(dts, 1), np.percentile(dts, 50), np.percentile(dts, 99),
        dict(sorted(collections_counter(round(d / 16.667, 1) for d in dts).items()))))
    # Speed per 1/60 s where the image moves (running), and the updates that break the pattern.
    moving = [r for r in rows if abs(r[2]) > 0.3 and r[4] > 0.05]
    if not moving:
        print("no horizontal motion found")
        return
    v = [r[2] / (r[1] / 16.667) for r in moving]
    med = statistics.median(v)
    print(f"horizontal speed (px per 1/60 s, where moving): median {med:.2f}, p10 {np.percentile(v, 10):.2f}, "
          f"p90 {np.percentile(v, 90):.2f}, sd {statistics.pstdev(v):.2f}")
    # Irregular: an update that took 2+ frame times, a repeated frame (no change) between moving ones, or a shift
    # that is off the local median speed (per 1/60 s) by more than 35%.
    irregular = []
    for k, r in enumerate(rows):
        lo, hi = max(0, k - 15), min(len(rows), k + 16)
        loc = [x[2] / (x[1] / 16.667) for x in rows[lo:hi] if abs(x[2]) > 0.3 and x[4] > 0.05]
        if len(loc) < 8:
            continue
        lm = statistics.median(loc)
        if abs(lm) < 1.0:
            continue
        frames = r[1] / 16.667
        expect = lm * frames
        kind = None
        if r[5] < 0.5:
            kind = "repeat"
        elif frames > 1.5:
            kind = f"held {frames:.1f}"
        elif abs(r[2] - lm) > 0.35 * abs(lm):
            kind = f"step {r[2] / lm:.2f}x"
        if kind:
            irregular.append((r, kind, expect))
    secs = rows[-1][0] - rows[0][0] if rows else 1
    print(f"irregular updates while moving: {len(irregular)} ({len(irregular) / secs:.2f}/s)")
    kinds = collections_counter(k.split()[0] for _, k, _ in irregular)
    print("  by kind:", dict(kinds))
    for r, kind, expect in irregular[: a.list]:
        print(f"  t {r[0]:7.3f}  dt {r[1]:6.2f} ms  dx {r[2]:7.2f} (local {expect:6.2f})  dy {r[3]:6.2f}  {kind}")


def collections_counter(it):
    out = {}
    for x in it:
        out[x] = out.get(x, 0) + 1
    return out


if __name__ == "__main__":
    main()
