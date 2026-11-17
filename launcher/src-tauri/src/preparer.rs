//! The preparer (tools/prepare_switch2.py, docs/REFLORA_PREPARER.md): finding it, running it, and its line protocol
//! turned into `preparer://event` events for the interface.
//!
//! The preparer runs as the components' portable Python, at below-normal priority, inside a Windows job object:
//! stopping it, or closing the launcher, ends the whole process tree (Python, CMake, Ninja, clang, the shader
//! translator). Everything it prints also goes to `<workspace>\work\reflora-prepare.log`. While it runs the PC is
//! kept from sleeping. The preparer resumes by itself: starting it again continues where it stopped.

use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex};

use serde::Serialize;
use tauri::Emitter;

pub const EVENT: &str = "preparer://event";
pub const LOG_FILE: &str = "reflora-prepare.log";

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum PrepEvent {
    /// A preparation began.
    Started,
    /// `STEP <i>/<n> <name>[ already done (skipped)]`
    Step { index: u32, total: u32, name: String, skipped: bool },
    /// `GROUP <i>/<n> <group> <phase> done|valid (kept): <detail>` or `GROUP <i>/<n> <group> error: <text>`
    Group { index: u32, total: u32, name: String, phase: String, state: GroupState, detail: String },
    /// `PROGRESS <0-100>`: progress inside the current step.
    Progress { percent: u8 },
    /// `COUNT <done>/<total> <unit>`: what the current step counts (the graphics step: pipelines).
    Count { done: u64, total: u64, unit: String },
    /// `RESULT <step> <status> key=value...`: the outcome of a step that reports one (graphics: ok, partial, failed,
    /// skipped; made, failed, total, compute, seconds).
    Result { step: String, status: String, made: u64, failed: u64, total: u64, compute: u64, seconds: f64 },
    Log { text: String },
    Warn { text: String },
    Error { text: String },
    /// `DONE`
    Done,
    /// The run ended: the preparer's exit code (0 ready, 2 needs the user, 1 failed), or stopped by the user.
    Finished { exit_code: Option<i32>, stopped: bool },
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum GroupState {
    Done,
    Kept,
    Error,
}

fn fraction(s: &str) -> Option<(u32, u32)> {
    let (a, b) = s.split_once('/')?;
    Some((a.parse().ok()?, b.parse().ok()?))
}

/// One stdout line of the preparer. Lines outside the protocol (a tool printing directly) become `Log`.
pub fn parse_line(line: &str) -> Option<PrepEvent> {
    let line = line.trim_end_matches(['\r', '\n']);
    if line.trim().is_empty() {
        return None;
    }
    Some(parse_known(line).unwrap_or_else(|| PrepEvent::Log { text: line.into() }))
}

fn parse_known(line: &str) -> Option<PrepEvent> {
    let (kind, rest) = line.split_once(' ').unwrap_or((line, ""));
    match kind {
        "STEP" => {
            let (frac, tail) = rest.split_once(' ')?;
            let (index, total) = fraction(frac)?;
            let skipped = tail.ends_with("(skipped)");
            let name = tail.split_whitespace().next()?.to_string();
            Some(PrepEvent::Step { index, total, name, skipped })
        }
        "GROUP" => {
            let mut it = rest.splitn(3, ' ');
            let (index, total) = fraction(it.next()?)?;
            let name = it.next()?.to_string();
            let tail = it.next()?;
            if let Some(text) = tail.strip_prefix("error:") {
                Some(PrepEvent::Group {
                    index, total, name, phase: String::new(), state: GroupState::Error, detail: text.trim().into(),
                })
            } else {
                let (head, detail) = tail.split_once(": ").unwrap_or((tail, ""));
                let (phase, state) = head.split_once(' ').unwrap_or((head, ""));
                let state = if state.starts_with("valid") { GroupState::Kept } else { GroupState::Done };
                Some(PrepEvent::Group { index, total, name, phase: phase.into(), state, detail: detail.into() })
            }
        }
        "PROGRESS" => rest.trim().parse::<u32>().ok().map(|p| PrepEvent::Progress { percent: p.min(100) as u8 }),
        "COUNT" => {
            let (frac, unit) = rest.split_once(' ').unwrap_or((rest, ""));
            let (a, b) = frac.split_once('/')?;
            Some(PrepEvent::Count { done: a.parse().ok()?, total: b.parse().ok()?, unit: unit.trim().into() })
        }
        "RESULT" => {
            let mut it = rest.split_whitespace();
            let (step, status) = (it.next()?.to_string(), it.next()?.to_string());
            let kv: std::collections::HashMap<&str, &str> = it.filter_map(|p| p.split_once('=')).collect();
            let n = |k: &str| kv.get(k).and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
            Some(PrepEvent::Result {
                step,
                status,
                made: n("made"),
                failed: n("failed"),
                total: n("total"),
                compute: n("compute"),
                seconds: kv.get("seconds").and_then(|v| v.parse().ok()).unwrap_or(0.0),
            })
        }
        "LOG" => Some(PrepEvent::Log { text: rest.into() }),
        "WARN" => Some(PrepEvent::Warn { text: rest.into() }),
        "ERROR" => Some(PrepEvent::Error { text: rest.into() }),
        "DONE" if rest.is_empty() => Some(PrepEvent::Done),
        _ => None,
    }
}

// ---- where the preparer is ------------------------------------------------------------------------------------

/// The preparer and the portable Python that runs it.
#[derive(Clone, Debug, Serialize)]
pub struct Tools {
    /// The folder holding tools\prepare_switch2.py (REFLORA's sources).
    pub root: PathBuf,
    /// Reflora's portable components (components.json, python\, spirv_harness\, spirv-val\).
    pub components: PathBuf,
    pub python: PathBuf,
}

pub const ROOT_ENV: &str = "REFLORA_ROOT";
pub const COMPONENTS_ENV: &str = "REFLORA_COMPONENTS";

/// `REFLORA_ROOT`, else the launcher's folder or one above it that holds `tools\prepare_switch2.py` (the packaged
/// layout puts the sources beside the launcher; in development the launcher is built inside the repository).
/// Components: `REFLORA_COMPONENTS`, else `<root>\components`.
pub fn locate(exe_dir: &Path, root_env: Option<PathBuf>, components_env: Option<PathBuf>) -> Result<Tools, String> {
    let is_root = |p: &Path| p.join("tools").join("prepare_switch2.py").is_file();
    let root = match root_env {
        Some(r) if is_root(&r) => r,
        Some(_) => return Err("no_preparer".into()),
        None => exe_dir
            .ancestors()
            .flat_map(|a| [a.to_path_buf(), a.join("reflora")])
            .find(|p| is_root(p))
            .ok_or("no_preparer")?,
    };
    let components = components_env.unwrap_or_else(|| root.join("components"));
    let python = components.join("python").join("python.exe");
    if !components.join("components.json").is_file() || !python.is_file() {
        return Err("no_components".into());
    }
    Ok(Tools { root, components, python })
}

// ---- running it -----------------------------------------------------------------------------------------------

pub struct Spec {
    pub tools: Tools,
    pub nup: PathBuf,
    pub workspace: PathBuf,
    /// Only these steps (None: the whole preparation).
    pub steps: Option<String>,
}

impl Spec {
    fn command(&self) -> Command {
        let mut cmd = Command::new(&self.tools.python);
        cmd.arg("-X").arg("utf8").arg(self.tools.root.join("tools").join("prepare_switch2.py"));
        cmd.arg("--nup").arg(&self.nup).arg("--workspace").arg(&self.workspace);
        cmd.arg("--components").arg(&self.tools.components);
        if let Some(s) = &self.steps {
            cmd.arg("--steps").arg(s);
        }
        cmd.current_dir(&self.tools.root)
            .env("PYTHONUTF8", "1")
            .env("PYTHONIOENCODING", "utf-8")
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            const CREATE_NO_WINDOW: u32 = 0x0800_0000;
            const BELOW_NORMAL_PRIORITY_CLASS: u32 = 0x0000_4000;
            cmd.creation_flags(CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS);
        }
        cmd
    }
}

