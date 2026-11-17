// The window frame: sidebar (logo, navigation, installation status) and the title bar's window buttons.

import { appWindow } from "../backend.js";
import { T } from "../strings.js";
import { progressBar } from "./components.js";
import { h, setAttr, setText } from "./dom.js";
import { icon } from "./icons.js";

export const PAGES = [
  { id: "home", icon: "home" },
  { id: "install", icon: "download" },
  { id: "settings", icon: "sliders" },
  { id: "about", icon: "info" },
];

export function mountSidebar(root, { onNavigate }) {
  const items = new Map();
  const nav = h("ul.nav");
  for (const p of PAGES) {
    const btn = h("button.nav-item", { type: "button", on: { click: () => onNavigate(p.id) } }, icon(p.icon), T.nav[p.id]);
    items.set(p.id, btn);
    nav.append(h("li", btn));
  }

  const dot = h("span.dot");
  const label = h("span");
  const bar = progressBar(T.status.title);
  const status = h("button.side-status", { type: "button", on: { click: () => onNavigate("install") } },
    h("span.side-status-head", T.status.title),
    h("span.side-status-label", dot, label),
    bar.el);

  root.append(
    h("div.brand", { "data-tauri-drag-region": "" },
      h("img", { src: "assets/reflora-logo-sm.webp", alt: "REFLORA", draggable: "false" })),
    h("nav", { "aria-label": T.navLabel }, nav),
    h("div.sidebar-spacer", { "data-tauri-drag-region": "" }),
    status,
  );

  return {
    update(state, view) {
      for (const [id, btn] of items) setAttr(btn, "aria-current", state.page === id ? "page" : null);
      setText(label, view.statusLabel);
      setAttr(dot, "data-tone", view.tone);
      bar.el.hidden = !state.prep.active;
      bar.update(view.percent, state.prep.active);
    },
  };
}

/** `confirmClose()`: false keeps the window open (asked while a preparation runs). */
export function mountTitlebar(root, { confirmClose = () => true } = {}) {
  const button = (cls, name, label, fn) =>
    h(`button.window-button${cls}`, { type: "button", title: label, "aria-label": label, on: { click: fn } }, icon(name));
  root.append(
    h("div.titlebar-drag", { "data-tauri-drag-region": "" }),
    h("div.window-controls",
      button("", "minimize", T.window.minimize, () => appWindow.minimize()),
      button("", "maximize", T.window.maximize, () => appWindow.toggleMaximize()),
      button(".close", "close", T.window.close, () => { if (confirmClose()) appWindow.close(); })),
  );
}
