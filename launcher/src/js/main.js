// Starts the interface: frame, pages, and the wiring between the store and the backend.

import { backend } from "./backend.js";
import { mountAbout } from "./pages/about.js";
import { mountHome } from "./pages/home.js";
import { mountInstall } from "./pages/install.js";
import { mountSettings } from "./pages/settings.js";
import {
  chooseAvatar, chooseLanguage, chooseNup, chooseWorkspace, connectPreparer, deleteAvatar, derive, importAvatar,
  importButtonFont, loadAppInfo, loadPlayerSettings, navigate, openPreparationLog, playGame, refreshAvatars,
  refreshButtonFont, refreshInstallation, refreshSaves, removeButtonFont, savePlayerName, startPreparation,
  stopPreparation, store, verifyNup, refreshTextures, setBc7Textures, prepareTextures,
} from "./state/app-state.js";
import { T } from "./strings.js";
import { mountSidebar, mountTitlebar } from "./ui/shell.js";

const app = document.getElementById("app");
const main = document.getElementById("main");
const sidebarRoot = document.getElementById("sidebar");
const titlebarRoot = document.getElementById("titlebar");

function onAction(action) {
  if (!action || action.disabled) return;
  if (action.intent === "play") playGame();
  else if (action.intent === "prepare") {
    navigate("install");
    startPreparation();
  } else if (action.intent === "install") navigate("install");
}

const PAGE_MOUNTS = {
  home: (root) => mountHome(root, { onAction }),
  install: (root) => mountInstall(root, {
    chooseNup, chooseWorkspace, start: startPreparation, stop: stopPreparation, openLog: openPreparationLog,
  }),
  settings: (root) => mountSettings(root, {
    onLanguage: chooseLanguage, onSaveName: savePlayerName, onRefreshSaves: refreshSaves,
    onRefreshAvatars: refreshAvatars, onImportAvatar: importAvatar, onChooseAvatar: chooseAvatar,
    onRemoveAvatar: deleteAvatar,
    onRefreshButtonFont: refreshButtonFont, onImportButtonFont: importButtonFont, onRemoveButtonFont: removeButtonFont,
    onRefreshTextures: refreshTextures, onBc7Textures: setBc7Textures, onPrepareTextures: prepareTextures,
    onStopTextures: stopPreparation,
  }),
  about: (root) => mountAbout(root),
};

// Closing while preparing stops the preparation (it continues next time): ask first.
const confirmClose = () => !(store.get().prep.active || store.get().texturesRun?.active)
  || window.confirm(T.install.progress.confirmClose);

let shown = { page: null, lang: null };
let sidebar = null;
let page = null;

function render(state) {
  const langChanged = state.lang !== shown.lang;
  if (langChanged) { // every view reads its texts when mounted: mount them again in the new language
    sidebarRoot.replaceChildren();
    titlebarRoot.replaceChildren();
    sidebar = mountSidebar(sidebarRoot, { onNavigate: navigate });
    mountTitlebar(titlebarRoot, { confirmClose });
  }
  if (langChanged || state.page !== shown.page) {
    const scroll = langChanged && state.page === shown.page ? main.firstElementChild?.scrollTop ?? 0 : 0;
    main.replaceChildren();
    page = PAGE_MOUNTS[state.page](main);
    app.dataset.page = state.page;
    if (scroll && main.firstElementChild) main.firstElementChild.scrollTop = scroll;
  }
  shown = { page: state.page, lang: state.lang };
  const view = derive(state);
  sidebar.update(state, view);
  page.update(state, view);
}

await loadPlayerSettings(); // the language first, so the first frame is already in it
store.subscribe(render);
render(store.get());

connectPreparer();
await Promise.all([loadAppInfo(), refreshInstallation()]);
// A chosen NUP is checked again at every start (about a second): it may have moved, or the disk filled up.
if (store.get().install?.nup && store.get().install.status.kind !== "ready") {
  backend.preparationRunning().then((running) => { if (!running) verifyNup(); });
}

// The game may be closed at any time: while it runs, look again every few seconds.
setInterval(() => {
  const kind = store.get().install?.status.kind;
  if (kind === "running") refreshInstallation();
}, 3000);
window.addEventListener("focus", () => {
  refreshInstallation();
  if (store.get().page === "settings") refreshSaves();
});
