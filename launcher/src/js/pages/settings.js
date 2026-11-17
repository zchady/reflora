// Settings: the launcher's language, the player's profile (the game's account nickname and picture) and the saves
// folder.

import { backend } from "../backend.js";
import { fmt, LANGUAGES, T } from "../strings.js";
import { cardTitle } from "../ui/components.js";
import { h, setAttr, setText } from "../ui/dom.js";
import { icon } from "../ui/icons.js";

const DEFAULT_NICKNAME = "Zagreus"; // runtime/src/svc_account.cpp kDefaultNickname

export function mountSettings(root, {
  onLanguage, onSaveName, onRefreshSaves, onRefreshAvatars, onImportAvatar, onChooseAvatar, onRemoveAvatar,
  onRefreshButtonFont, onImportButtonFont, onRemoveButtonFont,
}) {
  const s = T.settings;

  // ---- language
  const langButtons = LANGUAGES.map((l) =>
    h("button.choice", { type: "button", lang: l.code, on: { click: () => onLanguage(l.code) } }, l.name));
  const langCard = h("section.card",
    h("div.card-head", cardTitle("globe", s.language.title)),
    h("div.choices", { role: "group", "aria-label": s.language.title }, langButtons),
    h("p.hint", s.language.help));

  // ---- player name
  const input = h("input.text-input#player-name", {
    type: "text", autocomplete: "off", spellcheck: "false", placeholder: s.name.placeholder,
    on: { keydown: (e) => { if (e.key === "Enter") save(); }, input: () => setStatus("", null) },
  });
  const saveButton = h("button.button.primary", { type: "button", on: { click: () => save() } }, icon("save"), s.name.save);
  const status = h("p.form-status", { role: "status" });
  const current = h("p.hint");
  const help = h("p.hint");
  function setStatus(text, ok) {
    setText(status, text);
    setAttr(status, "data-ok", ok === null ? null : String(ok));
  }
  async function save() {
    saveButton.disabled = true;
    const r = await onSaveName(input.value);
    saveButton.disabled = false;
    setStatus(r.message, r.ok);
    if (r.ok) input.value = input.value.trim();
  }
  // ---- profile icon
  const preview = h("div.avatar-preview");
  const grid = h("div.avatar-grid", { role: "radiogroup", "aria-label": s.profile.icon });
  const iconStatus = h("p.form-status", { role: "status", "data-ok": "false" });
  let busy = false;
  async function addImage() {
    if (busy) return;
    busy = true;
    setText(iconStatus, "");
    setText(iconStatus, await onImportAvatar());
    busy = false;
  }
  let gridKey = "";
  function renderGrid(avatars, selected) {
    const key = avatars.map((a) => a.id).join(",") + "|" + selected;
    if (key === gridKey) return;
    gridKey = key;
    const tile = (id, content, label) => h("button.avatar-tile", {
      type: "button", role: "radio", "aria-checked": String(selected === id), title: label, "aria-label": label,
      on: { click: () => onChooseAvatar(id) },
    }, content);
    grid.replaceChildren(
      tile(null, h("span.avatar-none", icon("user")), s.profile.noIcon),
      ...avatars.map((a, i) => h("div.avatar-slot",
        tile(a.id, h("img", { src: a.data_url, alt: "", draggable: "false" }), `${s.profile.icon} ${i + 1}`),
        h("button.avatar-remove", {
          type: "button", title: s.profile.remove, "aria-label": s.profile.remove, on: { click: () => onRemoveAvatar(a.id) },
        }, icon("close")))),
      h("button.avatar-tile.avatar-add", { type: "button", title: s.profile.add, on: { click: addImage } },
        icon("sparkle", "filled"), h("span", s.profile.add)));
    const chosen = avatars.find((a) => a.id === selected);
    preview.replaceChildren(chosen ? h("img", { src: chosen.data_url, alt: s.profile.icon, draggable: "false" }) : icon("user"));
  }

  const nameCard = h("section.card",
    h("div.card-head", cardTitle("user", s.profile.title)),
    h("div.profile-top",
      preview,
      h("div.profile-name",
        h("label.field-label", { for: "player-name" }, s.name.label),
        h("div.field-row", input, saveButton),
        status, help, current)),
    h("p.field-label.icon-label", s.profile.icon),
    grid,
    iconStatus,
    h("p.hint", s.profile.help));

  // ---- saves
  const savesPath = h("dd.selectable");
  const savesState = h("p.hint");
  const fileList = h("ul.file-list");
  const openButton = h("button.button", { type: "button", on: { click: () => backend.openSavesFolder().catch(() => onRefreshSaves()) } },
    icon("folder"), s.saves.open);
  const savesCard = h("section.card.saves-card",
    h("div.card-head", cardTitle("save", s.saves.title), openButton),
    h("dl.facts", h("div.fact", h("span.fact-icon", icon("folder")), h("dt", s.saves.folder), savesPath)),
    savesState, fileList,
    h("p.hint", s.saves.note));

  // ---- button icons: the console's font, imported by the user
  const b = s.buttons;
  const fontState = h("p.verify-line");
  const fontError = h("p.form-status", { role: "status", "data-ok": "false" });
  const importFont = h("button.button.primary", {
    type: "button", on: { click: async () => setText(fontError, await onImportButtonFont()) },
  });
  const removeFont = h("button.button", { type: "button", on: { click: () => onRemoveButtonFont() } }, icon("close"), b.remove);
  const fontCard = h("section.card",
    h("div.card-head", cardTitle("sparkle", b.title)),
    h("p.hint.first", b.help),
    fontState,
    fontError,
    h("div.actions.compact", importFont, removeFont));

  root.append(h("section.page.inner", { "aria-label": T.nav.settings },
    h("header.page-head",
      h("div",
        h("span.eyebrow", icon("sparkle", "filled"), s.eyebrow),
        h("h1", s.title),
        h("p.lead", s.lead))),
    h("div.page-body",
      h("div.settings-grid", h("div.settings-col", langCard, nameCard), h("div.settings-col", savesCard, fontCard)))));

  onRefreshSaves();
  onRefreshAvatars();
  onRefreshButtonFont();
  let fontInstalled = null;
  let filledName = null;
  let filesKey = "";

  return {
    update(state) {
      for (const b of langButtons) setAttr(b, "aria-pressed", b.getAttribute("lang") === state.lang ? "true" : "false");

      const max = state.player?.max_name_chars ?? 10;
      setAttr(input, "maxlength", String(max * 2)); // the backend counts characters; this only stops runaway typing
      setText(help, fmt(s.name.help, { max }));
      const saved = state.player?.player_name ?? null;
      if (filledName !== saved) { // fill the field once, and again after a save
        filledName = saved;
        if (document.activeElement !== input) input.value = saved ?? "";
      }
      setText(current, saved ? "" : fmt(s.name.none, { fallback: DEFAULT_NICKNAME }));
      current.hidden = Boolean(saved);
      renderGrid(state.avatars ?? [], state.player?.avatar ?? null);

      const bf = state.buttonFont;
      const installed = Boolean(bf?.installed);
      setAttr(fontState, "data-status", installed ? "ok" : "needs_user");
      setText(fontState, !bf ? "" : installed
        ? fmt(b.installed, { size: Math.round(bf.size / 1024).toLocaleString(state.lang || undefined) })
        : b.missing);
      if (fontInstalled !== installed) {
        fontInstalled = installed;
        importFont.replaceChildren(icon(installed ? "refresh" : "download"), installed ? b.replace : b.import);
      }
      removeFont.hidden = !installed;

      const sv = state.saves;
      setText(savesPath, sv?.path ?? "…");
      openButton.disabled = !sv?.exists;
      const newest = Math.max(0, ...(sv?.files ?? []).map((f) => f.modified ?? 0));
      const dateFmt = new Intl.DateTimeFormat(state.lang || undefined, { dateStyle: "medium", timeStyle: "short" });
      if (!sv) setText(savesState, "");
      else if (!sv.exists) setText(savesState, s.saves.missing);
      else if (!sv.files.length) setText(savesState, s.saves.empty);
      else setText(savesState, fmt(s.saves.count, { n: sv.files.length, date: dateFmt.format(new Date(newest * 1000)) }));
      const key = JSON.stringify(sv?.files ?? []) + state.lang;
      if (key !== filesKey) {
        filesKey = key;
        fileList.replaceChildren(...(sv?.files ?? []).map((f) => h("li",
          h("span.file-name", f.name),
          h("span.file-meta", `${fmt(s.saves.fileSize, { size: (f.size / 1024).toLocaleString(state.lang || undefined, { maximumFractionDigits: 1 }) })}${f.modified ? ` · ${dateFmt.format(new Date(f.modified * 1000))}` : ""}`))));
      }
      fileList.hidden = !sv?.files?.length;
    },
  };
}
