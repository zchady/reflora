"""Pack a REFLORA Alpha for testers (WONDER_SWITCH2_PROGRESS.md §5zd7): a folder and a RAR made only from committed
files (git archive of HEAD, never the working tree), the portable components checked against components.json, the
launcher built from HEAD, every license, a README, and an audit that refuses anything private.

usage: python tools/pack_reflora_alpha.py --out <folder outside the repository> --name Reflora-Alpha-02
       [--rar "C:\\Program Files\\WinRAR\\Rar.exe"] [--cargo-home <dir> --rustup-home <dir>] [--no-build]

What goes in (the preparer's needs, nothing else): CMakeLists.txt, LICENSE, runtime/, recomp/, games/wonder2/, the
preparer's tools (TOOLS below), components/ (Python, the shader translator, spirv-val, each with its licenses), the
launcher (REFLORA.exe), licenses/ and LEIA-ME.txt / VERSAO.txt. Never: docs (game screenshots), tests, other games,
private/, saves, NUPs, game files, SPIR-V, pipeline caches, logs, personal paths.
"""
import argparse
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TREE = ["CMakeLists.txt", "LICENSE", "runtime", "recomp", "games/wonder2"]
TOOLS = ["prepare_switch2.py", "prepare_build.py", "prepare_shaders.py", "pipeline_catalog.py", "nup.py",
         "nxcontent.py", "nso2bin.py", "run_recomp.py", "ampere/lower.py", "ampere/maxwell.py", "ampere/sm86.py",
         "maxwell/apply_patch.py", "maxwell/containers.py", "maxwell/shader_sources.py", "maxwell/spirv_harness"]
# Nothing of these may be in the package (game data, generated game code, driver output, personal data).
FORBIDDEN_EXT = {".nup", ".nsp", ".xci", ".nca", ".nso", ".nro", ".npdm", ".spv", ".bfttf", ".bfotf", ".sav", ".keys",
                 ".ppm", ".bmp", ".log", ".tsv.gz", ".pyc"}
ALLOWED_BIN = {"games/wonder2/pipeline_catalog.bin"}  # the catalog: shader hashes and numbers (checked below)
# This machine's identifiers: never anywhere in the package. Read here, never written in the source: the Windows user
# name, the repository's folder and its parent, the author of HEAD (name and e-mail), plus REFLORA_AUDIT_PERSONAL
# (';'-separated) for anything else.
def _personal():
    words = [os.environ.get("USERNAME", ""), os.path.basename(REPO), os.path.basename(os.path.dirname(REPO))]
    try:
        words += subprocess.run(["git", "-C", REPO, "log", "-1", "--format=%an%n%ae"], capture_output=True,
                                text=True).stdout.splitlines()
    except OSError:
        pass
    words += os.environ.get("REFLORA_AUDIT_PERSONAL", "").split(";")
    words = sorted({w.strip() for w in words if len(w.strip()) >= 4})
    return re.compile(b"|".join(re.escape(w.encode()) for w in words), re.IGNORECASE) if words else re.compile(b"(?!)")
PERSONAL = _personal()
# Any user folder: never in our own files. Upstream component binaries (checked file by file against components.json)
# are exempt: python312.dll's docstring example 'C:/Users/Barney', Gommon.dll's author's PDB path.
USER_DIR = re.compile(rb"C:\\\\Users\\\\|C:\\Users\\|C:/Users/")


STANDARD = {
    "MIT": """MIT License

Copyright (c) the authors named in CRATES.txt for each crate

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
""",
    "BSD-3-Clause": """BSD 3-Clause License

Copyright (c) the authors named in CRATES.txt for each crate

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
   disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
   products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
""",
}


def say(*a):
    print(*a, flush=True)


