//! What the launcher needs from Windows before its window can exist, checked first, with a message the user can act
//! on instead of a launcher that silently does not open.
//!
//! The Microsoft Edge WebView2 Runtime draws the interface (Tauri 2). Windows 11 has it, and Windows 10 receives it
//! with Edge updates; a PC without it gets a message naming it and where to get it. Detection: Microsoft's documented
//! registry keys ("Detect if a WebView2 Runtime is already installed": EdgeUpdate\Clients\{F3017226-…}, value "pv",
//! not empty and not 0.0.0.0, per machine or per user).

/// The installed WebView2 Runtime's version, or None.
pub fn webview2_version() -> Option<String> {
    if std::env::var_os("REFLORA_TEST_NO_WEBVIEW2").is_some() {
        return None; // development: shows the message on a PC that has the runtime
    }
    #[cfg(windows)]
    {
        win::webview2_version()
    }
    #[cfg(not(windows))]
    {
        Some(String::new())
    }
}

/// The message for a missing requirement, in the system's language (Portuguese, Spanish, else English).
pub fn webview2_message() -> (String, String) {
    let link = "https://developer.microsoft.com/microsoft-edge/webview2/";
    match ui_language() {
        "pt" => (
            "REFLORA: falta um componente do Windows".into(),
            format!(
                "O REFLORA precisa do Microsoft Edge WebView2 Runtime para abrir a janela, e ele não foi encontrado neste PC.\n\n\
                 Ele vem com o Windows 11 e com as atualizações do Windows 10. Para instalar, baixe o \"Evergreen \
                 Bootstrapper\" gratuito da Microsoft em:\n\n{link}\n\nDepois de instalar, abra o REFLORA de novo."
            ),
        ),
        "es" => (
            "REFLORA: falta un componente de Windows".into(),
            format!(
                "REFLORA necesita Microsoft Edge WebView2 Runtime para abrir la ventana, y no se encontró en este PC.\n\n\
                 Viene con Windows 11 y con las actualizaciones de Windows 10. Para instalarlo, descarga el \"Evergreen \
                 Bootstrapper\" gratuito de Microsoft en:\n\n{link}\n\nDespués de instalarlo, abre REFLORA de nuevo."
            ),
        ),
        _ => (
            "REFLORA: a Windows component is missing".into(),
            format!(
                "REFLORA needs the Microsoft Edge WebView2 Runtime to open its window, and it was not found on this PC.\n\n\
                 It comes with Windows 11 and with Windows 10 updates. To install it, download Microsoft's free \
                 \"Evergreen Bootstrapper\" from:\n\n{link}\n\nThen open REFLORA again."
            ),
        ),
    }
}

/// The message for a launcher that could not start for another reason.
pub fn start_failed_message(error: &str) -> (String, String) {
    match ui_language() {
        "pt" => ("REFLORA não pôde abrir".into(), format!("O REFLORA não conseguiu abrir a janela:\n\n{error}")),
        "es" => ("REFLORA no pudo abrirse".into(), format!("REFLORA no pudo abrir la ventana:\n\n{error}")),
        _ => ("REFLORA could not start".into(), format!("REFLORA could not open its window:\n\n{error}")),
    }
}

fn ui_language() -> &'static str {
    #[cfg(windows)]
    {
        // the primary language of the user's interface language (LANGID & 0x3ff)
        match unsafe { windows_sys::Win32::Globalization::GetUserDefaultUILanguage() } & 0x3ff {
            0x16 => "pt",
            0x0a => "es",
            _ => "en",
        }
    }
    #[cfg(not(windows))]
    {
        "en"
    }
}

/// A message box (no window of ours exists yet).
pub fn show_error(title: &str, text: &str) {
    #[cfg(windows)]
    win::message_box(title, text);
    #[cfg(not(windows))]
    eprintln!("{title}\n{text}");
}

#[cfg(windows)]
mod win {
    use windows_sys::Win32::System::Registry::{
        RegGetValueW, HKEY, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, RRF_RT_REG_SZ,
    };
    use windows_sys::Win32::UI::WindowsAndMessaging::{MessageBoxW, MB_ICONERROR, MB_OK};

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(std::iter::once(0)).collect()
    }

    fn read_pv(root: HKEY, key: &str) -> Option<String> {
        let (key, value) = (wide(key), wide("pv"));
        let mut buf = [0u16; 64];
        let mut size = (buf.len() * 2) as u32;
        // SAFETY: the buffers outlive the call; size is in bytes.
        let r = unsafe {
            RegGetValueW(root, key.as_ptr(), value.as_ptr(), RRF_RT_REG_SZ, std::ptr::null_mut(),
                         buf.as_mut_ptr().cast(), &mut size)
        };
        if r != 0 {
            return None;
        }
        let n = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
        let v = String::from_utf16_lossy(&buf[..n]);
        (!v.is_empty() && v != "0.0.0.0").then_some(v)
    }

    pub fn webview2_version() -> Option<String> {
        const ID: &str = "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
        [
            (HKEY_LOCAL_MACHINE, format!("SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{ID}")),
            (HKEY_LOCAL_MACHINE, format!("SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{ID}")),
            (HKEY_CURRENT_USER, format!("Software\\Microsoft\\EdgeUpdate\\Clients\\{ID}")),
        ]
        .iter()
        .find_map(|(root, key)| read_pv(*root, key))
    }

    pub fn message_box(title: &str, text: &str) {
        let (t, m) = (wide(title), wide(text));
        // SAFETY: nul-terminated UTF-16 strings that live through the call; no owner window.
        unsafe {
            MessageBoxW(std::ptr::null_mut(), m.as_ptr(), t.as_ptr(), MB_OK | MB_ICONERROR);
        }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    #[cfg(windows)]
    fn this_pc_has_webview2() {
        // the development PC runs the launcher, so the runtime is there and the documented keys find it
        let v = super::webview2_version().expect("WebView2 Runtime version");
        assert!(v.split('.').count() == 4, "{v}");
    }

    #[test]
    fn messages_name_the_runtime_and_where_to_get_it() {
        let (_, text) = super::webview2_message();
        assert!(text.contains("WebView2") && text.contains("https://developer.microsoft.com/microsoft-edge/webview2/"));
    }
}
