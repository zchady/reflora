"""Where a recorded scene flickers: per pixel, how much it changes from one frame to the next while the camera holds
still (a shadow that blinks lights up; a static one stays dark). Reads a stretch of a video with ffmpeg.

usage: flicker.py <video> <start s> <seconds> <out.png> [--width 960]

Prints the mean change, the share of pixels that change by more than 24 grey levels in at least a quarter of the
frames (blinking), and writes a heat map (bright = changes often) next to the stretch's first frame.
"""
import argparse
import subprocess

import numpy as np

FFMPEG = r"C:\ffmpeg\bin\ffmpeg.exe"


def frames(video, start, seconds, width):
    probe = subprocess.run([FFMPEG, "-hide_banner", "-ss", str(start), "-i", video, "-frames:v", "1", "-vf",
                            f"scale={width}:-2", "-f", "rawvideo", "-pix_fmt", "gray", "-"], capture_output=True)
    first = probe.stdout
    raw = subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-ss", str(start), "-t", str(seconds), "-i",
                          video, "-vf", f"scale={width}:-2", "-f", "rawvideo", "-pix_fmt", "gray", "-"],
                         capture_output=True).stdout
    h = len(first) // width
    n = len(raw) // (width * h)
    return np.frombuffer(raw[: n * width * h], np.uint8).reshape(n, h, width).astype(np.int16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("start", type=float)
    ap.add_argument("seconds", type=float)
    ap.add_argument("out")
    ap.add_argument("--width", type=int, default=960)
    a = ap.parse_args()
    f = frames(a.video, a.start, a.seconds, a.width)
    d = np.abs(np.diff(f, axis=0))
    mean = d.mean(axis=0)
    blink = (d > 24).mean(axis=0)
    print(f"{len(f)} frames {f.shape[2]}x{f.shape[1]}: mean change {mean.mean():.2f} grey levels per frame; "
          f"pixels changing > 24 in >= 25% of frames: {100 * (blink >= 0.25).mean():.2f}%; in >= 10%: "
          f"{100 * (blink >= 0.10).mean():.2f}%")
    heat = np.clip(blink * 4 * 255, 0, 255).astype(np.uint8)
    img = np.concatenate([f[0].astype(np.uint8), heat], axis=1)
    h, w = img.shape
    subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "gray", "-s",
                    f"{w}x{h}", "-i", "-", "-y", a.out], input=img.tobytes(), check=True)


if __name__ == "__main__":
    main()