/// Runs the preparer to its end, passing every event to `sink` (the last one is `Finished`). `job` lets another
/// thread stop it.
pub fn run(spec: &Spec, job: &Job, stop_requested: &dyn Fn() -> bool, mut sink: impl FnMut(PrepEvent)) {
    let work = spec.workspace.join("work");
    let _ = std::fs::create_dir_all(&work);
    let mut log = std::fs::OpenOptions::new().create(true).append(true).open(work.join(LOG_FILE)).ok();
    let mut write_log = |line: &str| {
        if let Some(f) = log.as_mut() {
            let _ = writeln!(f, "{line}");
        }
    };
    write_log(&format!("==== {} REFLORA launcher: {:?}", timestamp(), spec.command()));
    sink(PrepEvent::Started);
    let _awake = KeepAwake::new();
    let mut child = match spec.command().spawn() {
        Ok(c) => c,
        Err(e) => {
            write_log(&format!("cannot start: {e}"));
            sink(PrepEvent::Error { text: format!("python: {e}") });
            sink(PrepEvent::Finished { exit_code: None, stopped: false });
            return;
        }
    };
    job.assign(&child);
    let stderr = child.stderr.take().map(|e| {
        std::thread::spawn(move || {
            BufReader::new(e).lines().map_while(Result::ok).collect::<Vec<_>>() // a traceback, if any
        })
    });
    if let Some(out) = child.stdout.take() {
        for line in BufReader::new(out).lines().map_while(Result::ok) {
            write_log(&line);
            if let Some(ev) = parse_line(&line) {
                sink(ev);
            }
        }
    }
    let status = child.wait().ok();
    for line in stderr.and_then(|h| h.join().ok()).unwrap_or_default() {
        write_log(&format!("stderr: {line}"));
        if !line.trim().is_empty() {
            sink(PrepEvent::Log { text: line });
        }
    }
    let stopped = stop_requested();
    let exit_code = if stopped { None } else { status.and_then(|s| s.code()) };
    write_log(&format!("==== {} exit {:?}{}", timestamp(), exit_code, if stopped { " (stopped)" } else { "" }));
    sink(PrepEvent::Finished { exit_code, stopped });
}

