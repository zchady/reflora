"""The portable components the Reflora launcher ships, rebuilt from pinned sources (for maintainers).

usage: python tools/reflora_components.py <out dir> [--only python,spirv_harness,spirv-val] [--work <dir>]

  python          Python 3.12.10 embeddable (python.org, Authenticode-signed by the PSF) + capstone 5.0.7,
                  zstandard 0.25.0, lz4 4.4.5 wheels (PyPI, SHA-256 pinned) unpacked into Lib/site-packages;
                  python312._pth lists exactly those paths (no PYTHONPATH, registry, user site or current directory).
  spirv_harness   tools/maxwell/spirv_harness published self-contained for win-x64 (the .NET 10 runtime inside: no
                  .NET install needed to run it; no trimming, no single file, no NativeAOT), built from the pinned
                  Ryubing a82350bb77. Building it needs the .NET 10 SDK on the maintainer's machine.
  spirv-val       SPIRV-Tools + SPIRV-Headers tag vulkan-sdk-1.4.363.0 (SPIRV-Tools v2026.4, the validator of the
                  Vulkan SDK 1.4.363), built with the preparer's portable llvm-mingw / CMake / Ninja and this Python,
                  statically linked (imports only KERNEL32 and the UCRT). SOURCE_DATE_EPOCH fixed: reproducible.
Each component gets licenses/ and the whole set a components.json (version, every file's SHA-256 and size), which
tools/prepare_shaders.py checks before using a component (--components).
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import prepare_build as pb  # noqa: E402
import prepare_shaders as ps  # noqa: E402

PYTHON = {
    "url": "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip",
    "sha256": "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3",
    "version": "3.12.10",
}
WHEELS = [
    ("capstone-5.0.7-py3-none-win_amd64.whl", "4ab8bcb7da8f221ff45926ca168ca33e76f7237d06fbf3c10780002faa2670e1",
     "https://files.pythonhosted.org/packages/70/39/2138d890a8e827636b9de9924fbc8527fe83e38b6d26605b30ac55e30ebe/"
     "capstone-5.0.7-py3-none-win_amd64.whl"),
    ("zstandard-0.25.0-cp312-cp312-win_amd64.whl", "ffef5a74088f1e09947aecf91011136665152e0b4b359c42be3373897fb39b01",
     "https://files.pythonhosted.org/packages/79/3b/fa54d9015f945330510cb5d0b0501e8253c127cca7ebe8ba46a965df18c5/"
     "zstandard-0.25.0-cp312-cp312-win_amd64.whl"),
    ("lz4-4.4.5-cp312-cp312-win_amd64.whl", "713a777de88a73425cf08eb11f742cd2c98628e79a8673d6a52e3c5f0c116f33",
     "https://files.pythonhosted.org/packages/a5/0b/a54cd7406995ab097fceb907c7eb13a6ddd49e0b231e448f1a81a50af65c/"
     "lz4-4.4.5-cp312-cp312-win_amd64.whl"),
]
SPIRV = {
    "SPIRV-Tools": {"url": "https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz",
                    "sha256": "e6c83a215538fbfd265ccf398d3877985055df1273dbed73912bd05307664688",
                    "content": "087024c47187be833411ebf0727f9757bc14e1df01acf51d47ba5892b2b159f6",
                    "root": "SPIRV-Tools-vulkan-sdk-1.4.363.0"},
    "SPIRV-Headers": {"url": "https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz",
                      "sha256": "5b3a0d099f5c7649e29c0938476c462e71d66ad89a0bde42fa1ba3eb476956c9",
                      "content": "dae74e27fc0fab68274dec4d2c080329950b6b61b1b02dffb303e9630a7c6416",
                      "root": "SPIRV-Headers-vulkan-sdk-1.4.363.0"},
}
SOURCE_DATE_EPOCH = "1793836800"  # 2026-11-05, recorded as the build date in spirv-val --version
DOTNET_RUNTIME = "10.0.14"
# NuGet packages inside the harness (via Ryujinx.Common) whose packages carry no licence file
NUGET_NOTICES = """The harness also contains these NuGet packages (dependencies of Ryujinx.Common):
  Gommon 2.8.1.2                         MIT (licence text: Gommon-LICENSE)
  Humanizer.Core 3.0.10                  MIT  https://github.com/Humanizr/Humanizer
  Microsoft.IO.RecyclableMemoryStream 3.0.1  MIT  https://github.com/Microsoft/Microsoft.IO.RecyclableMemoryStream
  MsgPack.Cli 1.0.1                      Apache-2.0  https://github.com/msgpack/msgpack-cli
  System.CodeDom 4.4.0                   MIT (System.CodeDom-LICENSE.TXT)
