<p align="center">
  <img src="launcher/src/assets/reflora-logo.webp" alt="REFLORA" width="420">
</p>

# REFLORA

REFLORA turns **your own copy** of the game **040015100B514001** (the NUP package, version 1.2.1 with its add-on) into a
native Windows game with a Vulkan renderer. No emulator runs: the game's AArch64 code is statically recompiled to C++
and built **on your PC**, and the console's NVN 2 graphics API is implemented on Vulkan.

REFLORA is built on the MIT-licensed static recompilation runtime by [BelmanteGu](https://github.com/BelmanteGu)
([Projeto Belmante](https://www.youtube.com/@belmante)), extended to this game's newer console edition.

> [!IMPORTANT]
> **No game files are included, and none can be downloaded from this project.** No game code, assets, keys,
> firmware, recompiled code, translated shaders or game executable are distributed, in this repository or in any
> release. You need your **own**, legally obtained NUP of the game. Everything derived from it is produced on your PC
> and must stay there: never share it. Instructions for dumping or obtaining game files are not provided.

> [!WARNING]
> **Experimental alpha.** It can crash, stutter or behave differently on each PC, and it has been tested on only a few
> hardware configurations. Back up your saves.

This project is independent and is not affiliated with, endorsed or sponsored by the rights holders of the game or
of the console.

## Download and play

Get the latest `Reflora-Alpha-NN-Windows-x64.rar` from [Releases](../../releases) and check its SHA-256. The full
guide (in Portuguese) is `LEIA-ME.txt` inside the package. In short:

1. Extract the RAR to a plain folder (e.g. `C:\REFLORA`; avoid OneDrive and *Program Files*) and open `REFLORA.exe`.
   It is not code-signed, so Windows SmartScreen may warn you.
2. **Installation:** choose your NUP. REFLORA checks the package, the CPU and the free space.
3. **Start preparation:** 9 steps, roughly 20–30 minutes and CPU heavy: verify, extract, recompile, translate the
   shaders, build the game (downloading llvm-mingw, CMake, Ninja, SDL3 and Vulkan-Headers from their official
   sites, pinned by SHA-256), configure, test, and prepare the graphics pipelines with your GPU's driver. It can be
   stopped and resumed.
4. **Play.**

Updating from an earlier alpha: copy your saves, extract the new version to a new folder, open it and use *Verify and
update*. Saves and settings are kept.

### Requirements

| | |
|---|---|
| OS | Windows 10 (2004 or newer) or Windows 11, 64-bit |
| CPU | x86-64 with AVX2 |
| GPU | Vulkan 1.3 with an up-to-date driver |
| RAM | 16 GB estimated minimum |
| Disk | ~8 GB free for the installation |
| Other | Microsoft Edge WebView2 Runtime (built into Windows 11); internet on the first preparation (~250 MB of tools) |

No Visual Studio, MSYS2, Python, .NET or Vulkan SDK is needed: the package carries a portable Python, the shader
translator and `spirv-val`, each with its license.

## How it works

- **Packages:** `tools/nup.py` reads NUP packages (program and add-on selection, SHA-256 verified extraction); the
  runtime reads the RomFS and the add-on straight from the NUP.
- **CPU:** the game's `main`, `sdk` and `subsdk0` modules are recompiled to C++, including RELR relocations and the
  ARMv8.3 instructions (PAC, LDAPR) this build uses.
- **Graphics:** the NVN 2 front end (texture, sampler and buffer layouts, conditional rendering, compute) on Vulkan.
- **Shaders:** an Ampere (SM86) decoder and an Ampere → Maxwell lowering (`tools/ampere`), so the offline Maxwell →
  SPIR-V translator (Ryujinx's, MIT) handles the game's shaders.
- **Pipeline catalog:** 2,687 known pipelines (shader hashes and state numbers only, no game data) compiled before
  play by your own driver, to reduce first-time stutter.
- **Launcher:** the REFLORA launcher (Tauri 2) and a preparer that needs nothing installed beyond Windows and the GPU
  driver (`tools/prepare_switch2.py`, see [docs/REFLORA_PREPARER.md](docs/REFLORA_PREPARER.md)).

## Known limitations

- Only version 1.2.1 of the game is supported.
- Much of the game is untested; new scenes may stutter the first time they appear. 60 FPS is not guaranteed.
- Button icons need the console's button font from your own console (optional import in Settings).
- Experimental runtime options (`NX_*` variables) are off by default and may show visual defects.

## Repository layout

| Path | What |
|---|---|
| `recomp/` | the AArch64 → C++ recompiler (Python) |
| `runtime/` | OS services, NVN → Vulkan, audio, input |
| `games/wonder2/` | this game's description and pipeline catalog |
| `tools/` | NUP reader, preparer, shader tools, packaging (`tools/pack_reflora_alpha.py`) |
| `launcher/` | the REFLORA launcher (Tauri 2) |
| `tests/` | tests that need no game files |

## Reporting problems

Open an issue with your CPU, GPU and driver version, RAM, Windows version, what happened and when, and the logs:
`<installation>\work\reflora-prepare.log`, `<installation>\nx.log`, `<installation>\work\graphics.log`. Logs may
contain your Windows user name in folder paths; remove it if you prefer. **Never attach** the NUP, extracted game
files, the console's font, saves, keys, or the `bin`, `data` or `work` folders.

## Credits and licenses

- The recompilation runtime by **[BelmanteGu](https://github.com/BelmanteGu)**
  ([Projeto Belmante](https://www.youtube.com/@belmante)), MIT — the foundation of this project.
- REFLORA and this edition's support: [zchady](https://github.com/zchady). Most of the code, tooling and
  documentation was written with AI assistance (Anthropic's Claude), directed, tested and reviewed by a human. If you
  don't trust AI-written code, that is your right; you run it at your own discretion.
- Ryujinx's shader translator (MIT), used as an offline tool; Mesa NAK and the NUP specification (MIT) as references;
  [Zstandard](https://github.com/facebook/zstd) (BSD-3-Clause); Dear ImGui (MIT); SDL 3 (zlib); the Vulkan headers
  and loader (Apache 2.0 / MIT). Each release carries the third-party licenses in `licenses/`. GPL projects were
  studied for behaviour only; no GPL code is used.

Licensed under the [MIT License](LICENSE). The license covers this project's code only and grants nothing for the
game.
