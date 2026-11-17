//! The controller button icons: the console's Nintendo extension font (shared font type 5), as the user dumped it
//! from their own console (`nintendo_ext_003.bfttf`, or the same font decoded to `.ttf`).
//!
//! Wonder draws every button icon in its texts with it. The font comes from the console's firmware, so REFLORA never
//! ships it: the user imports their own file, the launcher keeps it in its local data folder (`fonts\`) and copies
//! it to `<workspace>\data\fonts\`, where the runtime looks for it (runtime/src/hle_pl.cpp). The file is checked the
//! way the runtime reads it (decode_bfttf there), so a file the game could not use is refused at import.

use std::path::{Path, PathBuf};

use serde::Serialize;

pub const NAME: &str = "nintendo_ext_003";
const MAX_BYTES: u64 = 32 * 1024 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Kind {
    /// The system archive's encoded file.
    Bfttf,
    /// The same font, decoded.
    Ttf,
}

impl Kind {
    fn ext(self) -> &'static str {
        match self {
            Kind::Bfttf => "bfttf",
            Kind::Ttf => "ttf",
        }
    }
}

fn ttf_header(p: &[u8]) -> bool {
    p.len() >= 4 && (p[..4] == [0, 1, 0, 0] || &p[..4] == b"OTTO" || &p[..4] == b"true" || &p[..4] == b"ttcf")
}

/// Whether `data` is a font the runtime can use, and which kind (same rules as hle_pl.cpp decode_bfttf).
pub fn classify(data: &[u8]) -> Option<Kind> {
    if ttf_header(data) {
        return Some(Kind::Ttf);
    }
    if data.len() < 16 || data.len() % 4 != 0 {
        return None;
    }
    let word = |i: usize| u32::from_le_bytes(data[4 * i..4 * i + 4].try_into().unwrap());
    for swapped in [false, true] {
        let magic: u32 = if swapped { 0x18029A7Fu32.swap_bytes() } else { 0x18029A7F };
        let key = word(0) ^ magic;
        let dec = |w: u32| if swapped { (w ^ key).swap_bytes() } else { w ^ key };
        let mut size = dec(word(1)) as usize;
        if size == 0 || size > data.len() - 8 {
            size = (size as u32).swap_bytes() as usize;
        }
        if size == 0 || size > data.len() - 8 {
            continue;
        }
        if ttf_header(&dec(word(2)).to_le_bytes()) {
            return Some(Kind::Bfttf);
        }
    }
    None
}

#[derive(Clone, Debug, Serialize)]
pub struct Status {
    pub installed: bool,
    pub kind: Option<Kind>,
    pub size: u64,
}

pub struct Store {
    dir: PathBuf,
}

impl Store {
    pub fn new(dir: PathBuf) -> Self {
        Self { dir }
    }

    fn current(&self) -> Option<(PathBuf, Kind)> {
        [Kind::Bfttf, Kind::Ttf]
            .into_iter()
            .map(|k| (self.dir.join(format!("{NAME}.{}", k.ext())), k))
            .find(|(p, _)| p.is_file())
    }

    pub fn status(&self) -> Status {
        match self.current() {
            Some((p, kind)) => Status { installed: true, kind: Some(kind), size: p.metadata().map(|m| m.len()).unwrap_or(0) },
            None => Status { installed: false, kind: None, size: 0 },
        }
    }

    pub fn import(&self, source: &Path) -> Result<Kind, String> {
        let len = std::fs::metadata(source).map_err(|_| "unreadable".to_string())?.len();
        if len > MAX_BYTES {
            return Err("not_font".into());
        }
        let data = std::fs::read(source).map_err(|_| "unreadable".to_string())?;
        let kind = classify(&data).ok_or("not_font")?;
        std::fs::create_dir_all(&self.dir).map_err(|e| e.to_string())?;
        self.remove_files();
        let target = self.dir.join(format!("{NAME}.{}", kind.ext()));
        let tmp = target.with_extension("tmp");
        std::fs::write(&tmp, &data).map_err(|e| e.to_string())?;
        std::fs::rename(&tmp, &target).map_err(|e| e.to_string())?;
        Ok(kind)
    }

