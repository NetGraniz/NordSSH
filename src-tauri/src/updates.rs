use crate::{AppState, idle, storage};
use base64::{Engine, engine::general_purpose::STANDARD};
#[cfg(windows)]
use serde::Deserialize;
use serde::Serialize;
use std::{
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    time::Duration,
};
use tauri::{AppHandle, State, ipc::Channel};
use tauri_plugin_updater::{Update, UpdaterExt};

const MANIFEST: &str = "https://github.com/NetGraniz/NordSSH/releases/latest/download/latest.json";
const PUBLIC_KEY: &str = "dW50cnVzdGVkIGNvbW1lbnQ6IG1pbmlzaWduIHB1YmxpYyBrZXk6IENFNjRBNkQzN0UwOTgwMEEKUldRS2dBbCswNlprenA4RzE4dzFVVHdWb0diNTVNTHJxUzI4dHNyVHlMT3IyTXNxSnFTMlRZMnQK";
const MAX_DOWNLOAD: usize = 256 * 1024 * 1024;

#[derive(Serialize, Clone)]
pub(crate) struct Progress {
    phase: &'static str,
    received: u64,
    total: Option<u64>,
}
#[derive(Serialize)]
pub(crate) struct UpdateInfo {
    status: &'static str,
    version: String,
}

fn client() -> Result<reqwest::Client, String> {
    reqwest::Client::builder()
        .user_agent("NordSSH")
        .connect_timeout(Duration::from_secs(10))
        .timeout(Duration::from_secs(180))
        .redirect(reqwest::redirect::Policy::custom(|attempt| {
            if attempt.url().scheme() != "https" || attempt.previous().len() >= 5 {
                attempt.stop()
            } else {
                attempt.follow()
            }
        }))
        .build()
        .map_err(|_| "Cannot initialize secure update client".into())
}
async fn bounded_bytes(
    mut response: reqwest::Response,
    limit: usize,
    output: Option<&Channel<Progress>>,
) -> Result<Vec<u8>, String> {
    if !response.status().is_success() {
        return Err("GitHub rejected the download; nothing was installed".into());
    }
    let total = response.content_length();
    if total.is_some_and(|n| n > limit as u64) {
        return Err("Update exceeds size limit".into());
    }
    let mut bytes = Vec::new();
    let mut last = std::time::Instant::now();
    while let Some(chunk) = response
        .chunk()
        .await
        .map_err(|_| "Download interrupted; nothing was installed")?
    {
        if chunk.len() > limit - bytes.len() {
            return Err("Update exceeds size limit".into());
        }
        bytes.extend_from_slice(&chunk);
        if let Some(channel) = output
            && last.elapsed() >= Duration::from_millis(200)
        {
            let _ = channel.send(Progress {
                phase: "download",
                received: bytes.len() as u64,
                total,
            });
            last = std::time::Instant::now();
        }
    }
    Ok(bytes)
}
fn trusted_download(url: &reqwest::Url) -> bool {
    url.scheme() == "https"
        && url.host_str() == Some("github.com")
        && url.port_or_known_default() == Some(443)
        && url.username().is_empty()
        && url.password().is_none()
        && url
            .path()
            .starts_with("/NetGraniz/NordSSH/releases/download/")
}
async fn available(app: &AppHandle, portable: bool) -> Result<Option<Update>, String> {
    let response = client()?
        .get(MANIFEST)
        .timeout(Duration::from_secs(15))
        .send()
        .await
        .map_err(|_| "GitHub is unavailable; SSH is unaffected")?;
    if response.status() == reqwest::StatusCode::NOT_FOUND {
        return Ok(None);
    }
    let bytes = bounded_bytes(response, 256 * 1024, None).await?;
    let json = serde_json::from_slice(&bytes).map_err(|_| "Invalid update manifest")?;
    let builder = app.updater_builder();
    #[cfg(windows)]
    let builder = if portable {
        builder.target("windows-x86_64-portable")
    } else {
        builder
    };
    #[cfg(not(windows))]
    let _ = portable;
    let update = builder
        .build()
        .map_err(|_| "Updater configuration error")?
        .restore_update(json)
        .map_err(|_| "Invalid update metadata or unsupported package")?;
    if let Some(u) = &update
        && (!trusted_download(&u.download_url)
            || !semver::Version::parse(&u.version)
                .map_err(|_| "Invalid update version")?
                .pre
                .is_empty())
    {
        return Err("Untrusted update source or non-stable version".into());
    }
    Ok(update)
}
#[tauri::command]
pub(crate) async fn check_updates(
    app: AppHandle,
    state: State<'_, AppState>,
) -> Result<UpdateInfo, String> {
    match available(&app, state.portable).await? {
        Some(u) => Ok(UpdateInfo {
            status: "available",
            version: u.version,
        }),
        None => Ok(UpdateInfo {
            status: "current",
            version: env!("CARGO_PKG_VERSION").into(),
        }),
    }
}
struct Maintenance(Arc<AtomicBool>);
impl Drop for Maintenance {
    fn drop(&mut self) {
        self.0.store(false, Ordering::SeqCst);
    }
}

