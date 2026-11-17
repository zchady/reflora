"""Free-space and storage-budget check before an operation that can write many gigabytes (WONDER_SWITCH2_PROGRESS.md
§5zd10): a build, a preparation in a new workspace, frame dumps, an Android cross-build.

usage: python tools/space_check.py --path <where it writes> --need-gb <estimate>
                                   [--reserve-gb 50] [--budget-dir <folder> --budget-gb <limit>]

Refuses (exit 1) when the drive would keep less than --reserve-gb free after writing --need-gb, or when
--budget-dir would grow past --budget-gb. Prints what it measured either way. Reads only; never deletes anything.
"""
import argparse
import os
import shutil
import sys

GB = 1024 ** 3


def folder_bytes(path):
    """Size of the files under path (not following links or junctions)."""
    total = 0
    stack = [path]
    while stack:
        try:
            it = os.scandir(stack.pop())
        except OSError:
            continue
        with it:
            for e in it:
                try:
                    if e.is_symlink() or getattr(e.stat(follow_symlinks=False), "st_file_attributes", 0) & 0x400:
                        continue  # a link or a junction: counted where it points, if at all
                    if e.is_dir(follow_symlinks=False):
                        stack.append(e.path)
                    else:
                        total += e.stat(follow_symlinks=False).st_size
                except OSError:
                    pass
    return total


def check(path, need_gb, reserve_gb=50.0, budget_dir=None, budget_gb=None):
    """(ok, lines): whether the operation fits, and what was measured."""
    probe = path
    while not os.path.exists(probe):  # the destination may not exist yet: measure the drive it will be on
        parent = os.path.dirname(probe)
        if parent == probe:
            break
        probe = parent
    free = shutil.disk_usage(probe).free / GB
    ok = free - need_gb >= reserve_gb
    lines = [f"drive of {path}: {free:.1f} GB free; after {need_gb:.1f} GB: {free - need_gb:.1f} GB "
             f"(reserve {reserve_gb:.0f} GB) -> {'ok' if ok else 'REFUSED'}"]
    if budget_dir is not None and budget_gb is not None:
        used = folder_bytes(budget_dir) / GB if os.path.exists(budget_dir) else 0.0
        fits = used + need_gb <= budget_gb
        lines.append(f"budget of {budget_dir}: {used:.1f} GB used + {need_gb:.1f} GB = {used + need_gb:.1f} GB "
                     f"of {budget_gb:.0f} GB -> {'ok' if fits else 'REFUSED'}")
        ok = ok and fits
    return ok, lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--path", required=True, help="where the operation writes")
    ap.add_argument("--need-gb", type=float, required=True, help="the operation's estimated peak, in GB")
    ap.add_argument("--reserve-gb", type=float, default=50.0, help="free space to keep on the drive afterwards")
    ap.add_argument("--budget-dir", help="a folder with a storage budget (e.g. the Android work folder)")
    ap.add_argument("--budget-gb", type=float, help="that folder's limit, in GB")
    a = ap.parse_args(argv)
    if (a.budget_dir is None) != (a.budget_gb is None):
        ap.error("--budget-dir and --budget-gb go together")
    ok, lines = check(a.path, a.need_gb, a.reserve_gb, a.budget_dir, a.budget_gb)
    for line in lines:
        print(line)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
