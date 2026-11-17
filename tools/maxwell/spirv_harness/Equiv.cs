// spirv_harness --equiv: semantic equivalence of two builds of the same shader, on the CPU.
//
//   spirv_harness --equiv <A.vs.code> <A.vs.ctrl> <A.ps.code> <A.ps.ctrl> <B.vs.code> <B.vs.ctrl> <B.ps.code> <B.ps.ctrl>
//                 [--trials N] [--seed S] [--tol 1e-4]
//   spirv_harness --equiv-vs <A.code> <A.ctrl> <B.code> <B.ctrl> [...]     vertex stage alone (position only)
//
// A and B are two compiles of one vertex+pixel pair (here: the Ampere stage lowered to Maxwell, and Nintendo's
// Maxwell build of the same effect). The two compilers may give varyings different locations, so the pixel stage of
// each build reads what its own vertex stage wrote; what is compared is what the pipeline outputs: the position and
// every pixel output, plus whether the pixel was discarded. N trials with different seeds (constant buffers, vertex
// attributes). Differences above tol (relative, with an absolute floor) are reported per output.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace SpirvHarness
{
    static class Equiv
    {
        sealed class Result
        {
            public SortedDictionary<string, uint> Vs, Ps;
            public bool Discard;
            public SortedSet<string> Notes = new();
            public List<string> VsReads;
        }

        static Result Exec(byte[][] files, int seed, bool withPs, Dictionary<int, int> inAlias = null)
        {
            var env = new IrEnv(seed);
            if (inAlias != null) env.InAlias = inAlias;
            var (vsOps, vsTex, vsCb) = IrRunner.Emit(files[0], files[1]);
            var vs = new IrMachine(vsOps, env, vsTex, vsCb) { Const1 = IrRunner.Const1(files[0], files[1]) };
            vs.Run();
            var r = new Result { Vs = vs.Outputs, VsReads = vs.Reads };
            foreach (string n in vs.Notes) r.Notes.Add("vs: " + n);
            if (!withPs) return r;
            // pixel inputs = what the vertex stage wrote (one vertex: barycentrics (1,0,0))
            var inputs = new Dictionary<(int, int, int), uint>();
            foreach (var kv in vs.Outputs)
            {
                if (!kv.Key.StartsWith("UserDefined[")) continue;
                int[] ix = kv.Key[12..^1].Split(',').Select(int.Parse).ToArray();
                inputs[((int)Ryujinx.Graphics.Shader.IntermediateRepresentation.IoVariable.UserDefined, ix[0], ix[1])] = kv.Value;
            }
            env.StageInputs = inputs;
            float w = vs.Outputs.TryGetValue("Position[3]", out uint pw) ? BitConverter.UInt32BitsToSingle(pw) : 1f;
            float z = vs.Outputs.TryGetValue("Position[2]", out uint pz) ? BitConverter.UInt32BitsToSingle(pz) : 0f;
            env.FragCoord = new[] { 100.5f, 50.5f, w != 0 ? z / w : 0f, w != 0 ? 1f / w : 1f };
            var (psOps, psTex, psCb) = IrRunner.Emit(files[2], files[3]);
            var ps = new IrMachine(psOps, env, psTex, psCb) { Const1 = IrRunner.Const1(files[2], files[3]) };
            ps.Run();
            r.Ps = ps.Outputs;
            r.Discard = ps.Discarded;
            foreach (string n in ps.Notes) r.Notes.Add("ps: " + n);
            return r;
        }

        static bool Close(uint a, uint b, double tol, out double err)
        {
            float fa = BitConverter.UInt32BitsToSingle(a), fb = BitConverter.UInt32BitsToSingle(b);
            err = 0;
            if (a == b) return true;
            if (float.IsNaN(fa) && float.IsNaN(fb)) return true;
            err = Math.Abs(fa - fb) / Math.Max(1.0, Math.Max(Math.Abs(fa), Math.Abs(fb)));
            return err <= tol;
        }

        public static int Run(string[] args)
        {
            bool vsOnly = args[0] == "--equiv-vs";
            int nfiles = vsOnly ? 4 : 8;
            byte[][] all = args.Skip(1).Take(nfiles).Select(File.ReadAllBytes).ToArray();
            int trials = int.Parse(Opt(args, "--trials", "32"));
            int seed0 = int.Parse(Opt(args, "--seed", "1"));
            double tol = double.Parse(Opt(args, "--tol", "1e-4"), System.Globalization.CultureInfo.InvariantCulture);
            byte[][] a = vsOnly ? all.Take(2).ToArray() : all.Take(4).ToArray();
            byte[][] b = vsOnly ? all.Skip(2).ToArray() : all.Skip(4).ToArray();

            // --in-map a=b,...: vertex input location a in build A is location b in build B
            var bAlias = new Dictionary<int, int>();
            foreach (string pair in Opt(args, "--in-map", "").Split(',', StringSplitOptions.RemoveEmptyEntries))
            {
                string[] ab = pair.Split('=');
                bAlias[int.Parse(ab[1])] = int.Parse(ab[0]);
            }
            int bad = 0, compared = 0, discardBoth = 0;
            double worst = 0;
            var notes = new SortedSet<string>();
            var badKeys = new SortedDictionary<string, int>();
            for (int t = 0; t < trials; t++)
            {
                int seed = seed0 + t;
                Result ra, rb;
                try
                {
                    ra = Exec(a, seed, !vsOnly);
                    rb = Exec(b, seed, !vsOnly, bAlias);
                }
                catch (Exception e) when (e is InterpError || e.InnerException is InterpError)
                {
                    Console.WriteLine($"interpreter: {(e.InnerException ?? e).Message}");
                    return 3;
                }
                foreach (string n in ra.Notes) notes.Add("A " + n);
                foreach (string n in rb.Notes) notes.Add("B " + n);
                if (args.Contains("--dump") && t == 0)
                {
                    foreach (var (name, r) in new[] { ("A", ra), ("B", rb) })
                    {
                        Console.WriteLine($"[{name}] vs reads: " + string.Join(" ", r.VsReads.Take(40)));
                        Console.WriteLine($"[{name}] vs: " + string.Join(" ", r.Vs.Select(kv => $"{kv.Key}={BitConverter.UInt32BitsToSingle(kv.Value):G5}")));
                        if (r.Ps != null)
                            Console.WriteLine($"[{name}] ps{(r.Discard ? " (discarded)" : "")}: " + string.Join(" ", r.Ps.Select(kv => $"{kv.Key}={BitConverter.UInt32BitsToSingle(kv.Value):G5}")));
                    }
                }
                // position (fixed by the API) is always compared
                foreach (string k in new[] { "Position[0]", "Position[1]", "Position[2]", "Position[3]" })
                {
                    bool ha = ra.Vs.TryGetValue(k, out uint va), hb = rb.Vs.TryGetValue(k, out uint vb);
                    if (!ha && !hb) continue;
                    compared++;
                    if (ha != hb || !Close(va, vb, tol, out double e)) { bad++; badKeys[k] = badKeys.GetValueOrDefault(k) + 1; }
                    else worst = Math.Max(worst, e);
                }
                if (vsOnly) continue;
                if (ra.Discard != rb.Discard) { bad++; badKeys["discard"] = badKeys.GetValueOrDefault("discard") + 1; continue; }
                if (ra.Discard) { discardBoth++; continue; }
                foreach (string k in ra.Ps.Keys.Union(rb.Ps.Keys))
                {
                    bool ha = ra.Ps.TryGetValue(k, out uint va), hb = rb.Ps.TryGetValue(k, out uint vb);
                    compared++;
                    if (ha != hb || !Close(va, vb, tol, out double e))
                    {
                        bad++;
                        badKeys[k] = badKeys.GetValueOrDefault(k) + 1;
                        if (badKeys[k] == 1)
                            Console.WriteLine($"  seed {seed} {k}: A={(ha ? BitConverter.UInt32BitsToSingle(va).ToString("R") : "-")} " +
                                              $"B={(hb ? BitConverter.UInt32BitsToSingle(vb).ToString("R") : "-")}");
                    }
                    else worst = Math.Max(worst, e);
                }
            }
            Console.WriteLine($"{trials} trials, {compared} values compared, {bad} different, " +
                              $"{discardBoth} trials discarded by both, worst relative error {worst:E2}");
            foreach (var kv in badKeys) Console.WriteLine($"  differs: {kv.Key} x{kv.Value}");
            foreach (string n in notes) Console.WriteLine($"  note: {n}");
            Console.WriteLine(bad == 0 ? "EQUIVALENT" : "DIFFERENT");
            return bad == 0 ? 0 : 1;
        }

        static string Opt(string[] args, string name, string def)
        {
            int i = Array.IndexOf(args, name);
            return i >= 0 && i + 1 < args.Length ? args[i + 1] : def;
        }
    }
}
