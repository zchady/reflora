"""The `build` step of tools/prepare_switch2.py: wonder2.exe from the recompiled code with a portable toolchain.

No Visual Studio, MSYS2 or SDK on the user's PC: the preparer fetches pinned, checksum-verified archives (or takes
them from --toolchain-dir) into <workspace>/tools and builds with them only (PATH = those tools + Windows):
  llvm-mingw 20261020   LLVM 23.1.4 (the compiler of the tested MSVC-ABI build) + mingw-w64 UCRT headers/libs
  CMake 4.4.4, Ninja 1.13.2
  SDL3 3.4.18           official SDL3-devel-*-mingw (gamepads beyond XInput; no libiconv)
  Vulkan-Headers        tag vulkan-sdk-1.4.363.0 (only vulkan/vulkan.h is used; the loader comes with the driver)
CMakeLists.txt matches the clang-cl build's code generation in its MINGW branch (WONDER_SWITCH2_PROGRESS.md §5za4).

Result: <workspace>/bin/wonder2.exe + SDL3.dll + licenses/ (never distribute the executable: it is the recompiled
game). The executable's imports are checked: Windows' own DLLs, the UCRT (Windows 10+) and SDL3.dll only.
state.json (work/build/state.json) keeps fingerprints of the tools, the recompiled sources, the runtime sources and
the CMake options, and the SHA-256 of what was installed in bin/: a build is only skipped when all of them match.
"""
import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_VERSION = 1  # bump when this file changes what the build produces

# name: url, file, sha256 of the archive (or content: sha256 of the sorted "path<TAB>sha256" lines of its files, for
# archives GitHub generates and may re-compress), the directory inside it, a file that must be there, licences to
# copy next to the executable.
TOOLS = {
    "llvm-mingw": {
        "url": "https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip",
        "sha256": "fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd",
        "root": "llvm-mingw-20261020-ucrt-x86_64",
        "probe": "bin/x86_64-w64-mingw32-clang++.exe",
        "licenses": {"LLVM-LICENSE.TXT": "LICENSE.TXT",
                     "MinGW-w64-runtime-COPYING.txt": "x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt",
                     "winpthreads-COPYING.txt": "x86_64-w64-mingw32/share/mingw32/COPYING.winpthreads.txt"},
    },
    "cmake": {
        "url": "https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.zip",
        "sha256": "bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536",
        "root": "cmake-4.4.4-windows-x86_64",
        "probe": "bin/cmake.exe",
    },
    "ninja": {
        "url": "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
        "sha256": "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65",
        "root": "",
        "probe": "ninja.exe",
    },
    "sdl3": {
        "url": "https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-devel-3.4.18-mingw.zip",
        "sha256": "cd98158c8d025a816f430a600b4e9526524e6219ee884ec72bd498b80fd3bf58",
        "root": "SDL3-3.4.18",
        "probe": "x86_64-w64-mingw32/bin/SDL3.dll",
        "licenses": {"SDL3-LICENSE.txt": "LICENSE.txt"},
    },
    "vulkan-headers": {
        "url": "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz",
        "file": "Vulkan-Headers-vulkan-sdk-1.4.363.0.tar.gz",
        "sha256": "4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e",
        "content": "ae4af9df768ed124e5d6d9d0aa4ea4253ce3c75e431dfa3a58b702645892a8d9",
        "root": "Vulkan-Headers-vulkan-sdk-1.4.363.0",
        "probe": "include/vulkan/vulkan.h",
        "licenses": {"Vulkan-Headers-LICENSE.md": "LICENSE.md"},
    },
}

# What the executable may import: Windows' own DLLs (Windows 10+, the UCRT included) and SDL3.dll next to it.
SYSTEM_DLLS = {"kernel32.dll", "user32.dll", "gdi32.dll", "shell32.dll", "ole32.dll", "oleaut32.dll", "advapi32.dll",
               "dwmapi.dll", "xinput1_4.dll", "xaudio2_9.dll", "imm32.dll", "winmm.dll", "version.dll",
               "setupapi.dll", "psapi.dll", "ws2_32.dll", "bcrypt.dll", "ntdll.dll", "msvcrt.dll"}
BESIDE_DLLS = {"sdl3.dll"}

# Sources whose change means a rebuild (besides the recompiled code and the tools).
SOURCE_DIRS = ["runtime", "games/wonder2"]
SOURCE_FILES = ["CMakeLists.txt"]


class NeedsUser(Exception):
    pass


