//! Starting the prepared game: `<workspace>\bin\wonder2.exe` with its data folder `<workspace>\data`
//! (NX_DATA_ROOT; the runtime reads settings.ini, saves and the package path from there).

use std::path::Path;
use std::process::{Child, Command};
use std::sync::Mutex;

/// Variables of the test harnesses that must never reach a game the user plays. The pipeline ones: the game prepares
/// the catalog the preparer installed in data\ and loads the pipeline cache it made, as they are (a salt would make
/// every module new to the driver; a forced catalog, mode or cache switch would bypass the prepared ones).
/// The BC7 ones: the preparation mode (the game would convert and exit) and the quality limit kept for measurements.
const TEST_ONLY_ENV: [&str; 17] = [
    "NX_HEADLESS", "NX_MUTE", "NX_INPUT_SCRIPT", "NX_OVERLAY_TEST", "NX_PRESENT_LOG", "NX_DUMP_SHADER_MISSES",
    "NX_LOG", "NX_SPIRV_SALT", "NX_PIPELINE_RECORD", "NX_PIPELINE_PREWARM", "NX_PREWARM_MODE", "NX_PREWARM_EXIT",
    "NX_PIPELINE_KEY_RAW", "NX_PIPELINE_CACHE", "NX_PIPELINE_VERIFY", "NX_ASTC_BC7_PREPARE", "NX_ASTC_BC7_DB",
];

/// The experimental BC7 textures (runtime/src/nvn/nvn_astc_bc7.cpp): set from the player's choice, never inherited.
const BC7_ENV: &str = "NX_ASTC_BC7";

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

    /// `nickname`: the player's name (NX_NICKNAME), or `None` for the runtime's own choice. `bc7`: the experimental
    /// BC7 textures (NX_ASTC_BC7=1).
    pub fn launch(&self, workspace: &Path, nickname: Option<&str>, bc7: bool) -> Result<u32, String> {
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
        if bc7 {
            cmd.env(BC7_ENV, "1");
        } else {
            cmd.env_remove(BC7_ENV);
        }
        let child = cmd.spawn().map_err(|e| format!("spawn: {e}"))?;
        let pid = child.id();
        *self.child.lock().unwrap() = Some(child);
        Ok(pid)
    }
}
