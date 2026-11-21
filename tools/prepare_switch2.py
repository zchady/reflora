"""Prepare the game 040015100B514001 for this runtime from the user's own NUP package.

usage: python tools/prepare_switch2.py --nup <Wonder.nup> --workspace <dir> [--steps a,b,...] [--modules main,sdk,subsdk0]
                                       [--jobs N] [--force] [--harness <spirv_harness.exe>]
                                       [--ryubing-archive <ryubing.tar.gz>] [--spirv-val <spirv-val.exe>]
                                       [--shader-sample <hash list>] [--toolchain-dir <dir>] [--build-jobs N]
                                       [--components <dir>]

Everything it writes goes under the workspace (nothing in this repository, nothing next to the package):
  <workspace>/work/exefs/        the program's ExeFS, extracted with SHA-256 verification (tools/nup.py)
  <workspace>/work/gen/<module>  recompiled C++ (python -m recomp)
  <workspace>/work/build/        the CMake build directory
  <workspace>/data/              the runtime's data folder (NX_DATA_ROOT): analysis/*.bin, spirv/, nup.txt, user/ (saves)
  <workspace>/bin/wonder2.exe    the game executable built from the above (local only: never distribute it)

Steps (in order; each one is skipped when its outputs are already there, unless --force):
  verify     the processor has AVX2 and the workspace's disk has room (about 5.5 GB for a whole preparation); the
             package holds program 040015100B514001 (and the build this project was tested with)
  extract    ExeFS of that program -> work/exefs
  bins       flat module images -> data/analysis/*.bin (tools/nso2bin.py)
  recompile  main, sdk, subsdk0 -> work/gen (the longest step of the CPU side: main takes several minutes)
  shaders    the Switch 2 shaders found in the package and in main, extracted, lowered (Ampere -> Maxwell) and
             translated (Maxwell -> SPIR-V), validated with spirv-val -> data/spirv (tools/prepare_shaders.py; work in
             work/shaders, resumable group by group). Uses Reflora's components (--components, or <repo>/components:
             the self-contained translator and spirv-val, checked against components.json; then nothing installed is
             looked at). Without them (development): --harness / the .NET 10 SDK and the Vulkan SDK's spirv-val.
             --shader-sample <file> limits it to the listed code hashes (a controlled test, not a preparation).
  build      llvm-mingw + CMake + Ninja + SDL3 + Vulkan-Headers, pinned and verified, fetched into <workspace>/tools
             (or taken from --toolchain-dir) -> bin/wonder2.exe + SDL3.dll + licenses/ (tools/prepare_build.py).
             No Visual Studio, MSYS2 or SDK needed. Jobs: --build-jobs, else what fits the free memory (at most 8).
  config     data/nup.txt naming the package (the runtime reads the RomFS from it)
  check      starts bin/wonder2.exe for 45 s without a window on data/: the title screen is reached, every shader the
             game asks for is found, no Vulkan or runtime error (log: work/check.log, frame: work/check_title.ppm)
  graphics   the pipelines of games/wonder2/pipeline_catalog.bin compiled by this PC's GPU driver into data/'s
             pipeline cache, by the runtime itself (NX_PREWARM_MODE=exit; log work/graphics.log, result
             work/graphics.json). Skipped without starting anything when the GPU, driver, catalog, executable,
             SPIR-V and cache are those of its last complete run. Never fails the preparation: a partial or failed
             one is a WARN, and the game compiles what it lacks while playing.

Machine-readable lines for the launcher: "STEP <i>/<n> <name> <message>", "GROUP <i>/<n> <group> ...", "PROGRESS
<0-100>" (always progress inside the current step; 100 when it ends), "COUNT <done>/<total> <unit>" (what the step
counts: the graphics step's pipelines), "LOG <text>", "WARN <text>", "ERROR <text>",
"RESULT graphics <ok|partial|failed|skipped> made=<n> failed=<n> total=<n> compute=<n> seconds=<s>", "RESULT textures
<ok|partial|failed|skipped> converted=<n> accepted=<n> kept=<n> seconds=<s>" (the optional textures step) and "DONE". Exit code 0 on success, 2 when a step needs the user (a missing tool, an untested build, no AVX2, no
disk). A whole preparation (all the steps) writes work/prepared.json once the check step passed (the game is playable)
and again at DONE; starting one removes it.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROGRAM_ID = "040015100B514001"
TESTED_BUILD_ID = "ec47e6f8846d2a7b6892e2682845c7255f7117ae"
MODULES = {"main": "0x4000000000", "sdk": "0x4010000000", "subsdk0": "0x4012000000"}
HLE_LIST = {"sdk": os.path.join(REPO, "games", "wonder2", "hle_internal.txt")}
STEPS = ["verify", "extract", "bins", "recompile", "shaders", "build", "config", "check", "graphics"]
# Run only when named in --steps: the BC7 texture cache, for NX_ASTC_BC7 (off by default).
OPTIONAL_STEPS = ["textures"]
PREPARED_MARKER = "prepared.json"  # in work/: written when a whole preparation reaches DONE, removed when one starts
# A whole preparation writes the marker as soon as this step passed (the game is playable from there) and again at
# DONE: stopping it or failing in the graphics step, an optimization, never takes "ready" away.
PLAYABLE_AFTER = "check"
# Bumped when the recompiled code of the same input must change (a fix in recomp/ is caught by its own SHA-256).
RECOMPILE_VERSION = 1


class NeedsUser(Exception):
    pass


def say(kind, text=""):
    print(f"{kind} {text}".rstrip(), flush=True)


def run(cmd, cwd=REPO, env=None):
    say("LOG", "$ " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
    e = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUTF8="1", **(env or {}))
    p = subprocess.run(cmd, cwd=cwd, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       encoding="utf-8", errors="replace")
    for line in p.stdout.splitlines()[-40:]:
        say("LOG", line)
    if p.returncode != 0:
        raise RuntimeError(f"command failed ({p.returncode}): {cmd[0]} {cmd[1] if len(cmd) > 1 else ''}")
    return p.stdout


def py(*args):
    return [sys.executable, "-X", "utf8", *args]


# Disk the whole preparation leaves in a workspace, measured (WONDER_SWITCH2_PROGRESS.md §5za6): 4.76 GB allocated with
# the components (tools 0.96, work 2.97, data 0.60, bin 0.23) + 0.24 GB of kept downloads; nothing larger is written
# on the way (every output goes to .tmp and is renamed), plus room for the pipeline cache, saves and logs.
WORKSPACE_GB = 5.5
PF_AVX2_INSTRUCTIONS_AVAILABLE = 40  # IsProcessorFeaturePresent; Windows 10 2004 (build 19041) and later know it


def allocated_gb(path):
    total = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                total += -(-os.stat(os.path.join(root, f)).st_size // 4096) * 4096
            except OSError:
                pass
    return total / 2**30


def preflight(ws, avx2=None, free_gb=None, used_gb=None, windows_build=None):
    """What the machine must have before anything long starts: a CPU with AVX2 (the executable is built for
    -march=haswell) and the disk the preparation still needs. Arguments are for the tests."""
    if windows_build is None:
        windows_build = sys.getwindowsversion().build if sys.platform == "win32" else 0
    if avx2 is None and sys.platform == "win32" and windows_build >= 19041:
        import ctypes
        avx2 = bool(ctypes.windll.kernel32.IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE))
    if avx2 is False:
        raise NeedsUser("this PC's processor does not have AVX2 (Intel Haswell / AMD Excavator or newer), which the "
                        "game executable needs")
    if avx2 is None:
        say("WARN", f"AVX2 cannot be confirmed on Windows build {windows_build} (Windows 10 version 2004 or later "
                    "reports it): the game needs a processor with AVX2")
    if free_gb is None:
        free_gb = shutil.disk_usage(ws).free / 2**30
    if free_gb >= WORKSPACE_GB:
        return
    if used_gb is None:
        used_gb = allocated_gb(ws)
    need = max(0.0, WORKSPACE_GB - used_gb)
    if free_gb < need:
        raise NeedsUser(f"not enough free disk for the workspace {ws}: about {need:.1f} GB more are needed and "
                        f"{free_gb:.1f} GB are free (a whole preparation takes about {WORKSPACE_GB:.1f} GB)")


def step_verify(a, ws):
    preflight(ws)
    out = run(py(os.path.join(REPO, "tools", "nup.py"), "info", a.nup))
    m = re.search(r"selected \(prefer switch2\): ([0-9A-Fa-f]{16}) from package (\d+)", out)
    if not m or m.group(1).upper() != PROGRAM_ID:
        raise NeedsUser(f"the package does not hold program {PROGRAM_ID}. Select a NUP made from your own copy "
                        "of that game.")
    a.package = int(m.group(2))
    b = re.search(r"build_id ([0-9a-f]{40})", out)
    build = b.group(1) if b else "unknown"
    if build != TESTED_BUILD_ID:
        say("WARN", f"build id {build} is not the one this project was tested with ({TESTED_BUILD_ID}, version "
                    "1.2.1): its game-specific fixes may not apply. Continue only to experiment.")
    if "addon" in out and "belongs" in out:
        say("LOG", "add-on content linked to the program: found")
    else:
        say("WARN", "no add-on content in the package: the game's add-on data may be missing")
    with open(os.path.join(ws, "work", "package.txt"), "w") as f:
        f.write(f"{a.package}\n{build}\n")


def package_index(ws):
    with open(os.path.join(ws, "work", "package.txt")) as f:
        return int(f.readline())


def step_extract(a, ws):
    out = os.path.join(ws, "work", "exefs_tmp")
    if os.path.isdir(out):
        shutil.rmtree(out)
    run(py(os.path.join(REPO, "tools", "nup.py"), "extract", a.nup, out, "--package", str(package_index(ws)),
           "--subtree", "exefs"))
    # tools/nup.py keeps the package path (exefs/<module>): move the modules up into work/exefs.
    src = os.path.join(out, "exefs") if os.path.isdir(os.path.join(out, "exefs")) else out
    dst = os.path.join(ws, "work", "exefs")
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.move(src, dst)
    shutil.rmtree(out, ignore_errors=True)
    missing = [m for m in MODULES if not os.path.isfile(os.path.join(dst, m))]
    if missing:
        raise RuntimeError("ExeFS is missing " + ", ".join(missing))


def step_bins(a, ws):
    os.makedirs(os.path.join(ws, "data", "analysis"), exist_ok=True)
    for m in a.modules:
        run(py(os.path.join(REPO, "tools", "nso2bin.py"), os.path.join(ws, "work", "exefs", m),
               os.path.join(ws, "data", "analysis", m + ".bin")))


def _sha256(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _pb():
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import prepare_build
    return prepare_build


def recompile_inputs(ws, m):
    """What module m's generated code depends on: its ExeFS file, the recompiler's sources, the HLE list."""
    pb = _pb()
    rec = {"module": _sha256(os.path.join(ws, "work", "exefs", m)),
           "recomp": pb.tree_digest(os.path.join(REPO, "recomp"), (".py",))[0], "base": MODULES[m],
           "version": RECOMPILE_VERSION}
    if m in HLE_LIST:
        rec["hle"] = _sha256(HLE_LIST[m])
    return pb.digest(rec)


def recompile_state(ws):
    import json
    try:
        return json.load(open(os.path.join(ws, "work", "recompile-state.json"), encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def recompile_valid(ws, m):
    """Module m's generated code is there, made from these inputs, and unchanged since (every file's SHA-256)."""
    rec = recompile_state(ws).get(m, {})
    out = os.path.join(ws, "work", "gen", m)
    return (os.path.isdir(out) and os.path.isfile(os.path.join(ws, "work", "exefs", m))
            and rec.get("inputs") == recompile_inputs(ws, m) and rec.get("output") == _pb().tree_digest(out)[0])


def step_recompile(a, ws):
    import json
    pb = _pb()
    state = recompile_state(ws)
    for k, m in enumerate(a.modules):
        say("PROGRESS", str(int(100 * k / len(a.modules))))
        if not a.force and recompile_valid(ws, m):
            say("LOG", f"recompile {m}: unchanged inputs and outputs (SHA-256 checked): kept")
            continue
        out = os.path.join(ws, "work", "gen", m)
        tmp = out + ".tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        # tools/run_recomp.py = `-m recomp` that also works with the embeddable Python (no cwd on its sys.path)
        cmd = py(os.path.join(REPO, "tools", "run_recomp.py"), os.path.join(ws, "work", "exefs", m), tmp, "--name", m,
                 "--base", MODULES[m])
        if m in HLE_LIST:
            cmd += ["--hle-list", HLE_LIST[m]]
        run(cmd)
        shutil.rmtree(out, ignore_errors=True)
        os.replace(tmp, out)  # an interrupted recompile never leaves a partial module where the build looks
        state[m] = {"inputs": recompile_inputs(ws, m), "output": pb.tree_digest(out)[0]}
        path = os.path.join(ws, "work", "recompile-state.json")
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            json.dump(state, f, indent=1, sort_keys=True)
        os.replace(path + ".tmp", path)


def components_dir(a):
    """Reflora's portable components: --components, else <repository>/components when it holds a components.json."""
    if a.components:
        return os.path.abspath(a.components)
    d = os.path.join(REPO, "components")
    return d if os.path.isfile(os.path.join(d, "components.json")) else None