def disallowed_imports(dlls):
    """The DLLs an executable imports that are neither Windows' own (UCRT included) nor shipped beside it."""
    return [d for d in dlls if not (d.lower() in SYSTEM_DLLS or d.lower() in BESIDE_DLLS
                                    or d.lower().startswith("api-ms-win-crt-"))]


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def digest(mapping):
    return hashlib.sha256("".join(sorted(f"{k}\t{v}\n" for k, v in mapping.items())).encode()).hexdigest()


def tree_digest(root, exts=None):
    out = {}
    for d, _, files in os.walk(root):
        for f in files:
            if exts and not f.endswith(exts):
                continue
            p = os.path.join(d, f)
            out[os.path.relpath(p, root).replace("\\", "/")] = sha256_file(p)
    return digest(out), len(out)


def memory_gb():
    """(total, available) physical memory in GB (Windows)."""
    class MS(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong), ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong), ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong), ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong), ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
    m = MS()
    m.dwLength = ctypes.sizeof(MS)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m.ullTotalPhys / 2**30, m.ullAvailPhys / 2**30


def safe_jobs(requested):
    """Compile jobs that fit: the biggest generated file needs ~1.2 GB (table.cpp), most 0.3-0.4 GB (§5za3); keep
    6 GB for Windows and the rest, never more than 8 by default."""
    cores = os.cpu_count() or 4
    _, avail = memory_gb()
    by_ram = max(1, int((avail - 6) / 1.2))
    return max(1, min(requested or 8, cores - 2 if cores > 2 else 1, by_ram, 8 if not requested else requested))


