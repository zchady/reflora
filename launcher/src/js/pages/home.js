// Home: the approved scene (with the logo) fills the window; the dock under it holds the main button and the
// three stages of getting to play.

import { T } from "../strings.js";
import { bloomButton } from "../ui/components.js";
import { h, setAttr, setText } from "../ui/dom.js";
import { icon } from "../ui/icons.js";

export function mountHome(root, { onAction }) {
  const button = bloomButton((action) => onAction(action));
  const headline = h("h2");
  const detail = h("p");
  const error = h("p.form-status", { "data-ok": "false" });

  const stages = T.home.journey.map((j, i) => {
    const marker = h("span.journey-marker", String(i + 1));
    const detailEl = h("span.journey-detail");
    const el = h("li.journey-step", { "data-status": "todo" }, marker,
      h("span.journey-text", h("span.journey-title", j.title), detailEl));
    return { el, marker, detailEl, j, index: i + 1, status: "" };
  });

  root.append(h("section.page.home", { "aria-label": T.nav.home },
    h("div.home-dock",
      h("div.card.dock-main",
        button.el,
        h("div.dock-status",
          h("span.eyebrow", icon("sparkle", "filled"), T.home.eyebrow),
          headline, detail,
          error)),
      h("ol.card.journey", { "aria-label": T.home.stagesLabel }, stages.map((s) => s.el)))));

  return {
    update(state, view) {
      button.update(view.action);
      setText(headline, view.headline);
      setText(detail, view.detail ?? "");
      setText(error, state.gameError ?? state.actionError ?? state.installError ?? "");
      stages.forEach((s, i) => {
        const status = view.journey[i];
        setAttr(s.el, "data-status", status);
        let text = status === "done" ? (i === 1 && !view.optimized ? T.home.preparedNoGraphics : s.j.done) : s.j.todo;
        if (i === 1 && view.phase === "preparing") text = `${view.percent}% · ${view.activeLabel}`;
        setText(s.detailEl, text);
        if (s.status !== status) {
          s.status = status;
          s.marker.replaceChildren(status === "done" ? icon("check") : String(s.index));
        }
      });
    },
  };
}
