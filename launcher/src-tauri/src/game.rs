//! Starting the prepared game: `<workspace>\bin\wonder2.exe` with its data folder `<workspace>\data`
//! (NX_DATA_ROOT; the runtime reads settings.ini, saves and the package path from there).

use std::path::Path;
use std::process::{Child, Command};
use std::sync::Mutex;

/// Variables of the test harnesses that must never reach a game the user plays. The pipeline ones: the game prepares
/// the catalog the preparer installed in data\ and loads the pipeline cache it made, as they are (a salt would make
/// every module new to the driver; a forced catalog, mode or cache switch would bypass the prepared ones).
const TEST_ONLY_ENV: [&str; 15] = [
    "NX_HEADLESS", "NX_MUTE", "NX_INPUT_SCRIPT", "NX_OVERLAY_TEST", "NX_PRESENT_LOG", "NX_DUMP_SHADER_MISSES",
    "NX_LOG", "NX_SPIRV_SALT", "NX_PIPELINE_RECORD", "NX_PIPELINE_PREWARM", "NX_PREWARM_MODE", "NX_PREWARM_EXIT",
    "NX_PIPELINE_KEY_RAW", "NX_PIPELINE_CACHE", "NX_PIPELINE_VERIFY",
];

#[derive(Default)]
pub struct GameProcess {
    child: Mutex<Option<Child>>,
}

impl GameProcess {
    pub fn is_running(&self) -> bool {
        let mut slot = self.child.lock().unwrap();
        match slot.as_mut().map(|c| c.try_wait()) {
            Some(Ok(None)) => true,
            Some(_) => {
                *slot = None;
                false
            }
            None => false,
        }
    }

    /// `nickname`: the player's name (NX_NICKNAME), or `None` for the runtime's own choice.
    pub fn launch(&self, workspace: &Path, nickname: Option<&str>) -> Result<u32, String> {
        if self.is_running() {
            return Err("already_running".into());
        }
        let bin = workspace.join("bin");
        let mut cmd = Command::new(bin.join("wonder2.exe"));
        cmd.current_dir(&bin).env("NX_DATA_ROOT", workspace.join("data"));
        for v in TEST_ONLY_ENV {
            cmd.env_remove(v);
        }
        match nickname {
            Some(n) => cmd.env("NX_NICKNAME", n),
            None => cmd.env_remove("NX_NICKNAME"),
        };
        let child = cmd.spawn().map_err(|e| format!("spawn: {e}"))?;
        let pid = child.id();
        *self.child.lock().unwrap() = Some(child);
        Ok(pid)
    }
}
