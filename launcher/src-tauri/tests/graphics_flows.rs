//! The graphics preparation through the launcher's own code (manual: needs the NUP, a REFLORA package and minutes).
//!
//! What the interface does, without the window: `preparer::run` with the spec "Install" / "Check and update" build
//! (every step, `steps: None`), the Stop button (`Job::terminate`), `install::inspect` (what the pages show) and
//! `game::GameProcess::launch` (the Play button).
//!
//!   set REFLORA_TEST_NUP=<Wonder.nup>
//!   set REFLORA_TEST_ROOT=<a REFLORA package: the sources and components\>      (not the repository)
//!   set REFLORA_TEST_WORKSPACE=<an installation folder only for this test>
//!   set REFLORA_TEST_LAUNCH=1          also start the game (a window opens for about 25 s)
//!   cargo test --release --test graphics_flows -- --ignored --nocapture --test-threads 1
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use reflora_lib::install::{self, Status};
use reflora_lib::preparer::{self, Job, PrepEvent, Spec};

struct Env {
    tools: preparer::Tools,
    nup: PathBuf,
    ws: PathBuf,
}

fn env() -> Env {
    let var = |k: &str| PathBuf::from(std::env::var(k).unwrap_or_else(|_| panic!("set {k}")));
    let root = var("REFLORA_TEST_ROOT");
    let exe_dir = std::env::current_exe().unwrap().parent().unwrap().to_path_buf();
    let tools = preparer::locate(&exe_dir, Some(root.clone()), None).expect("a REFLORA package with components");
    assert!(!root.join(".git").exists(), "REFLORA_TEST_ROOT must be a package, not the repository");
    Env { tools, nup: var("REFLORA_TEST_NUP"), ws: var("REFLORA_TEST_WORKSPACE") }
}

/// "Install" / "Check and update": the whole preparation, as start_preparation does. `stop_at`: press Stop when
/// that step starts.
fn prepare(e: &Env, stop_at: Option<&str>) -> Vec<PrepEvent> {
    let spec = Spec { tools: e.tools.clone(), nup: e.nup.clone(), workspace: e.ws.clone(), steps: None };
    let job = Arc::new(Job::new().unwrap());
    let stop = Arc::new(AtomicBool::new(false));
    let mut events = Vec::new();
    let t0 = std::time::Instant::now();
    preparer::run(&spec, &job, &|| stop.load(Ordering::SeqCst), |ev| {
        if let (Some(at), PrepEvent::Step { name, .. }) = (stop_at, &ev) {
            if name == at {
                stop.store(true, Ordering::SeqCst);
                job.terminate();
            }
        }
        events.push(ev);
    });
    let steps: Vec<String> = events
        .iter()
        .filter_map(|ev| match ev {
            PrepEvent::Step { name, skipped, .. } => Some(format!("{name}{}", if *skipped { "(skipped)" } else { "" })),
            _ => None,
        })
        .collect();
    println!("  {:.0} s; steps {steps:?}; ended {:?}", t0.elapsed().as_secs_f64(), events.last().unwrap());
    for ev in &events {
        match ev {
            PrepEvent::Result { .. } => println!("  {ev:?}"),
            PrepEvent::Warn { text } | PrepEvent::Error { text } => println!("  {ev:?}: {text}"),
            _ => {}
        }
    }
    events
}

fn result(events: &[PrepEvent]) -> Option<(String, u64, u64, u64)> {
    events.iter().find_map(|ev| match ev {
        PrepEvent::Result { step, status, made, total, compute, .. } if step == "graphics" => {
            Some((status.clone(), *made, *total, *compute))
        }
        _ => None,
    })
}

fn counts(events: &[PrepEvent]) -> Vec<(u64, u64)> {
    events
        .iter()
        .filter_map(|ev| match ev {
            PrepEvent::Count { done, total, .. } => Some((*done, *total)),
            _ => None,
        })
        .collect()
}

fn graphics_skipped(events: &[PrepEvent]) -> bool {
    events.iter().any(|ev| matches!(ev, PrepEvent::Step { name, skipped: true, .. } if name == "graphics"))
}

fn finished_ok(events: &[PrepEvent]) -> bool {
    events.contains(&PrepEvent::Done) && events.last() == Some(&PrepEvent::Finished { exit_code: Some(0), stopped: false })
}

fn report(e: &Env) -> install::InstallReport {
    install::inspect(&e.ws, None, false)
}