fn timestamp() -> String {
    let secs = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    format!("unix {secs}")
}

/// One preparation at a time; the interface follows it through events.
#[derive(Default)]
pub struct Session {
    current: Mutex<Option<Arc<Running>>>,
}

pub struct Running {
    job: Job,
    stop: std::sync::atomic::AtomicBool,
    finished: std::sync::atomic::AtomicBool,
}

impl Session {
    pub fn is_running(&self) -> bool {
        self.current
            .lock()
            .unwrap()
            .as_ref()
            .is_some_and(|r| !r.finished.load(std::sync::atomic::Ordering::SeqCst))
    }

    pub fn start<R: tauri::Runtime>(&self, window: tauri::Window<R>, spec: Spec) -> Result<(), String> {
        let mut slot = self.current.lock().unwrap();
        if slot.as_ref().is_some_and(|r| !r.finished.load(std::sync::atomic::Ordering::SeqCst)) {
            return Err("already_preparing".into());
        }
        let running = Arc::new(Running {
            job: Job::new().map_err(|e| format!("job: {e}"))?,
            stop: Default::default(),
            finished: Default::default(),
        });
        *slot = Some(running.clone());
        drop(slot);
        std::thread::spawn(move || {
            let r = running.clone();
            run(&spec, &running.job, &|| r.stop.load(std::sync::atomic::Ordering::SeqCst), |ev| {
                if matches!(ev, PrepEvent::Finished { .. }) {
                    running.finished.store(true, std::sync::atomic::Ordering::SeqCst);
                }
                let _ = window.emit(EVENT, ev);
            });
        });
        Ok(())
    }

    /// Ends the whole process tree; the preparer continues from there next time.
    pub fn stop(&self) {
        if let Some(r) = self.current.lock().unwrap().as_ref() {
            r.stop.store(true, std::sync::atomic::Ordering::SeqCst);
            r.job.terminate();
        }
    }
}

// ---- Windows: the job object and keeping the PC awake ---------------------------------------------------------

