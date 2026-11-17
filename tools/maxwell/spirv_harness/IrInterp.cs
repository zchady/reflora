// CPU interpreter for the translator's intermediate representation, before SSA: one thread, 32-bit values.
//
// Purpose: semantic equivalence tests. The Ampere -> Maxwell lowering (tools/ampere/lower.py) and Nintendo's own
// Maxwell build of the same shader go through the same Ryujinx front end; running both IR programs on identical
// inputs and comparing what they write tells whether the lowered program computes what the original computes.
// "spirv-val passes" only says the SPIR-V is well formed; this says the values agree.
//
// The inputs are synthetic and deterministic: constant buffers filled from a seed, vertex attributes from a seed,
// textures replaced by a smooth function of the texture handle and of every sampling source (coordinates, LOD,
// offsets), so two programs that sample the same texture at the same place see the same texel. This is a test
// double, used only here; it is never presented as the game's output.

using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;

namespace SpirvHarness
{
    sealed class InterpError : Exception
    {
        public InterpError(string m) : base(m) { }
    }

    sealed class IrEnv
    {
        public readonly int Seed;
        public readonly Dictionary<(int, int), uint> VertexInputs = new();   // (location, component) -> bits
        public Dictionary<(int, int, int), uint> StageInputs = new();       // (ioVariable, index, comp) for PS
        public float[] FragCoord = { 100.5f, 50.5f, 0.5f, 1.0f };

        // Vertex input location of this build -> canonical location (the other build's), when two builds of a
        // shader gave the same named attribute different locations (from the BNSH reflection, --in-map).
        public Dictionary<int, int> InAlias = new();
        // --ir-run: pixel shader inputs from the hash (keyed by location, component) instead of a vertex stage
        public bool SyntheticStageInputs;

        public IrEnv(int seed) { Seed = seed; }

        // Constant buffer word: a float in [-2, 2) from a hash of (seed, slot, word). Bank 0 (the driver bank) gets
        // small values too; that is fine for float math, and both programs see the same words.
        public uint Cbuf(int slot, int word)
        {
            uint h = Hash((uint)Seed, (uint)slot, (uint)word, 0x51ed270b);
            // one word in four is a small integer (0..8): shaders read flags, counts and indices from constant
            // buffers, and a float read as an integer would send a loop round billions of times
            // (even seeds: three words in four, so that loops bounded by constant-buffer counts terminate)
            if ((Seed & 1) == 0 ? (h & 3) != 3 : (h & 3) == 0) return (h >> 8) % 9;
            float f = (h >> 8) / (float)(1 << 24) * 4f - 2f;
            return BitConverter.SingleToUInt32Bits(f);
        }

        public uint VertexAttr(int loc, int comp)
        {
            if (InAlias.TryGetValue(loc, out int canon)) loc = canon;
            if (VertexInputs.TryGetValue((loc, comp), out uint v)) return v;
            uint h = Hash((uint)Seed, (uint)loc, (uint)comp, 0x2545f491);
            float f = (h >> 8) / (float)(1 << 24) * 2f - 1f;
            if (comp == 3) f = 1f;  // w of a position-like attribute
            return BitConverter.SingleToUInt32Bits(f);
        }

        public uint StageAttr(int loc, int comp)
        {
            uint h = Hash((uint)Seed, (uint)loc, (uint)comp, 0x7f4a7c15);
            float f = (h >> 8) / (float)(1 << 24) * 2f - 1f;
            return BitConverter.SingleToUInt32Bits(f);
        }

        public static uint Hash(uint a, uint b, uint c, uint d)
        {
            ulong x = a * 0x9E3779B97F4A7C15UL ^ b * 0xC2B2AE3D27D4EB4FUL ^ c * 0x165667B19E3779F9UL ^ d;
            x ^= x >> 33; x *= 0xff51afd7ed558ccdUL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53UL; x ^= x >> 33;
            return (uint)x;
        }