#[tauri::command]
pub(crate) async fn install_update(
    app: AppHandle,
    state: State<'_, AppState>,
    output: Channel<Progress>,
) -> Result<&'static str, String> {
    state
        .maintenance
        .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
        .map_err(|_| "An update is already running")?;
    let _guard = Maintenance(state.maintenance.clone());
    idle(&state)?;
    state
        .store
        .lock()
        .map_err(|_| "Vault lock unavailable")?
        .require_unlocked()?;
    let Some(update) = available(&app, state.portable).await? else {
        return Ok("current");
    };
    let confirm=rfd::AsyncMessageDialog::new().set_title("Обновить NordSSH?")
        .set_description(format!("Установить версию {} из GitHub? Подпись будет проверена до установки. Программа перезапустится; хранилище и настройки сохранятся.",update.version))
        .set_buttons(rfd::MessageButtons::YesNo).show().await;
    if confirm != rfd::MessageDialogResult::Yes {
        return Ok("cancelled");
    }
    let response = client()?
        .get(update.download_url.clone())
        .send()
        .await
        .map_err(|_| "Cannot download update")?;
    let bytes = bounded_bytes(response, MAX_DOWNLOAD, Some(&output)).await?;
    let _ = output.send(Progress {
        phase: "verify",
        received: bytes.len() as u64,
        total: Some(bytes.len() as u64),
    });
    update
        .verify(&bytes)
        .map_err(|_| "Update signature or signed version is invalid; nothing was installed")?;
    verify_artifact(&bytes, &update.signature, &update.version)?;
    idle(&state)?;
    {
        let s = state.store.lock().map_err(|_| "Vault lock unavailable")?;
        let encrypted = storage::read_limited(&s.path, crate::crypto::MAX_FILE)?;
        let backup = s
            .path
            .with_extension(format!("before-update-{}.bak", uuid::Uuid::new_v4()));
        storage::atomic_write(&backup, &encrypted)?;
    }
    let _ = output.send(Progress {
        phase: "install",
        received: bytes.len() as u64,
        total: Some(bytes.len() as u64),
    });
    #[cfg(windows)]
    if state.portable {
        launch_portable_helper(&bytes, &update.signature, &update.version)?;
        app.exit(0);
        return Ok("restarting");
    }
    tauri::async_runtime::spawn_blocking(move || update.install(bytes))
        .await
        .map_err(|_| "Installation task failed")?
        .map_err(|_| "Installation failed; vault data was preserved")?;
    app.restart();
}

// Shared by the portable helper and tests. The trusted version is read only AFTER
// minisign validates both the payload signature and its signed comment.
fn verify_artifact(bytes: &[u8], signature: &str, version: &str) -> Result<(), String> {
    let decode = |text: &str| -> Result<String, String> {
        String::from_utf8(
            STANDARD
                .decode(text)
                .map_err(|_| "Invalid signature encoding")?,
        )
        .map_err(|_| "Invalid signature text".into())
    };
    let key = minisign_verify::PublicKey::decode(&decode(PUBLIC_KEY)?)
        .map_err(|_| "Invalid embedded public key")?;
    let sig = minisign_verify::Signature::decode(&decode(signature)?)
        .map_err(|_| "Invalid update signature")?;
    key.verify(bytes, &sig, true)
        .map_err(|_| "Update signature rejected")?;
    let signed = sig
        .trusted_comment()
        .split('\t')
        .find_map(|field| field.strip_prefix("version:"))
        .ok_or("Missing signed version")?;
    if semver::Version::parse(signed).map_err(|_| "Invalid signed version")?
        != semver::Version::parse(version).map_err(|_| "Invalid announced version")?
    {
        return Err("Signed version does not match update version".into());
    }
    Ok(())
}

#[cfg(windows)]
#[derive(Serialize, Deserialize)]
struct PortableRequest {
    target: std::path::PathBuf,
    signature: String,
    version: String,
    parent_pid: u32,
}

#[cfg(windows)]
fn launch_portable_helper(bytes: &[u8], signature: &str, version: &str) -> Result<(), String> {
    let executable = std::env::current_exe().map_err(|_| "Cannot locate portable executable")?;
    let dir = tempfile::Builder::new()
        .prefix("NordSSH-update-")
        .tempdir()
        .map_err(|_| "Cannot stage portable update")?;
    std::fs::write(dir.path().join("payload.exe"), bytes)
        .map_err(|_| "Cannot stage update executable")?;
    let request = PortableRequest {
        target: executable.clone(),
        signature: signature.into(),
        version: version.into(),
        parent_pid: std::process::id(),
    };
    std::fs::write(
        dir.path().join("request.json"),
        serde_json::to_vec(&request).map_err(|_| "Cannot encode update request")?,
    )
    .map_err(|_| "Cannot stage update request")?;
    let helper = dir.path().join("NordSSH-update-helper.exe");
    std::fs::copy(&executable, &helper).map_err(|_| "Cannot stage updater helper")?;
    use std::os::windows::process::CommandExt;
    std::process::Command::new(helper)
        .arg("--apply-portable-update")
        .creation_flags(0x08000000)
        .spawn()
        .map_err(|_| "Cannot start portable updater")?;
    let _ = dir.keep();
    Ok(())
}

