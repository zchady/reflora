//! The launcher's own settings: where the user's NUP is and where the workspace lives. Saved as JSON next to the
//! launcher's configuration (written to a `.tmp` and renamed, so a crash never leaves half a file).

use std::path::{Path, PathBuf};
use std::sync::Mutex;

use serde::{Deserialize, Serialize};

/// Development override for the workspace (for example a workspace prepared by hand with the preparer).
pub const WORKSPACE_ENV: &str = "REFLORA_WORKSPACE";

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct LauncherSettings {
    /// The user's NUP (selected in the installation page: next milestone).
    pub nup_path: Option<PathBuf>,
    /// The workspace chosen by the user; unset = the default under the launcher's local data folder.
    pub workspace: Option<PathBuf>,
    /// The launcher's language ("pt-BR", "en", "es"); unset = the system's language.
    pub language: Option<String>,
    /// The name the game receives as the user's account nickname (profile.rs).
    pub player_name: Option<String>,
    /// The chosen profile icon (an id of the local icon library, avatars.rs); unset = no picture.
    pub avatar: Option<String>,
    /// Experimental BC7 textures: the game starts with NX_ASTC_BC7=1 (off unless the user turns it on).
    pub bc7_textures: bool,
}

pub const LANGUAGES: [&str; 3] = ["pt-BR", "en", "es"];

#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum WorkspaceSource {
    Default,
    Settings,
    Environment,
}

#[derive(Clone, Debug, Serialize)]
pub struct Workspace {
    pub path: PathBuf,
    pub source: WorkspaceSource,
}

pub struct SettingsStore {
    path: PathBuf,
    inner: Mutex<LauncherSettings>,
}

impl SettingsStore {
    /// Missing or unreadable settings start from the defaults (the file is only written on a change).
    pub fn load(path: PathBuf) -> Self {
        let inner = std::fs::read_to_string(&path)
            .ok()
            .and_then(|text| serde_json::from_str(&text).ok())
            .unwrap_or_default();
        Self { path, inner: Mutex::new(inner) }
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn get(&self) -> LauncherSettings {
        self.inner.lock().unwrap().clone()
    }

    /// The change takes effect only once it is saved: when the file cannot be written (a folder the launcher may not
    /// write to, for example when started inside another app's container), nothing changes and the error is returned,
    /// so what the interface shows is what the next start will read.
    pub fn update(&self, change: impl FnOnce(&mut LauncherSettings)) -> std::io::Result<()> {
        let mut s = self.inner.lock().unwrap();
        let mut next = s.clone();
        change(&mut next);
        if let Some(dir) = self.path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        let tmp = self.path.with_extension("json.tmp");
        std::fs::write(&tmp, serde_json::to_vec_pretty(&next).expect("settings serialize"))?;
        if let Err(e) = std::fs::rename(&tmp, &self.path) {
            let _ = std::fs::remove_file(&tmp); // our own temporary file
            return Err(e);
        }
        *s = next;
        Ok(())
    }

    pub fn workspace(&self, default: &Path) -> Workspace {
        resolve_workspace(std::env::var_os(WORKSPACE_ENV).map(PathBuf::from), &self.get(), default)
    }
}

fn resolve_workspace(env: Option<PathBuf>, s: &LauncherSettings, default: &Path) -> Workspace {
    if let Some(p) = env.filter(|p| !p.as_os_str().is_empty()) {
        // components() drops the "." a script may leave in it (C:\x\.\ -> C:\x)
        return Workspace { path: p.components().collect(), source: WorkspaceSource::Environment };
    }
    match &s.workspace {
        Some(p) => Workspace { path: p.clone(), source: WorkspaceSource::Settings },
        None => Workspace { path: default.to_path_buf(), source: WorkspaceSource::Default },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("reflora-test-{}-{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn missing_or_broken_file_gives_defaults() {
        let d = temp_dir("settings-missing");
        assert_eq!(SettingsStore::load(d.join("settings.json")).get(), LauncherSettings::default());
        std::fs::write(d.join("broken.json"), "{ not json").unwrap();
        assert_eq!(SettingsStore::load(d.join("broken.json")).get(), LauncherSettings::default());
    }

    #[test]
    fn settings_of_an_earlier_version_keep_bc7_off() {
        let d = temp_dir("settings-earlier");
        let path = d.join("settings.json");
        std::fs::write(&path, r#"{ "nup_path": "D:\\Games\\Wonder.nup", "language": "en", "player_name": "Ana" }"#).unwrap();
        let s = SettingsStore::load(path).get();
        assert_eq!(s.player_name.as_deref(), Some("Ana"));
        assert!(!s.bc7_textures);
    }

    #[test]
    fn a_change_that_cannot_be_saved_does_not_apply() {
        let d = temp_dir("settings-unsaved");
        let path = d.join("settings.json");
        std::fs::create_dir_all(&path).unwrap(); // a folder where the file goes: the rename fails
        let store = SettingsStore::load(path.clone());
        assert!(store.update(|s| s.bc7_textures = true).is_err());
        assert!(!store.get().bc7_textures, "the interface must not show a choice the next start will not read");
        assert!(!path.with_extension("json.tmp").exists(), "no temporary file left behind");
    }

    #[test]
    fn update_is_saved_and_read_back() {
        let d = temp_dir("settings-roundtrip");
        let path = d.join("sub").join("settings.json");
        let store = SettingsStore::load(path.clone());
        store.update(|s| s.nup_path = Some(PathBuf::from(r"D:\Games\Wonder.nup"))).unwrap();
        assert!(!path.with_extension("json.tmp").exists());
        assert_eq!(SettingsStore::load(path).get().nup_path, Some(PathBuf::from(r"D:\Games\Wonder.nup")));
    }

    #[test]
    fn workspace_precedence_environment_settings_default() {
        let default = Path::new(r"C:\default");
        let mut s = LauncherSettings::default();
        assert_eq!(resolve_workspace(None, &s, default).source, WorkspaceSource::Default);
        s.workspace = Some(PathBuf::from(r"D:\ws"));
        let w = resolve_workspace(None, &s, default);
        assert_eq!((w.path, w.source), (PathBuf::from(r"D:\ws"), WorkspaceSource::Settings));
        let w = resolve_workspace(Some(PathBuf::from(r"E:\dev")), &s, default);
        assert_eq!((w.path, w.source), (PathBuf::from(r"E:\dev"), WorkspaceSource::Environment));
        assert_eq!(resolve_workspace(Some(PathBuf::new()), &s, default).source, WorkspaceSource::Settings);
        assert_eq!(resolve_workspace(Some(PathBuf::from(r"E:\dev\.")), &s, default).path, PathBuf::from(r"E:\dev"));
    }
}