def git(*args):
    return subprocess.run(["git", "-C", REPO, *args], check=True, capture_output=True).stdout


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def build_launcher(a, target_dir):
    """REFLORA.exe from HEAD, in its own target folder (outside the repository), with the build machine's paths
    (the sources', the cargo registry's, the toolchain's: panic locations embed them) remapped to neutral names."""
    exe = os.path.join(target_dir, "release", "reflora.exe")
    if a.no_build:
        return exe
    env = dict(os.environ, RUSTUP_HOME=a.rustup_home, CARGO_HOME=a.cargo_home, CARGO_TARGET_DIR=target_dir)
    env["PATH"] = os.path.join(a.cargo_home, "bin") + os.pathsep + env["PATH"]
    env["RUSTFLAGS"] = " ".join(f"--remap-path-prefix={src}={dst}" for src, dst in
                                ((REPO, "/reflora"), (a.cargo_home, "/cargo"), (a.rustup_home, "/rustup")))
    say("launcher: cargo build --release (from HEAD: the tree is clean; build paths remapped)")
    subprocess.run([os.path.join(a.cargo_home, "bin", "cargo.exe"), "build", "--release"],
                   cwd=os.path.join(REPO, "launcher", "src-tauri"), env=env, check=True)
    return exe


def launcher_notices(a, dest):
    """Every crate compiled into REFLORA.exe: its license files (from the cargo registry sources) and a list."""
    env = dict(os.environ, RUSTUP_HOME=a.rustup_home, CARGO_HOME=a.cargo_home)
    env["PATH"] = os.path.join(a.cargo_home, "bin") + os.pathsep + env["PATH"]
    meta = json.loads(subprocess.run([os.path.join(a.cargo_home, "bin", "cargo.exe"), "metadata", "--format-version", "1", "--filter-platform",
                                      "x86_64-pc-windows-msvc"], cwd=os.path.join(REPO, "launcher", "src-tauri"),
                                     env=env, check=True, capture_output=True).stdout)
    pk = {p["id"]: p for p in meta["packages"]}
    nodes = {n["id"]: n for n in meta["resolve"]["nodes"]}
    root = meta["resolve"]["root"]
    seen, stack = set(), [root]
    while stack:  # normal dependencies only: what is compiled into the executable
        i = stack.pop()
        if i in seen:
            continue
        seen.add(i)
        stack += [d["pkg"] for d in nodes[i]["deps"] if any(k["kind"] is None for k in d["dep_kinds"])]
    seen.discard(root)
    os.makedirs(dest, exist_ok=True)
    lines = ["Crates (Rust libraries) compiled into REFLORA.exe, with their licenses. The license files of each are in",
             "the folder of the same name. Sources: https://crates.io/crates/<name>/<version>", ""]
    missing = []
    for i in sorted(seen, key=lambda i: (pk[i]["name"], pk[i]["version"])):
        p = pk[i]
        name = f"{p['name']}-{p['version']}"
        lic = p["license"] or f"see {p['license_file']}"
        authors = ", ".join(p.get("authors") or []) or "see the repository"
        lines.append(f"{name}\t{lic}\t{p.get('repository') or ''}\t{authors}")
        src = os.path.dirname(p["manifest_path"])
        files = [f for f in os.listdir(src) if re.match(r"(?i)(licen[cs]e|copying|notice|unlicense|copyright)", f)]
        if not files:
            missing.append((name, lic))
        for f in files:
            if os.path.isfile(os.path.join(src, f)):
                os.makedirs(os.path.join(dest, name), exist_ok=True)
                shutil.copy2(os.path.join(src, f), os.path.join(dest, name, f))
    lines += ["", "MPL-2.0 crates (cssparser, cssparser-macros, selectors, dtoa-short, option-ext) are used unmodified;",
              "their source is at the crates.io addresses above, as MPL-2.0 section 3.2 asks.",
              "", "Crates whose package has no license file: their license is the SPDX expression above, under the",
              "copyright of the authors listed with them; the standard texts are in standard/:"]
    lines += [f"  {n}\t{l}" for n, l in missing]
    std = os.path.join(dest, "standard")
    os.makedirs(std, exist_ok=True)
    for spdx, text in STANDARD.items():
        with open(os.path.join(std, spdx + ".txt"), "w", encoding="utf-8") as f:
            f.write(text)
    mpl = [os.path.join(r, f) for r, _, fs in os.walk(dest) for f in fs
           if "Mozilla Public License Version 2.0" in open(os.path.join(r, f), encoding="utf-8", errors="replace").read(300)]
    if not mpl:
        raise SystemExit("licenses: no MPL-2.0 text among the crates")
    shutil.copy2(mpl[0], os.path.join(std, "MPL-2.0.txt"))
    with open(os.path.join(dest, "CRATES.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return len(seen), missing


def audit(stage):
    """Refuses the package if anything private or game-derived is in it. Returns the findings (empty: clean)."""
    bad = []
    for root, dirs, files in os.walk(stage):
        dirs[:] = [d for d in dirs if d != "__pycache__" or bad.append(os.path.join(root, d)) is None]
        for f in files:
            p = os.path.join(root, f)
            rel = os.path.relpath(p, stage).replace("\\", "/")
            low = f.lower()
            ext = os.path.splitext(low)[1]
            if ext in FORBIDDEN_EXT or low.endswith(".tsv.gz"):
                bad.append(f"forbidden type: {rel}")
            if ext == ".bin" and not any(rel.endswith(x) for x in ALLOWED_BIN):
                bad.append(f"binary data: {rel}")
            if re.search(r"(?i)(^|/)(private|\.git|saves?|shader_misses|collect|frames)(/|$)", rel):
                bad.append(f"private folder: {rel}")
            if low in {"settings.json", "settings.ini", "profile.ini", "profile.jpg", "nup.txt", "prepared.json",
                       "graphics.json", "enabled"} or low.startswith("pipelines_"):
                bad.append(f"installation or personal file: {rel}")
            if os.path.getsize(p) < 64 << 20:
                data = open(p, "rb").read()
                m = PERSONAL.search(data) or (None if rel.startswith("components/") else USER_DIR.search(data))
                if m:
                    bad.append(f"personal path or name in {rel}: {m.group(0)[:40]!r}")
    return bad


def check_catalog(path):
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import pipeline_catalog as pc
    recs, damaged, canon, version = pc.read(path)
    compute = sum(pc.kind(r) == pc.COMPUTE for r in recs)
    # a record is a kind, a size, two 16-character hex hashes and the key's numbers: nothing else can be in it
    for r in recs:
        for h in pc.hashes(r):
            if h and not re.fullmatch(r"[0-9a-f]{16}", h):
                raise SystemExit(f"catalog: a record names {h!r}, not a shader hash")
    if damaged:
        raise SystemExit(f"catalog: {damaged} damaged entries")
    return len(recs), compute, version, canon


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", required=True, help="Reflora-Alpha-NN: the folder, the RAR and the README's title")
    ap.add_argument("--rar", default=r"C:\Program Files\WinRAR\Rar.exe")
    ap.add_argument("--cargo-home", default=os.environ.get("CARGO_HOME") or os.path.join(os.path.expanduser("~"), ".cargo"))
    ap.add_argument("--rustup-home", default=os.environ.get("RUSTUP_HOME") or os.path.join(os.path.expanduser("~"), ".rustup"))
    ap.add_argument("--no-build", action="store_true", help="use the launcher already built (must be from HEAD)")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if os.path.commonpath([out, REPO]) == REPO:
        raise SystemExit("--out must be outside the repository")
    if git("status", "--porcelain", "--untracked-files=no").strip():
        raise SystemExit("the repository has uncommitted changes: commit first (the package is made from HEAD)")
    head = git("rev-parse", "HEAD").decode().strip()
    when = git("log", "-1", "--format=%ci").decode().strip()
    stage = os.path.join(out, a.name)
    if os.path.exists(stage):
        raise SystemExit(f"{stage} exists: choose a new --out or --name")
    os.makedirs(stage)

    # 1. committed sources: git archive of HEAD (only the paths the preparer and the build use)
    paths = TREE + ["tools/" + t for t in TOOLS]
    tar = git("archive", "--format=tar", "HEAD", "--", *paths)
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        t.extractall(stage, filter="data")
    say(f"sources: git archive {head[:10]} ({len(paths)} paths)")

    # 2. the components, checked file by file against components.json
    comp_src = os.path.join(REPO, "components")
    man = json.load(open(os.path.join(comp_src, "components.json"), encoding="utf-8"))
    for name, c in man["components"].items():
        for rel, (digest, size) in c["files"].items():
            p = os.path.join(comp_src, name, rel)
            if os.path.getsize(p) != size or sha256(p) != digest:
                raise SystemExit(f"component {name}: {rel} differs from components.json")
    shutil.copytree(comp_src, os.path.join(stage, "components"),
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    extra = [os.path.relpath(os.path.join(r, f), os.path.join(stage, "components")).replace("\\", "/")
             for r, _, fs in os.walk(os.path.join(stage, "components")) for f in fs]
    listed = {f"{n}/{rel}" for n, c in man["components"].items() for rel in c["files"]} | {"components.json"}
    unlisted = [x for x in extra if x not in listed]
    if unlisted:
        raise SystemExit(f"components: files not in components.json: {unlisted[:5]}")
    say(f"components: {', '.join(man['components'])} checked ({len(listed) - 1} files)")

    # 3. the launcher, built from HEAD
    exe = build_launcher(a, os.path.join(out, "_build", "launcher-target"))
    shutil.copy2(exe, os.path.join(stage, "REFLORA.exe"))
    say(f"launcher: REFLORA.exe {os.path.getsize(exe)} bytes, sha256 {sha256(exe)}")

    # 4. licenses
    lic = os.path.join(stage, "licenses")
    n, missing = launcher_notices(a, os.path.join(lic, "launcher-crates"))
    for f in ("Fredoka-OFL.txt", "Nunito-OFL.txt"):
        shutil.copy2(os.path.join(REPO, "launcher", "src", "fonts", f), os.path.join(lic, f))
    shutil.copytree(os.path.join(REPO, "tools", "packaging", "licenses"), lic, dirs_exist_ok=True)
    # the full Apache License 2.0 text (OpenSSL, and crates that name it without shipping it): tauri's own copy
    apache = [os.path.join(r, f) for r, _, fs in os.walk(os.path.join(lic, "launcher-crates"))
              for f in fs if f.upper().replace("_", "-") in ("LICENSE-APACHE-2.0", "LICENSE-APACHE")]
    apache = [p for p in apache if "Apache License" in open(p, encoding="utf-8", errors="replace").read(400)]
    if not apache:
        raise SystemExit("licenses: no Apache-2.0 text found among the crates")
    shutil.copy2(sorted(apache)[0], os.path.join(lic, "Apache-2.0.txt"))
    say(f"licenses: {n} crates ({len(missing)} without a license file in their package)")

    # 5. the testers' documents
    m = re.fullmatch(r"Reflora-Alpha-(\d+)", a.name)
    if not m:
        raise SystemExit("--name must be Reflora-Alpha-NN")
    readme = open(os.path.join(REPO, "tools", "packaging", "LEIA-ME.txt"), encoding="utf-8").read()
    if "{ALPHA}" not in readme:
        raise SystemExit("tools/packaging/LEIA-ME.txt: no {ALPHA} in its title")
    with open(os.path.join(stage, "LEIA-ME.txt"), "w", encoding="utf-8", newline="") as f:  # as committed
        f.write(readme.replace("{ALPHA}", f"Alpha {m[1]}"))
    recs, compute, version, canon = check_catalog(os.path.join(stage, "games", "wonder2", "pipeline_catalog.bin"))
    with open(os.path.join(stage, "VERSAO.txt"), "w", encoding="utf-8") as f:
        f.write(f"REFLORA {a.name}\ncommit {head} ({when})\nempacotado em {time.strftime('%Y-%m-%d %H:%M')}\n"
                f"REFLORA.exe sha256 {sha256(exe)}\n"
                f"catálogo de pipelines: {recs} registros ({recs - compute} gráficos, {compute} compute), formato "
                f"v{version}, regras canônicas v{canon}\n")
    say(f"catalog: {recs} records ({compute} compute), format v{version}: shader hashes and numbers only")

    # 6. audit
    bad = audit(stage)
    if bad:
        for b in bad[:40]:
            say("AUDIT:", b)
        raise SystemExit(f"audit: {len(bad)} findings; the package is not made")
    files = sum(len(fs) for _, _, fs in os.walk(stage))
    size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(stage) for f in fs)
    say(f"audit: clean ({files} files, {size / 2**20:.1f} MB)")

    # 7. the RAR (no password), tested
    rar = os.path.join(out, a.name + "-Windows-x64.rar")
    subprocess.run([a.rar, "a", "-r", "-m5", "-ma5", "-idq", "-ep1", rar, stage], check=True)
    subprocess.run([a.rar, "t", "-idq", rar], check=True)
    digest = sha256(rar)
    with open(rar + ".sha256", "w", encoding="utf-8", newline="\n") as f:
        f.write(f"{digest} *{os.path.basename(rar)}\n")
    say(f"rar: {rar} ({os.path.getsize(rar) / 2**20:.1f} MB), tested; sha256 {digest}")


if __name__ == "__main__":
    main()
