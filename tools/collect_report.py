"""Collection sessions (runtime/src/collect.h): what normal play recorded, and the way from it to the official catalog.

  python tools/collect_report.py report <collect dir> [--catalog <official catalog>] [--out <report.md>]
      sessions, play time, frames per minute; hitches (frames >= 50 ms) classified by cause and the worst ones;
      pipelines compiled while playing; missing or unusable shaders; costly textures; audio near the hitches; how
      much the collected catalog adds to the official one
  python tools/collect_report.py validate <collect dir> --catalog <official> --spirv <data/spirv> --out <candidates>
      the collected records that are intact (format, check), new to the official catalog and whose shaders exist as
      translated .spv files, written as a catalog of candidates; the others listed with the reason
  python tools/collect_report.py compile-check <candidates> --workspace <isolated installation>
      the installation's own runtime makes every candidate with this PC's driver (preparation mode, no pipeline cache
      written, no game): passes when none fails and none lacks a shader

Nothing here changes the official catalog (games/wonder2/pipeline_catalog.bin): after validate and compile-check,
merging is a separate, deliberate step:
  python tools/pipeline_catalog.py merge games/wonder2/pipeline_catalog.bin games/wonder2/pipeline_catalog.bin <candidates>
"""
import argparse
import collections
import glob
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pipeline_catalog as pc  # noqa: E402

SLOW_MS = 50.0


def read_sessions(cdir):
    """[(file, [(wall, seconds, kind, [fields])])] in time order."""
    out = []
    for path in sorted(glob.glob(os.path.join(cdir, "sessions", "*.tsv"))):
        events = []
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                parts = line.rstrip("\r\n").split("\t")
                if len(parts) < 3:
                    continue  # a line cut by a crash
                try:
                    events.append((parts[0], float(parts[1]), parts[2], parts[3:]))
                except ValueError:
                    continue
        out.append((path, events))
    return out


def num(text, pattern, default=0.0):
    m = re.search(pattern, text)
    return float(m.group(1)) if m else default


def hitch_fields(fields):
    t = "\t".join(fields)
    return {"ms": num(t, r"^([\d.]+) ms"), "pipelines": num(t, r"pipelines (\d+)"),
            "pipeline_ms": num(t, r"pipelines \d+ \(([\d.]+) ms\)"), "astc_ms": num(t, r"ASTC \d+ \(([\d.]+) ms\)"),
            "image_ms": num(t, r"images \d+ \(([\d.]+) ms\)"), "upload_mb": num(t, r"upload ([\d.]+) MB"),
            "read_ms": num(t, r"package reads \d+ \([\d.]+ MB, ([\d.]+) ms\)"), "submit_ms": num(t, r"submits ([\d.]+) ms"),
            "sync_ms": num(t, r"sync waits ([\d.]+) ms"),
            "lock_ms": num(t, r"pipeline-objects lock waits ([\d.]+) ms")}


def cause(h):
    """Where most of a slow frame went (the runtime's own attribution, hitch.cpp)."""
    if h["pipeline_ms"] >= 25 or (h["pipelines"] and h["pipeline_ms"] >= 0.4 * h["ms"]):
        return "pipelines compiled"
    if h["astc_ms"] + h["image_ms"] >= 25 or h["upload_mb"] >= 40:
        return "textures (ASTC, images, uploads)"
    if h["read_ms"] >= 25:
        return "package reads"
    if h["lock_ms"] >= 0.4 * h["ms"]:  # runtimes from §5zd9 on: the replay waiting for the catalog's workers
        return "pipeline-objects lock (catalog preparation)"
    if h["sync_ms"] >= 0.4 * h["ms"]:
        return "the game waiting on the GPU"
    return "the game's own work or other"


