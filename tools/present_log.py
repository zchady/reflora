"""Frame pacing from a present log (runtime/src/nvn/nvn_present_log.cpp, NX_PRESENT_LOG=<first>:<count>).

usage: present_log.py present_log.csv [--latency-refreshes K]

Cadence of the game's presents and of the host's vkQueuePresentKHR (mean, percentiles, worst), and, from the display's
vblank grid (DWM), the refresh at which each frame reached the screen. In MAILBOX mode the display shows, at each
vblank, the newest frame queued before it: a frame queued at t is shown from the first vblank after t (plus K refreshes
of compositor latency, the same for every frame, so it does not change the cadence) until the vblank that shows the
next one. Two frames before the same vblank: the older one is never shown (dropped). The game's time step per frame
gives the simulation speed over the logged stretch (100% = real time).
"""
import argparse
import csv
import math
import statistics


def pct(v, p):
    s = sorted(v)
    return s[min(len(s) - 1, max(0, int(math.ceil(p / 100 * len(s))) - 1))]


def summary(name, ms):
    if not ms:
        print(f"{name}: no data")
        return
    print(f"{name}: n={len(ms)} mean {statistics.mean(ms):.3f} ms, sd {statistics.pstdev(ms):.3f}, "
          f"p1 {pct(ms, 1):.2f}, p50 {pct(ms, 50):.2f}, p95 {pct(ms, 95):.2f}, p99 {pct(ms, 99):.2f}, "
          f"max {max(ms):.2f}; >20 ms {sum(1 for x in ms if x > 20)}, >25 ms {sum(1 for x in ms if x > 25)}, "
          f"<12 ms {sum(1 for x in ms if x < 12)}")


def phase_table(shown, period, window_s):
    """The beat between the presents and the display: a present close to a vblank lands on either side of it."""
    want = round((1e9 / 60) / period)
    t0 = shown[0][0]["queued_ns"]
    print(f"per {window_s:g} s: phase = host present after the last vblank, in ms of a {period / 1e6:.2f} ms refresh")
    print("    t(s)  presents  phase p5 / p50 / p95   uneven frames (refreshes: count)")
    k, n = 0, len(shown)
    while k < n - 1:
        t = shown[k][0]["queued_ns"]
        j = k
        ph, stays = [], {}
        while j < n - 1 and shown[j][0]["queued_ns"] - t < window_s * 1e9:
            r = shown[j][0]
            ph.append(((r["queued_ns"] - r["vblank_ns"]) % r["period_ns"]) / 1e6)
            refreshes = round((shown[j + 1][1] - shown[j][1]) / period)
            if refreshes != want:
                stays[refreshes] = stays.get(refreshes, 0) + 1
            j += 1
        print(f"  {(t - t0) / 1e9:6.1f}  {j - k:8d}  {pct(ph, 5):5.2f} / {pct(ph, 50):5.2f} / {pct(ph, 95):5.2f}   "
              f"{dict(sorted(stays.items())) if stays else '-'}")
        k = j


