"""Instructions first met in the Switch 2 Edition of Wonder (SDK 21): saturating shifts, SQDMLAL, shift-and-insert,
TBX, FMAXV/FMINV, UMNEGL, STG. Mixed into emit.Emitter. Each form is checked against Unicorn by tests/difftest.py.

Forms not seen in the game raise Unhandled rather than guess (64-bit lanes of the saturating operations).
"""

from capstone import arm64_const as A

from .emit import Unhandled, VAS


class V8xOps:
    def _lanes32(self, ops):
        bits, _ = self.vinfo(ops[0])
        if bits > 32:
            raise Unhandled("64-bit lanes")
        return bits

    # saturating shifts
    def i_sqrshl(self, f, i, ops):
        if ops[2].type != A.ARM64_OP_REG:
            raise Unhandled("sqrshl immediate")
        bits = self._lanes32(ops)
        return self.v_bin(i, ops, f"nx::sqrshl<int{bits}_t>(int64_t(a), int8_t(S1.u{bits}[k]))", signed=True)

    def i_sqshl(self, f, i, ops):
        if ops[2].type != A.ARM64_OP_IMM:
            bits = self._lanes32(ops)
            return self.v_bin(i, ops, f"nx::sqshl<int{bits}_t>(int64_t(a), int8_t(S1.u{bits}[k]))", signed=True)
        bits = self._lanes32(ops)
        return self.v_shift_imm(i, ops, "nx::sat<int{bits}_t>({a} << {n})", signed=True)

    def i_sqshlu(self, f, i, ops):
        bits = self._lanes32(ops)
        return self.v_shift_imm(i, ops, "int{bits}_t(nx::sat<uint{bits}_t>({a} << {n}))", signed=True)

    # SQDMLAL{2}: o + sat(2 * a * b), both steps saturating to the wide lane.
    def i_sqdmlal(self, f, i, ops):
        dbits, _ = self.vinfo(ops[0])
        if dbits > 32:
            raise Unhandled("sqdmlal 2d")
        return self.widen(i, ops, f"nx::sat<int{dbits}_t>(int64_t({{o}}) + nx::sat<int{dbits}_t>(2 * {{a}} * {{b}}))",
                          True)

    i_sqdmlal2 = i_sqdmlal

    # shift and insert
    def i_sri(self, f, i, ops):
        bits, _ = self.vinfo(ops[0])
        mask = (1 << bits) - 1
        return self.v_shift_imm(i, ops, "({n} >= {bits}) ? uint64_t({o}) : ((uint64_t({o}) & ~(uint64_t(" + hex(mask) +
                                "ull) >> {n})) | ({a} >> {n}))", acc=True)

    def i_sli(self, f, i, ops):
        return self.v_shift_imm(i, ops, "(uint64_t({o}) & ((uint64_t(1) << {n}) - 1)) | ({a} << {n})", acc=True)

    # table lookup that keeps the destination byte for out-of-range indices
    def i_tbx(self, f, i, ops):
        bits, n = self.vinfo(ops[0])
        d = self.regop(i, ops[0])
        tables = [self.regop(i, o) for o in ops[1:-1]]
        idx = self.regop(i, ops[-1])
        cp = " ".join(f"std::memcpy(t_ + {16 * k}, &c.v[{r.n}], 16);" for k, r in enumerate(tables))
        clear = "" if n == 16 else " r.u64[1] = 0;"
        return (f"{{ uint8_t t_[64]; {cp} nx::V128 I_ = c.v[{idx.n}]; nx::V128 r = c.v[{d.n}]; "
                f"for (int k = 0; k < {n}; k++) if (I_.u8[k] < {16 * len(tables)}) r.u8[k] = t_[I_.u8[k]];{clear} "
                f"c.v[{d.n}] = r; }}")

    # across-lanes FP max/min (NaN propagating), pairwise as the hardware reduces
    def _fred(self, i, ops, fn):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        if VAS[ops[1].vas] != (32, 4):
            raise Unhandled("fmaxv/fminv arrangement")
        return (f"{{ nx::V128 S = c.v[{a.n}]; float s_ = nx::{fn}(nx::{fn}(S.f32[0], S.f32[1]), nx::{fn}(S.f32[2], S.f32[3])); "
                f"nx::V128 r = nx::vzero(); r.f32[0] = s_; c.v[{d.n}] = r; }}")

    def i_fmaxv(self, f, i, ops):
        return self._fred(i, ops, "fmax_")

    def i_fminv(self, f, i, ops):
        return self._fred(i, ops, "fmin_")

    def i_umnegl(self, f, i, ops):
        d, n, m = (self.regop(i, o) for o in ops[:3])
        return d.w(f"uint64_t(0) - uint64_t(uint32_t({n.r()})) * uint64_t(uint32_t({m.r()}))")

    def i_smnegl(self, f, i, ops):
        d, n, m = (self.regop(i, o) for o in ops[:3])
        return d.w(f"uint64_t(-(int64_t(int32_t({n.r()})) * int64_t(int32_t({m.r()}))))")

    # STG (memory tagging): the host has no allocation tags. Without writeback it changes nothing.
    def i_stg(self, f, i, ops):
        if i.writeback or len(ops) > 2:
            raise Unhandled("stg with writeback")
        return ";"