        // The synthetic texture: smooth in every source, different per handle and per component.
        public static float Texel(int handle, float[] src, int comp)
        {
            double s = 0.37 * handle + 0.11 * comp;
            for (int i = 0; i < src.Length; i++) s += (1.3 + 0.7 * i + 0.2 * comp) * src[i];
            return (float)(0.5 + 0.45 * Math.Sin(s));
        }
    }

    sealed class IrMachine
    {
        readonly Operation[] _code;
        readonly Dictionary<Operand, int> _labels = new();
        readonly Dictionary<int, int> _texHandleByBinding;
        readonly Dictionary<int, int> _cbufSlotByBinding;
        readonly IrEnv _env;
        public byte[] Const1 = Array.Empty<byte>();   // c[1]: the constant data the compiler appended to the block
        readonly Dictionary<Operand, uint> _locals = new(ReferenceEqualityComparer.Instance);
        readonly Dictionary<(RegisterType, int), uint> _regs = new();
        readonly Dictionary<int, uint> _localMem = new();
        public readonly SortedDictionary<string, uint> Outputs = new(StringComparer.Ordinal);
        public bool Discarded;
        public long Steps;
        public readonly SortedSet<string> Notes = new();
        public readonly List<string> Reads = new();   // input / constant buffer reads, in order (for --dump)

        public IrMachine(Operation[] code, IrEnv env, Dictionary<int, int> texHandleByBinding,
                         Dictionary<int, int> cbufSlotByBinding)
        {
            _code = code;
            _env = env;
            _texHandleByBinding = texHandleByBinding;
            _cbufSlotByBinding = cbufSlotByBinding;
            for (int i = 0; i < code.Length; i++)
                if (code[i].Inst == Instruction.MarkLabel) _labels[code[i].Dest ?? code[i].GetSource(0)] = i;
        }

        uint Get(Operand o)
        {
            switch (o.Type)
            {
                case OperandType.Constant: return (uint)o.Value;
                case OperandType.ConstantBuffer: return CbufWord(o.GetCbufSlot(), o.GetCbufOffset());
                case OperandType.LocalVariable:
                    if (_locals.TryGetValue(o, out uint v)) return v;
                    Notes.Add("read of an unset local");
                    return 0;
                case OperandType.Register:
                    Register r = o.GetRegister();
                    if (_regs.TryGetValue((r.Type, r.Index), out uint rv)) return rv;
                    Notes.Add($"read of unset {r.Type} {r.Index}");
                    return 0;
                case OperandType.Undefined:
                    Notes.Add("read of undefined");
                    return 0;
                default:
                    throw new InterpError($"operand type {o.Type}");
            }
        }

        // c[1] holds the program's own inline constants (as NVN binds it); every other bank is synthetic input
        uint CbufWord(int slot, int word)
        {
            if (slot == 1)
                return word >= 0 && word * 4 + 4 <= Const1.Length ? BitConverter.ToUInt32(Const1, word * 4) : 0;
            return _env.Cbuf(slot, word);
        }

        static float F(uint v) => BitConverter.UInt32BitsToSingle(v);
        static float MinNum(float a, float b) => float.IsNaN(a) ? b : float.IsNaN(b) ? a : MathF.Min(a, b);
        static float MaxNum(float a, float b) => float.IsNaN(a) ? b : float.IsNaN(b) ? a : MathF.Max(a, b);
        static uint U(float f) => BitConverter.SingleToUInt32Bits(f);
        static uint B(bool b) => b ? 0xFFFFFFFFu : 0u;

        void Set(Operand d, uint v)
        {
            if (d == null) return;
            if (d.Type == OperandType.Register)
            {
                Register r = d.GetRegister();
                if (r.Type == RegisterType.Gpr && r.Index == RegisterConsts.RegisterZeroIndex) return;
                _regs[(r.Type, r.Index)] = v;
            }
            else if (d.Type == OperandType.LocalVariable) _locals[d] = v;
            else throw new InterpError($"dest type {d.Type}");
        }

