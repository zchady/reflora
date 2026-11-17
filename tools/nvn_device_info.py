#!/usr/bin/env python3
"""Read nvnDeviceGetInteger's answers out of a game's own NVN driver by running it (Unicorn), property by property.

Usage: python tools/nvn_device_info.py <sdk NSO> <function offset> [--max N] [--cpp]

<function offset> is the driver function nvnDeviceGetInteger forwards to: int f(pname, const NVNdevice*). For the
Switch 2 Edition of Wonder (SDK 21.4.1): nvnDeviceGetInteger = sdk+657190 (NX_NVN_REAL=1 records it), which calls
sdk+65b7c0. The device is all zeros; a value that comes from a device field is marked "provisional" (the real
nvnDeviceInitialize would have set that field), exactly as runtime/src/nvn/nvn_device.cpp marks its SDK 16 table.
--cpp prints the table in nvn_device.cpp's form.
"""
import argparse
import os
import sys

from unicorn import UC_ARCH_ARM64, UC_HOOK_MEM_READ, UC_MODE_ARM, Uc, UcError
from unicorn import arm64_const as A

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from recomp import nso  # noqa: E402

BASE, STACK, DEV, RET = 0x10000000, 0x7F000000, 0x60000000, 0x7E000000


def run(m, fn, pname):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    size = (len(m.image) + m.bss[1] + 0xFFFF) & ~0xFFFF
    uc.mem_map(BASE, size)
    image = bytearray(m.image)
    for r in m.relocs:  # RELATIVE pointers at this base
        if r.type == nso.R_AARCH64_RELATIVE and r.offset + 8 <= len(image):
            image[r.offset:r.offset + 8] = ((BASE + r.addend) & (2**64 - 1)).to_bytes(8, "little")
    # Unicorn's CPU has no pointer authentication: RETAA/RETAB become RET (PACIBSP/AUTIBSP are hints, NOPs there),
    # as the recompiler treats them (recomp/emit.py).
    t0, tl = m.text
    for a in range(t0, t0 + tl, 4):
        if image[a:a + 4] in (b"\xff\x0b\x5f\xd6", b"\xff\x0f\x5f\xd6"):
            image[a:a + 4] = b"\xc0\x03\x5f\xd6"
    uc.mem_write(BASE, bytes(image))
    uc.mem_map(STACK - 0x10000, 0x20000)
    uc.mem_map(DEV, 0x10000)
    uc.mem_map(RET, 0x1000)
    uc.mem_write(RET, b"\x1f\x20\x03\xd5")  # nop
    reads = set()

    def on_read(_uc, _access, addr, sz, _v, _d):
        reads.add(addr - DEV)

    uc.hook_add(UC_HOOK_MEM_READ, on_read, begin=DEV, end=DEV + 0x10000 - 1)
    uc.reg_write(A.UC_ARM64_REG_SP, STACK)
    uc.reg_write(A.UC_ARM64_REG_X0, pname)
    uc.reg_write(A.UC_ARM64_REG_X1, DEV)
    uc.reg_write(A.UC_ARM64_REG_LR, RET)
    try:
        uc.emu_start(BASE + fn, RET, count=20000)
    except UcError as e:
        return None, f"error {e} at pc {uc.reg_read(A.UC_ARM64_REG_PC) - BASE:#x}"
    if uc.reg_read(A.UC_ARM64_REG_PC) != RET:
        return None, "did not return"
    v = uc.reg_read(A.UC_ARM64_REG_X0) & 0xFFFFFFFF
    v = v - (1 << 32) if v & 0x80000000 else v
    return v, (f"provisional: reads the device at {', '.join(hex(r) for r in sorted(reads))}" if reads else "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sdk")
    ap.add_argument("fn")
    ap.add_argument("--max", type=lambda s: int(s, 0), default=0x100)
    ap.add_argument("--cpp", action="store_true")
    args = ap.parse_args()
    m = nso.load(args.sdk, "sdk")
    fn = int(args.fn, 16)
    rows = []
    for p in range(args.max):
        v, note = run(m, fn, p)
        rows.append((p, v, note))
    if args.cpp:
        for p, v, note in rows:
            if v is None:
                continue
            c = f"  // {note}" if note else ""
            print(f"    {{{p}, {v:#x}}},{c}" if v >= 0 else f"    {{{p}, {v}}},{c}")
    else:
        for p, v, note in rows:
            print(f"{p:4} {('-' if v is None else hex(v)):>12}  {note}")


if __name__ == "__main__":
    main()
