//! What is installed: a quick, read-only look at the workspace the preparer fills (docs/REFLORA_PREPARER.md).
//!
//! This only checks that each step's outputs are present, to choose what the interface offers (Install, Continue,
//! Play). It is not the verification: the preparer itself re-checks every input fingerprint and output SHA-256 each
//! time it runs, and a run of it is what turns "interrupted" into "ready".

use std::path::{Path, PathBuf};

use serde::Serialize;

/// The preparer's steps, in order (tools/prepare_switch2.py STEPS).
pub const STEPS: [&str; 9] =
    ["verify", "extract", "bins", "recompile", "shaders", "build", "config", "check", "graphics"];
const MODULES: [&str; 3] = ["main", "sdk", "subsdk0"];

#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Status {
    /// No workspace and no NUP chosen.
    NotInstalled,
    /// A NUP is chosen but nothing was prepared yet.
    NupSelected,
    /// A workspace with part of the preparation: the next step to run.
    Interrupted { next_step: String },
    /// Everything the game needs is there.
    Ready,
    /// The game started from this launcher is running.
    Running,
}

#[derive(Clone, Debug, Serialize)]
pub struct StepPresence {
    pub name: &'static str,
    /// Its outputs are present (`None`: a step whose result leaves nothing to look at: verify, check).
    pub present: Option<bool>,
}

/// The graphics preparation (the preparer's `graphics` step, docs/REFLORA_PREPARER.md): the last result it wrote in
/// `work\graphics.json`. It only says how far the game is optimized: a playable installation stays `Ready` whatever
/// this is.
#[derive(Clone, Debug, Default, Serialize, PartialEq)]
pub struct GraphicsReport {
    /// "none" (never ran here), "ok", "partial", "failed" or "skipped" (an executable from before the step, or no
    /// usable catalog).
    pub state: String,
    pub made: u64,
    pub failed: u64,
    pub total: u64,
    pub compute: u64,
    pub seconds: f64,
    pub cache_bytes: u64,
    /// The preparer's reason when not ok (English, for the log and the details).
    pub reason: Option<String>,
    pub finished: Option<String>,
}

pub const GRAPHICS_STATE: &str = "work/graphics.json";

pub fn graphics_report(ws: &Path) -> GraphicsReport {
    let none = GraphicsReport { state: "none".into(), ..Default::default() };
    let Ok(text) = std::fs::read_to_string(ws.join(GRAPHICS_STATE)) else { return none };
    let Ok(v) = serde_json::from_str::<serde_json::Value>(&text) else { return none };
    let state = v["status"].as_str().unwrap_or("");
    if v["format"].as_u64() != Some(1) || !["ok", "partial", "failed", "skipped"].contains(&state) {
        return none;
    }
    let n = |k: &str| v[k].as_u64().unwrap_or(0);
    GraphicsReport {
        state: state.into(),
        made: n("made"),
        failed: n("failed"),
        total: n("total"),
        compute: n("compute"),
        seconds: v["seconds"].as_f64().unwrap_or(0.0),
        cache_bytes: n("cache_bytes"),
        reason: v["reason"].as_str().map(String::from),
        finished: v["finished"].as_str().map(String::from),
    }
}

#[derive(Clone, Debug, Serialize)]
pub struct InstallReport {
    pub status: Status,
    pub workspace: PathBuf,
    pub workspace_exists: bool,
    pub nup: Option<PathBuf>,
    pub nup_exists: bool,
    /// The package recorded in data/nup.txt by the config step.
    pub configured_nup: Option<PathBuf>,
    pub steps: Vec<StepPresence>,
    pub graphics: GraphicsReport,
}

fn all_files(base: &Path, rels: &[String]) -> bool {
    rels.iter().all(|r| base.join(r).is_file())
}

fn non_empty_dir(p: &Path) -> bool {
    std::fs::read_dir(p).map(|mut d| d.next().is_some()).unwrap_or(false)
}

fn step_present(ws: &Path, step: &str) -> Option<bool> {
    let m = |fmt: &dyn Fn(&str) -> String| MODULES.iter().map(|x| fmt(x)).collect::<Vec<_>>();
    Some(match step {
        "extract" => all_files(ws, &m(&|x| format!("work/exefs/{x}"))),
        "bins" => all_files(ws, &m(&|x| format!("data/analysis/{x}.bin"))),
        "recompile" => ws.join("work/recompile-state.json").is_file() && non_empty_dir(&ws.join("work/gen")),
        "shaders" => ws.join("work/shaders/state.json").is_file() && non_empty_dir(&ws.join("data/spirv")),
        "build" => ws.join("bin/wonder2.exe").is_file() && ws.join("bin/SDL3.dll").is_file(),
        "config" => ws.join("data/nup.txt").is_file(),
        _ => return None,
    })
}

/// The package the config step recorded in `data\nup.txt`.
pub fn configured_nup(ws: &Path) -> Option<PathBuf> {
    let text = std::fs::read_to_string(ws.join("data/nup.txt")).ok()?;
    let line = text.lines().next()?.trim();
    (!line.is_empty()).then(|| PathBuf::from(line))
}

/// What the game itself needs to start (runtime: NX_DATA_ROOT = <ws>/data, the RomFS read from the NUP in nup.txt).
fn game_ready(ws: &Path, configured: Option<&Path>) -> bool {
    ws.join("bin/wonder2.exe").is_file()
        && ws.join("bin/SDL3.dll").is_file()
        && all_files(ws, &MODULES.iter().map(|x| format!("data/analysis/{x}.bin")).collect::<Vec<_>>())
        && non_empty_dir(&ws.join("data/spirv"))
        && configured.is_some_and(|p| p.is_file())
}