        public void Run(long maxSteps = 2_000_000)
        {
            int pc = 0;
            while (pc < _code.Length)
            {
                if (++Steps > maxSteps) throw new InterpError("step limit");
                Operation op = _code[pc];
                int next = pc + 1;
                Instruction inst = op.Inst & Instruction.Mask;
                bool fp = (op.Inst & Instruction.FP32) != 0;
                if ((op.Inst & Instruction.FP64) != 0) throw new InterpError("FP64");
                uint S(int i) => Get(op.GetSource(i));
                float Fs(int i) => F(Get(op.GetSource(i)));
                switch (inst)
                {
                    case Instruction.Comment:
                    case Instruction.MarkLabel:
                    case Instruction.MemoryBarrier:
                    case Instruction.GroupMemoryBarrier:
                    case Instruction.Barrier:
                        break;
                    case Instruction.Branch: next = Target(op.Dest); break;
                    case Instruction.BranchIfTrue: if (S(0) != 0) next = Target(op.Dest); break;
                    case Instruction.BranchIfFalse: if (S(0) == 0) next = Target(op.Dest); break;
                    case Instruction.Return: return;
                    case Instruction.Discard: Discarded = true; return;
                    case Instruction.Copy: Set(op.Dest, S(0)); break;

                    case Instruction.Absolute: Set(op.Dest, fp ? U(MathF.Abs(Fs(0))) : (uint)Math.Abs((int)S(0))); break;
                    case Instruction.Negate: Set(op.Dest, fp ? U(-Fs(0)) : (uint)(-(int)S(0))); break;
                    case Instruction.Add: Set(op.Dest, fp ? U(Fs(0) + Fs(1)) : S(0) + S(1)); break;
                    case Instruction.Subtract: Set(op.Dest, fp ? U(Fs(0) - Fs(1)) : S(0) - S(1)); break;
                    case Instruction.Multiply: Set(op.Dest, fp ? U(Fs(0) * Fs(1)) : S(0) * S(1)); break;
                    case Instruction.Divide:
                        if (fp) Set(op.Dest, U(Fs(0) / Fs(1)));
                        else Set(op.Dest, S(1) == 0 ? 0xFFFFFFFF : (uint)((int)S(0) / (int)S(1)));
                        break;
                    case Instruction.FusedMultiplyAdd: Set(op.Dest, U(MathF.FusedMultiplyAdd(Fs(0), Fs(1), Fs(2)))); break;
                    // GLSL.std.450 FMin/FMax leave NaN undefined; GPUs return the other operand (IEEE minNum/maxNum),
                    // which is also what the Maxwell/Ampere FMNMX does
                    case Instruction.Minimum: Set(op.Dest, fp ? U(MinNum(Fs(0), Fs(1))) : (uint)Math.Min((int)S(0), (int)S(1))); break;
                    case Instruction.Maximum: Set(op.Dest, fp ? U(MaxNum(Fs(0), Fs(1))) : (uint)Math.Max((int)S(0), (int)S(1))); break;
                    case Instruction.MinimumU32: Set(op.Dest, Math.Min(S(0), S(1))); break;
                    case Instruction.MaximumU32: Set(op.Dest, Math.Max(S(0), S(1))); break;
                    case Instruction.Clamp:
                        Set(op.Dest, fp ? U(MinNum(MaxNum(Fs(0), Fs(1)), Fs(2)))
                                        : (uint)Math.Min(Math.Max((int)S(0), (int)S(1)), (int)S(2)));
                        break;
                    case Instruction.ClampU32: Set(op.Dest, Math.Min(Math.Max(S(0), S(1)), S(2))); break;
                    case Instruction.ReciprocalSquareRoot: Set(op.Dest, U(1f / MathF.Sqrt(Fs(0)))); break;
                    case Instruction.SquareRoot: Set(op.Dest, U(MathF.Sqrt(Fs(0)))); break;
                    case Instruction.ExponentB2: Set(op.Dest, U(MathF.Pow(2f, Fs(0)))); break;
                    case Instruction.LogarithmB2: Set(op.Dest, U(MathF.Log2(Fs(0)))); break;
                    case Instruction.Sine: Set(op.Dest, U(MathF.Sin(Fs(0)))); break;
                    case Instruction.Cosine: Set(op.Dest, U(MathF.Cos(Fs(0)))); break;
                    case Instruction.Floor: Set(op.Dest, U(MathF.Floor(Fs(0)))); break;
                    case Instruction.Ceiling: Set(op.Dest, U(MathF.Ceiling(Fs(0)))); break;
                    case Instruction.Truncate: Set(op.Dest, U(MathF.Truncate(Fs(0)))); break;
                    case Instruction.Round: Set(op.Dest, U(MathF.Round(Fs(0), MidpointRounding.ToEven))); break;
                    case Instruction.IsNan: Set(op.Dest, B(float.IsNaN(Fs(0)))); break;

                    case Instruction.CompareEqual: Set(op.Dest, B(fp ? Fs(0) == Fs(1) : S(0) == S(1))); break;
                    case Instruction.CompareNotEqual: Set(op.Dest, B(fp ? Fs(0) != Fs(1) : S(0) != S(1))); break;
                    case Instruction.CompareLess: Set(op.Dest, B(fp ? Fs(0) < Fs(1) : (int)S(0) < (int)S(1))); break;
                    case Instruction.CompareLessOrEqual: Set(op.Dest, B(fp ? Fs(0) <= Fs(1) : (int)S(0) <= (int)S(1))); break;
                    case Instruction.CompareGreater: Set(op.Dest, B(fp ? Fs(0) > Fs(1) : (int)S(0) > (int)S(1))); break;
                    case Instruction.CompareGreaterOrEqual: Set(op.Dest, B(fp ? Fs(0) >= Fs(1) : (int)S(0) >= (int)S(1))); break;
                    case Instruction.CompareLessU32: Set(op.Dest, B(S(0) < S(1))); break;
                    case Instruction.CompareLessOrEqualU32: Set(op.Dest, B(S(0) <= S(1))); break;
                    case Instruction.CompareGreaterU32: Set(op.Dest, B(S(0) > S(1))); break;
                    case Instruction.CompareGreaterOrEqualU32: Set(op.Dest, B(S(0) >= S(1))); break;
                    case Instruction.ConditionalSelect: Set(op.Dest, S(0) != 0 ? S(1) : S(2)); break;

                    case Instruction.LogicalAnd: Set(op.Dest, B(S(0) != 0 && S(1) != 0)); break;
                    case Instruction.LogicalOr: Set(op.Dest, B(S(0) != 0 || S(1) != 0)); break;
                    case Instruction.LogicalExclusiveOr: Set(op.Dest, B((S(0) != 0) ^ (S(1) != 0))); break;
                    case Instruction.LogicalNot: Set(op.Dest, B(S(0) == 0)); break;
                    case Instruction.BitwiseAnd: Set(op.Dest, S(0) & S(1)); break;
                    case Instruction.BitwiseOr: Set(op.Dest, S(0) | S(1)); break;
                    case Instruction.BitwiseExclusiveOr: Set(op.Dest, S(0) ^ S(1)); break;
                    case Instruction.BitwiseNot: Set(op.Dest, ~S(0)); break;
                    case Instruction.ShiftLeft: Set(op.Dest, S(1) >= 32 ? 0 : S(0) << (int)S(1)); break;
                    case Instruction.ShiftRightU32: Set(op.Dest, S(1) >= 32 ? 0 : S(0) >> (int)S(1)); break;
                    case Instruction.ShiftRightS32: Set(op.Dest, (uint)((int)S(0) >> (int)Math.Min(S(1), 31))); break;
                    case Instruction.BitCount: Set(op.Dest, (uint)System.Numerics.BitOperations.PopCount(S(0))); break;
                    case Instruction.BitfieldReverse: Set(op.Dest, Reverse(S(0))); break;
                    case Instruction.BitfieldExtractU32:
                    {
                        int off = (int)S(1), w = (int)S(2);
                        Set(op.Dest, w == 0 ? 0 : (uint)((S(0) >> off) & (w >= 32 ? 0xFFFFFFFF : ((1u << w) - 1))));
                        break;
                    }
                    case Instruction.BitfieldExtractS32:
                    {
                        int off = (int)S(1), w = (int)S(2);
                        if (w == 0) { Set(op.Dest, 0); break; }
                        int v = (int)(S(0) << (32 - off - w)) >> (32 - w);
                        Set(op.Dest, (uint)v);
                        break;
                    }
                    case Instruction.BitfieldInsert:
                    {
                        uint b = S(0), ins = S(1); int off = (int)S(2), w = (int)S(3);
                        uint mask = (w >= 32 ? 0xFFFFFFFF : ((1u << w) - 1)) << off;
                        Set(op.Dest, (b & ~mask) | ((ins << off) & mask));
                        break;
                    }
                    case Instruction.FindMSBU32: Set(op.Dest, S(0) == 0 ? 0xFFFFFFFF : (uint)(31 - System.Numerics.BitOperations.LeadingZeroCount(S(0)))); break;
                    case Instruction.FindMSBS32:
                    {
                        int v = (int)S(0); uint x = v < 0 ? ~(uint)v : (uint)v;
                        Set(op.Dest, x == 0 ? 0xFFFFFFFF : (uint)(31 - System.Numerics.BitOperations.LeadingZeroCount(x)));
                        break;
                    }
                    case Instruction.FindLSB: Set(op.Dest, S(0) == 0 ? 0xFFFFFFFF : (uint)System.Numerics.BitOperations.TrailingZeroCount(S(0))); break;
                    case Instruction.MultiplyHighS32: Set(op.Dest, (uint)(((long)(int)S(0) * (int)S(1)) >> 32)); break;
                    case Instruction.MultiplyHighU32: Set(op.Dest, (uint)(((ulong)S(0) * S(1)) >> 32)); break;

                    case Instruction.ConvertFP32ToS32: Set(op.Dest, (uint)SatS32(Fs(0))); break;
                    case Instruction.ConvertFP32ToU32: Set(op.Dest, SatU32(Fs(0))); break;
                    case Instruction.ConvertS32ToFP32: Set(op.Dest, U((int)S(0))); break;
                    case Instruction.ConvertU32ToFP32: Set(op.Dest, U(S(0))); break;
                    case Instruction.PackHalf2x16:
                        Set(op.Dest, (uint)BitConverter.HalfToUInt16Bits((Half)Fs(0)) | ((uint)BitConverter.HalfToUInt16Bits((Half)Fs(1)) << 16));
                        break;
                    case Instruction.UnpackHalf2x16:
                    {
                        uint v = S(0);
                        int idx = op.Index;
                        Set(op.Dest, U((float)BitConverter.UInt16BitsToHalf((ushort)(idx == 0 ? v & 0xFFFF : v >> 16))));
                        break;
                    }
                    case Instruction.Ddx:
                    case Instruction.Ddy:
                        Notes.Add("derivative: 0 in a single-thread run");
                        Set(op.Dest, 0);
                        break;

                    case Instruction.Load: Load(op); break;
                    case Instruction.Store: Store(op); break;
                    case Instruction.TextureSample: Sample((TextureOperation)op); break;
                    case Instruction.Lod:
                    {
                        TextureOperation t = (TextureOperation)op;
                        float[] src = Floats(t);
                        Set(op.Dest, U(op.Index == 0 ? Math.Clamp(IrEnv.Texel(Handle(t) + 50, src, 0) * 3f, 0f, 2.5f)
                                                      : IrEnv.Texel(Handle(t) + 50, src, 0) * 3f));
                        break;
                    }
                    case Instruction.TextureQuerySize:
                    {
                        TextureOperation t = (TextureOperation)op;
                        Set(op.Dest, op.Index < 2 ? (uint)(256 >> Math.Min((int)(t.SourcesCount > 0 ? S(0) : 0), 8)) : 1u);
                        break;
                    }
                    default:
                        throw new InterpError($"instruction {op.Inst}");
                }
                pc = next;
            }
        }

