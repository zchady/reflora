// Application state: what is installed (from the backend), the preparation (from preparer events), the player's
// settings, and the page. `derive` turns it into what the interface shows, so every view agrees on the same reading.

import { backend, inTauri } from "../backend.js";
import { detectLanguage, fmt, setLanguage, T } from "../strings.js";
import { createStore } from "./store.js";
import { applyEvent, graphicsView, initialPreparation, overallPercent, STEPS } from "./preparation.js";

export const store = createStore({
  page: "home",
  lang: "",
  install: null, // InstallReport from the backend, null while loading
  installError: null,
  info: null, // AppInfo
  player: null, // { language, player_name, max_name_chars }
  saves: null, // SavesInfo
  avatars: [], // [{ id, data_url }] the user's own icons
  buttonFont: null, // { installed, kind, size }: the console's button-icon font
  prep: initialPreparation(),
  verification: { status: "idle", warnings: [], error: null }, // idle | checking | ok | needs_user | failed
  actionError: null, // the last refused action (start, choose...), translated
  gameError: null,
});

const errorText = (e) => T.errors[String(e).split(":")[0]] ?? String(e);

export async function refreshInstallation() {
  try {
    const install = await backend.installationStatus();
    store.update((s) => ({ ...s, install, installError: null }));
  } catch (e) {
    store.update((s) => ({ ...s, installError: inTauri ? String(e) : T.errors.outside }));
  }
}

export async function loadAppInfo() {
  try {
    const info = await backend.appInfo();
    store.update((s) => ({ ...s, info }));
  } catch {
    /* shown as unknown */
  }
}

/** The saved language (else the system's), applied before the first render. */
export async function loadPlayerSettings() {
  let player = null;
  try {
    player = await backend.playerSettings();
  } catch {
    /* outside the app: the system's language */
  }
  const lang = setLanguage(player?.language ?? detectLanguage());
  store.update((s) => ({ ...s, player: player ? { ...player, language: lang } : null, lang }));
}

export async function chooseLanguage(code) {
  const lang = setLanguage(code);
  store.update((s) => ({ ...s, lang, player: s.player ? { ...s.player, language: lang } : s.player }));
  try {
    await backend.setLanguage(lang);
  } catch {
    /* the choice still applies to this session */
  }
}

/** Returns { ok, message } for the settings page. */
export async function savePlayerName(name) {
  try {
    const r = await backend.setPlayerName(name);
    store.update((s) => ({ ...s, player: { ...(s.player ?? {}), player_name: r.name } }));
    return { ok: true, message: r.in_workspace ? T.settings.name.saved : T.settings.name.savedLater };
  } catch (e) {
    const max = store.get().player?.max_name_chars ?? 10;
    const text = T.settings.name.errors[String(e)];
    return { ok: false, message: text ? fmt(text, { max }) : String(e) };
  }
}

export async function refreshAvatars() {
  try {
    const [avatars, player] = await Promise.all([backend.listAvatars(), backend.playerSettings()]);
    store.update((s) => ({ ...s, avatars, player: { ...(s.player ?? {}), ...player, language: s.lang } }));
  } catch {
    /* outside the app */
  }
}

/** Returns an error message, or "" (also when the user cancelled the picker). */
export async function importAvatar() {
  try {
    await backend.importAvatar(T.settings.profile.pickTitle, T.settings.profile.filterName);
    await refreshAvatars();
    return "";
  } catch (e) {
    return T.settings.profile.errors[String(e)] ?? String(e);
  }
}

export async function chooseAvatar(id) {
  try {
    await backend.selectAvatar(id);
  } finally {
    await refreshAvatars();
  }
}

export async function deleteAvatar(id) {
  try {
    await backend.removeAvatar(id);
  } finally {
    await refreshAvatars();
  }
}

export async function refreshButtonFont() {
  try {
    const buttonFont = await backend.buttonFontStatus();
    store.update((s) => ({ ...s, buttonFont }));
  } catch {
    /* outside the app */
  }
}

/** Returns an error message, or "" (also when cancelled). */
export async function importButtonFont() {
  try {
    await backend.importButtonFont(T.settings.buttons.pickTitle, T.settings.buttons.filterName);
    return "";
  } catch (e) {
    return T.settings.buttons.errors[String(e)] ?? String(e);
  } finally {
    await refreshButtonFont();
  }
}

export async function removeButtonFont() {
  try {
    await backend.removeButtonFont();
  } finally {
    await refreshButtonFont();
  }
}

export async function refreshSaves() {
  try {
    const saves = await backend.savesInfo();
    store.update((s) => ({ ...s, saves }));
  } catch {
    /* outside the app */
  }
}

const setActionError = (e) => store.update((s) => ({ ...s, actionError: e ? errorText(e) : null }));