fn cache(e: &Env) -> Option<PathBuf> {
    std::fs::read_dir(e.ws.join("data")).ok()?.filter_map(Result::ok).map(|d| d.path()).find(|p| {
        let n = p.file_name().unwrap().to_string_lossy().to_string();
        n.starts_with("pipelines_") && n.ends_with(".bin")
    })
}

/// Every file under data\user (saves, profile), data\settings.ini and data\fonts: name -> (size, FNV-1a).
fn user_files(ws: &Path) -> BTreeMap<String, (u64, u64)> {
    let mut out = BTreeMap::new();
    let mut stack = vec![ws.join("data").join("user"), ws.join("data").join("fonts")];
    let mut files = vec![ws.join("data").join("settings.ini")];
    while let Some(d) = stack.pop() {
        for ent in std::fs::read_dir(&d).into_iter().flatten().flatten() {
            if ent.path().is_dir() {
                stack.push(ent.path());
            } else {
                files.push(ent.path());
            }
        }
    }
    for f in files {
        if let Ok(b) = std::fs::read(&f) {
            let h = b.iter().fold(0xcbf29ce484222325u64, |h, x| (h ^ *x as u64).wrapping_mul(0x100000001b3));
            out.insert(f.strip_prefix(ws).unwrap().display().to_string(), (b.len() as u64, h));
        }
    }
    out
}

