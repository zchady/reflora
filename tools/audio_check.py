"""Automatic checks of a captured game mix (NX_AUDIO_DUMP=file.wav, 48 kHz stereo s16, hle_audio.cpp).

usage: audio_check.py capture.wav [--block 240]

Level (peak, RMS per 100 ms, DC offset, channel balance), clipping (samples at full scale), gaps (runs of exact
silence of 10 ms or more between sound), clicks (sample-to-sample jumps far above the local level) and the 5 ms block
seams: the runtime hands the device one 240-sample block per audio frame, so a lost, repeated or misordered block
shows as a jump at a block boundary. The mean jump at the boundaries is compared with the mean jump inside the blocks:
about 1.0 means the seams are inaudible. Spectral flatness (geometric over arithmetic mean of the power spectrum,
100 Hz - 8 kHz, on 16 windows of 4096 samples) tells music from noise: white noise is about 0.56, music far lower.
"""
import argparse
import cmath
import math
import struct
import sys


def read_wav(path):
    b = open(path, "rb").read()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        sys.exit("not a WAV file")
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        cid, size = b[pos:pos + 4], struct.unpack_from("<I", b, pos + 4)[0]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", b, pos + 8)
        elif cid == b"data":
            data = b[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    tag, ch, rate, _, _, bits = fmt
    if tag != 1 or bits != 16:
        sys.exit("only PCM s16")
    n = len(data) // 2
    s = struct.unpack(f"<{n}h", data[:n * 2])
    return rate, ch, [s[c::ch] for c in range(ch)]


def fft(a):
    n = len(a)
    if n == 1:
        return a
    even, odd = fft(a[0::2]), fft(a[1::2])
    out = [0j] * n
    for k in range(n // 2):
        t = cmath.exp(-2j * math.pi * k / n) * odd[k]
        out[k], out[k + n // 2] = even[k] + t, even[k] - t
    return out


def spectrum_check(mono, rate):
    n = 4096
    starts = [int(k * (len(mono) - n) / 15) for k in range(16)] if len(mono) > n else []
    flat, peaks = [], {}
    hann = [0.5 - 0.5 * math.cos(2 * math.pi * k / (n - 1)) for k in range(n)]
    for s0 in starts:
        w = [mono[s0 + k] * hann[k] for k in range(n)]
        if max(abs(v) for v in w) < 50:
            continue  # near silence
        spec = fft([complex(v) for v in w])
        lo, hi = int(100 * n / rate), int(8000 * n / rate)
        pw = [abs(spec[k]) ** 2 + 1e-9 for k in range(lo, hi)]
        flat.append(math.exp(sum(math.log(v) for v in pw) / len(pw)) / (sum(pw) / len(pw)))
        k = max(range(len(pw)), key=lambda i: pw[i])
        f = round((lo + k) * rate / n)
        peaks[f] = peaks.get(f, 0) + 1
    if flat:
        print(f"  spectral flatness over {len(flat)} windows: median {sorted(flat)[len(flat) // 2]:.3f}, "
              f"max {max(flat):.3f} (white noise ~0.56); strongest frequencies (Hz): "
              + ", ".join(f"{f}" for f, _ in sorted(peaks.items(), key=lambda kv: -kv[1])[:8]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--block", type=int, default=240)
    a = ap.parse_args()
    rate, ch, chans = read_wav(a.wav)
    n = len(chans[0])
    print(f"{a.wav}: {rate} Hz, {ch} channels, {n} frames = {n / rate:.3f} s")
    for c, x in enumerate(chans):
        peak = max(abs(v) for v in x)
        rms = math.sqrt(sum(v * v for v in x) / n)
        dc = sum(x) / n
        clip = sum(1 for v in x if v >= 32767 or v <= -32768)
        print(f"  channel {c}: peak {peak} ({20 * math.log10(max(peak, 1) / 32768):.1f} dBFS), "
              f"RMS {rms:.0f} ({20 * math.log10(max(rms, 1) / 32768):.1f} dBFS), DC {dc:.1f}, clipped samples {clip}")
    mono = [sum(ch_[k] for ch_ in chans) / len(chans) for k in range(n)]
    # loudness every 100 ms
    w = rate // 10
    levels = [math.sqrt(sum(v * v for v in mono[k:k + w]) / w) for k in range(0, n - w + 1, w)]
    quiet = sum(1 for v in levels if v < 30)
    print(f"  RMS per 100 ms: min {min(levels):.0f}, median {sorted(levels)[len(levels) // 2]:.0f}, "
          f"max {max(levels):.0f}; {quiet} of {len(levels)} windows near silence (< 30)")
    # gaps: runs of exact zero on every channel, >= 10 ms, with sound before and after
    gaps, run, start, heard = [], 0, 0, False
    for k in range(n):
        zero = all(ch_[k] == 0 for ch_ in chans)
        if zero:
            if run == 0:
                start = k
            run += 1
        else:
            if run >= rate // 100 and heard:
                gaps.append((start / rate, run / rate * 1000))
            run, heard = 0, True
    print(f"  gaps of exact silence >= 10 ms inside the sound: {len(gaps)}"
          + (": " + ", ".join(f"{t:.2f} s ({d:.0f} ms)" for t, d in gaps[:8]) if gaps else ""))
    spectrum_check(mono, rate)
    # clicks and block seams, on each channel
    for c, x in enumerate(chans):
        d = [abs(x[k] - x[k - 1]) for k in range(1, n)]
        seam = [d[k - 1] for k in range(a.block, n, a.block)]
        inner = [d[k - 1] for k in range(1, n) if k % a.block]
        ms, mi = sum(seam) / max(1, len(seam)), sum(inner) / max(1, len(inner))
        # a click: a jump over 8x the mean jump of the 5 ms around it, and over 2000 (about -24 dBFS)
        clicks = 0
        for k in range(a.block, n - a.block, a.block):
            loc = d[k - a.block:k + a.block]
            m = sum(loc) / len(loc)
            clicks += sum(1 for v in loc[a.block // 2:a.block + a.block // 2] if v > 2000 and v > 8 * m) > 0
        print(f"  channel {c}: mean |jump| at 5 ms seams {ms:.1f}, inside blocks {mi:.1f}, ratio {ms / max(mi, 1e-9):.2f}; "
              f"blocks with a click {clicks} of {n // a.block}")


if __name__ == "__main__":
    main()