#[cfg(windows)]
fn apply_portable(dir: &std::path::Path, restart: bool) -> Result<(), String> {
    let request: PortableRequest = serde_json::from_slice(&storage::read_limited(
        &dir.join("request.json"),
        64 * 1024,
    )?)
    .map_err(|_| "Invalid update request")?;
    let bytes = storage::read_limited(&dir.join("payload.exe"), MAX_DOWNLOAD as u64)?;
    verify_artifact(&bytes, &request.signature, &request.version)?;
    if semver::Version::parse(&request.version).map_err(|_| "Invalid version")?
        <= semver::Version::parse(env!("CARGO_PKG_VERSION"))
            .map_err(|_| "Invalid current version")?
    {
        return Err("Update must be newer than current version".into());
    }
    if !bytes.starts_with(b"MZ") {
        return Err("Update is not a Windows executable".into());
    }
    let target =
        std::fs::canonicalize(&request.target).map_err(|_| "Cannot locate current executable")?;
    let parent = target.parent().ok_or("Invalid portable folder")?;
    if !target
        .file_name()
        .is_some_and(|n| n.to_string_lossy().eq_ignore_ascii_case("nordssh.exe"))
        || !parent.join("nordssh.portable").is_file()
    {
        return Err("Target is not a portable NordSSH installation".into());
    }
    if request.parent_pid != 0 {
        use windows_sys::Win32::{
            Foundation::CloseHandle,
            System::Threading::{OpenProcess, PROCESS_SYNCHRONIZE, WaitForSingleObject},
        };
        unsafe {
            let handle = OpenProcess(PROCESS_SYNCHRONIZE, 0, request.parent_pid);
            if !handle.is_null() {
                let result = WaitForSingleObject(handle, 60_000);
                CloseHandle(handle);
                if result != 0 {
                    return Err("Current application did not exit; update cancelled".into());
                }
            }
        }
    }
    let mut replacement = tempfile::Builder::new()
        .prefix("NordSSH-new-")
        .suffix(".exe")
        .tempfile_in(parent)
        .map_err(|_| "Cannot prepare replacement beside application")?;
    use std::io::Write;
    replacement
        .write_all(&bytes)
        .map_err(|_| "Cannot write replacement")?;
    replacement
        .as_file()
        .sync_all()
        .map_err(|_| "Cannot flush replacement")?;
    let backup = parent.join(format!("NordSSH-previous-{}.exe", uuid::Uuid::new_v4()));
    std::fs::rename(&target, &backup)
        .map_err(|_| "Cannot back up old executable; nothing replaced")?;
    if replacement.persist(&target).is_err() {
        let _ = std::fs::rename(&backup, &target);
        return Err("Cannot replace executable; previous version restored".into());
    }
    if restart {
        std::process::Command::new(&target)
            .arg("--portable")
            .current_dir(parent)
            .spawn()
            .map_err(|_| "Updated successfully; start NordSSH manually")?;
    }
    Ok(())
}

pub(crate) fn helper_mode() -> bool {
    if !std::env::args().any(|a| a == "--apply-portable-update") {
        return false;
    }
    #[cfg(windows)]
    {
        let result = std::env::current_exe()
            .map_err(|_| "Cannot locate updater".to_string())
            .and_then(|exe| {
                let dir = exe.parent().ok_or("Invalid updater folder")?;
                let result = apply_portable(dir, true);
                if let Err(error) = &result {
                    let _ = std::fs::write(dir.join("error.txt"), error);
                }
                result
            });
        if let Err(error) = result {
            rfd::MessageDialog::new()
                .set_title("Обновление NordSSH не завершено")
                .set_description(error)
                .set_level(rfd::MessageLevel::Error)
                .show();
        }
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn only_owned_https_release_assets_are_accepted() {
        for url in [
            "http://github.com/NetGraniz/NordSSH/releases/download/v1/a.exe",
            "https://github.com/other/project/releases/download/a.exe",
            "https://github.com.evil.invalid/NetGraniz/NordSSH/releases/download/a.exe",
            "https://github.com:444/NetGraniz/NordSSH/releases/download/a.exe",
        ] {
            assert!(!trusted_download(&url.parse().unwrap()));
        }
        assert!(trusted_download(
            &"https://github.com/NetGraniz/NordSSH/releases/download/v1/a.exe"
                .parse()
                .unwrap()
        ));
    }
    #[test]
    fn signed_artifact_rejects_tamper_and_wrong_version() {
        let bytes = include_bytes!("../../test/fixtures/update-test.txt");
        let signature = include_str!("../../test/fixtures/update-test.txt.sig").trim();
        verify_artifact(bytes, signature, "0.1.0").unwrap();
        assert!(verify_artifact(b"tampered", signature, "0.1.0").is_err());
        assert!(verify_artifact(bytes, signature, "9.9.9").is_err());
        assert!(verify_artifact(bytes, "invalid", "0.1.0").is_err());
    }
}