/** The preparer's verify step on the chosen NUP: package, processor and disk (about a second). */
export async function verifyNup() {
  store.update((s) => ({ ...s, verification: { status: "checking", warnings: [], error: null } }));
  try {
    const v = await backend.verifyNup();
    const status = v.exit_code === 0 ? "ok" : v.exit_code === 2 ? "needs_user" : "failed";
    store.update((s) => ({ ...s, verification: { status, warnings: v.warnings, error: v.error } }));
  } catch (e) {
    store.update((s) => ({ ...s, verification: { status: "idle", warnings: [], error: null } }));
    if (String(e) !== "no_nup") setActionError(e);
  }
  refreshInstallation();
}

export async function chooseNup() {
  setActionError(null);
  try {
    const path = await backend.chooseNup(T.install.files.pickNup, T.install.files.nupFilter);
    if (!path) return;
    await refreshInstallation();
    await verifyNup();
  } catch (e) {
    setActionError(e);
  }
}

export async function chooseWorkspace() {
  setActionError(null);
  try {
    const path = await backend.chooseWorkspace(T.install.files.pickFolder);
    if (!path) return;
    await Promise.all([loadAppInfo(), refreshInstallation()]);
    if (store.get().install?.nup) await verifyNup();
  } catch (e) {
    setActionError(e);
  }
}

export async function startPreparation() {
  setActionError(null);
  try {
    await backend.startPreparation();
  } catch (e) {
    setActionError(e);
  }
}

export const stopPreparation = () => backend.stopPreparation();
export const openPreparationLog = () => backend.openPreparationLog().catch(() => {});

export function connectPreparer() {
  return backend.onPreparerEvent((ev) => {
    store.update((s) => ({ ...s, prep: applyEvent(s.prep, ev) }));
    if (ev.type === "finished" || ev.type === "step") refreshInstallation();
  });
}

export function navigate(page) {
  store.update((s) => (s.page === page ? s : { ...s, page }));
}

export async function playGame() {
  try {
    await backend.launchGame();
    store.update((s) => ({ ...s, gameError: null }));
  } catch (e) {
    store.update((s) => ({ ...s, gameError: errorText(e) }));
  }
  refreshInstallation();
}

/** What every view shows: the phase, its texts and what the main button does. */
export function derive(state) {
  const { install, prep } = state;
  const percent = overallPercent(prep);
  const activeStep = prep.steps.find((s) => s.status === "active");
  const activeLabel = activeStep ? T.steps[activeStep.name] : "";

  let phase;
  if (prep.active) phase = "preparing";
  else if (!install) phase = "loading";
  else phase = install.status.kind;

  const nextStep = install?.status.kind === "interrupted" ? T.steps[install.status.next_step] ?? "" : "";
  const canPrepare = Boolean(install?.nup_exists) && state.verification.status !== "needs_user"
    && state.verification.status !== "failed";
  const action = {
    loading: { label: T.status.loading, icon: "sparkle", intent: "none", disabled: true },
    not_installed: { label: T.action.install, icon: "download", intent: "install" },
    nup_selected: { label: T.action.prepare, icon: "sparkle", intent: canPrepare ? "prepare" : "install" },
    interrupted: { label: T.action.resume, icon: "refresh", intent: canPrepare ? "prepare" : "install" },
    ready: { label: T.action.play, icon: "play", intent: "play" },
    running: { label: T.action.running, icon: "play", intent: "none", disabled: true, tone: "calm" },
    preparing: { label: T.action.preparing, sub: `${percent}%`, icon: "spinner", intent: "install", progress: percent },
  }[phase] ?? { label: T.action.install, icon: "download", intent: "install" };

  // ready, but the graphics preparation is not done: playable, yet not presented as optimized for this PC
  const installed = phase === "ready" || phase === "running";
  const optimized = installed && graphicsView(prep, install?.graphics, true).status === "done";
  const headline = T.home.headline[phase] ?? T.home.headline.not_installed;
  const detailKey = phase === "ready" && !optimized ? "readyNoGraphics" : phase;
  const detail = fmt(T.home.detail[detailKey] ?? "", { step: phase === "preparing" ? activeLabel : nextStep });

  const statusLabel = prep.active ? `${T.status.preparing} · ${percent}%` : T.status[phase] ?? T.status.loading;
  const tone = { ready: "ready", running: "ready", preparing: "active", interrupted: "attention" }[phase] ?? "idle";

  // the three stages of the home journey
  const nupDone = Boolean(install?.nup || install?.configured_nup || phase === "ready" || phase === "running");
  const prepDone = phase === "ready" || phase === "running";
  const journey = [
    nupDone ? "done" : "current",
    prepDone ? "done" : nupDone || phase === "interrupted" || phase === "preparing" ? "current" : "todo",
    prepDone ? (phase === "running" ? "done" : "current") : "todo",
  ];

  const tools = state.info?.tools;
  const toolsError = tools && "Err" in tools ? tools.Err : null;
  return {
    phase, percent, activeLabel, action, headline, detail, statusLabel, tone, journey, stepCount: STEPS.length,
    canPrepare: canPrepare && !toolsError, toolsError, optimized,
  };
}