def report(args):
    sessions = read_sessions(args.dir)
    lines = [f"# Collection report: {args.dir}", ""]
    total_s = 0.0
    hitches, pipes, shaders, textures, audio, minutes = [], [], {}, [], [], []
    captured = 0
    for path, ev in sessions:
        if not ev:
            continue
        captures = [e[1] for e in ev if e[2] == "capture"]  # frames written to disk by a test: slow by themselves
        dur = ev[-1][1]
        total_s += dur
        kinds = collections.Counter(e[2] for e in ev)
        lines.append(f"- `{os.path.basename(path)}`: started {ev[0][0]}, {dur / 60:.1f} min, "
                     + ", ".join(f"{k} {n}" for k, n in sorted(kinds.items())))
        for wall, sec, kind, f in ev:
            where = (os.path.basename(path), wall, sec)
            if kind == "hitch":
                if any(0 <= sec - c <= 1.0 for c in captures):
                    captured += 1
                    continue
                hitches.append((where, hitch_fields(f)))
            elif kind == "pipeline":
                pipes.append((where, f))
            elif kind == "shader":
                shaders.setdefault((f[0] if f else "?", f[1] if len(f) > 1 else "?"), where)
            elif kind == "texture":
                textures.append((where, f))
            elif kind == "audio":
                audio.append((where, f))
            elif kind == "minute":
                minutes.append((where, "\t".join(f)))
    lines += ["", f"**Play recorded:** {len(sessions)} session(s), {total_s / 60:.1f} min.", ""]

    # frames
    frames = sum(num(m, r"^(\d+) frames") for _, m in minutes)
    slow = sum(num(m, r">= 50 ms (\d+)") for _, m in minutes)
    worst = max((num(m, r"worst ([\d.]+) ms") for _, m in minutes), default=0)
    lines += ["## Frames", "", f"{frames:.0f} frames in the minute summaries; {slow:.0f} of 50 ms or more; the worst "
              f"{worst:.0f} ms.", ""]

    # hitches by cause
    by = collections.defaultdict(list)
    for w, h in hitches:
        by[cause(h)].append((w, h))
    lines += ["## Hitches (frames >= 50 ms) by cause", ""]
    if captured:
        lines += [f"({captured} slow frames right after a test's frame capture left out.)", ""]
    lines += ["| Cause | Frames | Time | Worst |", "|---|---|---|---|"]
    for c, hs in sorted(by.items(), key=lambda kv: -sum(h["ms"] for _, h in kv[1])):
        lines.append(f"| {c} | {len(hs)} | {sum(h['ms'] for _, h in hs) / 1000:.1f} s | "
                     f"{max(h['ms'] for _, h in hs):.0f} ms |")
    lines += ["", "The worst:", ""]
    for (sess, wall, sec), h in sorted(hitches, key=lambda x: -x[1]["ms"])[:15]:
        lines.append(f"- {wall} ({sess}, {sec:.0f} s): {h['ms']:.0f} ms, {cause(h)} (pipelines {h['pipelines']:.0f} "
                     f"/ {h['pipeline_ms']:.0f} ms, ASTC {h['astc_ms']:.0f} ms, images {h['image_ms']:.0f} ms, "
                     f"upload {h['upload_mb']:.0f} MB)")

    # pipelines compiled while playing
    status = collections.Counter(f[-1] for _, f in pipes if f)
    kinds = collections.Counter(f[0] for _, f in pipes if f)
    ms = sum(num(f[1], r"([\d.]+) ms") for _, f in pipes if len(f) > 1)
    pairs = collections.Counter((f[2], f[3]) for _, f in pipes if len(f) > 3)
    lines += ["", "## Pipelines compiled while playing", "",
              f"{len(pipes)} ({', '.join(f'{k} {n}' for k, n in kinds.items())}), {ms / 1000:.1f} s of compilation; "
              f"new to the collection {status['recorded']}, already collected or in a catalog {status['known']}, "
              f"failed {status['failed']}.", ""]
    for (vs, fs), n in pairs.most_common(10):
        lines.append(f"- {vs} / {fs}: {n}")

    # shaders, textures, audio
    lines += ["", "## Shaders", ""]
    lines += [f"- {what} {h} (first: {w[1]})" for (what, h), w in sorted(shaders.items())] or ["None missing or unusable."]
    tex_ms = [num(f[1], r"([\d.]+) ms") for _, f in textures if len(f) > 1]
    lines += ["", "## Costly textures (10 ms or more each)", "",
              f"{len(textures)} events; the worst {max(tex_ms, default=0):.0f} ms."]
    underruns = sum(1 for _, f in audio if f and f[0] == "underrun")
    windows = [(w, f) for w, f in audio if f and "frames silent" in f[0]]
    lines += ["", "## Audio", "", f"{underruns} device underruns. Windows of 10 s with 1 s or more of silence, and "
              "the hitches of 200 ms or more in them:"]
    for (sess, wall, sec), f in windows:
        silent = num(f[0], r"^(\d+) of") * 5
        if silent < 1000:
            continue
        near = [h["ms"] for (s2, _, t2), h in hitches if s2 == sess and sec - 10 <= t2 <= sec and h["ms"] >= 200]
        lines.append(f"- {wall}: {silent / 1000:.1f} s silent; hitches {', '.join(f'{x:.0f} ms' for x in near) or 'none'}")
    # Silences one by one (runtimes from §5zd9 on): zero underruns only say the device was fed, not that the game
    # chose the silence. One that overlaps a stall (a hitch of 200 ms or more, or the game's audio updates paused for
    # 200 ms or more) is counted apart from the rest.
    runs = [(w, num(f[1], r"([\d.]+) s"), num(f[2], r"([\d.]+) ms") if len(f) > 2 else 0.0)
            for w, f in audio if f and f[0] == "silence" and len(f) > 1]
    if runs:
        stalled = []
        for (sess, wall, sec), length, gap in runs:
            hits = [h["ms"] for (s2, _, t2), h in hitches
                    if s2 == sess and h["ms"] >= 200 and t2 - h["ms"] / 1000 <= sec and t2 >= sec - length]
            if hits or gap >= 200:
                stalled.append((wall, length, gap, hits))
        lines += ["", f"Silences of 250 ms or more: {len(runs)}, {sum(r[1] for r in runs):.1f} s; with a stall "
                  f"(a hitch of 200 ms or more, or the game's audio updates paused 200 ms or more): {len(stalled)}, "
                  f"{sum(s[1] for s in stalled):.1f} s. Those are not the game's choice even without underruns:"]
        for wall, length, gap, hits in stalled[:40]:
            lines.append(f"- {wall}: {length:.2f} s silent; audio updates paused up to {gap:.0f} ms; hitches "
                         f"{', '.join(f'{x:.0f} ms' for x in hits) or 'none'}")

    # the collected catalog
    cat = os.path.join(args.dir, "pipelines_collected.bin")
    lines += ["", "## Collected catalog", ""]
    if os.path.isfile(cat):
        recs, damaged, _, _ = pc.read(cat)
        new = recs
        if args.catalog:
            official = set(pc.read(args.catalog)[0])
            new = [r for r in recs if r not in official]
            lines.append(f"Official catalog: {len(official)} records.")
        lines.append(f"Collected: {len(recs)} records ({sum(pc.kind(r) == pc.COMPUTE for r in recs)} compute), "
                     f"{damaged} damaged; new to the official catalog: {len(new)}"
                     + (f" (+{100 * len(new) / max(1, len(official)):.1f} %)" if args.catalog else "") + ".")
    else:
        lines.append("None yet.")
    text = "\n".join(lines) + "\n"
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
    print(text)


