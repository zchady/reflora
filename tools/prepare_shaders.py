"""The `shaders` step of tools/prepare_switch2.py: the game's shaders from the user's NUP to SPIR-V, resumable.

Phases (all under <workspace>/work/shaders/, the result in <workspace>/data/spirv/):
  scan       every RomFS file of the program (audio/video skipped) and the flat `main` image are searched for shader
             code (tools/maxwell/shader_sources.py); the blocks with an NVN 2 control section (the Switch 2 build)
             are kept. One group per source file, named effect_/layout_/lib_/shader_<name> or exefs_main; each code
             block is assigned to the first group that holds it, in the order the harness reads groups.
  harness    the Maxwell -> SPIR-V translator (tools/maxwell/spirv_harness): a prebuilt one (--harness) or built
             into the workspace from the pinned Ryubing source (archive verified by SHA-256 and by content).
  extract    per group: its source re-read from the package, every stage written to sh/<group>/ (containers.py).
  lower      per group: tools/ampere/lower.py --batch --only <the group's blocks> -> low/<group>/ (+ failures.tsv).
  translate  per group: spirv_harness --batch -> spv/<group>/ (*.spv + results.tsv), checked with spirv-val.
  install    data/spirv/<hash>.spv + one results.tsv, what the runtime loads (nvn_shaders.cpp).

state.json records, per phase and group, a fingerprint of its inputs (package TOC, the source file's SHA-256, the
versions of the extractor / lowering / harness, the blocks assigned) and the SHA-256 of every file it wrote. A phase
is redone when its fingerprint changed or a file no longer matches: an output that merely exists is never trusted.
Blocks the lowering does not support yet are not an error: they are listed in unsupported.tsv with their source and
reason, and the report gives the coverage. Tool failures (a crash, a missing output) are errors.

Machine-readable lines (besides prepare_switch2's): "GROUP <i>/<n> <group> <phase> <result>" and "PROGRESS <0-100>"
(of this step).
"""
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import threading
import time
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, os.path.join(REPO, "tools", "maxwell"))
import containers as ct  # noqa: E402
import nup as nupmod  # noqa: E402
import shader_sources as ss  # noqa: E402

PIPELINE_VERSION = 1   # bump when this file changes what a phase produces
STATE_VERSION = 1
TARGET_ENV = "vulkan1.2"
SKIP_EXT = {"bwav", "webm"}  # audio, video: never shader code (checked by the inventory, §5za)
NVN_CONTROL = 0x98761234
NVN2_CONTROL_VERSION = 0xC9  # second word of an NVN 2 control section (lower.py refuses the others)

# Ryujinx's shader translator (MIT), maintained fork Ryubing, as the harness was developed and checked against.
RYUBING = {
    "commit": "a82350bb77",
    "url": "https://codeberg.org/Software_arch/Ryubing/archive/a82350bb77.tar.gz",
    "sha256": "aba9789bdcf3fda9e90f965636e9e85e9dcc299333fbf5451bcf22ecfd608506",
    # sha256 over the sorted "<member path>\t<sha256 of its bytes>\n" lines of every file in the archive: accepts a
    # re-compressed archive of the same tree
    "content": "8bd7581656dec08b456a51b624870ba7fe6f054465cfc48699afe4e62e8f8471",
}

RYUJINX_DECODERS = ("InstTable.cs", "InstDecoders.cs")  # read by tools/ampere/maxwell.py
EXTRACTOR_FILES = ["tools/nup.py", "tools/nxcontent.py", "tools/maxwell/containers.py",
                   "tools/maxwell/shader_sources.py"]
LOWERING_FILES = ["tools/ampere/lower.py", "tools/ampere/maxwell.py", "tools/ampere/sm86.py"]
HARNESS_SOURCES = ["tools/maxwell/apply_patch.py", "tools/maxwell/spirv_harness/Directory.Build.props",
                   "tools/maxwell/spirv_harness/SpirvHarness.csproj", "tools/maxwell/spirv_harness/Program.cs",
                   "tools/maxwell/spirv_harness/Equiv.cs", "tools/maxwell/spirv_harness/IrInterp.cs",
                   "tools/maxwell/spirv_harness/IrRun.cs", "tools/maxwell/spirv_harness/StaticGpuAccessor.cs",
                   "tools/maxwell/spirv_harness/translator/Ryujinx.Graphics.Shader.csproj",
                   "tools/maxwell/spirv_harness/translator/spirv-else-if.patch",
                   "tools/maxwell/spirv_harness/translator/spirv-fmz.patch"]