#[cfg(windows)]
pub use win::{Job, KeepAwake};

#[cfg(windows)]
mod win {
    use std::os::windows::io::AsRawHandle;
    use windows_sys::Win32::Foundation::{CloseHandle, HANDLE};
    use windows_sys::Win32::System::JobObjects::{
        AssignProcessToJobObject, CreateJobObjectW, JobObjectExtendedLimitInformation, SetInformationJobObject,
        TerminateJobObject, JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
    };
    use windows_sys::Win32::System::Power::{SetThreadExecutionState, ES_CONTINUOUS, ES_SYSTEM_REQUIRED};

    /// Every process of the preparation; closing it (the launcher exits) or terminating it ends them all.
    pub struct Job(HANDLE);
    // SAFETY: a job handle may be used from any thread.
    unsafe impl Send for Job {}
    unsafe impl Sync for Job {}

    impl Job {
        pub fn new() -> std::io::Result<Self> {
            // SAFETY: plain Win32 calls on a handle this struct owns.
            unsafe {
                let h = CreateJobObjectW(std::ptr::null(), std::ptr::null());
                if h.is_null() {
                    return Err(std::io::Error::last_os_error());
                }
                let mut info: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
                info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(
                    h,
                    JobObjectExtendedLimitInformation,
                    &info as *const _ as *const core::ffi::c_void,
                    std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
                );
                Ok(Job(h))
            }
        }

        pub fn assign(&self, child: &std::process::Child) {
            // SAFETY: the child's handle is valid while `child` lives.
            unsafe {
                AssignProcessToJobObject(self.0, child.as_raw_handle() as HANDLE);
            }
        }

        pub fn terminate(&self) {
            // SAFETY: the handle is owned and open.
            unsafe {
                TerminateJobObject(self.0, 1);
            }
        }
    }

    impl Drop for Job {
        fn drop(&mut self) {
            // SAFETY: closed once, here.
            unsafe {
                CloseHandle(self.0);
            }
        }
    }

    /// The PC does not go to sleep while this lives (on the thread that made it).
    pub struct KeepAwake;

    impl KeepAwake {
        pub fn new() -> Self {
            // SAFETY: changes only this thread's execution state.
            unsafe {
                SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
            }
            KeepAwake
        }
    }