        int Target(Operand label)
        {
            if (!_labels.TryGetValue(label, out int i)) throw new InterpError("unknown label");
            return i;
        }

        static uint Reverse(uint v)
        {
            uint r = 0;
            for (int i = 0; i < 32; i++) { r = (r << 1) | (v & 1); v >>= 1; }
            return r;
        }

        static int SatS32(float f) => float.IsNaN(f) ? 0 : f >= 2147483647f ? int.MaxValue : f <= -2147483648f ? int.MinValue : (int)f;
        static uint SatU32(float f) => float.IsNaN(f) || f <= 0 ? 0 : f >= 4294967295f ? uint.MaxValue : (uint)f;

        int Handle(TextureOperation t)
        {
            if ((t.Flags & TextureFlags.Bindless) != 0) return 2000 + (int)(Get(t.GetSource(0)) & 0xFFFF);
            return _texHandleByBinding.TryGetValue(t.Binding, out int h) ? h : 1000 + t.Binding;
        }

        float[] Floats(TextureOperation t)
        {
            int first = (t.Flags & TextureFlags.Bindless) != 0 ? 1 : 0;
            float[] src = new float[t.SourcesCount - first];
            for (int i = 0; i < src.Length; i++)
            {
                uint v = Get(t.GetSource(i + first));
                float f = F(v);
                // integer sources (offsets, array layers given as int) are used as their integer value
                src[i] = float.IsNaN(f) || Math.Abs(f) > 1e6f || (v != 0 && Math.Abs(f) < 1e-30f) ? (int)v : f;
            }
            return src;
        }

