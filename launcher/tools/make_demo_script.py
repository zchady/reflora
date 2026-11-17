"""Writes src-tauri/fixtures/demo_preparation.txt: a shortened preparation in the exact line format of
tools/prepare_switch2.py (STEP/GROUP/PROGRESS/LOG/WARN/DONE), used by the launcher's clearly marked demonstration.
Group names and paths are neutral placeholders: nothing in it comes from the game files.

  python launcher/tools/make_demo_script.py
"""
import os

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src-tauri", "fixtures",
                   "demo_preparation.txt")
WS = r"C:\Reflora\workspace"
GROUPS = [f"{kind}_Demo{n:02d}" for kind, count in (("effect", 8), ("layout", 6), ("lib", 3), ("shader", 3))
          for n in range(1, count + 1)] + ["exefs_main"]


def main():
    L = []
    L += ["STEP 1/8 verify", "LOG package 0: program 040015100B514001, build 1.2.1",
          "LOG add-on content linked to the program: found", "PROGRESS 12"]
    L += ["STEP 2/8 extract", f"LOG ExeFS -> {WS}\\work\\exefs (SHA-256 checked)", "PROGRESS 25"]
    L += ["STEP 3/8 bins"] + [f"LOG {m}: flat image written" for m in ("main", "sdk", "subsdk0")] + ["PROGRESS 37"]
    L += ["STEP 4/8 recompile"]
    for k, m in enumerate(("main", "sdk", "subsdk0"), 1):
        L += [f"LOG recompiling {m}", f"PROGRESS {int(100 * k / 3)}"]
    L += ["STEP 5/8 shaders", "LOG scanning the package for Switch 2 shaders"]
    L += [f"PROGRESS {p}" for p in range(0, 13, 3)]
    total = len(GROUPS)
    for i, g in enumerate(GROUPS, 1):
        L += [f"GROUP {i}/{total} {g} extract done: 4 stages",
              f"GROUP {i}/{total} {g} lower done: {12 * i} lowered, {i % 4 == 0 and 1 or 0} unsupported",
              f"GROUP {i}/{total} {g} translate done: {12 * i} ok, 0 failed",
              f"PROGRESS {12 + int(83 * i / total)}"]
    L += ["WARN shader coverage is not complete: 5 blocks not lowered, 0 not translated (see the shaders report)",
          "PROGRESS 100"]
    L += ["STEP 6/8 build", "LOG llvm-mingw: verified and extracted", "LOG cmake: verified and extracted",
          "LOG ninja: verified and extracted", "LOG sdl3: verified and extracted",
          "LOG vulkan-headers: verified and extracted", "LOG build: 1080 recompiled files; 5 jobs",
          "LOG build: compiling (the first time: about 6 minutes with 8 jobs on a 6-core CPU)"]
    for done in range(0, 1191, 70):
        L += [f"PROGRESS {int(100 * done / 1190)}", f"LOG [{done}/1190] Building CXX object game_main"]
    L += ["PROGRESS 100", "LOG imports: KERNEL32.dll, SDL3.dll, USER32.dll, ...", f"LOG executable: {WS}\\bin\\wonder2.exe"]
    L += ["STEP 7/8 config", "PROGRESS 87"]
    L += ["STEP 8/8 check", "LOG check: ran 45 s; shader records ['17245']; missing shaders 0; errors 0; title frame saved",
          "PROGRESS 100", "DONE"]
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L) + "\n")
    print(OUT, len(L), "lines")


if __name__ == "__main__":
    main()