def step_shaders(a, ws):
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import prepare_shaders as ps
    sample = ps.read_sample(a.shader_sample) if a.shader_sample else None
    p = ps.Pipeline(a.nup, package_index(ws), ws, say, jobs=a.jobs, harness=a.harness,
                    ryubing_archive=a.ryubing_archive, spirv_val=a.spirv_val, sample=sample,
                    components=components_dir(a))
    try:
        p.run()
    except ps.NeedsUser as e:
        raise NeedsUser(str(e))


def step_build(a, ws):
    pb = _pb()
    missing = [m for m in MODULES if not recompile_valid(ws, m)]
    if missing:
        raise NeedsUser("the recompiled code of " + ", ".join(missing) + " is missing or changed: run the recompile "
                        "step first")
    try:
        exe = pb.Build(ws, say, os.path.join(ws, "work", "gen"), jobs=a.build_jobs,
                       toolchain_dir=a.toolchain_dir).go()
    except pb.NeedsUser as e:
        raise NeedsUser(str(e))
    say("LOG", f"executable: {exe}")


def step_check(a, ws):
    """bin/wonder2.exe for 45 s without a window on data/: title reached, every requested shader found, no error."""
    import time
    exe = os.path.join(ws, "bin", "wonder2.exe")
    data = os.path.join(ws, "data")
    if not os.path.isfile(exe):
        raise NeedsUser("bin/wonder2.exe is missing: run the build step first")
    log = os.path.join(ws, "work", "check.log")
    env = dict(os.environ, NX_DATA_ROOT=data, NX_HEADLESS="1", NX_MUTE="1", NX_LOG=log, NX_DUMP_SHADER_MISSES="1",
               NX_INPUT_SCRIPT="22:SHOT:1")
    for k in ("NX_OVERLAY_TEST", "NX_PRESENT_LOG"):
        env.pop(k, None)
    frames = os.path.join(data, "frames")
    shutil.rmtree(frames, ignore_errors=True)
    p = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    t0 = time.time()
    while p.poll() is None and time.time() - t0 < 45:
        time.sleep(1)
    exited = p.poll() is not None
    if not exited:
        p.kill()
        p.wait()
    text = open(log, encoding="utf-8", errors="replace").read() if os.path.isfile(log) else ""
    records = re.findall(r"nvn: (\d+) shader records", text)
    missing = len(re.findall(r"no [^ ]*\.spv", text))
    errors = re.findall(r"[^\n]*(?:VK_ERROR|device lost|access violation|fatal)[^\n]*", text, re.I)
    shots = sorted(os.listdir(frames)) if os.path.isdir(frames) else []
    if shots:
        shutil.move(os.path.join(frames, shots[0]), os.path.join(ws, "work", "check_title.ppm"))
        shutil.rmtree(frames, ignore_errors=True)
    say("LOG", f"check: {'exited by itself' if exited else 'ran 45 s'}; shader records {records}; missing shaders "
               f"{missing}; errors {len(errors)}; title frame {'saved' if shots else 'not reached'}")
    for e in errors[:5]:
        say("LOG", e[:300])
    if exited or not records or missing or errors or not shots:
        raise RuntimeError("the executable did not pass the check (log: " + log + ")")


