// The only place that talks to the Rust backend (src-tauri/src/lib.rs). Pages and state never call Tauri directly,
// so the preparer can be connected next milestone without touching them.

const tauri = window.__TAURI__;

export const inTauri = Boolean(tauri);

function invoke(cmd, args) {
  if (!tauri) return Promise.reject(new Error("fora do aplicativo REFLORA"));
  return tauri.core.invoke(cmd, args);
}

export const backend = {
  /** { version, settings_file, workspace: { path, source } } */
  appInfo: () => invoke("app_info"),
  /** { status: { kind, next_step? }, workspace, workspace_exists, nup, nup_exists, configured_nup, steps[] } */
  installationStatus: () => invoke("installation_status"),
  /** File picker; -> the chosen NUP path (saved), or null when cancelled */
  chooseNup: (title, filterName) => invoke("choose_nup", { title, filterName }),
  /** Folder picker; -> the installation folder (saved), or null when cancelled */
  chooseWorkspace: (title) => invoke("choose_workspace", { title }),
  /** The preparer's verify step: { exit_code, warnings[], error } */
  verifyNup: () => invoke("verify_nup"),
  startPreparation: () => invoke("start_preparation"),
  stopPreparation: () => invoke("stop_preparation"),
  preparationRunning: () => invoke("preparation_running"),
  openPreparationLog: () => invoke("open_preparation_log"),
  launchGame: () => invoke("launch_game"),
  /** { language, player_name, max_name_chars } */
  playerSettings: () => invoke("player_settings"),
  setLanguage: (language) => invoke("set_language", { language }),
  /** -> { name, in_workspace }; rejects with "empty" | "too_long" | "invalid_char" */
  setPlayerName: (name) => invoke("set_player_name", { name }),
  /** { path, exists, files: [{ name, size, modified }] } */
  savesInfo: () => invoke("saves_info"),
  openSavesFolder: () => invoke("open_saves_folder"),
  /** [{ id, data_url }] */
  listAvatars: () => invoke("list_avatars"),
  /** Opens the file picker; -> the new id, or null when cancelled. Rejects with an error code. */
  importAvatar: (title, filterName) => invoke("import_avatar", { title, filterName }),
  selectAvatar: (id) => invoke("select_avatar", { id }),
  removeAvatar: (id) => invoke("remove_avatar", { id }),
  /** { installed, kind: "bfttf" | "ttf" | null, size } */
  buttonFontStatus: () => invoke("button_font_status"),
  /** File picker; -> false when cancelled. Rejects with "not_font" | "unreadable" */
  importButtonFont: (title, filterName) => invoke("import_button_font", { title, filterName }),
  removeButtonFont: () => invoke("remove_button_font"),
  /** Experimental BC7 textures: { enabled, last: work\textures.json or null, can_prepare } */
  texturesInfo: () => invoke("textures_info"),
  setBc7Textures: (enabled) => invoke("set_bc7_textures", { enabled }),
  /** The preparer's textures step alone; its events arrive as a preparation's. Rejects with an error code. */
  prepareTextures: () => invoke("prepare_textures"),

  /** Every preparer event (see PrepEvent in src-tauri/src/preparer.rs). Returns the unsubscribe function. */
  onPreparerEvent(handler) {
    if (!tauri) return () => {};
    let unlisten = null;
    let cancelled = false;
    tauri.event.listen("preparer://event", (e) => handler(e.payload)).then((fn) => {
      if (cancelled) fn();
      else unlisten = fn;
    });
    return () => {
      cancelled = true;
      if (unlisten) unlisten();
    };
  },
};

export const appWindow = {
  minimize: () => tauri?.window.getCurrentWindow().minimize(),
  toggleMaximize: () => tauri?.window.getCurrentWindow().toggleMaximize(),
  close: () => tauri?.window.getCurrentWindow().close(),
};