class NeedsUser(Exception):
    """Something only the user can provide (a tool to install, a setting)."""


def load_components(root, names):
    """{name: directory} of Reflora's portable components (tools/reflora_components.py) after checking every file of
    the named ones against components.json (SHA-256 and size). A changed or missing file is an error, not a fallback."""
    import json as _json
    try:
        man = _json.load(open(os.path.join(root, "components.json"), encoding="utf-8"))["components"]
    except (OSError, ValueError, KeyError) as e:
        raise NeedsUser(f"{root}: no valid components.json ({e})")
    out = {}
    for name in names:
        if name not in man:
            raise NeedsUser(f"{root}: component {name} missing from components.json")
        d = os.path.join(root, name)
        for rel, (sha, size) in man[name]["files"].items():
            f = os.path.join(d, rel)
            if not os.path.isfile(f) or os.path.getsize(f) != size or sha256_file(f) != sha:
                raise NeedsUser(f"component {name}: {rel} is missing or changed (reinstall the components)")
        out[name] = d
    return out


# ---- hashing and files ---------------------------------------------------------------------------------------------

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def digest(items):
    """A fingerprint of a mapping or a list of strings (order-independent for mappings)."""
    if isinstance(items, dict):
        lines = sorted(f"{k}\t{v}\n" for k, v in items.items())
    else:
        lines = [f"{x}\n" for x in items]
    return hashlib.sha256("".join(lines).encode("utf-8")).hexdigest()


def repo_files_fp(paths):
    return digest({p: sha256_file(os.path.join(REPO, p)) for p in paths})


def dir_files(path):
    """name -> sha256 for every file directly in `path`."""
    return {n: sha256_file(os.path.join(path, n)) for n in sorted(os.listdir(path))
            if os.path.isfile(os.path.join(path, n))}


def write_atomic(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(tmp, path)


def group_name(path):
    """romfs/Effect/static.Nin_Ounce_NVN2.esetb.byml.zs -> effect_static; extra/main.bin -> exefs_main."""
    if path.startswith("extra/"):
        return "exefs_" + os.path.basename(path).split(".")[0]
    parts = path.split("/")
    stem = parts[-1].split(".Nin_")[0]
    stem = re.sub(r"[^A-Za-z0-9._-]+", "_", stem)
    return f"{parts[1].lower()}_{stem}"


def fetch_ryubing(tools, archive_given=None, say=lambda *a: None):
    """<tools>/ryubing, extracted from the pinned Ryubing archive (given, or downloaded) after checking it."""
    dst = os.path.join(tools, "ryubing")
    mark = os.path.join(dst, ".verified")
    if os.path.isfile(mark) and open(mark, encoding="utf-8").read().strip() == RYUBING["content"]:
        return dst
    os.makedirs(os.path.join(tools, "downloads"), exist_ok=True)
    archive = archive_given or os.path.join(tools, "downloads", f"ryubing-{RYUBING['commit']}.tar.gz")
    if not os.path.isfile(archive):
        say("LOG", f"downloading the Ryubing shader translator source {RYUBING['commit']} (MIT): "
                        f"{RYUBING['url']}")
        tmp = archive + ".part"
        with urllib.request.urlopen(RYUBING["url"], timeout=120) as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f)
        os.replace(tmp, archive)
    if sha256_file(archive) != RYUBING["sha256"]:
        # a re-compressed archive of the same tree is fine; anything else is refused
        with tarfile.open(archive) as t:
            lines = sorted(f"{m.name}\t{hashlib.sha256(t.extractfile(m).read()).hexdigest()}\n"
                           for m in t.getmembers() if m.isfile())
        if hashlib.sha256("".join(lines).encode()).hexdigest() != RYUBING["content"]:
            raise RuntimeError(f"{archive} is not the Ryubing source {RYUBING['commit']} this project was "
                               "checked with (SHA-256 and content differ): refusing to build it")
    tmpdir = dst + ".tmp"
    if os.path.isdir(tmpdir):
        shutil.rmtree(tmpdir)
    with tarfile.open(archive) as t:
        t.extractall(tmpdir, filter="data")  # no links or paths outside tmpdir
    inner = [os.path.join(tmpdir, n) for n in os.listdir(tmpdir)]
    src = inner[0] if len(inner) == 1 and os.path.isdir(inner[0]) else tmpdir
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.move(src, dst)
    shutil.rmtree(tmpdir, ignore_errors=True)
    write_atomic(mark, RYUBING["content"] + "\n")
    return dst


