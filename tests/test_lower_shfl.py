"""SHFL lowering and spilled uniform predicates (tools/ampere/lower.py), on instruction words alone, no game data.

Wonder's Switch 2 cut-scene fragment shaders exchange values inside the pixel quad with SHFL (mode IDX, clamp/segment
0x1c03, lane 0/1/2 as an immediate or RZ) to build derivatives by hand. Maxwell has the same instruction: the lowering
maps it field for field (Ryujinx InstEmitWarp reads BFixShfl / CFixShfl as "operand is an immediate"). The two words
below are the encodings seen in those shaders (decoded by tools/ampere/sm86.py).

Some of the cut-scene vertex shaders use more uniform predicates than there are free regular ones, so the lowering
keeps them in registers ("spilled"). _writes_pred, asked whether an instruction writes a given regular predicate, must
not look a spilled uniform predicate up as a regular one (it raised "uniform predicate outside an instruction that
loads it" before)."""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools", "ampere"))
import lower  # noqa: E402
import sm86  # noqa: E402


def lowerer():
    lw = lower.Lowerer([], "pixel", 32, b"")
    lw.guard = (lower.PT, False)
    return lw


def emitted(lw):
    return [p for k, p in lw.out.items if k == "w"]


def lower_word(hi, lo):
    i = sm86.decode(lo, hi)
    assert i.op == "SHFL"
    lw = lowerer()
    lw.op_SHFL(i, i.f)
    (name, f), = emitted(lw)
    assert name == "Shfl"
    return i, f


def test_shfl_immediate_lane_and_clamp():
    # SHFL dst=R10 src=R19 pdst=PT mode=IDX lane=0x1 c=0x1c03
    i, f = lower_word(0x000fe800000e0000, 0x003c0300130a7f89)
    assert (f["Dest"], f["SrcA"], f["ShflMode"]) == (10, 19, 0)
    assert f["BFixShfl"] and f["SrcBImm"] == 1
    assert f["CFixShfl"] and f["SrcCImm"] == 0x1C03
    assert f["DestPred"] == lower.PT


def test_shfl_register_lane():
    # SHFL dst=R10 src=R3 pdst=PT mode=IDX lane=RZ c=0x1c03
    i, f = lower_word(0x000e6800000e0000, 0x001c03ff030a7589)
    assert (f["Dest"], f["SrcA"]) == (10, 3)
    assert not f["BFixShfl"] and f["SrcB"] == lower.RZ
    assert f["CFixShfl"] and f["SrcCImm"] == 0x1C03


def test_spilled_uniform_predicate_writes_no_regular_predicate():
    lw = lowerer()
    lw.up_spill = {1: 200}  # uniform predicate UP1 lives in R200
    insn = sm86.Insn(0, 0, "ISETP", sm86.Pred(lower.PT), {"pdst": sm86.Pred(1, uniform=True)})
    assert lw._writes_pred(insn, 1) is False
    regular = sm86.Insn(0, 0, "ISETP", sm86.Pred(lower.PT), {"pdst": sm86.Pred(1)})
    assert lw._writes_pred(regular, 1) is True