    impl Drop for KeepAwake {
        fn drop(&mut self) {
            // SAFETY: as above.
            unsafe {
                SetThreadExecutionState(ES_CONTINUOUS);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const DEMO_SCRIPT: &str = include_str!("../fixtures/demo_preparation.txt");

    #[test]
    fn steps() {
        assert_eq!(
            parse_line("STEP 5/8 shaders"),
            Some(PrepEvent::Step { index: 5, total: 8, name: "shaders".into(), skipped: false })
        );
        assert_eq!(
            parse_line("STEP 2/8 extract already done (skipped)\r\n"),
            Some(PrepEvent::Step { index: 2, total: 8, name: "extract".into(), skipped: true })
        );
    }

    #[test]
    fn groups_as_prepare_shaders_writes_them() {
        assert_eq!(
            parse_line("GROUP 3/81 effect_Example extract done: 4 stages"),
            Some(PrepEvent::Group {
                index: 3, total: 81, name: "effect_Example".into(), phase: "extract".into(),
                state: GroupState::Done, detail: "4 stages".into(),
            })
        );
        assert_eq!(
            parse_line("GROUP 40/81 layout_Example lower valid (kept): 12 blocks, 1 unsupported"),
            Some(PrepEvent::Group {
                index: 40, total: 81, name: "layout_Example".into(), phase: "lower".into(),
                state: GroupState::Kept, detail: "12 blocks, 1 unsupported".into(),
            })
        );
        assert!(matches!(
            parse_line("GROUP 7/81 lib_Example error: translator exited with 3"),
            Some(PrepEvent::Group { state: GroupState::Error, ref detail, .. }) if detail == "translator exited with 3"
        ));
    }

    #[test]
    fn progress_messages_and_done() {
        assert_eq!(parse_line("PROGRESS 42"), Some(PrepEvent::Progress { percent: 42 }));
        assert_eq!(parse_line("PROGRESS 140"), Some(PrepEvent::Progress { percent: 100 }));
        assert_eq!(parse_line("WARN no add-on"), Some(PrepEvent::Warn { text: "no add-on".into() }));
        assert_eq!(parse_line("ERROR verify: x"), Some(PrepEvent::Error { text: "verify: x".into() }));
        assert_eq!(parse_line("DONE"), Some(PrepEvent::Done));
        assert_eq!(parse_line("COUNT 412/773 pipelines"),
                   Some(PrepEvent::Count { done: 412, total: 773, unit: "pipelines".into() }));
        assert_eq!(
            parse_line("RESULT graphics ok made=773 failed=0 total=773 compute=5 seconds=7.7"),
            Some(PrepEvent::Result { step: "graphics".into(), status: "ok".into(), made: 773, failed: 0, total: 773,
                                     compute: 5, seconds: 7.7 }));
        assert_eq!(
            parse_line("RESULT graphics failed made=0 failed=0 total=0 compute=0 seconds=2.1"),
            Some(PrepEvent::Result { step: "graphics".into(), status: "failed".into(), made: 0, failed: 0, total: 0,
                                     compute: 0, seconds: 2.1 }));
        assert_eq!(parse_line("STEP 9/9 graphics already done (skipped)"),
                   Some(PrepEvent::Step { index: 9, total: 9, name: "graphics".into(), skipped: true }));
        assert_eq!(parse_line(""), None);
    }

    #[test]
    fn foreign_lines_become_log() {
        assert_eq!(parse_line("ninja: build stopped"), Some(PrepEvent::Log { text: "ninja: build stopped".into() }));
        assert_eq!(parse_line("STEP broken"), Some(PrepEvent::Log { text: "STEP broken".into() }));
        assert_eq!(parse_line("PROGRESS lots"), Some(PrepEvent::Log { text: "PROGRESS lots".into() }));
        assert_eq!(parse_line("COUNT many"), Some(PrepEvent::Log { text: "COUNT many".into() }));
        assert_eq!(parse_line("RESULT"), Some(PrepEvent::Log { text: "RESULT".into() }));
    }

    #[test]
    fn recorded_format_script_is_all_protocol() {
        let events: Vec<_> = DEMO_SCRIPT.lines().filter_map(parse_line).collect();
        assert_eq!(events.iter().filter(|e| matches!(e, PrepEvent::Step { .. })).count(), 8);
        assert_eq!(events.last(), Some(&PrepEvent::Done));
        for line in DEMO_SCRIPT.lines().filter(|l| !l.trim().is_empty()) {
            let kind = line.split(' ').next().unwrap();
            if kind != "LOG" {
                assert!(!matches!(parse_line(line), Some(PrepEvent::Log { .. })), "not parsed: {line}");
            }
        }
    }

    #[test]
    fn locate_finds_the_root_above_the_launcher_and_needs_components() {
        let d = std::env::temp_dir().join(format!("reflora-test-locate-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        let exe_dir = d.join("launcher").join("target").join("release");
        std::fs::create_dir_all(&exe_dir).unwrap();
        assert_eq!(locate(&exe_dir, None, None).unwrap_err(), "no_preparer");
        std::fs::create_dir_all(d.join("tools")).unwrap();
        std::fs::write(d.join("tools").join("prepare_switch2.py"), "").unwrap();
        assert_eq!(locate(&exe_dir, None, None).unwrap_err(), "no_components");
        std::fs::create_dir_all(d.join("components").join("python")).unwrap();
        std::fs::write(d.join("components").join("components.json"), "{}").unwrap();
        std::fs::write(d.join("components").join("python").join("python.exe"), "").unwrap();
        let t = locate(&exe_dir, None, None).unwrap();
        assert_eq!((t.root, t.python), (d.clone(), d.join("components").join("python").join("python.exe")));
        assert_eq!(locate(&exe_dir, Some(d.join("nowhere")), None).unwrap_err(), "no_preparer");
    }

    /// The real preparer on a prepared workspace (manual: needs the NUP and the components).
    ///   set REFLORA_TEST_NUP=...  REFLORA_TEST_WORKSPACE=...  then  cargo test -- --ignored real_preparer
    #[test]
    #[ignore]
    fn real_preparer() {
        let nup = PathBuf::from(std::env::var("REFLORA_TEST_NUP").unwrap());
        let ws = PathBuf::from(std::env::var("REFLORA_TEST_WORKSPACE").unwrap());
        let exe_dir = std::env::current_exe().unwrap().parent().unwrap().to_path_buf();
        let spec = Spec { tools: locate(&exe_dir, None, None).unwrap(), nup, workspace: ws, steps: None };
        let job = Job::new().unwrap();
        let mut events = Vec::new();
        run(&spec, &job, &|| false, |e| events.push(e));
        let steps: Vec<_> = events.iter().filter_map(|e| match e {
            PrepEvent::Step { name, skipped, .. } => Some(format!("{name}{}", if *skipped { "(skipped)" } else { "" })),
            _ => None,
        }).collect();
        println!("steps: {steps:?}");
        for e in &events {
            if let PrepEvent::Error { text } | PrepEvent::Warn { text } = e {
                println!("{e:?}: {text}");
            }
        }
        assert_eq!(events.last(), Some(&PrepEvent::Finished { exit_code: Some(0), stopped: false }));
        assert!(events.contains(&PrepEvent::Done));
    }

    /// Stop during the check step (the game running under the preparer), confirm nothing of the tree is left, then
    /// run again to DONE. Manual, as `real_preparer`:  cargo test -- --ignored real_stop_and_resume --nocapture
    #[test]
    #[ignore]
    fn real_stop_and_resume() {
        use std::sync::atomic::{AtomicBool, Ordering};
        let nup = PathBuf::from(std::env::var("REFLORA_TEST_NUP").unwrap());
        let ws = PathBuf::from(std::env::var("REFLORA_TEST_WORKSPACE").unwrap());
        let exe_dir = std::env::current_exe().unwrap().parent().unwrap().to_path_buf();
        let tools = locate(&exe_dir, None, None).unwrap();
        let spec = Spec { tools: tools.clone(), nup: nup.clone(), workspace: ws.clone(), steps: None };
        let job = Arc::new(Job::new().unwrap());
        let stop = Arc::new(AtomicBool::new(false));
        let (j2, s2) = (job.clone(), stop.clone());
        let mut events = Vec::new();
        run(&spec, &job, &|| stop.load(Ordering::SeqCst), |e| {
            if matches!(&e, PrepEvent::Step { name, .. } if name == "check") {
                let (j3, s3) = (j2.clone(), s2.clone());
                std::thread::spawn(move || {
                    std::thread::sleep(std::time::Duration::from_secs(15)); // the game is up by then
                    s3.store(true, Ordering::SeqCst);
                    j3.terminate();
                });
            }
            events.push(e);
        });
        println!("first run ended with {:?}", events.last());
        assert_eq!(events.last(), Some(&PrepEvent::Finished { exit_code: None, stopped: true }));
        assert!(!ws.join("work").join("prepared.json").exists(), "stopped: not prepared");
        std::thread::sleep(std::time::Duration::from_secs(2));
        let left = Command::new("tasklist").args(["/FO", "CSV", "/NH"]).output().unwrap();
        let left = String::from_utf8_lossy(&left.stdout).to_lowercase();
        println!("still running after the stop: wonder2 {} python {}", left.contains("\"wonder2.exe\""), left.contains("\"python.exe\""));
        // (no other wonder2.exe may run during this test)
        assert!(!left.contains("\"wonder2.exe\""), "the game started by the check step must be gone");

        let job = Job::new().unwrap();
        let mut events = Vec::new();
        run(&Spec { tools, nup, workspace: ws.clone(), steps: None }, &job, &|| false, |e| events.push(e));
        let skipped = events.iter().filter(|e| matches!(e, PrepEvent::Step { skipped: true, .. })).count();
        println!("second run: {skipped} steps skipped, ended with {:?}", events.last());
        assert_eq!(events.last(), Some(&PrepEvent::Finished { exit_code: Some(0), stopped: false }));
        assert!(ws.join("work").join("prepared.json").is_file());
    }
}