        void Sample(TextureOperation t)
        {
            float[] src = Floats(t);
            int h = Handle(t);
            int comp = 0;
            for (int c = 0; c < 4; c++)
            {
                if ((t.Index & (1 << c)) == 0) continue;
                if (comp < t.DestsCount) Set(t.GetDest(comp), U(IrEnv.Texel(h, src, c)));
                comp++;
            }
        }

        void Load(Operation op)
        {
            switch (op.StorageKind)
            {
                case StorageKind.Input:
                {
                    var io = (IoVariable)op.GetSource(0).Value;
                    int a = op.SourcesCount > 1 ? (int)Get(op.GetSource(1)) : 0;
                    int b = op.SourcesCount > 2 ? (int)Get(op.GetSource(2)) : 0;
                    uint v;
                    if (io == IoVariable.UserDefined && _env.SyntheticStageInputs) v = _env.StageAttr(a, b);
                    else if (io == IoVariable.UserDefined && _env.StageInputs.Count == 0) v = _env.VertexAttr(a, b);
                    else if (io == IoVariable.FragmentCoord) v = U(_env.FragCoord[a]);
                    else if (_env.StageInputs.TryGetValue(((int)io, a, b), out uint sv)) v = sv;
                    else if (io == IoVariable.FrontFacing) v = 0xFFFFFFFF;
                    else if (io == IoVariable.UserDefined) { Notes.Add($"PS input {a}.{b} not written by VS"); v = 0; }
                    else { Notes.Add($"input {io}"); v = 0; }
                    Reads.Add($"in {io}[{a},{b}]={F(v):G4}");
                    Set(op.Dest, v);
                    break;
                }
                case StorageKind.ConstantBuffer:
                {
                    // (binding, field, vector, element) for buffers declared as structs (game constant buffers,
                    // field 0; the support buffer, field = SupportBufferField), (binding, vector, element) otherwise
                    int binding = op.GetSource(0).Value;
                    int n = op.SourcesCount;
                    int field = n >= 4 ? (int)Get(op.GetSource(1)) : 0;
                    int vec = (int)Get(op.GetSource(n - 2)), elem = (int)Get(op.GetSource(n - 1));
                    if (n == 2) { field = (int)Get(op.GetSource(1)); vec = elem = 0; }
                    if (_cbufSlotByBinding.TryGetValue(binding, out int slot))
                        Set(op.Dest, CbufWord(slot, vec * 4 + elem));
                    else if (binding == SupportBuffer.Binding && (field == (int)SupportBufferField.FragmentIsBgra ||
                                                                  (n == 3 && vec == (int)SupportBufferField.FragmentIsBgra)))
                        Set(op.Dest, 0);      // render targets are RGBA: no R/B swap
                    else
                        Set(op.Dest, U(1f));  // translator support buffer (render scale and similar): 1.0
                    break;
                }
                case StorageKind.LocalMemory:
                {
                    int addr = (int)Get(op.GetSource(0));
                    Set(op.Dest, _localMem.TryGetValue(addr, out uint v) ? v : 0);
                    break;
                }
                default:
                    throw new InterpError($"load from {op.StorageKind}");
            }
        }