#[test]
#[ignore]
fn real_graphics_flows() {
    let e = env();
    println!("package {} / installation {}", e.tools.root.display(), e.ws.display());

    // 1. a new installation: the graphics step runs by itself at the end, with real counts
    if !e.ws.join("work").join("prepared.json").is_file() {
        println!("1. new installation");
        let ev = prepare(&e, None);
        assert!(finished_ok(&ev));
        let (status, made, total, compute) = result(&ev).expect("RESULT graphics");
        assert_eq!(status, "ok");
        assert!(made == total && total > 0 && compute > 0, "made {made} of {total}, {compute} compute");
        let c = counts(&ev);
        assert!(c.first() == Some(&(0, total)) && c.last() == Some(&(total, total)), "{c:?}");
        let r = report(&e);
        assert_eq!((r.status, r.graphics.state.as_str(), r.graphics.made), (Status::Ready, "ok", total));
    } else {
        println!("1. (installation already there: not new)");
    }

    // saves and settings the player has: the rest must leave them as they are
    let user = e.ws.join("data").join("user");
    std::fs::create_dir_all(user.join("save")).unwrap();
    std::fs::write(user.join("save").join("reflora_test_sentinel.sav"), b"a save the preparation must keep").unwrap();
    std::fs::write(user.join("profile.ini"), "nickname=Tester\n").unwrap();
    let settings = e.ws.join("data").join("settings.ini");
    if !settings.is_file() {
        std::fs::write(&settings, "; reflora test\n[video]\nvsync=1\n").unwrap();
    }
    let before = user_files(&e.ws);

    // 2. "Check and update" on a prepared installation: the graphics step is recognized as done
    println!("2. check and update, nothing changed");
    let ev = prepare(&e, None);
    assert!(finished_ok(&ev) && graphics_skipped(&ev) && result(&ev).is_none());
    assert_eq!(report(&e).graphics.state, "ok");

    // 3. the cache deleted, and a version-2 local catalog (an older recording) beside the project's version 3
    println!("3. cache missing, a version-2 local catalog");
    std::fs::remove_file(cache(&e).expect("a pipeline cache")).unwrap();
    let v2 = std::env::var("REFLORA_TEST_V2_CATALOG").ok().map(PathBuf::from);
    if let Some(v2) = &v2 {
        std::fs::copy(v2, e.ws.join("data").join("pipeline_catalog_local.bin")).unwrap();
    }
    let ev = prepare(&e, None);
    assert!(finished_ok(&ev) && !graphics_skipped(&ev));
    let (status, made, total, _) = result(&ev).unwrap();
    assert!(status == "ok" && made == total);
    assert!(cache(&e).is_some(), "the cache is back");
    let log = std::fs::read_to_string(e.ws.join("work").join("graphics.log")).unwrap();
    assert!(log.contains("from 2 file(s)") == v2.is_some() && !log.contains("another format"), "both catalogs read");
    if v2.is_some() {
        std::fs::remove_file(e.ws.join("data").join("pipeline_catalog_local.bin")).unwrap();
    }

    // 4. a cache made for another GPU or driver (the driver's header changed, the file's own check redone)
    println!("4. cache for another GPU or driver");
    let p = cache(&e).unwrap();
    let mut b = std::fs::read(&p).unwrap();
    b[24 + 16] ^= 0xFF; // pipelineCacheUUID[0], after the NXVC header (24 bytes) and the driver header's 16
    let h = b[24..].iter().fold(0xcbf29ce484222325u64, |h, x| (h ^ *x as u64).wrapping_mul(0x100000001b3));
    b[16..24].copy_from_slice(&h.to_le_bytes());
    std::fs::write(&p, &b).unwrap();
    let ev = prepare(&e, None);
    assert!(finished_ok(&ev) && result(&ev).map(|r| r.0) == Some("ok".into()));
    // the first runtime to open it drops it: the check step's game (which then prepares the installed catalog in the
    // background and saves a valid cache), else the graphics step's
    let logs = ["check.log", "graphics.log"].map(|f| std::fs::read_to_string(e.ws.join("work").join(f)).unwrap_or_default());
    assert!(logs.iter().any(|l| l.contains("made for another GPU or driver")), "the runtime dropped it");

    // 5. Stop pressed during the graphics step: still playable, the step shown as not done, done by the next run
    println!("5. stopped during the graphics step");
    std::fs::remove_file(cache(&e).unwrap()).unwrap(); // so that it has work to do
    let ev = prepare(&e, Some("graphics"));
    assert_eq!(ev.last(), Some(&PrepEvent::Finished { exit_code: None, stopped: true }));
    let r = report(&e);
    assert_eq!((r.status.clone(), r.graphics.state.as_str()), (Status::Ready, "failed"), "{:?}", r.graphics);
    let ev = prepare(&e, None);
    assert!(finished_ok(&ev) && result(&ev).map(|r| r.0) == Some("ok".into()));

    // 6. the step cannot run (a package whose catalog is damaged): a warning, the preparation still DONE
    println!("6. damaged project catalog");
    let catalog = e.tools.root.join("games").join("wonder2").join("pipeline_catalog.bin");
    let good = std::fs::read(&catalog).unwrap();
    std::fs::write(&catalog, b"damaged").unwrap();
    let ev = prepare(&e, None);
    std::fs::write(&catalog, &good).unwrap();
    assert!(finished_ok(&ev));
    assert_eq!(result(&ev).map(|r| r.0), Some("skipped".into()));
    assert!(ev.iter().any(|x| matches!(x, PrepEvent::Warn { text } if text.starts_with("graphics preparation skipped"))));
    let r = report(&e);
    assert_eq!((r.status, r.graphics.state.as_str()), (Status::Ready, "skipped"));
    println!("   restored: the next check and update prepares again");
    let ev = prepare(&e, None);
    assert!(finished_ok(&ev) && result(&ev).map(|r| r.0) == Some("ok".into()));

    // saves and settings untouched by all of it
    assert_eq!(user_files(&e.ws), before, "saves, profile, settings and fonts unchanged");
    std::fs::remove_file(user.join("save").join("reflora_test_sentinel.sav")).unwrap();

    // 7. Play: the game started as the launcher starts it loads the prepared cache and the installed catalog
    if std::env::var("REFLORA_TEST_LAUNCH").is_ok_and(|v| v == "1") {
        println!("7. play");
        let log = e.ws.join("nx.log");
        let _ = std::fs::remove_file(&log);
        let game = reflora_lib::game::GameProcess::default();
        let pid = game.launch(&e.ws, Some("Tester"), false).unwrap();
        std::thread::sleep(std::time::Duration::from_secs(25));
        let _ = std::process::Command::new("taskkill").args(["/PID", &pid.to_string(), "/T", "/F"]).output();
        let text = std::fs::read_to_string(&log).unwrap_or_default();
        for l in text.lines().filter(|l| l.contains("pipeline cache") || l.contains("pipeline catalog")) {
            println!("   {l}");
        }
        assert!(text.contains("bytes loaded") && !text.contains("(0 bytes loaded)"), "the prepared cache loaded");
        assert!(text.contains(" compute) from 1 file(s)") && text.contains("mode background"), "the installed catalog");
        assert!(!text.contains("NX_SPIRV_SALT is set"));
    }
}
