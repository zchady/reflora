"""Host thread stacks of a minidump, by scanning (no debugger, no PDB): for each thread, its instruction pointer and
the return addresses found on its stack that fall in a module's code, named from the link map for the executable
(wonder2.map, "Publics by Value") and by module for system DLLs. Scanned frames include stale ones: read from the
top (the innermost frames come first).

usage: stackscan.py <dump> <exe .map> [--frames 25] [--grep text]
"""
import argparse
import bisect
import re

from minidump.minidumpfile import MinidumpFile


def load_map(path):
    base, syms = 0x140000000, []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*Preferred load address is ([0-9a-fA-F]+)", line)
        if m:
            base = int(m.group(1), 16)
        m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s", line)
        if m:
            syms.append((int(m.group(2), 16), m.group(1)))
    syms.sort()
    return base, [a for a, _ in syms], [n for _, n in syms]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("map")
    ap.add_argument("--frames", type=int, default=25)
    ap.add_argument("--grep", default="")
    a = ap.parse_args()
    pref, addrs, names = load_map(a.map)
    md = MinidumpFile.parse(a.dump)
    reader = md.get_reader()
    mods = [(m.baseaddress, m.baseaddress + m.size, m.name.split("\\")[-1]) for m in md.modules.modules]
    exe = next(m for m in mods if m[2].lower().endswith(".exe"))
    slide = exe[0] - pref

    def name(addr):
        if exe[0] <= addr < exe[1]:
            k = bisect.bisect_right(addrs, addr - slide) - 1
            return f"{names[k]}+{addr - slide - addrs[k]:x}" if k >= 0 else f"exe+{addr - exe[0]:x}"
        for lo, hi, n in mods:
            if lo <= addr < hi:
                return f"{n}+{addr - lo:x}"
        return None

    ctx_by_id = {}
    for t in md.threads.threads:
        ctx_by_id[t.ThreadId] = t
    for t in md.threads.threads:
        ctx = t.ContextObject
        rip, rsp = ctx.Rip, ctx.Rsp
        lo = t.Stack.StartOfMemoryRange
        hi = lo + t.Stack.MemoryLocation.DataSize
        frames = [name(rip) or hex(rip)]
        try:
            data = reader.read(rsp, hi - rsp)
        except Exception:
            data = b""
        for k in range(0, len(data) - 7, 8):
            v = int.from_bytes(data[k:k + 8], "little")
            n = name(v)
            if n and not n.startswith(("wonder2.exe", "exe+")) or (n and exe[0] <= v < exe[1]):
                frames.append(n)
            if len(frames) >= a.frames:
                break
        text = " < ".join(frames)
        if a.grep and a.grep not in text:
            continue
        print(f"thread {t.ThreadId}: {text}\n")


if __name__ == "__main__":
    main()