        void Store(Operation op)
        {
            switch (op.StorageKind)
            {
                case StorageKind.Output:
                {
                    var io = (IoVariable)op.GetSource(0).Value;
                    var idx = new List<int>();
                    for (int i = 1; i < op.SourcesCount - 1; i++) idx.Add((int)Get(op.GetSource(i)));
                    uint v = Get(op.GetSource(op.SourcesCount - 1));
                    Outputs[$"{io}[{string.Join(",", idx)}]"] = v;
                    break;
                }
                case StorageKind.LocalMemory:
                    _localMem[(int)Get(op.GetSource(0))] = Get(op.GetSource(1));
                    break;
                default:
                    throw new InterpError($"store to {op.StorageKind}");
            }
        }
    }

    // Runs a translated stage's IR: builds the translator context the same way Translate() does, but stops after
    // the front end (Translator.EmitShader) and hands the operations to IrMachine.
    static class IrRunner
    {
        public static byte[] Const1(byte[] code, byte[] ctrl)
        {
            int constSize = BitConverter.ToInt32(ctrl, 0x6fc), constOff = BitConverter.ToInt32(ctrl, 0x700);
            return constSize > 0 && constOff + constSize <= code.Length ? code[constOff..(constOff + constSize)] : Array.Empty<byte>();
        }