def event_list(shown, period, limit):
    """Each uneven frame with its neighbours: what the game asked and measured, and when the host presented it."""
    want = round((1e9 / 60) / period)
    t0 = shown[0][0]["queued_ns"]
    stays = [round((shown[k + 1][1] - shown[k][1]) / period) for k in range(len(shown) - 1)]
    print("uneven frames (-1, the frame, +1):  t(s)  n  refreshes  interval  step  game frame ms  "
          "game dt ms  D(p) dt ms  host dt ms  late ms (host present after D(p))")
    listed, last = 0, -10
    for k, st in enumerate(stays):
        if st == want or k - last < 2:
            continue
        last = k
        if listed >= limit:
            print("  ...")
            break
        listed += 1
        for j in range(max(1, k - 1), min(len(stays), k + 2)):
            r, q = shown[j][0], shown[j - 1][0]
            print(f"  {'>' if j == k else ' '} {(r['queued_ns'] - t0) / 1e9:7.2f}  {r['n']:6d}  {stays[j]:3d}  {r['interval']}  "
                  f"{r['step_x1000'] / 1000:.1f}  {r['frame_ticks'] / 31250:6.2f}  {(r['game_ns'] - q['game_ns']) / 1e6:6.2f}  "
                  f"{(r['shown_ns'] - q['shown_ns']) / 1e6:6.2f}  {(r['queued_ns'] - q['queued_ns']) / 1e6:6.2f}  "
                  f"{(r['queued_ns'] - r['shown_ns']) / 1e6:6.2f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--latency-refreshes", type=int, default=0)
    ap.add_argument("--events", type=int, default=0, metavar="N",
                    help="list the first N frames that did not stay 1/60 s, with the frames around each")
    ap.add_argument("--phase", type=float, default=0, metavar="SECONDS",
                    help="per window of SECONDS: where in the refresh the host presents fall and the uneven frames")
    a = ap.parse_args()
    rows = [{k: int(v) for k, v in r.items()} for r in csv.DictReader(open(a.csv))]
    print(f"{len(rows)} presents, game presents {rows[0]['n']}..{rows[-1]['n']}")
    gaps = [r2["n"] - r1["n"] for r1, r2 in zip(rows, rows[1:])]
    if any(g != 1 for g in gaps):
        print(f"present numbers not consecutive: {sum(1 for g in gaps if g != 1)} gaps")
    ints = {}
    for r in rows:
        ints[r["interval"]] = ints.get(r["interval"], 0) + 1
    print("intervals asked:", ints)
    span = (rows[-1]["game_ns"] - rows[0]["game_ns"]) / 1e9
    print(f"game presents: {len(rows) - 1} in {span:.3f} s = {(len(rows) - 1) / span:.2f} fps")
    summary("game present to present", [(b["game_ns"] - a_["game_ns"]) / 1e6 for a_, b in zip(rows, rows[1:])])
    sh = [r for r in rows if r["shown_ns"]]
    summary("display model D(p) to D(p+1)", [(b["shown_ns"] - a_["shown_ns"]) / 1e6 for a_, b in zip(sh, sh[1:])])
    q = [r for r in rows if r["queued_ns"]]
    if not q:
        print("no host presents (headless?)")
    else:
        summary("host vkQueuePresentKHR to next", [(b["queued_ns"] - a_["queued_ns"]) / 1e6 for a_, b in zip(q, q[1:])])
        summary("host present cost (gpu_present entered -> queued)", [(r["queued_ns"] - r["gpu_ns"]) / 1e6 for r in q])
        summary("game present -> host queued", [(r["queued_ns"] - r["game_ns"]) / 1e6 for r in q])
        per = [r for r in q if r["period_ns"]]
        if per:
            period = statistics.median(r["period_ns"] for r in per)
            print(f"display refresh: {1e9 / period:.2f} Hz (period {period / 1e6:.3f} ms); frames at 60 fps should "
                  f"each stay {period and (1e9 / 60) / period:.2f} refreshes")
            # The vblank index each frame is first shown at, on the grid of its own DWM sample.
            shown = []
            for r in per:
                k = math.floor((r["queued_ns"] - r["vblank_ns"]) / r["period_ns"]) + 1 + a.latency_refreshes
                shown.append((r, r["vblank_ns"] + k * r["period_ns"]))
            dropped = 0
            stays = {}
            durations = []
            for (r1, t1), (r2, t2) in zip(shown, shown[1:]):
                refreshes = round((t2 - t1) / period)
                if refreshes <= 0:
                    dropped += 1
                    continue
                stays[refreshes] = stays.get(refreshes, 0) + 1
                durations.append((t2 - t1) / 1e6)
            print(f"frames dropped (replaced before a vblank showed them): {dropped}")
            print("refreshes each shown frame stayed on screen:", dict(sorted(stays.items())))
            summary("on-screen time per frame", durations)
            even = stays.get(round((1e9 / 60) / period), 0)
            print(f"frames shown for exactly 1/60 s: {even} of {sum(stays.values())} "
                  f"({100 * even / max(1, sum(stays.values())):.1f}%)")
            if a.phase > 0:
                phase_table(shown, period, a.phase)
            if a.events > 0:
                event_list(shown, period, a.events)
    steps = [r["step_x1000"] / 1000 for r in rows if r["step_x1000"]]
    if steps and span > 0:
        game_s = sum(s / 60 for s in steps[:-1])
        print(f"simulation: {game_s:.3f} s of game time in {span:.3f} s = {100 * game_s / span:.2f}% speed; "
              f"step values {sorted(set(steps))}")
    ft = [r["frame_ticks"] / 31250 for r in rows if r["frame_ticks"]]
    if ft:
        summary("game's measured frame time fw[0x130]", ft)


if __name__ == "__main__":
    main()