/// Written by the preparer when a whole preparation reaches DONE, removed when one starts (tools/prepare_switch2.py).
pub const PREPARED_MARKER: &str = "work/prepared.json";

pub fn inspect(ws: &Path, nup: Option<&Path>, game_running: bool) -> InstallReport {
    let made_by_preparer = ws.join(".nx-workspace").is_file();
    let workspace_exists = made_by_preparer || ws.join("data").is_dir();
    // A preparer's workspace is ready once a whole preparation finished; a folder put together by hand (development)
    // only by its contents.
    let finished = !made_by_preparer || ws.join(PREPARED_MARKER).is_file();
    let steps: Vec<StepPresence> = STEPS
        .iter()
        .map(|s| StepPresence { name: s, present: step_present(ws, s).map(|p| p && workspace_exists) })
        .collect();
    let configured = configured_nup(ws);
    let status = if game_running {
        Status::Running
    } else if workspace_exists && finished && game_ready(ws, configured.as_deref()) {
        Status::Ready
    } else if workspace_exists && steps.iter().any(|s| s.present == Some(true)) {
        let next = steps.iter().find(|s| s.present == Some(false)).map(|s| s.name).unwrap_or("check");
        Status::Interrupted { next_step: next.to_string() }
    } else if nup.is_some() {
        Status::NupSelected
    } else {
        Status::NotInstalled
    };
    InstallReport {
        status,
        workspace: ws.to_path_buf(),
        workspace_exists,
        nup: nup.map(Path::to_path_buf),
        nup_exists: nup.is_some_and(Path::is_file),
        configured_nup: configured,
        steps,
        graphics: graphics_report(ws),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ws(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("reflora-test-{}-{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    fn touch(base: &Path, rel: &str) {
        let p = base.join(rel);
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(p, b"x").unwrap();
    }

    #[test]
    fn nothing_and_nup_only() {
        let d = ws("install-none");
        let missing = d.join("nowhere");
        assert_eq!(inspect(&missing, None, false).status, Status::NotInstalled);
        assert_eq!(inspect(&missing, None, false).graphics.state, "none");
        let nup = d.join("Wonder.nup");
        touch(&d, "Wonder.nup");
        let r = inspect(&missing, Some(&nup), false);
        assert_eq!(r.status, Status::NupSelected);
        assert!(r.nup_exists && !r.workspace_exists);
    }

    #[test]
    fn interrupted_names_the_first_missing_step() {
        let d = ws("install-partial");
        touch(&d, ".nx-workspace");
        for m in MODULES {
            touch(&d, &format!("work/exefs/{m}"));
            touch(&d, &format!("data/analysis/{m}.bin"));
        }
        touch(&d, "work/recompile-state.json");
        touch(&d, "work/gen/main/a.cpp");
        assert_eq!(inspect(&d, None, false).status, Status::Interrupted { next_step: "shaders".into() });
    }

    #[test]
    fn ready_needs_the_package_named_in_nup_txt() {
        let d = ws("install-ready");
        touch(&d, ".nx-workspace");
        for m in MODULES {
            touch(&d, &format!("work/exefs/{m}"));
            touch(&d, &format!("data/analysis/{m}.bin"));
        }
        for f in ["work/recompile-state.json", "work/gen/main/a.cpp", "work/shaders/state.json", "data/spirv/a.spv",
                  "bin/wonder2.exe", "bin/SDL3.dll", "Wonder.nup"] {
            touch(&d, f);
        }
        std::fs::write(d.join("data/nup.txt"), format!("{}\n", d.join("gone.nup").display())).unwrap();
        assert_eq!(inspect(&d, None, false).status, Status::Interrupted { next_step: "check".into() });
        std::fs::write(d.join("data/nup.txt"), format!("{}\n", d.join("Wonder.nup").display())).unwrap();
        // everything there, but the preparation did not reach DONE (no marker): its last step still has to run
        assert_eq!(inspect(&d, None, false).status, Status::Interrupted { next_step: "check".into() });
        touch(&d, PREPARED_MARKER);
        let r = inspect(&d, None, false);
        assert_eq!(r.status, Status::Ready);
        assert_eq!(r.configured_nup, Some(d.join("Wonder.nup")));
        assert_eq!(inspect(&d, None, true).status, Status::Running);
        // the graphics preparation never changes "ready"
        for (json, state) in [
            (r#"{"format": 1, "status": "failed", "reason": "no answer from the runtime after 120 s"}"#, "failed"),
            (r#"{"format": 1, "status": "partial", "made": 770, "failed": 3, "total": 773}"#, "partial"),
            ("not json", "none"),
            (r#"{"format": 2, "status": "ok"}"#, "none"),
            (r#"{"format": 1, "status": "ok", "made": 773, "total": 773, "compute": 5, "seconds": 7.7}"#, "ok"),
        ] {
            std::fs::write(d.join(GRAPHICS_STATE), json).unwrap();
            let r = inspect(&d, None, false);
            assert_eq!((r.status, r.graphics.state.as_str()), (Status::Ready, state));
        }
        let g = graphics_report(&d);
        assert_eq!((g.made, g.total, g.compute, g.seconds), (773, 773, 5, 7.7));
        assert_eq!(graphics_report(&d.join("nowhere")).state, "none");
        // a folder assembled by hand (no .nx-workspace) is judged by its contents alone
        std::fs::remove_file(d.join(".nx-workspace")).unwrap();
        std::fs::remove_file(d.join(PREPARED_MARKER)).unwrap();
        assert_eq!(inspect(&d, None, false).status, Status::Ready);
    }
}