        public static (Operation[] code, Dictionary<int, int> tex, Dictionary<int, int> cbuf) Emit(byte[] code, byte[] ctrl)
        {
            int start = BitConverter.ToInt32(ctrl, 0x708);
            int constSize = BitConverter.ToInt32(ctrl, 0x6fc);
            int constOff = BitConverter.ToInt32(ctrl, 0x700);
            bool compute = BitConverter.ToUInt32(code, 0) == 0x12345679;
            int sphType = compute ? 0 : (BitConverter.ToInt32(code, start) >> 10) & 0xf;
            ShaderStage stage = compute ? ShaderStage.Compute : sphType == 5 ? ShaderStage.Fragment : ShaderStage.Vertex;
            StaticGpuAccessor gpu = new(code, constOff, constSize, stage, ctrl);
            TranslationFlags flags = (compute ? TranslationFlags.Compute : TranslationFlags.None) | TranslationFlags.DebugMode;
            TranslationOptions options = new(TargetLanguage.Spirv, TargetApi.Vulkan, flags);
            TranslatorContext ctx = Translator.CreateContext((ulong)start, gpu, options);

            var t = typeof(TranslatorContext);
            const System.Reflection.BindingFlags NP = System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance;
            var rm = (ResourceManager)t.GetMethod("CreateResourceManager", NP).Invoke(ctx, new object[] { false });
            var program = t.GetField("_program", NP).GetValue(ctx);
            var emit = typeof(Translator).GetMethod("EmitShader", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
            object[] args = { ctx, rm, program, false, true, 0 };
            var funcs = (Array)emit.Invoke(null, args);
            if (funcs.Length != 1) throw new InterpError($"{funcs.Length} functions (calls are not interpreted)");
            var fc = funcs.GetValue(0);
            var ops = (Operation[])fc.GetType().GetProperty("Code").GetValue(fc);

            var tex = new Dictionary<int, int>();
            foreach (TextureDescriptor d in rm.GetTextureDescriptors()) tex[d.Binding] = d.HandleIndex;
            // constant buffer bindings are created on demand (ResourceManager.GetConstantBufferBinding), so ask for
            // every slot: the same binding the front end got, or a new one nobody uses
            var cb = new Dictionary<int, int>();
            for (int slot = 0; slot < 18; slot++) cb[rm.GetConstantBufferBinding(slot)] = slot;
            return (ops, tex, cb);
        }
    }
}
