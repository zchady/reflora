// The graphics preparation as the interface reads it (src/js/state/preparation.js, app-state.js), from the
// preparer's events (src-tauri/src/preparer.rs PrepEvent) and the installation report (install.rs GraphicsReport).
//   node launcher/tools/check_graphics_state.mjs
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

globalThis.window = {}; // backend.js looks for window.__TAURI__ (absent: outside the app)
const js = join(dirname(fileURLToPath(import.meta.url)), "..", "src", "js");
const load = (p) => import(pathToFileURL(join(js, p)).href);
const { setLanguage, T } = await load("strings.js");
const { applyEvent, graphicsView, initialPreparation, overallPercent, STEPS } = await load("state/preparation.js");
const { derive } = await load("state/app-state.js");
setLanguage("pt-BR");

let failures = 0;
function check(name, got, want) {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) failures++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${ok ? "" : `\n     got  ${JSON.stringify(got)}\n     want ${JSON.stringify(want)}`}`);
}

const run = (events, prep = initialPreparation()) => events.reduce(applyEvent, prep);
const step = (name, skipped = false) => ({ type: "step", index: STEPS.indexOf(name) + 1, total: STEPS.length, name, skipped });
const g = (state, extra = {}) => ({ state, made: 0, failed: 0, total: 0, compute: 0, seconds: 0, cache_bytes: 0,
  reason: null, finished: null, ...extra });
const okReport = g("ok", { made: 773, total: 773, compute: 5, seconds: 7.7 });
const install = (graphics, kind = "ready") => ({ status: { kind }, nup_exists: true, configured_nup: "W.nup", steps: [], graphics });
const state = (prep, inst) => ({ install: inst, prep, verification: { status: "idle" }, info: null });

check("graphics is the ninth step, after check", STEPS.slice(-2), ["check", "graphics"]);
check("its label", T.steps.graphics, "Preparação gráfica");

// a whole preparation: the step counts pipelines, then reports
const before = STEPS.slice(0, -1).flatMap((s) => [step(s), { type: "progress", percent: 100 }]);
let p = run([{ type: "started" }, ...before, step("graphics"), { type: "count", done: 0, total: 773, unit: "pipelines" }]);
check("starting: n of N", graphicsView(p, g("none"), false), { status: "active", meta: "em andamento · 0 de 773 pipelines", notice: "" });
p = run([{ type: "progress", percent: 52 }, { type: "count", done: 400, total: 773, unit: "pipelines" }], p);
check("real progress", graphicsView(p, g("none"), false).meta, "em andamento · 400 de 773 pipelines");
check("overall percent below 100 while it runs", overallPercent(p) < 100, true);
p = run([{ type: "result", step: "graphics", status: "ok", made: 773, failed: 0, total: 773, compute: 5, seconds: 7.7 },
  { type: "progress", percent: 100 }, { type: "done" }, { type: "finished", exit_code: 0, stopped: false }], p);
check("finished", graphicsView(p, okReport, true), { status: "done", meta: "concluída · 773 de 773 pipelines", notice: "" });
check("its log line", p.log.at(-2).text, "Preparação gráfica: 773 de 773 pipelines preparados (5 de compute) em 7.7 s");
check("ready and optimized", derive(state(p, install(okReport))).optimized, true);

// "Check and update" with nothing changed: the step is skipped, the cache reused
p = run([{ type: "started" }, ...before, step("graphics", true), { type: "done" }, { type: "finished", exit_code: 0, stopped: false }]);
check("reused", graphicsView(p, okReport, true), { status: "done", meta: "cache reaproveitado · 773 pipelines já preparados", notice: "" });

// it failed: the installation still plays, but is not called optimized
p = run([{ type: "started" }, ...before, step("graphics"),
  { type: "result", step: "graphics", status: "failed", made: 0, failed: 0, total: 0, compute: 0, seconds: 2.1 },
  { type: "warn", text: "graphics preparation failed: ..." }, { type: "done" }, { type: "finished", exit_code: 0, stopped: false }]);
const failedReport = g("failed", { reason: "no answer from the runtime after 120 s" });
let v = graphicsView(p, failedReport, true);
check("failed: attention, its message", [v.status, v.meta, v.notice], ["attention", "não concluída", T.install.graphics.notice.failed]);
let d = derive(state(p, install(failedReport)));
check("failed: still Play", [d.phase, d.action.intent, d.optimized], ["ready", "play", false]);
check("failed: home does not say optimized", d.detail, T.home.detail.readyNoGraphics);

// later, with no run in this session: read from work\graphics.json alone
for (const [report, status, notice] of [
  [g("partial", { made: 770, failed: 3, total: 773 }), "attention", T.install.graphics.notice.partial],
  [g("skipped", { reason: "the executable predates the graphics preparation" }), "attention", T.install.graphics.notice.skipped],
  [g("none"), "pending", T.install.graphics.notice.none], // prepared before the step existed
]) {
  v = graphicsView(initialPreparation(), report, true);
  d = derive(state(initialPreparation(), install(report)));
  check(`installed, graphics ${report.state}`, [v.status, v.notice, d.action.intent, d.optimized], [status, notice, "play", false]);
}
v = graphicsView(initialPreparation(), okReport, true);
d = derive(state(initialPreparation(), install(okReport)));
check("installed, graphics ok", [v.status, v.meta, d.detail, d.optimized],
  ["done", "concluída · 773 de 773 pipelines", T.home.detail.ready, true]);

// stopped during the step: back to pending in this run; the report (interrupted) then says it is not done
p = run([{ type: "started" }, ...before, step("graphics"), { type: "count", done: 100, total: 773, unit: "pipelines" },
  { type: "finished", exit_code: null, stopped: true }]);
v = graphicsView(p, g("failed", { reason: "interrupted before it finished" }), true);
check("stopped halfway", [p.outcome, v.status], ["stopped", "attention"]);

// not installed yet: no message about graphics
check("not installed: nothing to say", graphicsView(initialPreparation(), g("none"), false).notice, "");

if (failures) {
  console.log(`${failures} failed`);
  process.exit(1);
}
console.log("graphics state: all checks passed");
