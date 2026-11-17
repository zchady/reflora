// Reusable pieces. Each returns { el, update(...) } so pages patch the DOM instead of rebuilding it.

import { T } from "../strings.js";
import { h, setAttr, setText } from "./dom.js";
import { icon } from "./icons.js";

/** The main button, styled like the logo's letters. Its look and label come from derive().action. */
export function bloomButton(onPress) {
  const iconSlot = h("span.bloom-icon");
  const label = h("span.bloom-text");
  const sub = h("small");
  const bar = h("span.button-progress");
  const el = h("button.bloom-button", { type: "button", on: { click: () => onPress(current) } },
    iconSlot, h("span.bloom-label", label, sub), bar);
  let current = null;
  let iconName = "";
  return {
    el,
    update(action) {
      current = action;
      if (action.icon !== iconName) {
        iconName = action.icon;
        iconSlot.replaceChildren(icon(action.icon));
      }
      setText(label, action.label);
      setText(sub, action.sub ?? "");
      sub.hidden = !action.sub;
      el.disabled = Boolean(action.disabled);
      setAttr(el, "data-tone", action.tone ?? null);
      bar.hidden = action.progress === undefined;
      el.style.setProperty("--progress", `${action.progress ?? 0}%`);
      setAttr(el, "aria-label", action.sub ? `${action.label} ${action.sub}` : action.label);
    },
  };
}

export function progressBar(label) {
  const fill = h("span.bar-fill");
  const el = h("div.bar", { role: "progressbar", "aria-label": label, "aria-valuemin": "0", "aria-valuemax": "100" }, fill);
  return {
    el,
    update(percent, active) {
      el.style.setProperty("--value", `${percent}%`);
      setAttr(el, "aria-valuenow", String(percent));
      setAttr(el, "data-active", active ? "true" : "false");
    },
  };
}

export function stepList() {
  const rows = new Map();
  const el = h("ol.steps", { "aria-label": T.install.progress.title });
  for (const [i, name] of Object.keys(T.steps).entries()) {
    const marker = h("span.step-marker", String(i + 1));
    const meta = h("span.step-meta");
    const row = h("li.step", { "data-status": "pending" }, marker,
      h("span.step-text", h("span.step-name", T.steps[name]), meta));
    rows.set(name, { row, marker, meta, status: "", index: i + 1 });
    el.append(row);
  }
  return {
    el,
    update(steps) {
      for (const s of steps) {
        const r = rows.get(s.name);
        if (!r) continue;
        setAttr(r.row, "data-status", s.status);
        const metaText = s.meta ?? (s.status === "active" && s.progress > 0
          ? `${T.install.progress.stepMeta.active} · ${s.progress}%`
          : T.install.progress.stepMeta[s.status] ?? "");
        setText(r.meta, metaText);
        if (r.status !== s.status) {
          r.status = s.status;
          if (s.status === "done" || s.status === "skipped") r.marker.replaceChildren(icon("check"));
          else if (s.status === "error") r.marker.replaceChildren(icon("close"));
          else if (s.status === "attention") r.marker.replaceChildren("!");
          else r.marker.replaceChildren(String(r.index));
        }
      }
    },
  };
}

/** Appends only the new lines; follows the end unless the user scrolled up to read. */
export function logView() {
  const empty = h("p.log-empty", T.install.log.empty);
  const el = h("div.log", { role: "log", "aria-live": "off", tabindex: "0" }, empty);
  let lastSeq = 0;
  return {
    el,
    update(log) {
      if (!log.length) {
        if (lastSeq) {
          el.replaceChildren(empty);
          lastSeq = 0;
        }
        return;
      }
      const newest = log[log.length - 1].seq;
      if (newest === lastSeq) return;
      if (newest < lastSeq || log[0].seq > lastSeq + 1) {
        el.replaceChildren();
        lastSeq = log[0].seq - 1;
      }
      const atEnd = el.scrollHeight - el.scrollTop - el.clientHeight < 24;
      empty.remove();
      const frag = document.createDocumentFragment();
      for (const line of log) {
        if (line.seq <= lastSeq) continue;
        frag.append(h("div.log-line", { "data-kind": line.kind }, line.text));
      }
      el.append(frag);
      while (el.childElementCount > 500) el.firstElementChild.remove();
      lastSeq = newest;
      if (atEnd) el.scrollTop = el.scrollHeight;
    },
  };
}

export function cardTitle(iconName, text) {
  return h("h2.card-title", h("span.badge-icon", icon(iconName)), text);
}
