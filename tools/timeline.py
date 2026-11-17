"""Critical path of each frame from a frame timeline (runtime/src/timeline.cpp, the `timeline` request file).

usage: timeline.py timeline.txt [--frames N] [--show K]

From each nvnQueuePresentTexture back to the previous one, it follows the chain of waits: on the presenting thread,
the time since its last blocking SVC returned is "run"; that SVC was ended by a signal from another thread (matched by
what it waited on: an address, a condition variable key, a mutex word, an event), so the chain moves to the signalling
thread at the moment of the signal, the gap being "wake latency"; a sleep, a timeout or the display vsync ends a wait
by itself, so the chain stays on the thread, before its wait. NVN calls inside a run segment (submit = our replay and
GPU wait, pace = the runtime's present pacing) are counted apart. The sum per frame is the frame's length.
"""
import argparse
import bisect
import collections
import re

BLOCK_BEGIN, BLOCK_END, SIGNAL, NVN, VSYNC = 1, 2, 3, 4, 5
SUBMIT_B, SUBMIT_E, PRESENT_B, PACE_B, PACE_E, PRESENT_E, ACQUIRE, SYNC_B, SYNC_E, FIN_B, FIN_E, FRAME_TIME = range(1, 13)
SVC_NAME = {0x0B: "sleep", 0x18: "wait-sync", 0x1A: "mutex", 0x1C: "condvar", 0x34: "wait-addr"}
# blocking SVC -> the SVCs that wake it
WAKERS = {0x18: (0x11,), 0x1A: (0x1B,), 0x1C: (0x1D,), 0x34: (0x35,)}


def short(desc):
    m = re.search(r"\((.*)\)", desc)
    s = m.group(1) if m else desc
    s = re.sub(r"\(.*", "", s)
    return s[:70]


def load(path):
    threads, syms, events = {}, {}, []
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.split(" ", 2)
        if p[0] == "T":
            rest = p[2].split(" ", 1)
            threads[int(p[1], 16)] = short(rest[1].strip() if len(rest) > 1 else "")
        elif p[0] == "S":
            syms[int(p[1], 16)] = short(p[2].strip())
        elif p[0] == "E":
            f = line.split()
            events.append((int(f[1]), int(f[2], 16), int(f[3]), int(f[4], 16), int(f[5], 16), int(f[6], 16),
                           tuple(int(x, 16) for x in f[7:11])))
    events.sort(key=lambda e: e[0])
    return threads, syms, events


