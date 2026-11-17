"""ARMv8.3 instructions of Switch 2 code (pointer authentication, LDAPR) in the emitter, on SYNTHETIC code.

The instruction words are hand-encoded here (no game code). Checks capstone's decoding, that each word is handled,
and the emitted C++. tests/test_v83_runtime.cpp runs the emitted forms natively.
Run: private/venv/Scripts/python -m pytest tests/test_emit_v83.py
"""
import os
import struct
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from recomp import analysis, emit, nso  # noqa: E402

CASES = [  # word, capstone mnemonic, a fragment the emitted statement must contain
    (0xD503237F, "pacibsp", ";"),
    (0xD503233F, "paciasp", ";"),
    (0xD50323FF, "autibsp", ";"),
    (0xD50323BF, "autiasp", ";"),
    (0xDAC10020, "pacia", ";"),
    (0xDAC11483, "autib", ";"),
    (0xDAC143E0, "xpaci", ";"),
    (0x9AC23020, "pacga", "nx::pacga(c.x[1], c.x[2])"),
    (0xF8BFC020, "ldapr", "nx::ld_acq<uint64_t>(c.x[1])"),
    (0x38BFC062, "ldaprb", "nx::ld_acq<uint8_t>(c.x[3])"),
    (0x78BFC0A4, "ldaprh", "nx::ld_acq<uint16_t>(c.x[5])"),
    (0xD73F0909, "blraa", "nx::call(c, t)"),
    (0xD71F0822, "braa", "nx::call(c, c.x[1]); return;"),
    (0xD61F0A1F, "braaz", "nx::call(c, c.x[16]); return;"),
    (0xD65F0BFF, "retaa", "return;"),
    (0xD65F0FFF, "retab", "return;"),
]


def emitter_for(words):
    code = struct.pack(f"<{len(words)}I", *words)
    m = nso.Module("t", bytearray(code), (0, len(code)), (0, 0), (len(code), 0), (len(code), 0))
    an = analysis.Analysis(module=m, insns={})
    an.functions = [analysis.Function(0, len(code))]
    return emit.Emitter(an, prefix="t"), an.functions[0]


@pytest.mark.parametrize("word,mn,frag", CASES, ids=[c[1] for c in CASES])
def test_v83_instruction(word, mn, frag):
    e, f = emitter_for([word])
    i = next(e.cs.disasm(struct.pack("<I", word), 0))
    assert i.mnemonic == mn
    stmt = e.insn(f, i, {})
    assert frag in stmt, stmt
    out = e.function(f)
    assert e.unhandled == {} and "nx::unimpl" not in out


NATIVE_MAIN = r"""
#include <cstdio>
#include <cstring>
namespace nx {
uint64_t g_called = 0;
void call(CPU& c, uint64_t t) { g_called = t; c.x[0] = 0xC0FFEE; }
[[noreturn]] void unimpl(CPU&, uint64_t pc, const char* what) { std::printf("unimpl %llx %s\n", (unsigned long long)pc, what); std::exit(3); }
}
static int fails = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)
int main() {
    alignas(16) static uint64_t cell[2] = {0x1122334455667788ull, 0};
    nx::CPU c{};
    // leaf: pacibsp; ldapr x0, [x1]; autibsp; retab
    c.x[1] = uint64_t(cell); c.x[30] = 0x1234;
    t_00000000(c);
    CHECK(c.x[0] == 0x1122334455667788ull); CHECK(c.x[30] == 0x1234);
    // ldaprb w2, [x3]; ldaprh w4, [x5]; retaa
    c.x[3] = uint64_t(cell); c.x[5] = uint64_t(cell); c.x[2] = c.x[4] = ~0ull;
    t_00000010(c);
    CHECK(c.x[2] == 0x88); CHECK(c.x[4] == 0x7788);
    // pacia x0, x1; autib x3, x4; xpaci x0; pacga x0, x1, x2; ret
    c.x[0] = 0xAAAA; c.x[1] = 5; c.x[2] = 6; c.x[3] = 0xBBBB;
    t_0000001C(c);
    uint64_t g = c.x[0];
    CHECK(c.x[3] == 0xBBBB);
    CHECK((g & 0xFFFFFFFFull) == 0); CHECK(g == nx::pacga(5, 6)); CHECK(g != nx::pacga(5, 7)); CHECK(g != nx::pacga(6, 6));
    // blraa x8, x9; ret: calls x8 with LR set to the next instruction
    c.x[8] = 0x4000ABCD; nx::g_called = 0;
    t_00000030(c);
    CHECK(nx::g_called == 0x4000ABCD); CHECK(c.x[30] == 0x34);
    // braaz x16: tail call
    c.x[16] = 0x4000BEEF; nx::g_called = 0;
    t_00000038(c);
    CHECK(nx::g_called == 0x4000BEEF);
    std::printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
"""