def cmake_cache_source(build_dir):
    """The source folder an existing CMake build directory was configured for (None: no cache)."""
    try:
        with open(os.path.join(build_dir, "CMakeCache.txt"), encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("CMAKE_HOME_DIRECTORY:"):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return None


def same_source(cached, repo):
    if cached is None:
        return True  # nothing configured yet: nothing to refuse
    norm = lambda p: os.path.normcase(os.path.normpath(p.replace("/", os.sep)))
    return norm(cached) == norm(repo)


class Build:
    def __init__(self, ws, say, gen_dir, jobs=0, toolchain_dir=None):
        self.ws, self.say, self.gen = ws, say, gen_dir
        self.jobs_requested, self.toolchain_dir = jobs, toolchain_dir
        self.tools_root = os.path.join(ws, "tools")
        self.build_dir = os.path.join(ws, "work", "build")
        self.bin = os.path.join(ws, "bin")
        self.state_path = os.path.join(ws, "work", "build-state.json")
        try:
            self.state = json.load(open(self.state_path, encoding="utf-8"))
        except (OSError, ValueError):
            self.state = {}

    def save(self):
        tmp = self.state_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.state, f, indent=1, sort_keys=True)
        os.replace(tmp, self.state_path)

    # ---- tools ----------------------------------------------------------------------------------------------------

    def archive_ok(self, path, t):
        if sha256_file(path) == t["sha256"]:
            return True
        if "content" in t and path.endswith((".tar.gz", ".tgz")):
            with tarfile.open(path) as tf:
                lines = sorted(f"{m.name}\t{hashlib.sha256(tf.extractfile(m).read()).hexdigest()}\n"
                               for m in tf.getmembers() if m.isfile())
            return hashlib.sha256("".join(lines).encode()).hexdigest() == t["content"]
        return False

    def tool(self, name):
        """The verified, extracted tool directory (its root inside the archive)."""
        t = TOOLS[name]
        dst = os.path.join(self.tools_root, name)
        rec = self.state.get("tools", {}).get(name, {})
        probe = os.path.join(dst, t["root"], t["probe"])
        if rec.get("sha256") == t["sha256"] and os.path.isfile(probe) and sha256_file(probe) == rec.get("probe"):
            return os.path.join(dst, t["root"])
        fname = t.get("file") or t["url"].rsplit("/", 1)[1]
        archive = None
        if self.toolchain_dir and os.path.isfile(os.path.join(self.toolchain_dir, fname)):
            archive = os.path.join(self.toolchain_dir, fname)
        else:
            dl = os.path.join(self.tools_root, "downloads", name)
            os.makedirs(dl, exist_ok=True)
            archive = os.path.join(dl, fname)
            if not (os.path.isfile(archive) and self.archive_ok(archive, t)):
                self.say("LOG", f"downloading {name}: {t['url']}")
                part = archive + ".part"
                with urllib.request.urlopen(t["url"], timeout=300) as r, open(part, "wb") as f:
                    shutil.copyfileobj(r, f)
                os.replace(part, archive)
        if not self.archive_ok(archive, t):
            raise RuntimeError(f"{archive} does not match the pinned {name} (SHA-256 {t['sha256'][:16]}...): refusing "
                               "to use it")
        tmp = dst + ".tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(tmp)
        if archive.endswith(".zip"):
            with zipfile.ZipFile(archive) as z:
                base = os.path.realpath(tmp)
                for n in z.namelist():  # nothing may land outside the tool's directory
                    if not os.path.realpath(os.path.join(tmp, n)).startswith(base + os.sep):
                        raise RuntimeError(f"{archive}: unsafe path {n}")
                z.extractall(tmp)
        else:
            with tarfile.open(archive) as tf:
                tf.extractall(tmp, filter="data")
        shutil.rmtree(dst, ignore_errors=True)
        os.replace(tmp, dst)
        if not os.path.isfile(probe):
            raise RuntimeError(f"{name}: {t['probe']} missing after extraction")
        self.state.setdefault("tools", {})[name] = {"sha256": t["sha256"], "probe": sha256_file(probe)}
        self.save()
        self.say("LOG", f"{name}: verified and extracted ({os.path.basename(archive)})")
        return os.path.join(dst, t["root"])

    # ---- build ----------------------------------------------------------------------------------------------------

    def clean_env(self, paths):
        env = {k: v for k, v in os.environ.items() if k.upper() not in (
            "PATH", "VULKAN_SDK", "INCLUDE", "LIB", "LIBPATH", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH",
            "LIBRARY_PATH", "MSYSTEM", "VCINSTALLDIR", "VSINSTALLDIR", "CMAKE_PREFIX_PATH", "CC", "CXX")}
        windir = os.environ.get("SystemRoot", r"C:\Windows")
        env["PATH"] = ";".join(paths + [os.path.join(windir, "System32"), windir])
        return env

    def run(self, cmd, env, log_name):
        log = os.path.join(self.ws, "work", log_name)
        with open(log, "w", encoding="utf-8", errors="replace") as f:
            f.write("$ " + subprocess.list2cmdline(cmd) + "\n")
            f.flush()
            p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                 encoding="utf-8", errors="replace")
            last = 0.0
            for line in p.stdout:
                f.write(line)
                s = line.strip()
                if s.startswith("[") and "/" in s.split("]")[0]:  # ninja: [n/total]
                    done, total = s[1:].split("]")[0].split("/")
                    if time.time() - last > 5 and total.isdigit():
                        last = time.time()
                        self.say("PROGRESS", str(int(100 * int(done) / max(1, int(total)))))
                        self.say("LOG", s[:160])
                elif "error" in s.lower():
                    self.say("LOG", s[:300])
            p.wait()
        if p.returncode:
            raise RuntimeError(f"{os.path.basename(cmd[0])} failed ({p.returncode}); log: {log}")

    def imports(self, llvm, exe):
        out = subprocess.run([os.path.join(llvm, "bin", "llvm-readobj.exe"), "--coff-imports", exe],
                             stdout=subprocess.PIPE, text=True).stdout
        return sorted({l.split("Name:")[1].strip() for l in out.splitlines() if "Name:" in l and ".dll" in l.lower()})

    def go(self):
        llvm = self.tool("llvm-mingw")
        cmake = self.tool("cmake")
        ninja = self.tool("ninja")
        sdl = self.tool("sdl3")
        vk = self.tool("vulkan-headers")
        sdl_prefix = os.path.join(sdl, "x86_64-w64-mingw32")
        tools_fp = digest({k: v["sha256"] for k, v in TOOLS.items()})

        self.say("LOG", "fingerprinting the recompiled sources and the runtime")
        gen_fp, gen_n = tree_digest(self.gen)
        src = {}
        for d in SOURCE_DIRS:
            src[d] = tree_digest(os.path.join(REPO, d))[0]
        for f in SOURCE_FILES:
            src[f] = sha256_file(os.path.join(REPO, f))
        options = {"NX_GAME": "wonder2", "CMAKE_BUILD_TYPE": "Release", "generator": "Ninja", "version": BUILD_VERSION}
        fp = digest({"tools": tools_fp, "gen": gen_fp, "src": digest(src), "options": digest(options)})

        rec = self.state.get("build", {})
        exe = os.path.join(self.bin, "wonder2.exe")
        if rec.get("fp") == fp and os.path.isfile(exe) and rec.get("bin") == tree_digest(self.bin)[0]:
            self.say("LOG", "build: sources, tools and bin/ unchanged (all SHA-256 checked): kept")
            return exe
        # A build directory configured with other tools or options is not reused (incremental builds stay valid
        # for source changes: ninja tracks those itself).
        cfg_fp = digest({"tools": tools_fp, "options": digest(options), "gen_dir": self.gen})
        moved = not same_source(cmake_cache_source(self.build_dir), REPO)
        if (rec.get("configured") != cfg_fp or moved) and os.path.isdir(self.build_dir):
            if moved:  # REFLORA's sources moved (a new launcher folder): CMake refuses a cache made elsewhere
                self.say("LOG", "build: the sources are in another folder than the build directory was made for: "
                                "configuring again")
            shutil.rmtree(self.build_dir)
        os.makedirs(self.build_dir, exist_ok=True)
        env = self.clean_env([os.path.join(llvm, "bin"), os.path.join(cmake, "bin"), ninja])
        jobs = safe_jobs(self.jobs_requested)
        total, avail = memory_gb()
        self.say("LOG", f"build: {gen_n} recompiled files; {jobs} jobs ({os.cpu_count()} logical CPUs, "
                        f"{avail:.1f} of {total:.1f} GB free)")
        cfg = [os.path.join(cmake, "bin", "cmake.exe"), "-S", REPO, "-B", self.build_dir, "-G", "Ninja",
               "-DNX_GAME=wonder2", "-DCMAKE_BUILD_TYPE=Release", f"-DNX_GEN_DIR={self.gen}",
               f"-DCMAKE_C_COMPILER={os.path.join(llvm, 'bin', 'x86_64-w64-mingw32-clang.exe')}",
               f"-DCMAKE_CXX_COMPILER={os.path.join(llvm, 'bin', 'x86_64-w64-mingw32-clang++.exe')}",
               f"-DCMAKE_MAKE_PROGRAM={os.path.join(ninja, 'ninja.exe')}",
               f"-DNX_VULKAN_INCLUDE={os.path.join(vk, 'include')}", f"-DNX_SDL3_DIR={sdl_prefix}",
               "-DCMAKE_IGNORE_PREFIX_PATH=C:/msys64/ucrt64;C:/msys64"]
        self.run(cfg, env, "build-configure.log")
        self.state["build"] = {"configured": cfg_fp}
        self.save()
        self.say("LOG", "build: compiling (the first time: about 6 minutes with 8 jobs on a 6-core CPU)")
        self.run([os.path.join(ninja, "ninja.exe"), "-C", self.build_dir, "-j", str(jobs), "wonder2"], env,
                 "build-ninja.log")
        built = os.path.join(self.build_dir, "wonder2.exe")

        dlls = self.imports(llvm, built)
        bad = disallowed_imports(dlls)
        if bad:
            raise RuntimeError("the executable imports DLLs that are not part of Windows or beside it: " + ", ".join(bad))
        self.say("LOG", "imports: " + ", ".join(dlls))

        tmp = self.bin + ".tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(os.path.join(tmp, "licenses"))
        shutil.copy2(built, os.path.join(tmp, "wonder2.exe"))
        shutil.copy2(os.path.join(sdl_prefix, "bin", "SDL3.dll"), os.path.join(tmp, "SDL3.dll"))
        for name, t in TOOLS.items():
            for dst, src_rel in t.get("licenses", {}).items():
                shutil.copy2(os.path.join(self.tools_root, name, t["root"], src_rel), os.path.join(tmp, "licenses", dst))
        with open(os.path.join(tmp, "licenses", "README.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("wonder2.exe was built on this PC from your own game files: never distribute it.\n"
                    "It contains libc++/libunwind (LLVM, Apache-2.0 WITH LLVM-exception), the mingw-w64 runtime and\n"
                    "winpthreads (their notices here). SDL3.dll: zlib licence (SDL3-LICENSE.txt). Vulkan headers:\n"
                    "Apache-2.0 OR MIT (Vulkan-Headers-LICENSE.md).\n")
        if os.path.isdir(self.bin):
            prev = self.bin + ".previous"  # the last working executable stays until the next build replaces it
            shutil.rmtree(prev, ignore_errors=True)
            os.replace(self.bin, prev)
            self.say("LOG", f"previous bin/ kept as {prev}")
        os.replace(tmp, self.bin)
        self.state["build"] = {"configured": cfg_fp, "fp": fp, "bin": tree_digest(self.bin)[0], "imports": dlls,
                               "jobs": jobs, "made": time.strftime("%Y-%m-%d %H:%M:%S")}
        self.save()
        return os.path.join(self.bin, "wonder2.exe")
