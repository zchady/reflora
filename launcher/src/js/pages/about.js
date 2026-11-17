// About: what REFLORA does, what happens to the user's files, where things are kept, and the components' licences.

import { fmt, T } from "../strings.js";
import { cardTitle } from "../ui/components.js";
import { h, setText } from "../ui/dom.js";
import { icon } from "../ui/icons.js";

export function mountAbout(root) {
  const a = T.about;
  const version = h("span.eyebrow");
  const settingsPath = h("dd.selectable");
  const wsPath = h("dd.selectable");

  root.append(h("section.page.inner", { "aria-label": T.nav.about },
    h("div.page-body",
      h("section.card.about-hero",
        h("img", { src: "assets/reflora-logo.webp", alt: "REFLORA", draggable: "false" }),
        h("div.text-block",
          version,
          a.intro.map((p) => h("p", p)))),
      h("div.about-grid",
        h("section.card",
          h("div.card-head", cardTitle("shield", a.filesTitle)),
          h("div.text-block", a.files.map((p) => h("p", p))),
          h("dl.facts", { style: { marginTop: "18px" } },
            h("div.fact", h("span.fact-icon", icon("cpu")), h("dt", a.paths.settings), settingsPath),
            h("div.fact", h("span.fact-icon", icon("folder")), h("dt", a.paths.workspace), wsPath))),
        h("section.card",
          h("div.card-head", cardTitle("package", a.componentsTitle)),
          h("ul.licenses", a.components.map(([name, lic]) => h("li", h("span", name), h("span", lic)))))))));

  return {
    update(state) {
      const info = state.info;
      setText(version, info ? fmt(a.version, { v: info.version }) : "");
      setText(settingsPath, info?.settings_file ?? "…");
      setText(wsPath, info?.workspace?.path ?? "…");
    },
  };
}
