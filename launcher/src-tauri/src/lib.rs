//! REFLORA launcher backend. Each concern lives in its own module and the interface only talks to them through the
//! commands below (and the `preparer://event` events), so the real preparer can be plugged in without touching the
//! interface:
//!
//! - `settings`  the launcher's own settings (`%APPDATA%\app.reflora.launcher\settings.json`)
//! - `install`   what is installed in the workspace (read-only inspection of the preparer's outputs)
//! - `preparer`  finding and running the preparer, its line protocol, the session that feeds it to the interface
//! - `game`      starting the prepared game and following its process
//! - `profile`   the player's name (the game's account nickname) and the saves folder
//! - `avatars`   the user's own profile icons, kept on this PC, and the one given to the game
//! - `button_font` the console's button-icon font the user imports from their own console
//!
//! See launcher/README.md for the architecture decisions.

pub mod avatars;
pub mod button_font;
pub mod game;
pub mod install;
pub mod preparer;
pub mod profile;
pub mod requirements;
pub mod settings;

use std::path::{Path, PathBuf};

use serde::Serialize;
use tauri::{Manager, State};
use tauri_plugin_dialog::DialogExt;

pub struct AppState {
    pub settings: settings::SettingsStore,
    pub default_workspace: PathBuf,
    pub preparer: preparer::Session,
    pub game: game::GameProcess,
    pub avatars: avatars::Library,
    pub button_font: button_font::Store,
    /// Where the launcher runs from (to find the preparer).
    pub exe_dir: PathBuf,
}

impl AppState {
    fn workspace(&self) -> settings::Workspace {
        self.settings.workspace(&self.default_workspace)
    }

    /// The chosen icon's file, if it is still in the library.
    fn chosen_avatar(&self) -> Option<PathBuf> {
        self.settings.get().avatar.and_then(|id| self.avatars.path_of(&id))
    }

    fn tools(&self) -> Result<preparer::Tools, String> {
        let env = |k| std::env::var_os(k).filter(|v| !v.is_empty()).map(PathBuf::from);
        preparer::locate(&self.exe_dir, env(preparer::ROOT_ENV), env(preparer::COMPONENTS_ENV))
    }

    /// The preparer's spec for the chosen NUP and workspace.
    fn spec(&self, steps: Option<&str>) -> Result<preparer::Spec, String> {
        // the chosen NUP, else the one this installation was prepared from (checking and updating it)
        let nup = self.settings.get().nup_path.or_else(|| install::configured_nup(&self.workspace().path)).ok_or("no_nup")?;
        if !nup.is_file() {
            return Err("nup_missing".into());
        }
        Ok(preparer::Spec { tools: self.tools()?, nup, workspace: self.workspace().path, steps: steps.map(String::from) })
    }

    /// Nothing that changes the workspace while it is being prepared or the game runs from it.
    fn idle(&self) -> Result<(), String> {
        if self.preparer.is_running() {
            return Err("already_preparing".into());
        }
        if self.game.is_running() {
            return Err("already_running".into());
        }
        Ok(())
    }
}

#[derive(Serialize)]
struct AppInfo {
    version: String,
    settings_file: PathBuf,
    workspace: settings::Workspace,
    /// The preparer and components found, or why not ("no_preparer", "no_components").
    tools: Result<preparer::Tools, String>,
    log_file: PathBuf,
}

#[tauri::command]
fn app_info(app: tauri::AppHandle, state: State<'_, AppState>) -> AppInfo {
    let workspace = state.workspace();
    AppInfo {
        version: app.package_info().version.to_string(),
        settings_file: state.settings.path().to_path_buf(),
        log_file: workspace.path.join("work").join(preparer::LOG_FILE),
        workspace,
        tools: state.tools(),
    }
}

#[tauri::command]
fn installation_status(state: State<'_, AppState>) -> install::InstallReport {
    let ws = state.workspace();
    let nup = state.settings.get().nup_path;
    install::inspect(&ws.path, nup.as_deref(), state.game.is_running())
}

