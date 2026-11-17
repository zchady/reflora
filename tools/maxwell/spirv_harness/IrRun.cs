// spirv_harness --ir-run: run every block of an extracted directory through the IR interpreter, stage by stage,
// and write what each one outputs (one JSON line per block and seed). tools/ampere/ref_test.py runs the original
// Ampere code of the same blocks with its reference interpreter on identical inputs and compares.
//
//   spirv_harness --ir-run <dir with manifest.tsv> --out <file.jsonl> [--seeds 8] [--limit N]
//
// Inputs (IrEnv): constant buffers and vertex attributes from the seed; pixel shader inputs synthetic too (the same
// hash, keyed by location and component) with FragCoord = (100.5, 50.5, 0.5, 0.8), so perspective-corrected inputs
// are exercised; c[1] = the block's own constants.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace SpirvHarness
{
    static class IrRun
    {
        // --ir-dump <code> <ctrl>: the front end's operations for one block, as text
        public static int Dump(string[] args)
        {
            var (ops, tex, cb) = IrRunner.Emit(File.ReadAllBytes(args[1]), File.ReadAllBytes(args[2]));
            string O(Ryujinx.Graphics.Shader.IntermediateRepresentation.Operand o) => o == null ? "-" : o.Type switch
            {
                Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandType.Constant => $"#{o.Value:x}",
                Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandType.ConstantBuffer => $"c{o.GetCbufSlot()}[{o.GetCbufOffset()}]",
                Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandType.Register => $"{o.GetRegister().Type}{o.GetRegister().Index}",
                Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandType.LocalVariable => $"L{o.GetHashCode() % 10000}",
                _ => o.Type.ToString(),
            };
            foreach (var op in ops)
            {
                var srcs = new List<string>();
                for (int i = 0; i < op.SourcesCount; i++) srcs.Add(O(op.GetSource(i)));
                string extra = op is Ryujinx.Graphics.Shader.IntermediateRepresentation.TextureOperation t
                    ? $" [{t.Type} {t.Flags} binding {t.Binding} mask/idx {t.Index} handle {(tex.TryGetValue(t.Binding, out int h) ? h : -1)}]" : "";
                Console.WriteLine($"{op.Inst} {op.StorageKind} {O(op.Dest)} <- {string.Join(", ", srcs)}{extra}");
            }
            return 0;
        }

        public static int Run(string[] args)
        {
            string dir = args[1];
            string outPath = Opt(args, "--out", "ir_run.jsonl");
            int seeds = int.Parse(Opt(args, "--seeds", "8"));
            int limit = int.Parse(Opt(args, "--limit", "0"));
            var seen = new HashSet<string>();
            int n = 0, errors = 0;
            using var w = new StreamWriter(outPath) { NewLine = "\n" };
            foreach (string line in File.ReadLines(Path.Combine(dir, "manifest.tsv")).Skip(1))
            {
                string[] r = line.Split('\t');
                if (r.Length < 13 || !seen.Add(r[11])) continue;
                if (limit > 0 && n >= limit) break;
                n++;
                byte[] code = File.ReadAllBytes(Path.Combine(dir, r[11]));
                byte[] ctrl = File.ReadAllBytes(Path.Combine(dir, r[12]));
                bool compute = code.Length < 4 || BitConverter.ToUInt32(code, 0) == 0x12345679;
                if (compute || ctrl.Length < 0x70c) continue;
                int start = BitConverter.ToInt32(ctrl, 0x708);
                if (start + 4 > code.Length) continue;
                bool pixel = ((BitConverter.ToInt32(code, start) >> 10) & 0xf) == 5;
                (Ryujinx.Graphics.Shader.IntermediateRepresentation.Operation[] ops, Dictionary<int, int> tex, Dictionary<int, int> cb) emitted;
                try
                {
                    emitted = IrRunner.Emit(code, ctrl);
                }
                catch (Exception e)
                {
                    errors++;
                    w.WriteLine($"{{\"code\":\"{Esc(r[11])}\",\"error\":\"emit: {Esc((e.InnerException ?? e).Message)}\"}}");
                    continue;
                }
                byte[] c1 = IrRunner.Const1(code, ctrl);
                for (int s = 1; s <= seeds; s++)
                {
                    var env = new IrEnv(s) { SyntheticStageInputs = pixel, FragCoord = new[] { 100.5f, 50.5f, 0.5f, 0.8f } };
                    var m = new IrMachine(emitted.ops, env, emitted.tex, emitted.cb) { Const1 = c1 };
                    var sb = new StringBuilder();
                    sb.Append($"{{\"code\":\"{Esc(r[11])}\",\"seed\":{s},\"stage\":\"{(pixel ? "pixel" : "vertex")}\"");
                    try
                    {
                        m.Run();
                        sb.Append($",\"discard\":{(m.Discarded ? "true" : "false")},\"out\":{{");
                        sb.Append(string.Join(",", m.Outputs.Select(kv => $"\"{kv.Key}\":{kv.Value}")));
                        sb.Append('}');
                    }
                    catch (Exception e)
                    {
                        // any failure of one block (interpreter limit or a front-end exception) is reported for
                        // that block; the run goes on
                        sb.Append($",\"error\":\"{Esc((e is InterpError ? "" : e.GetType().Name + ": ") + e.Message)}\"");
                        errors++;
                    }
                    if (m.Notes.Count > 0) sb.Append($",\"notes\":[{string.Join(",", m.Notes.Select(x => $"\"{Esc(x)}\""))}]");
                    sb.Append('}');
                    w.WriteLine(sb.ToString());
                }
            }
            Console.WriteLine($"{n} blocks, {seeds} seeds, {errors} errors -> {outPath}");
            return 0;
        }

        static string Esc(string s) => s.Replace("\\", "\\\\").Replace("\"", "\\\"");

        static string Opt(string[] args, string name, string def)
        {
            int i = Array.IndexOf(args, name);
            return i >= 0 && i + 1 < args.Length ? args[i + 1] : def;
        }
    }
}