# ---- graphics: the known pipelines compiled by this PC's driver before the first game ------------------------------
#
# The runtime does the work (runtime/src/nvn/nvn_pipeline_catalog.cpp, NX_PREWARM_MODE=exit): it reads the catalog,
# makes every pipeline with the local GPU's driver into data/pipelines_<vendor>_<device>.bin (the pipeline cache the
# game loads at every start) and exits. This step installs the project's catalog in data/ (the game prepares from it
# at every start too, in the background, cheaply once the cache holds them), runs that, reports its progress and
# records the result in work/graphics.json. It is an optimization: whatever happens to it, the installation stays
# playable (the game compiles what it lacks when it needs it, as before), so it never fails the preparation.

PROJECT_CATALOG = os.path.join(REPO, "games", "wonder2", "pipeline_catalog.bin")
INSTALLED_CATALOG = "pipeline_catalog.bin"      # in data/: the project's catalog (read by the game at every start)
LOCAL_CATALOG = "pipeline_catalog_local.bin"    # in data/: pipelines recorded on this PC (NX_PIPELINE_RECORD=1)
GRAPHICS_STATE = "graphics.json"                # in work/
GRAPHICS_FORMAT = 1
RUNTIME_MARK = b"NX_PREWARM_MODE"               # an executable built before the graphics preparation lacks it
EXIT_CODES = {0: "ok", 4: "partial", 5: "partial"}  # 4: pipelines the driver refused; 5: the cache was not saved
GRAPHICS_START_LIMIT = 120  # seconds for the runtime to report the catalog (else it is not preparing)
DISPLAY_CLASS = r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"