/// The system file picker for the NUP. Returns the chosen path (saved), or None when cancelled.
#[tauri::command]
async fn choose_nup(app: tauri::AppHandle, state: State<'_, AppState>, title: String, filter_name: String)
    -> Result<Option<PathBuf>, String> {
    state.idle()?;
    let picked = app.dialog().file().set_title(title).add_filter(filter_name, &["nup"]).blocking_pick_file();
    let Some(path) = picked.and_then(|f| f.into_path().ok()) else { return Ok(None) };
    state.settings.update(|s| s.nup_path = Some(path.clone())).map_err(|e| e.to_string())?;
    Ok(Some(path))
}

/// The folder for the installation. A folder that is empty or already a REFLORA installation is used as it is;
/// any other gets a "REFLORA" folder inside it, so nothing of the user's is mixed with the game's files.
fn workspace_from_choice(chosen: &Path) -> PathBuf {
    let empty = std::fs::read_dir(chosen).map(|mut d| d.next().is_none()).unwrap_or(true);
    if empty || chosen.join(".nx-workspace").is_file() {
        chosen.to_path_buf()
    } else {
        chosen.join("REFLORA")
    }
}

#[tauri::command]
async fn choose_workspace(app: tauri::AppHandle, state: State<'_, AppState>, title: String)
    -> Result<Option<PathBuf>, String> {
    state.idle()?;
    let picked = app.dialog().file().set_title(title).blocking_pick_folder();
    let Some(dir) = picked.and_then(|f| f.into_path().ok()) else { return Ok(None) };
    let ws = workspace_from_choice(&dir);
    state.settings.update(|s| s.workspace = Some(ws.clone())).map_err(|e| e.to_string())?;
    Ok(Some(ws))
}

#[derive(Serialize)]
struct Verification {
    /// 0: the package is the game (warnings may follow); 2: it needs the user (wrong package, no AVX2, no disk).
    exit_code: Option<i32>,
    warnings: Vec<String>,
    error: Option<String>,
}

/// The preparer's verify step alone (about a second): the package, the processor and the disk.
#[tauri::command]
async fn verify_nup(state: State<'_, AppState>) -> Result<Verification, String> {
    state.idle()?;
    let spec = state.spec(Some("verify"))?;
    tauri::async_runtime::spawn_blocking(move || {
        let job = preparer::Job::new().map_err(|e| e.to_string())?;
        let mut v = Verification { exit_code: None, warnings: vec![], error: None };
        preparer::run(&spec, &job, &|| false, |e| match e {
            preparer::PrepEvent::Warn { text } => v.warnings.push(text),
            preparer::PrepEvent::Error { text } => v.error = Some(text),
            preparer::PrepEvent::Finished { exit_code, .. } => v.exit_code = exit_code,
            _ => {}
        });
        Ok(v)
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
fn start_preparation(window: tauri::Window, state: State<'_, AppState>) -> Result<(), String> {
    state.idle()?;
    let spec = state.spec(None)?;
    state.preparer.start(window, spec)
}

#[tauri::command]
fn stop_preparation(state: State<'_, AppState>) {
    state.preparer.stop();
}

#[tauri::command]
fn preparation_running(state: State<'_, AppState>) -> bool {
    state.preparer.is_running()
}

/// Shows the preparation's log in File Explorer.
#[tauri::command]
fn open_preparation_log(state: State<'_, AppState>) -> Result<(), String> {
    let log = state.workspace().path.join("work").join(preparer::LOG_FILE);
    if !log.is_file() {
        return Err("missing".into());
    }
    let mut arg = std::ffi::OsString::from("/select,");
    arg.push(&log);
    std::process::Command::new("explorer.exe").arg(arg).spawn().map(|_| ()).map_err(|e| e.to_string())
}

#[tauri::command]
fn launch_game(state: State<'_, AppState>) -> Result<u32, String> {
    if state.preparer.is_running() {
        return Err("already_preparing".into());
    }
    let ws = state.workspace();
    let s = state.settings.get();
    let report = install::inspect(&ws.path, s.nup_path.as_deref(), state.game.is_running());
    if !matches!(report.status, install::Status::Ready) {
        return Err("not_ready".into());
    }
    if let Some(name) = &s.player_name {
        let _ = profile::write_profile(&ws.path, name); // keeps starts outside the launcher in agreement
    }
    let _ = avatars::apply_to_workspace(&ws.path, state.chosen_avatar().as_deref());
    let _ = state.button_font.apply_to_workspace(&ws.path);
    state.game.launch(&ws.path, s.player_name.as_deref(), s.bc7_textures)
}

// ---- experimental BC7 textures (NX_ASTC_BC7, off by default) ---------------------------------------------------------

/// The texture preparation's record (written by the preparer's optional `textures` step).
const TEXTURES_STATE: &str = "textures.json";

#[derive(Serialize)]
struct TexturesInfo {
    /// The player's choice: the game starts with NX_ASTC_BC7=1.
    enabled: bool,
    /// work\textures.json of the last texture preparation (status, accepted, cache_bytes, finished, reason), if any.
    last: Option<serde_json::Value>,
    /// The game is installed, so its textures can be prepared.
    can_prepare: bool,
    /// The last preparation finished for the game installed now (its bin\wonder2.exe, by size and time): nothing to
    /// prepare again. False after an update rebuilt the game, or for a record written before the step kept them.
    ready: bool,
}

/// Whether `last` (work\textures.json) is a finished preparation of this exact executable.
fn textures_ready(last: Option<&serde_json::Value>, exe: &Path) -> bool {
    let Some(last) = last else { return false };
    let Ok(meta) = std::fs::metadata(exe) else { return false };
    let mtime = meta
        .modified()
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_secs());
    last.get("status").and_then(|s| s.as_str()) == Some("ok")
        && last.get("exe_size").and_then(|v| v.as_u64()) == Some(meta.len())
        && last.get("exe_mtime").and_then(|v| v.as_u64()).is_some_and(|m| Some(m) == mtime)
}

#[tauri::command]
fn textures_info(state: State<'_, AppState>) -> TexturesInfo {
    let ws = state.workspace().path;
    let last = std::fs::read_to_string(ws.join("work").join(TEXTURES_STATE))
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok());
    let report = install::inspect(&ws, state.settings.get().nup_path.as_deref(), state.game.is_running());
    let can_prepare = matches!(report.status, install::Status::Ready | install::Status::Running);
    let ready = can_prepare && textures_ready(last.as_ref(), &ws.join("bin").join("wonder2.exe"));
    TexturesInfo { enabled: state.settings.get().bc7_textures, last, can_prepare, ready }
}

/// Takes effect the next time the game starts.
#[tauri::command]
fn set_bc7_textures(state: State<'_, AppState>, enabled: bool) -> Result<(), String> {
    state.settings.update(|s| s.bc7_textures = enabled).map_err(|e| e.to_string())
}

/// The preparer's `textures` step alone (the BC7 cache in <workspace>\data\astc_bc7_cache, from the player's own
/// package), followed through the same events as a preparation; stopping it keeps what is done.
#[tauri::command]
fn prepare_textures(window: tauri::Window, state: State<'_, AppState>) -> Result<(), String> {
    state.idle()?;
    let ws = state.workspace().path;
    let report = install::inspect(&ws, state.settings.get().nup_path.as_deref(), false);
    if !matches!(report.status, install::Status::Ready) {
        return Err("not_ready".into());
    }
    let last = std::fs::read_to_string(ws.join("work").join(TEXTURES_STATE))
        .ok()
        .and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok());
    if textures_ready(last.as_ref(), &ws.join("bin").join("wonder2.exe")) {
        return Err("textures_ready".into()); // done for this game: nothing to redo
    }
    let spec = state.spec(Some("textures"))?;
    state.preparer.start(window, spec)
}

#[tauri::command]
fn button_font_status(state: State<'_, AppState>) -> button_font::Status {
    state.button_font.status()
}

/// The system file picker for the console's button font; imported, checked and copied to the installation.
/// Returns false when the user cancelled.
#[tauri::command]
async fn import_button_font(app: tauri::AppHandle, state: State<'_, AppState>, title: String, filter_name: String)
    -> Result<bool, String> {
    let picked = app.dialog().file().set_title(title).add_filter(filter_name, &["bfttf", "ttf"]).blocking_pick_file();
    let Some(path) = picked.and_then(|f| f.into_path().ok()) else { return Ok(false) };
    state.button_font.import(&path)?;
    if !state.game.is_running() {
        state.button_font.apply_to_workspace(&state.workspace().path).map_err(|e| e.to_string())?;
    }
    Ok(true)
}

#[tauri::command]
fn remove_button_font(state: State<'_, AppState>) -> Result<(), String> {
    state.button_font.remove();
    if !state.game.is_running() {
        state.button_font.apply_to_workspace(&state.workspace().path).map_err(|e| e.to_string())?;
    }
    Ok(())
}

#[derive(Serialize)]
struct PlayerSettings {
    language: Option<String>,
    player_name: Option<String>,
    avatar: Option<String>,
    max_name_chars: usize,
}

#[tauri::command]
fn player_settings(state: State<'_, AppState>) -> PlayerSettings {
    let s = state.settings.get();
    let avatar = s.avatar.filter(|id| state.avatars.path_of(id).is_some());
    PlayerSettings { language: s.language, player_name: s.player_name, avatar, max_name_chars: profile::MAX_CHARS }
}

#[tauri::command]
fn list_avatars(state: State<'_, AppState>) -> Vec<avatars::Avatar> {
    state.avatars.list()
}

/// Opens the system's file picker and imports the image. `None`: the user cancelled. The new icon is selected.
#[tauri::command]
async fn import_avatar(
    app: tauri::AppHandle,
    state: State<'_, AppState>,
    title: String,
    filter_name: String,
) -> Result<Option<String>, String> {
    let picked = app
        .dialog()
        .file()
        .set_title(title)
        .add_filter(filter_name, &["png", "jpg", "jpeg", "webp", "bmp", "gif"])
        .blocking_pick_file();
    let Some(file) = picked else { return Ok(None) };
    let path = file.into_path().map_err(|_| "unreadable".to_string())?;
    let id = state.avatars.import(&path)?;
    select(&state, Some(id.clone()))?;
    Ok(Some(id))
}

fn select(state: &AppState, id: Option<String>) -> Result<(), String> {
    if let Some(i) = &id {
        if state.avatars.path_of(i).is_none() {
            return Err("unknown".into());
        }
    }
    state.settings.update(|s| s.avatar = id).map_err(|e| e.to_string())?;
    avatars::apply_to_workspace(&state.workspace().path, state.chosen_avatar().as_deref()).map_err(|e| e.to_string())?;
    Ok(())
}

#[tauri::command]
fn select_avatar(state: State<'_, AppState>, id: Option<String>) -> Result<(), String> {
    select(&state, id)
}

#[tauri::command]
fn remove_avatar(state: State<'_, AppState>, id: String) -> Result<(), String> {
    if state.settings.get().avatar.as_deref() == Some(id.as_str()) {
        select(&state, None)?;
    }
    state.avatars.remove(&id)
}

#[tauri::command]
fn set_language(state: State<'_, AppState>, language: String) -> Result<(), String> {
    if !settings::LANGUAGES.contains(&language.as_str()) {
        return Err("unknown_language".into());
    }
    state.settings.update(|s| s.language = Some(language)).map_err(|e| e.to_string())
}

#[derive(Serialize)]
struct NameSaved {
    name: String,
    /// Also written to <workspace>\data\user\profile.ini (false: no installation yet).
    in_workspace: bool,
}

#[tauri::command]
fn set_player_name(state: State<'_, AppState>, name: String) -> Result<NameSaved, String> {
    let name = profile::validate_name(&name).map_err(String::from)?;
    state.settings.update(|s| s.player_name = Some(name.clone())).map_err(|e| e.to_string())?;
    let in_workspace = profile::write_profile(&state.workspace().path, &name).map_err(|e| e.to_string())?;
    Ok(NameSaved { name, in_workspace })
}

#[tauri::command]
fn saves_info(state: State<'_, AppState>) -> profile::SavesInfo {
    profile::saves_info(&state.workspace().path)
}

#[tauri::command]
fn open_saves_folder(state: State<'_, AppState>) -> Result<(), String> {
    profile::open_saves(&state.workspace().path)
}

/// Development and tests: REFLORA_HOME=<folder> keeps the launcher's settings (<folder>\config) and its own data
/// (<folder>\data: the default installation folder, the icon library, the console font) out of the user's
/// %APPDATA% and %LOCALAPPDATA%, as REFLORA_WORKSPACE does for the installation folder alone.
pub const HOME_ENV: &str = "REFLORA_HOME";

pub fn run() {
    // The interface needs the WebView2 Runtime: without it, a message saying so, not a launcher that never opens.
    if requirements::webview2_version().is_none() {
        let (title, text) = requirements::webview2_message();
        requirements::show_error(&title, &text);
        std::process::exit(1);
    }
    let result = tauri::Builder::default()
        // One launcher at a time: two would overwrite each other's settings and could prepare the same folder twice.
        // Opening it again brings the open window to the front.
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            if let Some(w) = app.get_webview_window("main") {
                let _ = w.unminimize();
                let _ = w.set_focus();
            }
        }))
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            let home = std::env::var_os(HOME_ENV).filter(|v| !v.is_empty()).map(PathBuf::from);
            let config_dir = match &home {
                Some(h) => h.join("config"),
                None => app.path().app_config_dir()?,
            };
            let data_dir = match &home {
                Some(h) => h.join("data"),
                None => app.path().app_local_data_dir()?,
            };
            app.manage(AppState {
                settings: settings::SettingsStore::load(config_dir.join("settings.json")),
                default_workspace: data_dir.join("workspace"),
                preparer: preparer::Session::default(),
                game: game::GameProcess::default(),
                avatars: avatars::Library::new(data_dir.join("avatars")),
                button_font: button_font::Store::new(data_dir.join("fonts")),
                exe_dir: std::env::current_exe()?.parent().map(Path::to_path_buf).unwrap_or_default(),
            });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            app_info,
            installation_status,
            choose_nup,
            choose_workspace,
            verify_nup,
            start_preparation,
            stop_preparation,
            preparation_running,
            open_preparation_log,
            launch_game,
            player_settings,
            set_language,
            set_player_name,
            saves_info,
            open_saves_folder,
            list_avatars,
            import_avatar,
            select_avatar,
            remove_avatar,
            button_font_status,
            import_button_font,
            remove_button_font,
            textures_info,
            set_bc7_textures,
            prepare_textures
        ])
        .run(tauri::generate_context!());
    if let Err(e) = result {
        let (title, text) = requirements::start_failed_message(&e.to_string());
        requirements::show_error(&title, &text);
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn textures_ready_only_for_the_same_finished_executable() {
        let d = std::env::temp_dir().join(format!("reflora-test-textures-{}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        let exe = d.join("wonder2.exe");
        std::fs::write(&exe, b"game").unwrap();
        let m = std::fs::metadata(&exe).unwrap();
        let mtime = m.modified().unwrap().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs();
        let rec = |status: &str, size: u64, mtime: u64| {
            serde_json::json!({ "status": status, "exe_size": size, "exe_mtime": mtime })
        };
        assert!(textures_ready(Some(&rec("ok", 4, mtime)), &exe));
        assert!(!textures_ready(Some(&rec("partial", 4, mtime)), &exe), "not finished");
        assert!(!textures_ready(Some(&rec("ok", 5, mtime)), &exe), "another executable (size)");
        assert!(!textures_ready(Some(&rec("ok", 4, mtime + 60)), &exe), "another executable (time)");
        assert!(!textures_ready(Some(&serde_json::json!({ "status": "ok" })), &exe), "record of an older preparer");
        assert!(!textures_ready(None, &exe));
        assert!(!textures_ready(Some(&rec("ok", 4, mtime)), &d.join("missing.exe")));
    }

    #[test]
    fn workspace_choice_never_mixes_with_other_files() {
        let d = std::env::temp_dir().join(format!("reflora-test-wschoice-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join("empty")).unwrap();
        std::fs::create_dir_all(d.join("games")).unwrap();
        std::fs::write(d.join("games").join("other.txt"), "x").unwrap();
        std::fs::create_dir_all(d.join("ws")).unwrap();
        std::fs::write(d.join("ws").join(".nx-workspace"), "x").unwrap();
        std::fs::write(d.join("ws").join("other.txt"), "x").unwrap();
        assert_eq!(workspace_from_choice(&d.join("empty")), d.join("empty"));
        assert_eq!(workspace_from_choice(&d.join("games")), d.join("games").join("REFLORA"));
        assert_eq!(workspace_from_choice(&d.join("ws")), d.join("ws"));
    }
}
