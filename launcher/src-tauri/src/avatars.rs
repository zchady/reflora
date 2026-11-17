//! Profile icons: the user's own images, kept only on this PC (the launcher's local data folder, `avatars\`).
//!
//! An imported image is cut to its centre square and stored as a 256x256 JPEG of at most 128 KiB, the console's
//! profile picture format. The chosen one is copied to `<workspace>\data\user\profile.jpg`, which the runtime gives
//! the game (runtime/src/svc_account.cpp, nn::account::LoadProfileImage). The launcher never ships any icon.

use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use base64::Engine;
use image::imageops::FilterType;
use image::{codecs::jpeg::JpegEncoder, DynamicImage, ImageReader};
use serde::Serialize;

pub const SIDE: u32 = 256;
pub const MAX_BYTES: usize = 0x20000;
const MAX_SOURCE_BYTES: u64 = 40 * 1024 * 1024;

#[derive(Clone, Debug, Serialize)]
pub struct Avatar {
    pub id: String,
    /// `data:image/jpeg;base64,...` for the interface.
    pub data_url: String,
}

/// Ids are the launcher's own file names: `avatar-<digits>.jpg`. Anything else (a path, ..) is refused.
pub fn valid_id(id: &str) -> bool {
    id.strip_prefix("avatar-")
        .and_then(|r| r.strip_suffix(".jpg"))
        .is_some_and(|d| !d.is_empty() && d.chars().all(|c| c.is_ascii_digit()))
}

/// The centre square, 256x256, as a JPEG within the size limit.
pub fn to_profile_jpeg(img: &DynamicImage) -> Result<Vec<u8>, String> {
    let (w, h) = (img.width(), img.height());
    if w < 16 || h < 16 {
        return Err("too_small".into());
    }
    let side = w.min(h);
    let square = img.crop_imm((w - side) / 2, (h - side) / 2, side, side);
    // a transparent icon is laid on white, as the console's pictures are opaque
    let rgba = square.resize_exact(SIDE, SIDE, FilterType::Lanczos3).to_rgba8();
    let mut rgb = image::RgbImage::new(SIDE, SIDE);
    for (x, y, p) in rgba.enumerate_pixels() {
        let a = p[3] as u32;
        let mix = |c: u8| ((c as u32 * a + 255 * (255 - a)) / 255) as u8;
        rgb.put_pixel(x, y, image::Rgb([mix(p[0]), mix(p[1]), mix(p[2])]));
    }
    for quality in [92u8, 85, 75, 65, 55] {
        let mut out = Vec::new();
        JpegEncoder::new_with_quality(&mut out, quality).encode_image(&rgb).map_err(|e| e.to_string())?;
        if out.len() <= MAX_BYTES {
            return Ok(out);
        }
    }
    Err("too_big".into())
}

pub struct Library {
    dir: PathBuf,
}

impl Library {
    pub fn new(dir: PathBuf) -> Self {
        Self { dir }
    }

    pub fn path_of(&self, id: &str) -> Option<PathBuf> {
        valid_id(id).then(|| self.dir.join(id)).filter(|p| p.is_file())
    }

    pub fn list(&self) -> Vec<Avatar> {
        let mut ids: Vec<String> = std::fs::read_dir(&self.dir)
            .map(|rd| rd.filter_map(Result::ok).map(|e| e.file_name().to_string_lossy().into_owned()).collect())
            .unwrap_or_default();
        ids.retain(|id| valid_id(id));
        ids.sort();
        ids.into_iter()
            .filter_map(|id| {
                let bytes = std::fs::read(self.dir.join(&id)).ok()?;
                let data_url = format!("data:image/jpeg;base64,{}", base64::engine::general_purpose::STANDARD.encode(bytes));
                Some(Avatar { id, data_url })
            })
            .collect()
    }