def sha256_file(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def gpu_signature():
    """The display adapters and their drivers as Windows lists them (registry, a few ms): a new GPU or driver changes
    it. None when it cannot be read: then the runtime is asked (it reads the device itself, in a few seconds)."""
    if sys.platform != "win32":
        return None
    import winreg
    out = []
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, DISPLAY_CLASS) as cls:
            for i in range(winreg.QueryInfoKey(cls)[0]):
                name = winreg.EnumKey(cls, i)
                if not name.isdigit():
                    continue
                try:
                    with winreg.OpenKey(cls, name) as k:
                        item = []
                        for v in ("MatchingDeviceId", "DriverDesc", "DriverVersion", "DriverDate"):
                            try:
                                item.append(str(winreg.QueryValueEx(k, v)[0]))
                            except OSError:
                                item.append("")
                        out.append(item)
                except OSError:
                    continue
    except OSError:
        return None
    return sorted(out) or None


def spirv_digest(data):
    """The translated shaders as the runtime reads them: results.tsv's SHA-256 and the .spv files' count, total size
    and newest change (a re-translation rewrites them). About 0.1 s for 17,245 files."""
    d = os.path.join(data, "spirv")
    count = size = newest = 0
    try:
        with os.scandir(d) as it:
            for e in it:
                if e.name.endswith(".spv"):
                    st = e.stat()
                    count, size, newest = count + 1, size + st.st_size, max(newest, st.st_mtime_ns)
    except OSError:
        return None
    tsv = os.path.join(d, "results.tsv")
    return [sha256_file(tsv) if os.path.isfile(tsv) else "", count, size, newest]


def graphics_inputs(ws):
    """What a prepared pipeline cache depends on, read without starting the runtime."""
    exe = os.path.join(ws, "bin", "wonder2.exe")
    data = os.path.join(ws, "data")
    local = os.path.join(data, LOCAL_CATALOG)
    st = os.stat(exe) if os.path.isfile(exe) else None
    return {"exe": [st.st_size, st.st_mtime_ns] if st else None,
            "catalog": sha256_file(PROJECT_CATALOG) if os.path.isfile(PROJECT_CATALOG) else None,
            "local_catalog": sha256_file(local) if os.path.isfile(local) else None,
            "spirv": spirv_digest(data), "gpu": gpu_signature()}


def graphics_outputs(ws):
    """The pipeline caches and stamps the runtime left in data/, by SHA-256 (a damaged or replaced file differs)."""
    data = os.path.join(ws, "data")
    out = {}
    for name in sorted(os.listdir(data)) if os.path.isdir(data) else []:
        if re.fullmatch(r"pipelines_[0-9a-f]{4}_[0-9a-f]{4}\.(bin|stamp)", name):
            out[name] = sha256_file(os.path.join(data, name))
    return out


def read_graphics_state(ws):
    import json
    try:
        with open(os.path.join(ws, "work", GRAPHICS_STATE), encoding="utf-8") as f:
            state = json.load(f)
        return state if state.get("format") == GRAPHICS_FORMAT else None
    except (OSError, ValueError):
        return None


def graphics_done(ws):
    """The last preparation finished for these exact inputs and its files are as it left them: nothing to redo, and
    the runtime is not even started. Anything else (another GPU or driver, a new catalog, executable or SPIR-V, a
    cache deleted or damaged, a preparation that was partial or failed) runs it again."""
    state = read_graphics_state(ws)
    if not state or state.get("status") != "ok":
        return False
    now = graphics_inputs(ws)
    return now["gpu"] is not None and state.get("inputs") == now and state.get("outputs") == graphics_outputs(ws) \
        and any(n.endswith(".bin") for n in state.get("outputs", {}))