NATIVE_FUNCS = [  # (start, words)
    (0x00, [0xD503237F, 0xF8BFC020, 0xD50323FF, 0xD65F0FFF]),
    (0x10, [0x38BFC062, 0x78BFC0A4, 0xD65F0BFF]),
    (0x1C, [0xDAC10020, 0xDAC11483, 0xDAC143E0, 0x9AC23020, 0xD65F03C0]),
    (0x30, [0xD73F0909, 0xD65F03C0]),
    (0x38, [0xD61F0A1F]),
]


def _clang_cl():
    import shutil
    for c in (os.environ.get("NX_CLANG_CL"), shutil.which("clang-cl"), r"C:\msys64\ucrt64\bin\clang-cl.exe"):
        if c and os.path.exists(c):
            return c
    return None


@pytest.mark.skipif(os.name != "nt" or not _clang_cl() or not os.environ.get("INCLUDE"),
                    reason="needs clang-cl and a VS developer environment (INCLUDE/LIB)")
def test_v83_native(tmp_path):
    """Emit the functions above with the real emitter, compile them with clang-cl and run them."""
    import subprocess
    words = []
    for start, ws in NATIVE_FUNCS:
        assert start == len(words) * 4
        words += ws
    code = struct.pack(f"<{len(words)}I", *words)
    m = nso.Module("t", bytearray(code), (0, len(code)), (0, 0), (len(code), 0), (len(code), 0))
    an = analysis.Analysis(module=m, insns={})
    ends = [s for s, _ in NATIVE_FUNCS[1:]] + [len(code)]
    an.functions = [analysis.Function(s, e) for (s, _), e in zip(NATIVE_FUNCS, ends)]
    e = emit.Emitter(an, prefix="t")
    bodies = [e.function(f) for f in an.functions]
    assert e.unhandled == {}
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src = tmp_path / "v83.cpp"
    decls = "".join(f"void {e.fn(f.start)}(nx::CPU& c);\n" for f in an.functions)
    src.write_text("#include <cstdlib>\n#include <nx/cpu.h>\n#define NXB 0ull\n" + decls + "\n".join(bodies)
                   + NATIVE_MAIN)
    exe = tmp_path / "v83.exe"
    r = subprocess.run([_clang_cl(), "/nologo", "/std:c++20", "/O1", "/arch:AVX2", "/EHsc", f"/I{root}/runtime/include",
                        "-Wno-everything", str(src), f"/Fe{exe}", f"/Fo{tmp_path}/"], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout[-3000:] + r.stderr[-3000:]
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    assert r.returncode == 0 and r.stdout.strip() == "ok", r.stdout


def test_prologue_epilogue_function():
    """A typical Switch 2 leaf: pacibsp; ldapr x0, [x1]; autibsp; retab. Must end in a return, no fall-through."""
    e, f = emitter_for([0xD503237F, 0xF8BFC020, 0xD50323FF, 0xD65F0FFF])
    out = e.function(f)
    assert e.unhandled == {}
    body = [ln.strip() for ln in out.splitlines()[1:-1]]
    assert body[-1].startswith("return;"), out
    assert "nx::call" not in out  # retab must not fall through into the next function