def validate(args):
    recs, damaged, _, version = pc.read(os.path.join(args.dir, "pipelines_collected.bin"))
    official = set(pc.read(args.catalog)[0])
    tsv = os.path.join(args.spirv, "results.tsv")
    known = set()
    if os.path.isfile(tsv):
        with open(tsv, encoding="utf-8", errors="replace") as f:
            known = {line.split("\t", 1)[0] for line in f}
    good, why = [], collections.Counter()
    for r in recs:
        if r in official:
            why["already in the official catalog"] += 1
            continue
        missing = [h for h in pc.hashes(r) if h and not os.path.isfile(os.path.join(args.spirv, h + ".spv"))]
        if missing:
            why["a shader without a translated .spv"] += 1
            continue
        if known and any(h and h not in known for h in pc.hashes(r)):
            why["a shader not in results.tsv"] += 1
            continue
        good.append(r)
    pc.write(args.out, good)
    print(f"{len(recs)} collected (format v{version}, {damaged} damaged entries skipped): {len(good)} candidates -> "
          f"{args.out}")
    for k, n in why.most_common():
        print(f"  left out: {n} ({k})")


def compile_check(args):
    if not pc.read(args.candidates)[0]:  # nothing to prepare: the runtime would start the game instead of exiting
        print("compile check: no candidates")
        return 0
    ws = os.path.abspath(args.workspace)
    exe = os.path.join(ws, "bin", "wonder2.exe")
    log = os.path.join(ws, "work", "collect_compile_check.log")
    os.makedirs(os.path.dirname(log), exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("NX_")}
    env.update(NX_DATA_ROOT=os.path.join(ws, "data"), NX_HEADLESS="1", NX_MUTE="1", NX_LOG=log, NX_COLLECT="0",
               NX_PIPELINE_PREWARM=os.path.abspath(args.candidates), NX_PREWARM_MODE="exit", NX_PIPELINE_CACHE="0")
    code = subprocess.run([exe], cwd=os.path.dirname(exe), env=env, timeout=1200, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode
    text = open(log, encoding="utf-8", errors="replace").read() if os.path.isfile(log) else ""
    loaded = re.search(r"pipeline catalog: (\d+) pipelines \((\d+) compute\) from \d+ file\(s\) \((\d+) records, "
                       r"(\d+) with a missing shader, (\d+) damaged", text)
    made = re.search(r"prepared \((\d+) made, (\d+) failed\)", text)
    if not loaded or not made:
        print(f"compile check: no result (exit {code}; log {log})")
        return 1
    total, compute, records, missing, damaged = map(int, loaded.groups())
    n_made, n_failed = map(int, made.groups())
    ok = n_failed == 0 and missing == 0 and damaged == 0 and n_made == total
    print(f"compile check: {n_made} of {total} made ({compute} compute), {n_failed} failed, {missing} with a missing "
          f"shader, {damaged} damaged; {'PASS' if ok else 'FAIL'} (log {log})")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("report")
    r.add_argument("dir")
    r.add_argument("--catalog")
    r.add_argument("--out")
    v = sub.add_parser("validate")
    v.add_argument("dir")
    v.add_argument("--catalog", required=True)
    v.add_argument("--spirv", required=True)
    v.add_argument("--out", required=True)
    c = sub.add_parser("compile-check")
    c.add_argument("candidates")
    c.add_argument("--workspace", required=True)
    a = ap.parse_args()
    return {"report": report, "validate": validate, "compile-check": compile_check}[a.cmd](a) or 0


if __name__ == "__main__":
    sys.exit(main())
