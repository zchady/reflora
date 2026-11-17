//! The player: the name the game receives as the account nickname, and where the saves are.
//!
//! The runtime (runtime/src/svc_account.cpp) takes the nickname from NX_NICKNAME, which the launcher sets when it
//! starts the game, else from `nickname=` in `<workspace>\data\user\profile.ini`, which the launcher also writes so
//! a start outside it agrees. Saves are in `<workspace>\data\user\save` (the runtime's save data folder).

use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use serde::Serialize;

/// As on the console: at most 10 characters; the game's field holds 31 bytes of UTF-8.
pub const MAX_CHARS: usize = 10;
const MAX_BYTES: usize = 31;

/// The name, trimmed, or the reason it is refused (a code the interface translates).
pub fn validate_name(raw: &str) -> Result<String, &'static str> {
    let name = raw.trim();
    if name.is_empty() {
        return Err("empty");
    }
    if name.chars().any(|c| c.is_control()) {
        return Err("invalid_char");
    }
    if name.chars().count() > MAX_CHARS || name.len() > MAX_BYTES {
        return Err("too_long");
    }
    Ok(name.to_string())
}

pub fn profile_file(workspace: &Path) -> PathBuf {
    workspace.join("data").join("user").join("profile.ini")
}

/// Writes profile.ini when the workspace has its data folder (else the name waits in the launcher's settings and
/// reaches the game through NX_NICKNAME). Returns whether it was written.
pub fn write_profile(workspace: &Path, name: &str) -> std::io::Result<bool> {
    let data = workspace.join("data");
    if !data.is_dir() {
        return Ok(false);
    }
    let path = profile_file(workspace);
    std::fs::create_dir_all(path.parent().unwrap())?;
    let tmp = path.with_extension("ini.tmp");
    std::fs::write(&tmp, format!("# REFLORA: the name the game receives for this user\nnickname={name}\n"))?;
    std::fs::rename(&tmp, &path)?;
    Ok(true)
}

pub fn saves_dir(workspace: &Path) -> PathBuf {
    workspace.join("data").join("user").join("save")
}

#[derive(Clone, Debug, Serialize)]
pub struct SaveFile {
    pub name: String,
    pub size: u64,
    /// Seconds since 1970 (UTC).
    pub modified: Option<u64>,
}

#[derive(Clone, Debug, Serialize)]
pub struct SavesInfo {
    pub path: PathBuf,
    pub exists: bool,
    pub files: Vec<SaveFile>,
}

pub fn saves_info(workspace: &Path) -> SavesInfo {
    let path = saves_dir(workspace);
    let mut files: Vec<SaveFile> = std::fs::read_dir(&path)
        .map(|rd| {
            rd.filter_map(Result::ok)
                .filter_map(|e| {
                    let meta = e.metadata().ok()?;
                    meta.is_file().then(|| SaveFile {
                        name: e.file_name().to_string_lossy().into_owned(),
                        size: meta.len(),
                        modified: meta.modified().ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map(|d| d.as_secs()),
                    })
                })
                .collect()
        })
        .unwrap_or_default();
    files.sort_by(|a, b| a.name.cmp(&b.name));
    SavesInfo { exists: path.is_dir(), path, files }
}

/// Shows the saves folder in File Explorer.
pub fn open_saves(workspace: &Path) -> Result<(), String> {
    let path = saves_dir(workspace);
    if !path.is_dir() {
        return Err("missing".into());
    }
    std::process::Command::new("explorer.exe").arg(&path).spawn().map(|_| ()).map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names() {
        assert_eq!(validate_name("  Luigi "), Ok("Luigi".into()));
        assert_eq!(validate_name("Ação Ñandú"), Ok("Ação Ñandú".into()));
        assert_eq!(validate_name("   "), Err("empty"));
        assert_eq!(validate_name("abcdefghijk"), Err("too_long"));
        assert_eq!(validate_name("a\tb"), Err("invalid_char"));
        assert_eq!(validate_name("🌸🌸🌸🌸🌸🌸🌸🌸"), Err("too_long")); // 8 characters, 32 bytes
        assert_eq!(validate_name("🌸🌸🌸🌸🌸🌸🌸"), Ok("🌸🌸🌸🌸🌸🌸🌸".into())); // 28 bytes
    }

    #[test]
    fn profile_and_saves() {
        let ws = std::env::temp_dir().join(format!("reflora-test-profile-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&ws);
        std::fs::create_dir_all(&ws).unwrap();
        assert!(!write_profile(&ws, "Ana").unwrap()); // no data folder yet: nothing written
        assert!(!saves_info(&ws).exists);
        std::fs::create_dir_all(saves_dir(&ws)).unwrap();
        std::fs::write(saves_dir(&ws).join("game_data.sav"), [0u8; 12]).unwrap();
        assert!(write_profile(&ws, "Ana").unwrap());
        assert!(std::fs::read_to_string(profile_file(&ws)).unwrap().contains("\nnickname=Ana\n"));
        let info = saves_info(&ws);
        assert!(info.exists);
        assert_eq!((info.files.len(), info.files[0].name.as_str(), info.files[0].size), (1, "game_data.sav", 12));
    }
}