class Pipeline:
    def __init__(self, nup_path, package, ws, say, jobs=4, harness=None, ryubing_archive=None, spirv_val=None,
                 sample=None, components=None):
        self.nup_path, self.package, self.ws, self.say = nup_path, package, ws, say
        # Reflora's portable components (self-contained harness, spirv-val): when given, nothing installed on the
        # PC (.NET, Vulkan SDK, PATH) is ever looked at.
        self.components = load_components(components, ["spirv_harness", "spirv-val"]) if components else None
        self.jobs = max(1, jobs)
        self.harness_arg, self.ryubing_archive, self.spirv_val_arg = harness, ryubing_archive, spirv_val
        self.sample = sample  # None or a set of code hashes (a controlled test, not a complete preparation)
        self.root = os.path.join(ws, "work", "shaders")
        self.state_path = os.path.join(self.root, "state.json")
        self.lock = threading.Lock()
        self.main_bin = os.path.join(ws, "data", "analysis", "main.bin")
        for d in ("", "sh", "low", "spv", "assign", "logs", "scan"):
            os.makedirs(os.path.join(self.root, d), exist_ok=True)
        self.state = self.load_state()

    # ---- state ----------------------------------------------------------------------------------------------------

    def load_state(self):
        try:
            with open(self.state_path, encoding="utf-8") as f:
                s = json.load(f)
            if s.get("version") == STATE_VERSION:
                return s
        except (OSError, ValueError):
            pass
        return {"version": STATE_VERSION, "groups": {}}

    def save_state(self):
        with self.lock:
            write_atomic(self.state_path, json.dumps(self.state, indent=1, sort_keys=True))

    def inside_root(self, path):
        real, root = os.path.realpath(path), os.path.realpath(self.root)
        if not (real == root or real.startswith(root + os.sep)):
            raise RuntimeError(f"refusing to touch {path}: not inside {self.root}")
        return real

    def clear(self, path):
        """Remove a directory this pipeline made (never anything outside work/shaders)."""
        p = self.inside_root(path)
        if os.path.isdir(p):
            shutil.rmtree(p)

    # ---- tools ----------------------------------------------------------------------------------------------------

    def ryujinx_shader_src(self):
        cands = []
        if self.components:
            cands.append(os.path.join(self.components["spirv_harness"], "ryujinx-shader"))
        else:
            cands.append(os.path.join(REPO, "private", "third_party", "ryubing", "src", "Ryujinx.Graphics.Shader"))
        for c in cands:
            if all(os.path.isfile(os.path.join(c, "Decoders", f)) for f in RYUJINX_DECODERS):
                return c
        if self.components:
            raise NeedsUser("the translator component has no ryujinx-shader/Decoders (rebuild the components)")
        ryu = self.ryubing_source()  # development without the checkout: the pinned, verified source
        return os.path.join(ryu, "src", "Ryujinx.Graphics.Shader")

    def find_spirv_val(self):
        if self.spirv_val_arg:
            return os.path.abspath(self.spirv_val_arg)
        if self.components:
            return os.path.join(self.components["spirv-val"], "spirv-val.exe")
        cands = []  # development machines without the components: the Vulkan SDK's or one on PATH
        sdk = os.environ.get("VULKAN_SDK")
        if sdk:
            cands.append(os.path.join(sdk, "Bin", "spirv-val.exe"))
        cands.append(shutil.which("spirv-val"))
        for c in cands:
            if c and os.path.isfile(c):
                return os.path.abspath(c)
        raise NeedsUser("spirv-val (SPIRV-Tools, Apache-2.0) was not found: install the Vulkan SDK from LunarG or "
                        "pass --spirv-val <path to spirv-val.exe>, then run this step again.")

    def find_dotnet(self):
        exe = shutil.which("dotnet") or os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"),
                                                     "dotnet", "dotnet.exe")
        if not os.path.isfile(exe):
            return None, None
        out = subprocess.run([exe, "--list-sdks"], stdout=subprocess.PIPE, text=True).stdout
        sdks = [l.split()[0] for l in out.splitlines() if l.strip()]
        ten = [v for v in sdks if v.startswith("10.")]
        return exe, (ten[-1] if ten else None)

    def ryubing_source(self):
        """<ws>/tools/ryubing, extracted from the pinned archive after checking it."""
        return fetch_ryubing(os.path.join(self.ws, "tools"), self.ryubing_archive, self.say)

    def prepare_harness(self):
        """(path to spirv_harness.exe, fingerprint of what translates)."""
        if self.harness_arg:
            exe = os.path.abspath(self.harness_arg)
            if not os.path.isfile(exe):
                raise NeedsUser(f"--harness {exe} does not exist")
            self.say("LOG", f"harness: {exe} (given)")
        elif self.components:
            exe = os.path.join(self.components["spirv_harness"], "spirv_harness.exe")
            self.say("LOG", f"harness: {exe} (Reflora component, self-contained: no .NET install used)")
        else:
            build = os.path.join(self.ws, "tools", "spirv_harness")
            exe = os.path.join(build, "bin", "Release", "net10.0", "spirv_harness.exe")
            dotnet, sdk = self.find_dotnet()
            src_fp = digest({"sources": repo_files_fp(HARNESS_SOURCES), "ryubing": RYUBING["content"],
                             "sdk": sdk or "none"})
            rec = self.state.get("harness", {})
            bin_dir = os.path.dirname(exe)
            if not (rec.get("sources") == src_fp and os.path.isfile(exe) and rec.get("bin") == digest(
                    dir_files(bin_dir))):
                if not sdk:
                    raise NeedsUser("the .NET 10 SDK (Microsoft, MIT) is needed once, to build the shader translator: "
                                    "install it from dot.net or pass --harness <spirv_harness.exe>, then run this "
                                    "step again.")
                ryu = self.ryubing_source()
                if os.path.isdir(build):
                    shutil.rmtree(build)
                cmd = [dotnet, "build", os.path.join(REPO, "tools", "maxwell", "spirv_harness", "SpirvHarness.csproj"),
                       "-c", "Release", "-nologo", "-v:minimal",
                       "-p:RyuSrc=" + os.path.join(ryu, "src").replace("\\", "/") + "/",
                       "-p:HarnessBuildRoot=" + build.replace("\\", "/") + "/",
                       "-p:PatchPython=" + sys.executable]
                self.say("LOG", f"building the shader translator (.NET SDK {sdk}, Ryubing {RYUBING['commit']})")
                self.run_logged(cmd, os.path.join(self.root, "logs", "harness.build.log"))
                if not os.path.isfile(exe):
                    raise RuntimeError("the harness build finished without spirv_harness.exe")
                self.state["harness"] = {"sources": src_fp, "bin": digest(dir_files(bin_dir)), "sdk": sdk}
                self.save_state()
            self.say("LOG", f"harness: {exe} (built from Ryubing {RYUBING['commit']})")
        bin_dir = os.path.dirname(exe)
        val = self.find_spirv_val()
        ver = subprocess.run([val, "--version"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True).stdout.splitlines()
        fp = digest({"bin": digest({n: h for n, h in dir_files(bin_dir).items()
                                    if n.endswith((".dll", ".exe", ".json"))}),
                     "target_env": TARGET_ENV, "spirv_val": ver[0] if ver else "?", "retry": "on"})
        return exe, val, fp

    def run_logged(self, cmd, log_path, cwd=REPO, extra_env=None):
        env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUTF8="1", DOTNET_CLI_TELEMETRY_OPTOUT="1",
                   DOTNET_NOLOGO="1", **(extra_env or {}))
        with open(log_path, "w", encoding="utf-8", newline="\n") as log:
            log.write("$ " + subprocess.list2cmdline(cmd) + "\n")
            log.flush()
            p = subprocess.run(cmd, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT)
        if p.returncode != 0:
            tail = open(log_path, encoding="utf-8", errors="replace").read().splitlines()[-15:]
            for line in tail:
                self.say("LOG", line)
            raise RuntimeError(f"{os.path.basename(cmd[0])} failed ({p.returncode}), log: {log_path}")

    # ---- scan -----------------------------------------------------------------------------------------------------

    def nvn2_stages(self, data, name):
        stages = ss.scan_data(data, name, {}, {})[0]
        out = []
        for s in stages:
            if s.ctrl_off + 8 <= len(data) and int.from_bytes(data[s.ctrl_off:s.ctrl_off + 4], "little") == \
                    NVN_CONTROL and int.from_bytes(data[s.ctrl_off + 4:s.ctrl_off + 8], "little") == \
                    NVN2_CONTROL_VERSION:
                out.append(s)
        return out

    def sources(self, n):
        """[(path, nup FileNode or None, sha256)] in scan order: RomFS (sorted) then the main image."""
        pkg = n.packages[self.package]
        files = n.files_of(n.subtree(pkg, "romfs"))
        out = []
        for path, fn in sorted(files.items()):
            ext = path.rsplit(".", 2)
            e = (ext[-2] if path.endswith(".zs") and len(ext) > 2 else ext[-1]).lower()
            if e not in SKIP_EXT:
                out.append(("romfs/" + path, fn, fn.sha256.hex()))
        if not os.path.isfile(self.main_bin):
            raise NeedsUser(f"{self.main_bin} is missing: run the bins step first")
        out.append(("extra/main.bin", None, sha256_file(self.main_bin)))
        return out

    def read_source(self, n, path, fn):
        if fn is None:
            return open(self.main_bin, "rb").read()
        data = n.read(fn)
        return ss.decompress(data) if path.endswith(".zs") else data

    def scan(self, n):
        fp = digest({"toc": n.toc_sha256.hex(), "package": self.package, "main": sha256_file(self.main_bin),
                     "extractor": repo_files_fp(EXTRACTOR_FILES), "pipeline": PIPELINE_VERSION})
        rec = self.state.get("scan", {})
        out = os.path.join(self.root, "scan", "groups.json")
        if rec.get("fp") == fp and os.path.isfile(out) and sha256_file(out) == rec.get("sha256"):
            self.say("LOG", "scan: unchanged package and tools, using the recorded inventory")
            return json.load(open(out, encoding="utf-8"))
        srcs = self.sources(n)
        groups = {}
        t0 = time.time()
        for k, (path, fn, sha) in enumerate(srcs):
            stages = self.nvn2_stages(self.read_source(n, path, fn), os.path.basename(path))
            if stages:
                g = group_name(path)
                while g in groups:
                    g += "_"
                groups[g] = {"source": path, "sha256": sha, "stages": len(stages),
                             "blocks": {s.hash: s.kind for s in stages}}
            if k % 1000 == 0:
                self.say("PROGRESS", str(int(10 * k / len(srcs))))
        self.say("LOG", f"scan: {len(srcs)} files, {len(groups)} with Switch 2 shader code, "
                        f"{len({h for g in groups.values() for h in g['blocks']})} distinct blocks, "
                        f"{time.time() - t0:.0f} s")
        write_atomic(out, json.dumps(groups, indent=0, sort_keys=True))
        self.state["scan"] = {"fp": fp, "sha256": sha256_file(out)}
        self.save_state()
        return groups

    def assign(self, groups):
        """group -> sorted list of the blocks it lowers/translates (each block once, first group in name order)."""
        owner = {}
        for g in sorted(groups):  # ordinal order, as spirv_harness --batch reads manifests
            for h in groups[g]["blocks"]:
                if self.sample is None or h in self.sample:
                    owner.setdefault(h, g)
        out = {}
        for h, g in owner.items():
            out.setdefault(g, []).append(h)
        return {g: sorted(v) for g, v in sorted(out.items())}

    # ---- per group ------------------------------------------------------------------------------------------------

    def files_ok(self, path, rec):
        return os.path.isdir(path) and rec.get("files") is not None and dir_files(path) == rec["files"]

    def extract(self, n, g, info):
        rec = self.state["groups"].setdefault(g, {}).get("extract", {})
        fp = digest({"source": info["source"], "sha256": info["sha256"], "extractor": self.extractor_fp,
                     "pipeline": PIPELINE_VERSION})
        out = os.path.join(self.root, "sh", g)
        if rec.get("fp") == fp and self.files_ok(out, rec):
            return rec, True
        tmp = out + ".tmp"
        self.clear(tmp)
        ct.check_output_dir(tmp)  # raises OutputRefused (outside a prepared workspace, a foreign folder, ...)
        data = self.read_source(n, info["source"], None if info["source"].startswith("extra/") else
                                self.nup_files["/".join(info["source"].split("/")[1:])])
        stages = self.nvn2_stages(data, os.path.basename(info["source"]))
        ct.extract_stages(stages, tmp, data, unique=True)
        self.clear(out)
        os.replace(tmp, out)
        rec = {"fp": fp, "files": dir_files(out), "stages": len(stages)}
        return rec, False

    def lower(self, g, assigned, ext_rec):
        rec = self.state["groups"][g].get("lower", {})
        assign_file = os.path.join(self.root, "assign", g + ".txt")
        fp = digest({"extract": digest(ext_rec["files"]), "lowering": self.lowering_fp,
                     "assigned": digest(assigned)})
        out = os.path.join(self.root, "low", g)
        if rec.get("fp") == fp and self.files_ok(out, rec):
            return rec, True
        write_atomic(assign_file, "".join(h + "\n" for h in assigned))
        tmp = out + ".tmp"
        self.clear(tmp)
        self.run_logged([sys.executable, "-X", "utf8", os.path.join(REPO, "tools", "ampere", "lower.py"), "--batch",
                         os.path.join(self.root, "sh", g), tmp, "--only", assign_file],
                        os.path.join(self.root, "logs", g + ".lower.log"),
                        extra_env={"NX_RYUJINX_SHADER_SRC": self.ryu_shader})
        lowered = {l.split("\t")[10] for l in open(os.path.join(tmp, "manifest.tsv"), encoding="utf-8")
                   .read().splitlines()[1:]}
        failed = {l.split("\t")[0] for l in open(os.path.join(tmp, "failures.tsv"), encoding="utf-8")
                  .read().splitlines()[1:]}
        missing = set(assigned) - lowered - failed
        if missing:
            raise RuntimeError(f"{g}: lowering gave no result for {len(missing)} blocks (e.g. {sorted(missing)[0]})")
        self.clear(out)
        os.replace(tmp, out)
        return {"fp": fp, "files": dir_files(out), "lowered": len(lowered), "failed": len(failed)}, False

    def translate(self, g, low_rec):
        rec = self.state["groups"][g].get("translate", {})
        fp = digest({"lower": digest(low_rec["files"]), "harness": self.harness_fp})
        out = os.path.join(self.root, "spv", g)
        if rec.get("fp") == fp and self.files_ok(out, rec):
            return rec, True
        tmp = out + ".tmp"
        self.clear(tmp)
        os.makedirs(tmp)
        lowered = {l.split("\t")[10] for l in open(os.path.join(self.root, "low", g, "manifest.tsv"),
                                                   encoding="utf-8").read().splitlines()[1:]}
        if lowered:
            self.run_logged([self.harness_exe, "--batch", os.path.join(self.root, "low", g), "--out", tmp,
                             "--jobs", str(self.harness_jobs), "--spirv-val", self.spirv_val, "--target-env",
                             TARGET_ENV], os.path.join(self.root, "logs", g + ".translate.log"))
        else:
            write_atomic(os.path.join(tmp, "results.tsv"), "hash\n")
        rows = [l.split("\t") for l in open(os.path.join(tmp, "results.tsv"), encoding="utf-8").read().splitlines()[1:]]
        got = {r[0] for r in rows}
        if got != lowered:
            raise RuntimeError(f"{g}: the harness reported {len(got)} blocks, {len(lowered)} were lowered")
        ok = failed = 0
        for r in rows:
            if r[4] in ("ok", "ok-retry"):
                p = os.path.join(tmp, r[0] + ".spv")
                b = open(p, "rb").read() if os.path.isfile(p) else b""
                if len(b) < 20 or len(b) % 4 or b[:4] != b"\x03\x02\x23\x07":
                    raise RuntimeError(f"{g}: {r[0]}.spv is missing or not SPIR-V although the harness said {r[4]}")
                ok += 1
            else:
                failed += 1
        self.clear(out)
        os.replace(tmp, out)
        return {"fp": fp, "files": dir_files(out), "ok": ok, "failed": failed}, False

    def do_group(self, n, g, info, assigned, i, total):
        try:
            for phase, fn in (("extract", lambda: self.extract(n, g, info)),
                              ("lower", lambda: self.lower(g, assigned, self.state["groups"][g]["extract"])),
                              ("translate", lambda: self.translate(g, self.state["groups"][g]["lower"]))):
                rec, reused = fn()
                # (a redone phase changes the fingerprint of the next one, which is then redone too)
                with self.lock:
                    grp = self.state["groups"][g]
                    grp[phase] = rec
                    grp["source"], grp["assigned"] = info["source"], len(assigned)
                    grp.pop("error", None)
                self.save_state()
                detail = {"extract": lambda r: f"{r.get('stages', '?')} stages",
                          "lower": lambda r: f"{r['lowered']} lowered, {r['failed']} unsupported",
                          "translate": lambda r: f"{r['ok']} ok, {r['failed']} failed"}[phase](rec)
                self.say("GROUP", f"{i}/{total} {g} {phase} {'valid (kept)' if reused else 'done'}: {detail}")
            return None
        except NeedsUser:
            raise
        except Exception as e:  # noqa: BLE001 - recorded per group, reported at the end
            with self.lock:
                self.state["groups"].setdefault(g, {})["error"] = f"{type(e).__name__}: {e}"
            self.save_state()
            self.say("GROUP", f"{i}/{total} {g} error: {e}")
            return f"{g}: {e}"

    # ---- install and report ---------------------------------------------------------------------------------------

    def install(self, plan, groups):
        data = os.path.join(self.ws, "data", "spirv")
        rows, files = ["hash\tgroup\tstage\tstages_using\tstatus\tms\tspv_bytes\tresources\ttranslator_messages\t"
                       "error\tdraw_time_queries"], {}
        for g in plan:
            d = os.path.join(self.root, "spv", g)
            for line in open(os.path.join(d, "results.tsv"), encoding="utf-8").read().splitlines()[1:]:
                rows.append(line)
                h, status = line.split("\t")[0], line.split("\t")[4]
                if status in ("ok", "ok-retry"):
                    files[h + ".spv"] = os.path.join(d, h + ".spv")
        fp = digest({"groups": digest({g: digest(self.state["groups"][g]["translate"]["files"]) for g in plan}),
                     "sample": digest(sorted(self.sample)) if self.sample is not None else "complete"})
        rec = self.state.get("install", {})
        marker = os.path.join(data, ".prepared.json")
        if rec.get("fp") == fp and os.path.isfile(marker) and digest(
                {k: v for k, v in dir_files(data).items() if k != ".prepared.json"}) == rec.get("files"):
            self.say("LOG", f"install: {data} is up to date ({len(files)} shaders)")
            return len(files)
        if os.path.isdir(data) and not os.path.isfile(marker):
            keep = data + time.strftime(".before-%Y%m%d-%H%M%S")
            os.replace(data, keep)
            self.say("WARN", f"{data} was not made by this step: kept as {keep}")
        tmp = data + ".tmp"
        if os.path.isdir(tmp):
            shutil.rmtree(tmp)
        os.makedirs(tmp)
        for name, src in files.items():
            shutil.copyfile(src, os.path.join(tmp, name))
        write_atomic(os.path.join(tmp, "results.tsv"), "\n".join(rows) + "\n")
        prepared = {"fp": fp, "shaders": len(files), "complete": self.sample is None,
                    "made": time.strftime("%Y-%m-%d %H:%M:%S")}
        write_atomic(os.path.join(tmp, ".prepared.json"), json.dumps(prepared, indent=1))
        if os.path.isdir(data):
            shutil.rmtree(data)
        os.replace(tmp, data)
        self.state["install"] = {"fp": fp, "files": digest({k: v for k, v in dir_files(data).items()
                                                            if k != ".prepared.json"})}
        self.save_state()
        self.say("LOG", f"install: {len(files)} shaders and results.tsv written to {data}")
        return len(files)

    def report(self, plan, groups, installed):
        unsupported = ["hash\tstage\tgroup\tsource\tid\treason\tmessage"]
        tfail = ["hash\tgroup\tsource\tstatus\terror"]
        for g in plan:
            src = groups[g]["source"]
            for l in open(os.path.join(self.root, "low", g, "failures.tsv"), encoding="utf-8").read().splitlines()[1:]:
                h, stage, sid, reason, msg = l.split("\t")
                unsupported.append(f"{h}\t{stage}\t{g}\t{src}\t{sid}\t{reason}\t{msg}")
            for l in open(os.path.join(self.root, "spv", g, "results.tsv"), encoding="utf-8").read().splitlines()[1:]:
                f = l.split("\t")
                if len(f) > 4 and f[4] not in ("ok", "ok-retry"):
                    tfail.append(f"{f[0]}\t{g}\t{src}\t{f[4]}\t{f[9] if len(f) > 9 else ''}")
        write_atomic(os.path.join(self.root, "unsupported.tsv"), "\n".join(unsupported) + "\n")
        write_atomic(os.path.join(self.root, "translate_failures.tsv"), "\n".join(tfail) + "\n")
        total = sum(len(v) for v in plan.values())
        all_blocks = len({h for g in groups.values() for h in g["blocks"]})
        lines = [
            f"Switch 2 shader blocks in the package: {all_blocks} ({len(groups)} source files)",
            f"in this preparation: {total} blocks in {len(plan)} groups"
            + (" (SAMPLE: a controlled test, not a complete preparation)" if self.sample is not None else ""),
            f"translated and validated (spirv-val {TARGET_ENV}): {installed}",
            f"not lowered (instructions the Ampere -> Maxwell lowering does not support yet): {len(unsupported) - 1}"
            " -> unsupported.tsv",
            f"lowered but not translated: {len(tfail) - 1} -> translate_failures.tsv",
            "coverage is of translation only: the shaders the game actually requests are known only from runs; a "
            "block above that a course requests is not drawn (the runtime logs it and continues)",
        ]
        write_atomic(os.path.join(self.root, "report.txt"), "\n".join(lines) + "\n")
        for l in lines:
            self.say("LOG", l)
        if len(unsupported) > 1 or len(tfail) > 1:
            self.say("WARN", f"shader coverage is not complete: {len(unsupported) - 1} blocks not lowered, "
                             f"{len(tfail) - 1} not translated (see {os.path.join(self.root, 'report.txt')})")

    # ---- run ------------------------------------------------------------------------------------------------------

    def run(self):
        self.extractor_fp = digest({"files": repo_files_fp(EXTRACTOR_FILES), "pipeline": PIPELINE_VERSION})
        # The Maxwell encodings the lowering writes come from two Ryujinx decoder sources (tools/ampere/maxwell.py):
        # the translator component's copy, else the development checkout, else this workspace's verified Ryubing.
        self.ryu_shader = self.ryujinx_shader_src()
        self.lowering_fp = digest({"files": repo_files_fp(LOWERING_FILES), "decoders": digest(
            {f: sha256_file(os.path.join(self.ryu_shader, "Decoders", f)) for f in RYUJINX_DECODERS})})
        with nupmod.Nup(self.nup_path) as n:
            pkg = n.packages[self.package]
            self.nup_files = n.files_of(n.subtree(pkg, "romfs"))
            groups = self.scan(n)
            plan = self.assign(groups)
            self.harness_exe, self.spirv_val, self.harness_fp = self.prepare_harness()
            self.say("PROGRESS", "12")
            workers = max(1, min(self.jobs, len(plan)))
            self.harness_jobs = max(1, self.jobs // workers)
            for stale in [g for g in self.state["groups"] if g not in plan]:
                self.state["groups"].pop(stale)  # not part of this preparation any more (outputs stay on disk)
            errors = []
            names = list(plan)
            done = 0
            # The NUP reader is one file handle: reading the sources is serialized, the work after it is not.
            read_lock = threading.Lock()
            orig_read = n.read

            def locked_read(fn, limit=None):
                with read_lock:
                    return orig_read(fn, limit)
            n.read = locked_read
            with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
                futs = {pool.submit(self.do_group, n, g, groups[g], plan[g], k + 1, len(names)): g
                        for k, g in enumerate(names)}
                for f in concurrent.futures.as_completed(futs):
                    err = f.result()
                    if err:
                        errors.append(err)
                    done += 1
                    self.say("PROGRESS", str(12 + int(83 * done / max(1, len(names)))))
            if errors:
                raise RuntimeError(f"{len(errors)} groups failed (kept for the next run: the others are not "
                                   f"redone): " + "; ".join(errors[:3]))
            installed = self.install(plan, groups)
            self.report(plan, groups, installed)
            self.say("PROGRESS", "100")
            return installed


def read_sample(path):
    out = set()
    for line in open(path, encoding="utf-8"):
        line = line.split("#")[0].strip()
        if line:
            out.add(line.split()[0])
    return out