def site(syms, lrs, depth=3):
    names = [syms.get(a, hex(a)) for a in lrs[:depth] if a]
    return " < ".join(names)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("timeline")
    ap.add_argument("--show", type=int, default=3, help="frames to print in full")
    args = ap.parse_args()
    threads, syms, events = load(args.timeline)

    # Blocking intervals per thread, and signals per (svc, object).
    blocks = collections.defaultdict(list)  # thread -> [(begin, end, svc, obj, lrs)]
    open_block = {}
    signals = collections.defaultdict(list)  # (svc, obj) -> [(t, thread, lrs)]
    vsyncs = []
    nvn = collections.defaultdict(list)  # thread -> [(begin, end, label)]
    open_nvn = {}
    presents = []
    frame_times = []
    for t, th, kind, code, a, b, lrs in events:
        if kind == BLOCK_BEGIN:
            open_block[th] = (t, code, a, lrs)
        elif kind == BLOCK_END and th in open_block:
            b0, c0, a0, l0 = open_block.pop(th)
            blocks[th].append((b0, t, c0, a0, l0, b))
        elif kind == SIGNAL:
            signals[(code, a)].append((t, th, lrs))
        elif kind == VSYNC:
            vsyncs.append(t)
            signals[(0x11, a)].append((t, -1, ()))
        elif kind == NVN:
            if code in (SUBMIT_B, PACE_B, SYNC_B, FIN_B):
                open_nvn[(th, code)] = (t, lrs)
            elif code in (SUBMIT_E, PACE_E, SYNC_E, FIN_E):
                bcode = code - 1
                if (th, bcode) in open_nvn:
                    t0, l0 = open_nvn.pop((th, bcode))
                    label = {SUBMIT_B: "nvn submit (replay + GPU fence)", PACE_B: "nvn display wait / present pacing",
                             SYNC_B: "nvn sync wait", FIN_B: "nvn queue finish"}[bcode]
                    nvn[th].append((t0, t, label))
            if code == PRESENT_B:
                presents.append((t, th))
            if code == FRAME_TIME:
                frame_times.append((t, a, b))
    for v in blocks.values():
        v.sort()
    ends = {th: [b[1] for b in v] for th, v in blocks.items()}
    sig_times = {k: [s[0] for s in v] for k, v in signals.items()}

    def waker(blk):
        b0, b1, svc, obj, lrs, rc = blk
        best = None
        for w in WAKERS.get(svc, ()):
            k = (w, obj)
            if k not in sig_times:
                continue
            i = bisect.bisect_right(sig_times[k], b1) - 1
            if i >= 0 and signals[k][i][0] >= b0 - 1000:  # a signal while it waited (or just before it slept)
                if not best or signals[k][i][0] > best[0]:
                    best = signals[k][i]
        return best

    def nvn_split(th, t0, t1):
        """The NVN segments inside [t0, t1] on th: [(label, ns)], and the rest."""
        out, rest = [], t1 - t0
        for a, b, label in nvn.get(th, ()):
            lo, hi = max(a, t0), min(b, t1)
            if hi > lo:
                out.append((label, hi - lo))
                rest -= hi - lo
        return out, rest

    def name(th):
        if th == -1:
            return "vsync"
        return f"g{th:x} {threads.get(th, '?')}"

    present_thread = collections.Counter(th for _, th in presents).most_common(1)
    if not present_thread:
        print("no presents in the timeline")
        return
    pth = present_thread[0][0]
    ps = [t for t, th in presents if th == pth]
    total = collections.Counter()
    frames = 0
    for k in range(1, len(ps)):
        start, t, th = ps[k - 1], ps[k], pth
        acc = collections.Counter()
        steps = []
        guard = 0
        while t > start and guard < 10000:
            guard += 1
            i = bisect.bisect_right(ends.get(th, []), t) - 1
            blk = blocks[th][i] if i >= 0 else None
            seg0 = max(blk[1] if blk else start, start)
            parts, rest = nvn_split(th, seg0, t)
            where = site(syms, blk[4]) if blk else "?"
            for label, ns in parts:
                acc[(name(th), label, "")] += ns
            acc[(name(th), "run", where)] += rest
            steps.append((seg0, t, name(th), "run (after " + (SVC_NAME.get(blk[2], hex(blk[2])) if blk else "?") + " at " + where + ")"))
            if not blk or blk[1] <= start:
                break
            b0, b1, svc, obj, lrs, rc = blk
            w = waker(blk)
            what = SVC_NAME.get(svc, hex(svc))
            if w and w[1] == -1:  # the vsync
                lo = max(b0, start)
                acc[(name(th), "waits for vsync", site(syms, lrs))] += b1 - lo
                steps.append((lo, b1, name(th), "waits for vsync at " + site(syms, lrs)))
                t = b0
            elif w and w[1] != th:
                lo = max(w[0], start)
                acc[(name(th), "wake latency", "")] += b1 - lo
                steps.append((lo, b1, name(th), f"{what} at {site(syms, lrs)}: woken by {name(w[1])} at {site(syms, w[2])}"))
                t, th = w[0], w[1]
            else:
                lo = max(b0, start)
                kind = "sleeps" if svc == 0x0B else ("waits (timeout)" if rc == 0xEA01 else f"{what} (no waker seen)")
                acc[(name(th), kind, site(syms, lrs))] += b1 - lo
                steps.append((lo, b1, name(th), kind + " at " + site(syms, lrs)))
                t = b0
        frames += 1
        total.update(acc)
        if k <= args.show:
            print(f"--- frame {k}: {(ps[k] - ps[k - 1]) / 1e6:.2f} ms (critical path, newest first)")
            for a, b, who, what in steps[:40]:
                print(f"  {(b - a) / 1e6:7.2f} ms  {who[:40]:40s} {what[:150]}")
    span = (ps[-1] - ps[0]) / 1e6
    print(f"\n{frames} frames on {name(pth)}, {span:.0f} ms: {span / max(frames, 1):.2f} ms per frame "
          f"({1000 * frames / span if span else 0:.1f} fps)")
    if frame_times:
        vals = [a for _, a, _ in frame_times]
        print(f"game frame time fw[0x130]: mean {sum(vals) / len(vals) / 31250:.1f} ms; intervals "
              f"{dict(collections.Counter(b for _, _, b in frame_times))}")
    print(f"vsyncs: {len(vsyncs)}")
    print("\ncritical path per frame (ms), by thread / what / where:")
    for (who, what, where), ns in total.most_common(30):
        print(f"  {ns / 1e6 / frames:7.2f}  {who[:40]:40s} {what:36s} {where[:120]}")
    by_kind = collections.Counter()
    for (who, what, where), ns in total.items():
        by_kind[what] += ns
    print("\nby kind:", ", ".join(f"{k} {v / 1e6 / frames:.2f}" for k, v in by_kind.most_common()))


if __name__ == "__main__":
    main()