    /// Converts the picked image into the library. Returns the new id.
    pub fn import(&self, source: &Path) -> Result<String, String> {
        let meta = std::fs::metadata(source).map_err(|_| "unreadable".to_string())?;
        if meta.len() > MAX_SOURCE_BYTES {
            return Err("too_big".into());
        }
        let img = ImageReader::open(source)
            .and_then(|r| r.with_guessed_format())
            .map_err(|_| "unreadable".to_string())?
            .decode()
            .map_err(|_| "unsupported".to_string())?;
        let jpeg = to_profile_jpeg(&img)?;
        std::fs::create_dir_all(&self.dir).map_err(|e| e.to_string())?;
        let mut stamp = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis()).unwrap_or(0);
        let id = loop {
            let id = format!("avatar-{stamp}.jpg");
            if !self.dir.join(&id).exists() {
                break id;
            }
            stamp += 1;
        };
        let tmp = self.dir.join(format!("{id}.tmp"));
        std::fs::write(&tmp, &jpeg).map_err(|e| e.to_string())?;
        std::fs::rename(&tmp, self.dir.join(&id)).map_err(|e| e.to_string())?;
        Ok(id)
    }

    pub fn remove(&self, id: &str) -> Result<(), String> {
        match self.path_of(id) {
            Some(p) => std::fs::remove_file(p).map_err(|e| e.to_string()),
            None => Err("unknown".into()),
        }
    }
}

/// Puts the chosen icon (or none) where the game reads it. Only when the installation has its data folder.
pub fn apply_to_workspace(workspace: &Path, chosen: Option<&Path>) -> std::io::Result<bool> {
    let data = workspace.join("data");
    if !data.is_dir() {
        return Ok(false);
    }
    let target = data.join("user").join("profile.jpg");
    match chosen {
        Some(src) => {
            std::fs::create_dir_all(target.parent().unwrap())?;
            let tmp = target.with_extension("jpg.tmp");
            std::fs::copy(src, &tmp)?;
            std::fs::rename(&tmp, &target)?;
        }
        None => {
            if target.exists() {
                std::fs::remove_file(&target)?;
            }
        }
    }
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("reflora-test-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn ids() {
        assert!(valid_id("avatar-1794448794953.jpg"));
        for bad in ["avatar-.jpg", "../avatar-1.jpg", "avatar-1.png", "avatar-1a.jpg", "x.jpg", "avatar-1.jpg\\..\\"] {
            assert!(!valid_id(bad), "{bad}");
        }
    }

    #[test]
    fn import_makes_a_square_jpeg_within_the_limit() {
        let d = temp("avatars");
        // a 600x300 noisy PNG with transparency: the worst case for the size limit
        let mut src = image::RgbaImage::new(600, 300);
        let mut seed = 12345u32;
        for p in src.pixels_mut() {
            seed = seed.wrapping_mul(1103515245).wrapping_add(12345);
            *p = image::Rgba([(seed >> 8) as u8, (seed >> 16) as u8, (seed >> 24) as u8, (seed >> 4) as u8]);
        }
        let png = d.join("wide.png");
        src.save(&png).unwrap();
        let lib = Library::new(d.join("lib"));
        let id = lib.import(&png).unwrap();
        assert!(valid_id(&id));
        let bytes = std::fs::read(lib.path_of(&id).unwrap()).unwrap();
        assert!(bytes.len() <= MAX_BYTES && bytes.starts_with(&[0xFF, 0xD8]));
        let back = image::load_from_memory(&bytes).unwrap();
        assert_eq!((back.width(), back.height()), (SIDE, SIDE));
        assert_eq!(lib.list().len(), 1);
        assert!(lib.list()[0].data_url.starts_with("data:image/jpeg;base64,/9j/"));

        std::fs::write(d.join("not.png"), b"hello").unwrap();
        assert_eq!(lib.import(&d.join("not.png")), Err("unsupported".into()));

        let ws = d.join("ws");
        assert!(!apply_to_workspace(&ws, lib.path_of(&id).as_deref()).unwrap());
        std::fs::create_dir_all(ws.join("data")).unwrap();
        assert!(apply_to_workspace(&ws, lib.path_of(&id).as_deref()).unwrap());
        assert_eq!(std::fs::read(ws.join("data/user/profile.jpg")).unwrap(), bytes);
        assert!(apply_to_workspace(&ws, None).unwrap());
        assert!(!ws.join("data/user/profile.jpg").exists());

        lib.remove(&id).unwrap();
        assert!(lib.list().is_empty());
    }
}