def cache_file_ok(path):
    """A pipeline cache file as nvn_pipeline_cache.cpp writes it: "NXVC", version 1, the data's size, its check."""
    import struct
    try:
        with open(path, "rb") as f:
            head = f.read(24)
        magic, version, size, _ = struct.unpack("<IIQQ", head)
        return magic == 0x4356584E and version == 1 and size + 24 == os.path.getsize(path) and size > 0
    except (OSError, struct.error):
        return False


def process_usage(handle):
    """CPU seconds (user + kernel) and peak working set (bytes) of a process handle (Windows), or (None, None)."""
    if sys.platform != "win32":
        return None, None
    import ctypes
    from ctypes import wintypes

    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]
    try:
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        t = [wintypes.FILETIME() for _ in range(4)]
        cpu = None
        if k32.GetProcessTimes(wintypes.HANDLE(handle), *[ctypes.byref(x) for x in t]):
            cpu = sum((x.dwHighDateTime << 32 | x.dwLowDateTime) for x in t[2:]) / 1e7
        c = Counters()
        c.cb = ctypes.sizeof(c)
        peak = c.PeakWorkingSetSize if psapi.GetProcessMemoryInfo(wintypes.HANDLE(handle), ctypes.byref(c), c.cb) \
            else None
        return cpu, peak
    except (OSError, AttributeError):
        return None, None


def graphics_starting(ws):
    """Until the step ends, its last result no longer holds: stopped halfway, the launcher shows it as not done.
    Written before the step is announced, so that no stop, however early, leaves the previous "ok" standing."""
    write_graphics_state(ws, {"format": GRAPHICS_FORMAT, "status": "failed", "finished": None,
                              "reason": "interrupted before it finished"})


def write_graphics_state(ws, state):
    import json
    path = os.path.join(ws, "work", GRAPHICS_STATE)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=1)
    os.replace(tmp, path)


