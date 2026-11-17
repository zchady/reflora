"""Flicker while the camera moves: per tile of the frame, the brightness series' second difference
|x[t+1] - 2 x[t] + x[t-1]| (a camera pan changes a tile's brightness smoothly; a shadow that blinks from one frame
to the next makes spikes). Prints the tiles with the most spikes and writes a heat map over a frame of the stretch.

usage: flicker_walk.py <video> <start s> <seconds> <out.png> [--tile 32] [--width 960]
"""
import argparse
import subprocess
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from flicker import FFMPEG, frames  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("start", type=float)
    ap.add_argument("seconds", type=float)
    ap.add_argument("out")
    ap.add_argument("--tile", type=int, default=24)
    ap.add_argument("--width", type=int, default=960)
    a = ap.parse_args()
    f = frames(a.video, a.start, a.seconds, a.width).astype(np.float32)
    n, h, w = f.shape
    t = a.tile
    th, tw = h // t, w // t
    tiles = f[:, : th * t, : tw * t].reshape(n, th, t, tw, t).mean(axis=(2, 4))
    d2 = np.abs(tiles[2:] - 2 * tiles[1:-1] + tiles[:-2])
    spikes = (d2 > 6).mean(axis=0)  # share of frames with a jump of > 6 grey levels against the trend
    print(f"{n} frames, {th}x{tw} tiles of {t}px: mean second difference {d2.mean():.2f}; tiles spiking in >= 10% of "
          f"frames: {100 * (spikes >= 0.10).mean():.1f}%, >= 25%: {100 * (spikes >= 0.25).mean():.1f}%")
    heat = np.kron(np.clip(spikes * 4, 0, 1), np.ones((t, t)))
    base = f[n // 2, : th * t, : tw * t]
    img = np.stack([np.clip(base * 0.5 + heat * 255, 0, 255), base * 0.5, base * 0.5], axis=-1).astype(np.uint8)
    subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s",
                    f"{tw * t}x{th * t}", "-i", "-", "-y", a.out], input=img.tobytes(), check=True)


if __name__ == "__main__":
    main()
