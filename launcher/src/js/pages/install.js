// Installation: the game files (the NUP, checked by the preparer's verify step, and the installation folder), the
// preparation (start, stop, continue; steps, shader groups, overall progress, what went wrong) and its log.

import { fmt, T } from "../strings.js";
import { graphicsView } from "../state/preparation.js";
import { cardTitle, logView, progressBar, stepList } from "../ui/components.js";
import { h, setAttr, setText } from "../ui/dom.js";
import { icon } from "../ui/icons.js";

export function mountInstall(root, actions) {
  const t = T.install;

  // ---- game files
  const nupValue = h("dd.selectable");
  const verifyLine = h("p.verify-line");
  const verifyList = h("ul.verify-notes");
  const wsValue = h("dd.selectable");
  const chooseNup = h("button.button", { type: "button", on: { click: actions.chooseNup } }, icon("package"), t.files.choose);
  const chooseWs = h("button.button", { type: "button", on: { click: actions.chooseWorkspace } }, icon("folder"), t.files.change);
  const filesCard = h("section.card",
    h("div.card-head", cardTitle("package", t.files.title)),
    h("dl.facts",
      h("div.fact", h("span.fact-icon", icon("package")), h("dt", t.files.nup), nupValue)),
    verifyLine, verifyList,
    h("div.actions.compact", chooseNup),
    h("dl.facts.spaced",
      h("div.fact", h("span.fact-icon", icon("folder")), h("dt", t.files.workspace), wsValue)),
    h("p.hint", t.files.space),
    h("div.actions.compact", chooseWs));

  // ---- preparation
  const overallLabel = h("span.overall-label");
  const overallPercent = h("span.overall-percent");
  const overallDetail = h("p.overall-detail");
  const bar = progressBar(t.progress.title);
  const steps = stepList();
  const problem = h("div.problem", { role: "alert" });
  const graphicsNote = h("div.problem", { role: "status", "data-kind": "attention" });
  const startButton = h("button.button.primary", { type: "button", on: { click: actions.start } });
  const stopButton = h("button.button", { type: "button", on: { click: actions.stop } }, icon("stop"), t.progress.stop);
  const logButton = h("button.button", { type: "button", on: { click: actions.openLog } }, icon("list"), t.progress.openLog);
  const actionError = h("p.form-status", { "data-ok": "false" });
  const progressCard = h("section.card",
    h("div.card-head", cardTitle("list", t.progress.title)),
    h("div.overall",
      h("div.overall-line", overallLabel, overallPercent),
      bar.el,
      overallDetail),
    problem,
    graphicsNote,
    steps.el,
    h("div.actions", startButton, stopButton, logButton, actionError, h("p.note", t.progress.note)));

  // ---- log
  const log = logView();
  const warnCount = h("span.count", { "data-kind": "warn" });
  const errorCount = h("span.count", { "data-kind": "error" });
  const logCard = h("section.card",
    h("div.card-head", cardTitle("list", t.log.title), h("div.counts", warnCount, errorCount)),
    log.el);

  root.append(h("section.page.inner", { "aria-label": T.nav.install },
    h("header.page-head",
      h("div",
        h("span.eyebrow", icon("sparkle", "filled"), t.eyebrow),
        h("h1", t.title),
        h("p.lead", t.lead))),
    h("div.page-body",
      h("div.install-grid", progressCard, filesCard),
      logCard)));

  let startLabel = null;

  return {
    update(state, view) {
      const { install, prep, verification } = state;
      const busy = prep.active || view.phase === "running";

      // files
      const nup = install?.nup ?? install?.configured_nup;
      setText(nupValue, nup ?? t.files.noNup);
      const v = verification;
      setAttr(verifyLine, "data-status", v.status);
      const vText = { checking: t.files.verify.checking, ok: t.files.verify.ok, needs_user: t.files.verify.needs_user,
        failed: t.files.verify.failed }[v.status] ?? "";
      setText(verifyLine, vText);
      const notes = [...(v.error ? [v.error] : []), ...v.warnings];
      const notesKey = notes.join("\n");
      if (verifyList.dataset.key !== notesKey) {
        verifyList.dataset.key = notesKey;
        verifyList.replaceChildren(...notes.map((n) => h("li", n)));
      }
      verifyList.hidden = !notes.length;
      const ws = state.info?.workspace;
      setText(wsValue, ws ? `${ws.path} (${t.files.source[ws.source] ?? ws.source})` : "…");
      chooseNup.disabled = busy || v.status === "checking";
      chooseWs.disabled = busy || v.status === "checking" || ws?.source === "environment";

      // overall
      const active = prep.steps.find((s) => s.status === "active");
      const idx = active ? prep.steps.indexOf(active) : -1;
      const installed = view.phase === "ready" || view.phase === "running";
      const gv = graphicsView(prep, install?.graphics, installed);
      const optimized = gv.status === "done";
      let label;
      if (prep.active && active) label = fmt(t.progress.stepOf, { i: idx + 1, n: prep.steps.length, label: T.steps[active.name] });
      else if (prep.active) label = t.progress.started;
      else if (prep.outcome === "done") label = optimized ? t.progress.done : t.progress.doneNoGraphics;
      else if (prep.outcome === "stopped") label = t.progress.stopped;
      else if (prep.outcome === "needs_user") label = t.progress.needs_user;
      else if (prep.outcome === "failed") label = t.progress.failed;
      else if (installed) label = optimized ? t.progress.ready : t.progress.readyNoGraphics;
      else label = t.progress.idle;
      setText(overallLabel, label);
      const shown = prep.active || prep.outcome ? view.percent : installed ? 100 : 0;
      setText(overallPercent, `${shown}%`);
      bar.update(shown, prep.active);
      const g = prep.group;
      setText(overallDetail, g && prep.active
        ? fmt(t.progress.group, { i: g.index, n: g.total, name: g.name, phase: T.groupPhases[g.phase] ?? g.phase })
        : "");
      const shownSteps = prep.active || prep.outcome ? prep.steps : stepsFromInstall(install, prep.steps);
      steps.update(shownSteps.map((s) => {
        if (s.name !== "graphics") return s;
        if (s.status === "active") return { ...s, meta: gv.meta }; // n of N pipelines
        if (prep.active) return s; // not reached yet in this run
        return { ...s, status: gv.status, meta: gv.meta };
      }));

      // the graphics preparation did not finish: the game is playable, only not optimized for this PC yet
      setText(graphicsNote, prep.active ? "" : gv.notice);
      graphicsNote.hidden = prep.active || !gv.notice;

      // what went wrong (the preparer's own words)
      const failed = !prep.active && (prep.outcome === "needs_user" || prep.outcome === "failed");
      setText(problem, failed ? prep.lastError ?? "" : "");
      setAttr(problem, "data-kind", prep.outcome === "needs_user" ? "attention" : "error");
      problem.hidden = !failed || !prep.lastError;

      // buttons
      // installed: run it again to check everything and rebuild what changed (a new REFLORA version)
      const resume = view.phase === "interrupted" || prep.outcome === "stopped";
      const wanted = installed ? "update" : resume ? "resume" : "start";
      if (startLabel !== wanted) {
        startLabel = wanted;
        startButton.replaceChildren(icon(wanted === "start" ? "sparkle" : "refresh", wanted === "start" ? "filled" : ""),
          t.progress[wanted]);
      }
      startButton.hidden = prep.active;
      startButton.disabled = !(view.canPrepare || (installed && install?.configured_nup)) || v.status === "checking"
        || view.phase === "running";
      stopButton.hidden = !prep.active;
      logButton.hidden = !(prep.active || prep.outcome || install?.workspace_exists);
      setText(actionError, view.toolsError ? T.errors[view.toolsError] ?? view.toolsError : state.actionError ?? "");

      // log
      log.update(prep.log);
      setText(warnCount, `${prep.warnings} ${t.log.warnings}`);
      setAttr(warnCount, "data-nonzero", prep.warnings > 0 ? "true" : "false");
      setText(errorCount, `${prep.errors} ${t.log.errors}`);
      setAttr(errorCount, "data-nonzero", prep.errors > 0 ? "true" : "false");
    },
  };
}

/** Without a running preparation: what the workspace inspection says is already there. */
function stepsFromInstall(install, fallback) {
  if (!install) return fallback;
  const ready = install.status.kind === "ready" || install.status.kind === "running";
  return install.steps.map((s) => ({
    name: s.name,
    progress: 0,
    status: ready || s.present === true ? "done" : "pending",
  }));
}