def step_graphics(a, ws):
    import time
    exe = os.path.join(ws, "bin", "wonder2.exe")
    data = os.path.join(ws, "data")
    if not os.path.isfile(exe):
        raise NeedsUser("bin/wonder2.exe is missing: run the build step first")
    state = {"format": GRAPHICS_FORMAT, "status": "failed", "finished": None}
    graphics_starting(ws)
    try:
        state.update(run_graphics(a, ws, exe, data))
    except Exception as e:  # noqa: BLE001 - an optimization: the installation stays playable
        state.update(status="failed", reason=f"{type(e).__name__}: {e}")
    state["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
    state["inputs"] = graphics_inputs(ws)
    state["outputs"] = graphics_outputs(ws)
    write_graphics_state(ws, state)
    detail = (f"made={state.get('made', 0)} failed={state.get('failed', 0)} total={state.get('total', 0)} "
              f"compute={state.get('compute', 0)} seconds={state.get('seconds', 0)}")
    say("RESULT", f"graphics {state['status']} {detail}")
    if state["status"] != "ok":
        say("WARN", f"graphics preparation {state['status']}: {state.get('reason', '')} (the game compiles what it "
                    "lacks while playing, as without this step)")


def run_graphics(a, ws, exe, data):
    """The runtime in its preparation mode; returns the facts for work/graphics.json."""
    import mmap
    import time
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import pipeline_catalog as pc
    with open(exe, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        if m.find(RUNTIME_MARK) < 0:
            return {"status": "skipped", "reason": "the executable predates the graphics preparation (run the build "
                                                   "step to rebuild it)"}
    # the project's catalog, checked, installed in data/ (atomically, only when it changed)
    try:
        records, damaged, _, _ = pc.read(PROJECT_CATALOG)
    except (OSError, SystemExit) as e:
        return {"status": "skipped", "reason": f"no usable pipeline catalog ({e})"}
    if not records:
        return {"status": "skipped", "reason": "the pipeline catalog is empty"}
    say("LOG", f"pipeline catalog: {len(records)} records ({sum(pc.kind(r) == pc.COMPUTE for r in records)} compute), "
               f"{damaged} damaged")
    installed = os.path.join(data, INSTALLED_CATALOG)
    if not os.path.isfile(installed) or sha256_file(installed) != sha256_file(PROJECT_CATALOG):
        shutil.copyfile(PROJECT_CATALOG, installed + ".tmp")
        os.replace(installed + ".tmp", installed)
    catalogs = [installed] + ([os.path.join(data, LOCAL_CATALOG)] if os.path.isfile(os.path.join(data, LOCAL_CATALOG))
                              else [])
    # The runtime's own stamp would let it skip; this step only runs it when something changed, so it prepares.
    for name in os.listdir(data):
        if re.fullmatch(r"pipelines_[0-9a-f]{4}_[0-9a-f]{4}\.stamp", name):
            os.remove(os.path.join(data, name))
    log = os.path.join(ws, "work", "graphics.log")
    if os.path.isfile(log):
        os.remove(log)
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("NX_")}  # no measurement switch leaks in
    env.update(NX_DATA_ROOT=data, NX_HEADLESS="1", NX_MUTE="1", NX_LOG=log, NX_PIPELINE_PREWARM=";".join(catalogs),
               NX_PREWARM_MODE="exit", NX_PREWARM_THREADS=str(a.graphics_jobs))
    if a.graphics_salt:  # development: a cold driver for these modules (a measurement, not a new PC)
        env["NX_SPIRV_SALT"] = a.graphics_salt
        say("WARN", "graphics: NX_SPIRV_SALT set (--graphics-salt): a measurement, not a normal preparation")
    flags = 0x4000 if sys.platform == "win32" else 0  # BELOW_NORMAL_PRIORITY_CLASS: the PC stays usable
    t0 = time.time()
    p = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, creationflags=flags)
    total = compute = made = failed = 0
    started = skipped_by_stamp = False
    seen, last_pct, text = 0, -1, ""
    cpu = peak = None
    reason = ""
    while True:
        exited = p.poll() is not None
        if os.path.isfile(log):
            with open(log, encoding="utf-8", errors="replace") as f:
                f.seek(seen)
                chunk = f.read()
                seen = f.tell()
            text += chunk
            for line in chunk.splitlines():
                if m := re.search(r"pipeline catalog: (\d+) pipelines \((\d+) compute\) from", line):
                    total, compute, started = int(m[1]), int(m[2]), True
                    say("LOG", line.split("nvn/vk: ", 1)[-1])
                    say("COUNT", f"0/{total} pipelines")
                elif m := re.search(r"pipeline catalog: progress (\d+)/(\d+)", line):
                    pct = int(m[1]) * 100 // max(1, int(m[2]))
                    if pct != last_pct:
                        say("PROGRESS", str(min(99, pct)))
                        say("COUNT", f"{m[1]}/{m[2]} pipelines")
                        last_pct = pct
                elif m := re.search(r"pipeline catalog: prepared \((\d+) made, (\d+) failed\)", line):
                    made, failed = int(m[1]), int(m[2])
                    say("LOG", line.split("nvn/vk: ", 1)[-1])
                elif "pipeline catalog: already prepared" in line:
                    skipped_by_stamp = True
                elif re.search(r"pipeline catalog .*(another format|damaged)|pipeline cache .*(damaged|another GPU|"
                               r"too short)", line):
                    say("LOG", line.split("nvn/vk: ", 1)[-1])
        c, pk = process_usage(int(p._handle)) if sys.platform == "win32" else (None, None)
        cpu, peak = c if c is not None else cpu, max(peak or 0, pk or 0) or peak
        if exited:
            break
        elapsed = time.time() - t0
        if elapsed > a.graphics_timeout or (not started and elapsed > GRAPHICS_START_LIMIT):
            reason = (f"no answer from the runtime after {elapsed:.0f} s" if not started else
                      f"stopped after {a.graphics_timeout} s")
            p.kill()
            p.wait()
            break
        time.sleep(0.25)
    code = p.wait()
    seconds = round(time.time() - t0, 1)
    caches = [n for n in os.listdir(data) if re.fullmatch(r"pipelines_[0-9a-f]{4}_[0-9a-f]{4}\.bin", n)]
    cache = max(caches, key=lambda n: os.path.getmtime(os.path.join(data, n))) if caches else None
    cache_ok = bool(cache) and cache_file_ok(os.path.join(data, cache))
    status = EXIT_CODES.get(code, "failed") if not reason else "failed"
    if status == "ok" and not cache_ok:
        status, reason = "partial", "the pipeline cache file is missing or unreadable"
    if not reason and status != "ok":
        reason = {4: f"{failed} pipelines refused by the driver", 5: "the pipeline cache could not be saved"}.get(
            code, f"the runtime exited with {code}" + ("" if started else " before reading the catalog"))
    facts = {"status": status, "exit_code": code, "total": total, "compute": compute, "made": made, "failed": failed,
             "seconds": seconds, "cpu_seconds": round(cpu, 1) if cpu is not None else None,
             "peak_memory_mb": round(peak / 2**20) if peak else None, "threads": a.graphics_jobs,
             "skipped_by_stamp": skipped_by_stamp, "cache": cache,
             "cache_bytes": os.path.getsize(os.path.join(data, cache)) if cache else 0,
             "catalog_bytes": os.path.getsize(installed)}
    if reason:
        facts["reason"] = reason
    say("LOG", f"graphics: {status} in {seconds} s (exit {code}); {made} of {total} made, {failed} failed; CPU "
               f"{facts['cpu_seconds']} s; peak memory {facts['peak_memory_mb']} MB; cache {cache} "
               f"({facts['cache_bytes'] / 2**20:.1f} MB)")
    return facts


# ---- textures (optional): the ASTC -> BC7 cache for NX_ASTC_BC7 (off by default) -------------------------------------
#
# The runtime does the work (runtime/src/nvn/nvn_astc_bc7_prepare.cpp, NX_ASTC_BC7_PREPARE=1): it reads every ASTC
# texture of the package, converts and judges it into data/astc_bc7_cache (verdicts already there are skipped, so a
# stopped run continues) and exits. A game started with NX_ASTC_BC7=1 then keeps the accepted ones as BC7 instead of
# RGBA8 from its first session. An optimization of an optional mode: it never fails the preparation. A few minutes on
# a desktop CPU and about 3 GB of cache for the whole game.

TEXTURES_STATE = "textures.json"   # in work/
TEXTURES_CACHE = "astc_bc7_cache"  # in data/
TEXTURES_MARK = b"NX_ASTC_BC7_PREPARE"
TEXTURES_START_LIMIT = 120  # seconds for the runtime to report the package's files


def folder_size(path):
    total = files = 0
    try:
        with os.scandir(path) as it:
            for e in it:
                if e.is_file():
                    total, files = total + e.stat().st_size, files + 1
    except OSError:
        pass
    return files, total