"""


def say(*a):
    print(*a, flush=True)


def fetch(url, sha, path, content=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if not os.path.isfile(path):
        say("downloading", url)
        urllib.request.urlretrieve(url, path + ".part")
        os.replace(path + ".part", path)
    if pb.sha256_file(path) == sha:
        return path
    if content:
        with tarfile.open(path) as t:
            lines = sorted(f"{m.name}\t{hashlib.sha256(t.extractfile(m).read()).hexdigest()}\n"
                           for m in t.getmembers() if m.isfile())
        if hashlib.sha256("".join(lines).encode()).hexdigest() == content:
            return path
    raise SystemExit(f"{path}: not the pinned file (SHA-256 {sha[:16]}...)")


def make_python(out, work):
    dst = os.path.join(out, "python")
    shutil.rmtree(dst, ignore_errors=True)
    z = fetch(PYTHON["url"], PYTHON["sha256"], os.path.join(work, "downloads", os.path.basename(PYTHON["url"])))
    with zipfile.ZipFile(z) as f:
        f.extractall(dst)
    sp = os.path.join(dst, "Lib", "site-packages")
    os.makedirs(sp)
    for name, sha, url in WHEELS:
        with zipfile.ZipFile(fetch(url, sha, os.path.join(work, "downloads", name))) as f:
            f.extractall(sp)
    with open(os.path.join(dst, "python312._pth"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write("python312.zip\n.\nLib\\site-packages\n\n"
                "# Reflora: only these paths (no site, no PYTHONPATH, no registry, no user site).\n")
    lic = os.path.join(dst, "licenses")
    os.makedirs(lic)
    shutil.copy2(os.path.join(dst, "LICENSE.txt"), os.path.join(lic, "Python-LICENSE.txt"))
    for d in os.listdir(sp):
        if d.endswith(".dist-info"):
            for root, _, files in os.walk(os.path.join(sp, d)):
                for fn in files:
                    if "LICEN" in fn.upper() or "COPYING" in fn.upper():
                        shutil.copy2(os.path.join(root, fn), os.path.join(lic, d.split("-")[0] + "-" + fn))
    return {"version": PYTHON["version"], "wheels": [w[0] for w in WHEELS]}


def make_harness(out, work):
    dst = os.path.join(out, "spirv_harness")
    shutil.rmtree(dst, ignore_errors=True)
    ryu = ps.fetch_ryubing(os.path.join(work, "tools"), None, lambda k, m: say(m))
    build = os.path.join(work, "harness-build")
    shutil.rmtree(build, ignore_errors=True)
    cmd = ["dotnet", "publish", os.path.join(REPO, "tools", "maxwell", "spirv_harness", "SpirvHarness.csproj"),
           "-c", "Release", "-r", "win-x64", "--self-contained", "true", "-nologo", "-v:minimal",
           "-p:RyuSrc=" + os.path.join(ryu, "src").replace("\\", "/") + "/",
           "-p:HarnessBuildRoot=" + build.replace("\\", "/") + "/", "-p:PatchPython=" + sys.executable,
           "-p:PublishTrimmed=false", "-p:PublishSingleFile=false", "-p:PublishReadyToRun=false",
           # reproducible: source paths mapped to /_/ and no PDB, so where the sources were does not change a byte
           "-p:ContinuousIntegrationBuild=true", "-p:Deterministic=true", "-p:DebugType=none",
           # the Ryubing archive is no git checkout (no SourceRoot): map both source trees by hand (%2C = ",")
           "-p:PathMap=" + ryu.rstrip("\\/") + "\\=/_/ryubing/%2C" + REPO.rstrip("\\/") + "\\=/_/repo/",
           f"-p:RuntimeFrameworkVersion={DOTNET_RUNTIME}", "-o", dst]
    subprocess.run(cmd, check=True, env=dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_NOLOGO="1"))
    for f in os.listdir(dst):
        if f.endswith(".pdb"):
            os.remove(os.path.join(dst, f))
    # the two Ryujinx decoder sources tools/ampere/maxwell.py reads its Maxwell encodings from (lowering needs them)
    dec = os.path.join(dst, "ryujinx-shader", "Decoders")
    os.makedirs(dec)
    for f in ("InstTable.cs", "InstDecoders.cs"):
        shutil.copy2(os.path.join(ryu, "src", "Ryujinx.Graphics.Shader", "Decoders", f), os.path.join(dec, f))
    lic = os.path.join(dst, "licenses")
    os.makedirs(lic)
    shutil.copy2(os.path.join(REPO, "LICENSE"), os.path.join(lic, "SwitchGamesRecomp-LICENSE.txt"))
    shutil.copy2(os.path.join(ryu, "LICENSE.txt"), os.path.join(lic, "Ryujinx-LICENSE.txt"))
    nuget = os.path.join(os.path.expanduser("~"), ".nuget", "packages")
    rt = os.path.join(nuget, "microsoft.netcore.app.runtime.win-x64", DOTNET_RUNTIME)
    shutil.copy2(os.path.join(rt, "LICENSE.TXT"), os.path.join(lic, "dotnet-runtime-LICENSE.TXT"))
    shutil.copy2(os.path.join(rt, "THIRD-PARTY-NOTICES.TXT"), os.path.join(lic, "dotnet-runtime-THIRD-PARTY-NOTICES.TXT"))
    shutil.copy2(os.path.join(nuget, "gommon", "2.8.1.2", "content", "LICENSE"), os.path.join(lic, "Gommon-LICENSE"))
    shutil.copy2(os.path.join(nuget, "system.codedom", "4.4.0", "LICENSE.TXT"), os.path.join(lic, "System.CodeDom-LICENSE.TXT"))
    with open(os.path.join(lic, "NUGET-NOTICES.txt"), "w", encoding="utf-8") as f:
        f.write(NUGET_NOTICES)
    return {"ryubing": ps.RYUBING["commit"], "dotnet_runtime": DOTNET_RUNTIME, "self_contained": True}


def make_spirv_val(out, work, python):
    dst = os.path.join(out, "spirv-val")
    shutil.rmtree(dst, ignore_errors=True)
    src = {}
    for name, t in SPIRV.items():
        a = fetch(t["url"], t["sha256"], os.path.join(work, "downloads", name + ".tar.gz"), t["content"])
        x = os.path.join(work, "src", name)
        shutil.rmtree(x, ignore_errors=True)
        with tarfile.open(a) as tf:
            tf.extractall(x, filter="data")
        src[name] = os.path.join(x, t["root"])
    b = pb.Build(work, lambda k, m: say(m), gen_dir="")
    llvm, cmake, ninja = b.tool("llvm-mingw"), b.tool("cmake"), b.tool("ninja")
    env = b.clean_env([os.path.join(llvm, "bin"), os.path.join(cmake, "bin"), ninja])
    env["SOURCE_DATE_EPOCH"] = SOURCE_DATE_EPOCH
    bdir = os.path.join(work, "spirv-val-build")
    shutil.rmtree(bdir, ignore_errors=True)
    subprocess.run([os.path.join(cmake, "bin", "cmake.exe"), "-S", src["SPIRV-Tools"], "-B", bdir, "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_C_COMPILER=" + os.path.join(llvm, "bin", "x86_64-w64-mingw32-clang.exe"),
                    "-DCMAKE_CXX_COMPILER=" + os.path.join(llvm, "bin", "x86_64-w64-mingw32-clang++.exe"),
                    "-DCMAKE_MAKE_PROGRAM=" + os.path.join(ninja, "ninja.exe"),
                    "-DSPIRV-Headers_SOURCE_DIR=" + src["SPIRV-Headers"].replace("\\", "/"),
                    "-DPython3_EXECUTABLE=" + python, "-DSPIRV_SKIP_TESTS=ON", "-DSPIRV_WERROR=OFF",
                    "-DBUILD_SHARED_LIBS=OFF", "-DCMAKE_EXE_LINKER_FLAGS=-static -Wl,--no-insert-timestamp",
                    "-DCMAKE_IGNORE_PREFIX_PATH=C:/msys64/ucrt64;C:/msys64"], check=True, env=env)
    subprocess.run([os.path.join(ninja, "ninja.exe"), "-C", bdir, "-j", str(pb.safe_jobs(0)), "spirv-val"],
                   check=True, env=env)
    os.makedirs(os.path.join(dst, "licenses"))
    shutil.copy2(os.path.join(bdir, "tools", "spirv-val.exe"), dst)
    lic = os.path.join(dst, "licenses")
    shutil.copy2(os.path.join(src["SPIRV-Tools"], "LICENSE"), os.path.join(lic, "SPIRV-Tools-LICENSE"))
    shutil.copy2(os.path.join(src["SPIRV-Headers"], "LICENSE"), os.path.join(lic, "SPIRV-Headers-LICENSE"))
    lm = pb.TOOLS["llvm-mingw"]
    for name, rel in lm["licenses"].items():
        shutil.copy2(os.path.join(llvm, rel), os.path.join(lic, name))
    return {"spirv_tools": "vulkan-sdk-1.4.363.0 (v2026.4)", "toolchain": "llvm-mingw 20261020"}


def manifest(out, info):
    comps = {}
    for name, meta in info.items():
        root = os.path.join(out, name)
        files = {}
        for d, _, fs in os.walk(root):
            for f in fs:
                p = os.path.join(d, f)
                files[os.path.relpath(p, root).replace("\\", "/")] = [pb.sha256_file(p), os.path.getsize(p)]
        comps[name] = dict(meta, files=files, bytes=sum(v[1] for v in files.values()))
    with open(os.path.join(out, "components.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"format": 1, "components": comps}, f, indent=1, sort_keys=True)
    for name, c in comps.items():
        say(f"{name}: {len(c['files'])} files, {c['bytes'] / 2**20:.1f} MB")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--only", default="python,spirv_harness,spirv-val")
    ap.add_argument("--work", default=None, help="downloads and build directories (default: <out>/../components-work)")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    work = os.path.abspath(a.work or os.path.join(os.path.dirname(out), "components-work"))
    os.makedirs(os.path.join(work, "work"), exist_ok=True)
    old = {}
    try:
        old = json.load(open(os.path.join(out, "components.json"), encoding="utf-8"))["components"]
    except (OSError, ValueError, KeyError):
        pass
    info = {k: {kk: vv for kk, vv in v.items() if kk not in ("files", "bytes")} for k, v in old.items()}
    only = a.only.split(",")
    if "python" in only:
        info["python"] = make_python(out, work)
    if "spirv_harness" in only:
        info["spirv_harness"] = make_harness(out, work)
    if "spirv-val" in only:
        info["spirv-val"] = make_spirv_val(out, work, os.path.join(out, "python", "python.exe"))
    manifest(out, info)


if __name__ == "__main__":
    main()