    fn remove_files(&self) {
        for k in [Kind::Bfttf, Kind::Ttf] {
            let _ = std::fs::remove_file(self.dir.join(format!("{NAME}.{}", k.ext())));
        }
    }

    pub fn remove(&self) {
        self.remove_files();
    }

    /// Puts the imported font (or none) where the runtime reads it. Only when the installation has its data folder.
    pub fn apply_to_workspace(&self, workspace: &Path) -> std::io::Result<bool> {
        let data = workspace.join("data");
        if !data.is_dir() {
            return Ok(false);
        }
        let fonts = data.join("fonts");
        for k in [Kind::Bfttf, Kind::Ttf] {
            let p = fonts.join(format!("{NAME}.{}", k.ext()));
            if p.exists() {
                std::fs::remove_file(p)?;
            }
        }
        if let Some((src, kind)) = self.current() {
            std::fs::create_dir_all(&fonts)?;
            std::fs::copy(src, fonts.join(format!("{NAME}.{}", kind.ext())))?;
        }
        Ok(true)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn encoded(ttf: &[u8], key: u32, big_endian_size: bool) -> Vec<u8> {
        let mut words = vec![0x18029A7Fu32, if big_endian_size { (ttf.len() as u32).swap_bytes() } else { ttf.len() as u32 }];
        for chunk in ttf.chunks(4) {
            let mut b = [0u8; 4];
            b[..chunk.len()].copy_from_slice(chunk);
            words.push(u32::from_le_bytes(b));
        }
        words.iter().flat_map(|w| (w ^ key).to_le_bytes()).collect()
    }

    #[test]
    fn classify_fonts() {
        let ttf: Vec<u8> = [0u8, 1, 0, 0].iter().copied().chain(std::iter::repeat(7).take(60)).collect();
        assert_eq!(classify(&ttf), Some(Kind::Ttf));
        assert_eq!(classify(&encoded(&ttf, 0x49621806, false)), Some(Kind::Bfttf));
        assert_eq!(classify(&encoded(&ttf, 0x1234_5678, true)), Some(Kind::Bfttf)); // the console's size word order
        assert_eq!(classify(b"PNG not a font at all..........."), None);
        assert_eq!(classify(&[1, 2, 3]), None);
    }

    /// The user's real dump (manual): REFLORA_TEST_FONT=<nintendo_ext_003.bfttf>  cargo test -- --ignored real_font
    #[test]
    #[ignore]
    fn real_font() {
        let data = std::fs::read(std::env::var("REFLORA_TEST_FONT").unwrap()).unwrap();
        assert_eq!(classify(&data), Some(Kind::Bfttf));
    }

    #[test]
    fn import_and_apply() {
        let d = std::env::temp_dir().join(format!("reflora-test-font-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join("ws").join("data")).unwrap();
        let ttf: Vec<u8> = [0u8, 1, 0, 0].iter().copied().chain(std::iter::repeat(9).take(60)).collect();
        let src = d.join("dump.bfttf");
        std::fs::write(&src, encoded(&ttf, 0xA5A5_5A5A, true)).unwrap();
        std::fs::write(d.join("bad.bfttf"), b"0123456789abcdef0123").unwrap();
        let store = Store::new(d.join("store"));
        assert!(!store.status().installed);
        assert_eq!(store.import(&d.join("bad.bfttf")), Err("not_font".into()));
        assert_eq!(store.import(&src), Ok(Kind::Bfttf));
        assert!(store.status().installed);
        assert!(store.apply_to_workspace(&d.join("ws")).unwrap());
        assert!(d.join("ws/data/fonts/nintendo_ext_003.bfttf").is_file());
        store.remove();
        store.apply_to_workspace(&d.join("ws")).unwrap();
        assert!(!d.join("ws/data/fonts/nintendo_ext_003.bfttf").exists());
        assert!(!store.apply_to_workspace(&d.join("nowhere")).unwrap());
    }
}