def write_textures_state(ws, state):
    import json
    path = os.path.join(ws, "work", TEXTURES_STATE)
    with open(path + ".tmp", "w", encoding="utf-8") as f:
        json.dump(state, f, indent=1)
    os.replace(path + ".tmp", path)


def step_textures(a, ws):
    import time
    exe = os.path.join(ws, "bin", "wonder2.exe")
    if not os.path.isfile(exe):
        raise NeedsUser("bin/wonder2.exe is missing: run the build step first")
    state = {"format": 1, "status": "failed", "finished": None, "reason": "interrupted before it finished"}
    write_textures_state(ws, state)
    try:
        state.update(run_textures(a, ws, exe, os.path.join(ws, "data")))
        if state["status"] == "ok":
            state.pop("reason", None)
    except Exception as e:  # noqa: BLE001 - an optimization: the installation stays playable
        state.update(status="failed", reason=f"{type(e).__name__}: {e}")
    state["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
    write_textures_state(ws, state)
    say("RESULT", f"textures {state['status']} converted={state.get('converted', 0)} accepted={state.get('accepted', 0)} "
                  f"kept={state.get('kept', 0)} seconds={state.get('seconds', 0)}")
    if state["status"] != "ok":
        say("WARN", f"texture preparation {state['status']}: {state.get('reason', '')} (with NX_ASTC_BC7=1 the game "
                    "converts what is missing while it plays, for the next session)")


def run_textures(a, ws, exe, data):
    """The runtime in its BC7 preparation mode; returns the facts for work/textures.json."""
    import mmap
    import time
    with open(exe, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        if m.find(TEXTURES_MARK) < 0:
            return {"status": "skipped", "reason": "the executable predates the texture preparation (run the build "
                                                   "step to rebuild it)"}
    log = os.path.join(ws, "work", "textures.log")
    if os.path.isfile(log):
        os.remove(log)
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("NX_")}  # no measurement switch leaks in
    env.update(NX_DATA_ROOT=data, NX_HEADLESS="1", NX_MUTE="1", NX_LOG=log, NX_ASTC_BC7_PREPARE="1",
               NX_ASTC_BC7_PREPARE_THREADS=str(a.texture_jobs))
    flags = 0x4000 if sys.platform == "win32" else 0  # BELOW_NORMAL_PRIORITY_CLASS: the PC stays usable
    t0 = time.time()
    p = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, creationflags=flags)
    facts = {"converted": 0, "accepted": 0, "kept": 0, "already": 0, "files": 0}
    started = finished = False
    seen, last_pct, reason = 0, -1, ""
    cpu = peak = None
    while True:
        exited = p.poll() is not None
        if os.path.isfile(log):
            with open(log, encoding="utf-8", errors="replace") as f:
                f.seek(seen)
                chunk = f.read()
                seen = f.tell()
            for line in chunk.splitlines():
                if m := re.search(r"astc bc7 prepare: (\d+) files", line):
                    facts["files"], started = int(m[1]), True
                    say("LOG", line)
                    say("COUNT", f"0/{m[1]} files")
                elif m := re.search(r"astc bc7 prepare: progress (\d+)/(\d+)", line):
                    pct = int(m[1]) * 100 // max(1, int(m[2]))
                    if pct != last_pct:
                        say("PROGRESS", str(min(99, pct)))
                        say("COUNT", f"{m[1]}/{m[2]} files")
                        last_pct = pct
                elif m := re.search(r"astc bc7 prepare: done .*converted (\d+) .*accepted (\d+), kept RGBA8 (\d+), .*"
                                    r"already in the cache (\d+)", line):
                    facts.update(converted=int(m[1]), accepted=int(m[2]), kept=int(m[3]), already=int(m[4]))
                    finished = True
                    say("LOG", line)
                elif "astc bc7 prepare: no package" in line or "cannot use the cache folder" in line:
                    say("LOG", line.split("nvn/vk: ", 1)[-1])
        c, pk = process_usage(int(p._handle)) if sys.platform == "win32" else (None, None)
        cpu, peak = c if c is not None else cpu, max(peak or 0, pk or 0) or peak
        if exited:
            break
        elapsed = time.time() - t0
        if elapsed > a.textures_timeout or (not started and elapsed > TEXTURES_START_LIMIT):
            reason = (f"no answer from the runtime after {elapsed:.0f} s" if not started else
                      f"stopped after {a.textures_timeout} s (run the step again: it continues)")
            p.kill()
            p.wait()
            break
        time.sleep(0.25)
    code = p.wait()
    files, size = folder_size(os.path.join(data, TEXTURES_CACHE))
    status = "failed" if reason else "ok" if code == 0 and finished else "partial" if code == 5 else "failed"
    if not reason and status != "ok":
        reason = {3: "no package or no cache folder", 5: "some verdicts could not be written (disk full?)"}.get(
            code, f"the runtime exited with {code}")
    st = os.stat(exe)  # which game the cache was prepared for: the launcher offers it again only for another one
    facts.update(status=status, exit_code=code, seconds=round(time.time() - t0, 1), exe_size=st.st_size,
                 exe_mtime=int(st.st_mtime),
                 cpu_seconds=round(cpu, 1) if cpu is not None else None,
                 peak_memory_mb=round(peak / 2**20) if peak else None, threads=a.texture_jobs, cache_files=files,
                 cache_bytes=size)
    if reason:
        facts["reason"] = reason
    say("LOG", f"textures: {status} in {facts['seconds']} s (exit {code}); converted {facts['converted']} (accepted "
               f"{facts['accepted']}, kept RGBA8 {facts['kept']}), {facts['already']} already there; CPU "
               f"{facts['cpu_seconds']} s; peak memory {facts['peak_memory_mb']} MB; cache {files} files "
               f"({size / 2**30:.2f} GB)")
    return facts


