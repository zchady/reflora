// The preparation as the interface sees it, built only from preparer events (PrepEvent in src-tauri/src/preparer.rs).
// The same reducer will receive the real preparer's events next milestone.

import { fmt, T } from "../strings.js";

export const STEPS = ["verify", "extract", "bins", "recompile", "shaders", "build", "config", "check", "graphics"];

// Share of the whole preparation per step, from the measured first run (WONDER_SWITCH2_PROGRESS.md §5za6:
// recompile ~3.4 min, shaders ~7.2, tools + build ~7.8, check ~0.75 of ~19.3 min; graphics ~8 s, §5zd2).
const WEIGHTS = { verify: 1, extract: 1, bins: 1, recompile: 17, shaders: 36, build: 39, config: 1, check: 4, graphics: 1 };
const LOG_LIMIT = 500;

export function initialPreparation() {
  return {
    active: false,
    outcome: null, // null | "done" | "stopped" | "needs_user" | "failed"
    lastError: null, // the preparer's last ERROR text
    current: -1,
    steps: STEPS.map((name) => ({ name, status: "pending", progress: 0 })),
    group: null,
    // the graphics step of this run: pipelines counted so far, its RESULT, and whether it found its work done
    graphics: { count: null, result: null, reused: false },
    warnings: 0,
    errors: 0,
    log: [],
    logSeq: 0,
  };
}

export function overallPercent(prep) {
  let total = 0, all = 0;
  for (const s of prep.steps) {
    const w = WEIGHTS[s.name] ?? 1;
    all += w;
    if (s.status === "done" || s.status === "skipped") total += w;
    else if (s.status === "active") total += (w * s.progress) / 100;
  }
  // 100 only when every step is through (floor: a step still running never shows as 100)
  return all ? Math.min(100, Math.floor((100 * total) / all)) : 0;
}

function withLog(prep, kind, text) {
  const seq = prep.logSeq + 1;
  const log = prep.log.length >= LOG_LIMIT ? prep.log.slice(-LOG_LIMIT + 1) : prep.log.slice();
  log.push({ seq, kind, text });
  return { ...prep, log, logSeq: seq };
}

function closeActive(steps, status = "done") {
  return steps.map((s) => (s.status === "active" ? { ...s, status, progress: 100 } : s));
}

export function applyEvent(prep, ev) {
  switch (ev.type) {
    case "started":
      return withLog({ ...initialPreparation(), active: true, log: prep.log, logSeq: prep.logSeq }, "step",
        T.install.progress.started);
    case "step": {
      const idx = STEPS.indexOf(ev.name);
      let steps = closeActive(prep.steps);
      if (idx >= 0) {
        steps = steps.map((s, i) => {
          if (i < idx && s.status === "pending") return { ...s, status: "skipped", progress: 100 };
          if (i === idx) return { ...s, status: ev.skipped ? "skipped" : "active", progress: ev.skipped ? 100 : 0 };
          return s;
        });
      }
      const line = `${ev.index}/${ev.total} ${ev.name}${ev.skipped ? ` (${T.install.progress.alreadyDone})` : ""}`;
      const graphics = ev.name === "graphics" ? { ...prep.graphics, reused: ev.skipped } : prep.graphics;
      return withLog({ ...prep, steps, current: idx, group: null, graphics }, "step", line);
    }
    case "count":
      return { ...prep, graphics: { ...prep.graphics, count: { done: ev.done, total: ev.total } } };
    case "result": {
      if (ev.step !== "graphics") return withLog(prep, "log", `${ev.step}: ${ev.status}`);
      const r = { status: ev.status, made: ev.made, failed: ev.failed, total: ev.total, compute: ev.compute, seconds: ev.seconds };
      return withLog({ ...prep, graphics: { ...prep.graphics, result: r } }, ev.status === "ok" ? "done" : "warn",
        graphicsResultText(r));
    }
    case "progress": {
      const steps = prep.steps.map((s) => (s.status === "active" ? { ...s, progress: ev.percent } : s));
      return { ...prep, steps };
    }
    case "group": {
      const group = { index: ev.index, total: ev.total, name: ev.name, phase: ev.phase, state: ev.state };
      const text = ev.state === "error"
        ? `${ev.index}/${ev.total} ${ev.name}: ${ev.detail}`
        : `${ev.index}/${ev.total} ${ev.name} ${ev.phase}: ${ev.detail}`;
      const next = withLog({ ...prep, group }, ev.state === "error" ? "error" : "group", text);
      return ev.state === "error" ? { ...next, errors: next.errors + 1 } : next;
    }
    case "log":
      return withLog(prep, "log", ev.text);
    case "warn":
      return { ...withLog(prep, "warn", ev.text), warnings: prep.warnings + 1 };
    case "error": {
      const steps = closeActive(prep.steps, "error");
      return { ...withLog({ ...prep, steps, lastError: ev.text }, "error", ev.text), errors: prep.errors + 1 };
    }
    case "done":
      return withLog({ ...prep, steps: closeActive(prep.steps), group: null }, "done", T.install.progress.finished);
    case "finished": {
      let outcome = "failed";
      if (ev.stopped) outcome = "stopped";
      else if (ev.exit_code === 0) outcome = "done";
      else if (ev.exit_code === 2) outcome = "needs_user";
      const steps = ev.stopped
        ? prep.steps.map((s) => (s.status === "active" ? { ...s, status: "pending", progress: 0 } : s))
        : prep.steps;
      return { ...prep, steps, active: false, outcome, group: null };
    }
    default:
      return prep;
  }
}

/** One line for the log and the step: what the graphics step did. */
export function graphicsResultText(r) {
  const g = T.install.graphics;
  if (r.status === "ok") return fmt(g.resultOk, { made: r.made, total: r.total, compute: r.compute, seconds: r.seconds });
  return g.result[r.status] ?? r.status;
}

/**
 * The graphics step as the step list and the messages show it, from this run's events when it has any, else from
 * the installation's work\graphics.json (install.graphics). Returns { status, meta, notice }: status for the step
 * row (done, attention, pending, active), meta its line, notice the message when it is not done (or "").
 */
export function graphicsView(prep, installGraphics, installed) {
  const g = T.install.graphics;
  const step = prep.steps.find((s) => s.name === "graphics");
  if (prep.active && step?.status === "active") {
    const c = prep.graphics.count;
    return { status: "active", meta: c ? fmt(g.counting, { done: c.done, total: c.total }) : g.starting, notice: "" };
  }
  if (prep.graphics.reused && installGraphics?.state === "ok") {
    return { status: "done", meta: fmt(g.reused, { total: installGraphics.total }), notice: "" };
  }
  const r = prep.graphics.result ?? (installGraphics && installGraphics.state !== "none"
    ? { status: installGraphics.state, made: installGraphics.made, total: installGraphics.total,
        compute: installGraphics.compute, seconds: installGraphics.seconds }
    : null);
  if (r?.status === "ok") return { status: "done", meta: fmt(g.done, { made: r.made, total: r.total }), notice: "" };
  if (r) return { status: "attention", meta: g.meta[r.status] ?? r.status, notice: g.notice[r.status] ?? g.notice.failed };
  // never ran here: an installation prepared before the step existed, or a preparation not that far yet
  return { status: "pending", meta: installed ? g.meta.none : "", notice: installed ? g.notice.none : "" };
}