def step_config(a, ws):
    os.makedirs(os.path.join(ws, "data", "user"), exist_ok=True)
    with open(os.path.join(ws, "data", "nup.txt"), "w", encoding="utf-8") as f:
        f.write(os.path.abspath(a.nup) + "\n")


def done(step, ws, a):
    if a.force:
        return False
    if step == "verify":
        return False  # always re-checked: it is fast and sets the package index
    if step == "extract":
        return all(os.path.isfile(os.path.join(ws, "work", "exefs", m)) for m in MODULES)
    if step == "bins":
        return all(os.path.isfile(os.path.join(ws, "data", "analysis", m + ".bin")) for m in a.modules)
    if step in ("recompile", "build", "check"):
        return False  # each checks its own inputs and outputs (SHA-256), or must run every time (check)
    if step == "graphics":
        return graphics_done(ws)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nup", required=True)
    ap.add_argument("--workspace", required=True)
    ap.add_argument("--steps", default=",".join(STEPS))
    ap.add_argument("--modules", default=",".join(MODULES))
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--harness", help="a prebuilt spirv_harness.exe (else it is built into the workspace)")
    ap.add_argument("--ryubing-archive", help="the pinned Ryubing source archive (else it is downloaded)")
    ap.add_argument("--spirv-val", help="spirv-val.exe (else from VULKAN_SDK or PATH)")
    ap.add_argument("--shader-sample", help="translate only the code hashes listed in this file (a test)")
    ap.add_argument("--toolchain-dir", help="a folder holding the build step's pinned archives (offline)")
    ap.add_argument("--components", help="Reflora's portable components (self-contained shader translator, "
                                         "spirv-val; tools/reflora_components.py): no .NET or Vulkan SDK needed")
    ap.add_argument("--build-jobs", type=int, default=0,
                    help="compile jobs (default: what fits the free memory, at most 8)")
    ap.add_argument("--graphics-jobs", type=int, default=max(1, min(8, (os.cpu_count() or 4) - 2)),
                    help="threads compiling pipelines in the graphics step (default: logical CPUs - 2, at most 8)")
    ap.add_argument("--graphics-timeout", type=int, default=600,
                    help="seconds before the graphics step gives up (default 600)")
    ap.add_argument("--texture-jobs", type=int, default=max(1, min(16, (os.cpu_count() or 4) - 2)),
                    help="threads converting in the optional textures step (default: logical CPUs - 2, at most 16)")
    ap.add_argument("--textures-timeout", type=int, default=3600,
                    help="seconds before the textures step stops (default 3600; run it again, it continues)")
    ap.add_argument("--graphics-salt", help="development only: NX_SPIRV_SALT for the graphics step (a cold-driver "
                                            "measurement, not a normal preparation)")
    a = ap.parse_args()
    a.modules = [m for m in a.modules.split(",") if m]
    steps = [s for s in a.steps.split(",") if s]
    bad = [s for s in steps + a.modules if s not in STEPS + OPTIONAL_STEPS and s not in MODULES]
    if bad:
        say("ERROR", "unknown step or module: " + ", ".join(bad))
        return 1
    if not os.path.isfile(a.nup):
        say("ERROR", f"package not found: {a.nup}")
        return 1
    ws = os.path.abspath(a.workspace)
    os.makedirs(os.path.join(ws, "work"), exist_ok=True)
    # the mark that lets tools/maxwell/containers.py extract game data here (and nowhere else outside private/)
    mark = os.path.join(ws, ".nx-workspace")
    if not os.path.isfile(mark):
        with open(mark, "w", encoding="utf-8") as f:
            f.write("Workspace of tools/prepare_switch2.py: holds data derived from the user's own game files.\n")
    funcs = {s: globals()["step_" + s] for s in STEPS + OPTIONAL_STEPS}
    full = steps == STEPS
    marker = os.path.join(ws, "work", PREPARED_MARKER)
    if full and os.path.isfile(marker):
        os.remove(marker)  # until this run reaches DONE, the workspace is not "prepared"
    for i, s in enumerate(steps, 1):
        if done(s, ws, a):
            say("STEP", f"{i}/{len(steps)} {s} already done (skipped)")
            continue
        if s == "graphics":
            graphics_starting(ws)
        say("STEP", f"{i}/{len(steps)} {s}")
        try:
            funcs[s](a, ws)
        except NeedsUser as e:
            say("ERROR", str(e))
            return 2
        except Exception as e:  # noqa: BLE001 - reported to the user with the log above
            say("ERROR", f"{s}: {e}")
            return 1
        say("PROGRESS", "100")  # the end of this step (PROGRESS is always progress inside the current step)
        if full and s == PLAYABLE_AFTER:
            write_marker(marker, a)  # playable from here: what follows (graphics) only optimizes
    if full:
        write_marker(marker, a)
    say("DONE")
    return 0


def write_marker(path, a):
    """work/prepared.json: a whole preparation reached DONE (the launcher shows "ready" from it)."""
    import json
    import time
    info = {"format": 1, "nup": os.path.abspath(a.nup), "finished": time.strftime("%Y-%m-%d %H:%M:%S"),
            "steps": STEPS}
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(info, f, indent=1)
    os.replace(tmp, path)


if __name__ == "__main__":
    sys.exit(main())
