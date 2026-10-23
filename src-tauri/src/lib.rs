mod crypto;
mod device;
mod model;
mod ssh;
mod storage;

use model::*;
use serde::Serialize;
use ssh::{Input, Session, TerminalEvent};
use std::{
    collections::HashMap,
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
    },
};
use tauri::{Manager, State, ipc::Channel};

pub struct AppState {
    store: Arc<Mutex<storage::Store>>,
    sessions: Arc<Mutex<HashMap<String, Arc<Session>>>>,
    pending: Arc<AtomicUsize>,
    transfers: AtomicUsize,
    portable: bool,
}
#[derive(Serialize)]
struct Snapshot {
    hosts: Vec<Host>,
    settings: Settings,
    snippets: Vec<Snippet>,
    version: String,
}
#[tauri::command]
fn vault_status(state: State<AppState>) -> Result<storage::Status, String> {
    Ok(state
        .store
        .lock()
        .map_err(|_| "Vault lock unavailable")?
        .status(state.portable))
}
#[tauri::command]
async fn vault_unlock(
    state: State<'_, AppState>,
    password: String,
    remember: bool,
    create: bool,
) -> Result<Option<String>, String> {
    let password = zeroize::Zeroizing::new(password);
    let store = state.store.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let mut s = store.lock().map_err(|_| "Vault lock unavailable")?;
        if create {
            s.initialize(&password, remember)
        } else {
            s.unlock(&password, remember)
        }
    })
    .await
    .map_err(|_| "Unlock task failed")?
}
fn idle(state: &AppState) -> Result<(), String> {
    if state.pending.load(Ordering::SeqCst) > 0
        || state.transfers.load(Ordering::SeqCst) > 0
        || !state
            .sessions
            .lock()
            .map_err(|_| "Session lock unavailable")?
            .is_empty()
    {
        Err("Close active SSH sessions and transfers first".into())
    } else {
        Ok(())
    }
}
#[tauri::command]
fn vault_lock(state: State<AppState>) -> Result<(), String> {
    let mut store = state.store.lock().map_err(|_| "Vault lock unavailable")?;
    idle(&state)?;
    store.lock();
    Ok(())
}
#[tauri::command]
fn vault_forget_device(state: State<AppState>) -> Result<(), String> {
    state
        .store
        .lock()
        .map_err(|_| "Vault lock unavailable")?
        .forget_device()
}
#[tauri::command]
async fn vault_export(state: State<'_, AppState>, password: String) -> Result<bool, String> {
    let password = zeroize::Zeroizing::new(password);
    let mut vault = {
        let s = state.store.lock().map_err(|_| "Vault lock unavailable")?;
        s.require_unlocked()?;
        s.vault.clone()
    };
    // Bundle selected SSH key material, not just machine-specific file paths.
    for host in &vault.hosts {
        if host.auth != "password" {
            let secret = vault.secrets.entry(host.id.clone()).or_default();
            if secret.private_key.is_empty() {
                secret.private_key = ssh::read_key_file(&host.key_path).await?;
            }
            if host.auth == "certificate" && secret.certificate.is_empty() {
                secret.certificate = ssh::read_key_file(&host.certificate_path).await?;
            }
        }
    }
    let Some(file) = rfd::AsyncFileDialog::new()
        .set_title("Экспорт зашифрованного хранилища")
        .set_file_name("NordSSH-export.nordvault")
        .add_filter("NordSSH vault", &["nordvault"])
        .save_file()
        .await
    else {
        return Ok(false);
    };
    let path = file.path().to_owned();
    let store = state.store.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let s = store.lock().map_err(|_| "Vault lock unavailable")?;
        s.export(&path, &password, &vault)
    })
    .await
    .map_err(|_| "Export task failed")??;
    Ok(true)
}
#[tauri::command]
async fn vault_import(
    state: State<'_, AppState>,
    password: String,
    remember: bool,
) -> Result<Option<String>, String> {
    let password = zeroize::Zeroizing::new(password);
    idle(&state)?;
    let Some(file) = rfd::AsyncFileDialog::new()
        .set_title("Импорт зашифрованного хранилища")
        .add_filter("NordSSH vault", &["nordvault"])
        .pick_file()
        .await
    else {
        return Err("Import cancelled".into());
    };
    let confirm=rfd::AsyncMessageDialog::new().set_title("Заменить текущее хранилище?").set_description("Импорт заменит список серверов, ключи и настройки. Текущий зашифрованный файл сохранится в резервной копии. Импортируйте только доверенный файл: он содержит также доверенные ключи серверов.").set_buttons(rfd::MessageButtons::YesNo).set_level(rfd::MessageLevel::Warning).show().await;
    if confirm != rfd::MessageDialogResult::Yes {
        return Err("Import cancelled".into());
    }
    let path = file.path().to_owned();
    let store = state.store.clone();
    let sessions = state.sessions.clone();
    let pending = state.pending.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let mut s = store.lock().map_err(|_| "Vault lock unavailable")?;
        if pending.load(Ordering::SeqCst) > 0
            || !sessions
                .lock()
                .map_err(|_| "Session lock unavailable")?
                .is_empty()
        {
            return Err("Close active sessions before importing".into());
        }
        s.import(&path, &password, remember)
    })
    .await
    .map_err(|_| "Import task failed")?
}
#[tauri::command]
fn snapshot(state: State<AppState>) -> Result<Snapshot, String> {
    let store = state.store.lock().map_err(|_| "Vault lock unavailable")?;
    store.require_unlocked()?;
    Ok(Snapshot {
        hosts: store.vault.hosts.clone(),
        settings: store.vault.settings.clone(),
        snippets: store.vault.snippets.clone(),
        version: env!("CARGO_PKG_VERSION").into(),
    })
}
#[tauri::command]
async fn save_host(
    state: State<'_, AppState>,
    host: Host,
    mut secret: Option<Secret>,
    forget_secret: bool,
) -> Result<(), String> {
    host.validate()?;
    if !forget_secret && host.auth != "password" {
        let mut credentials = secret.unwrap_or_else(|| {
            state
                .store
                .lock()
                .ok()
                .and_then(|s| s.vault.secrets.get(&host.id).cloned())
                .unwrap_or_default()
        });
        let saved_same = {
            let store = state.store.lock().map_err(|_| "Vault lock unavailable")?;
            store
                .vault
                .hosts
                .iter()
                .find(|h| h.id == host.id)
                .is_some_and(|h| h.same_login_as(&host))
        };
        if !saved_same || credentials.private_key.is_empty() {
            credentials.private_key = ssh::read_key_file(&host.key_path).await?;
        }
        if host.auth == "certificate" && (!saved_same || credentials.certificate.is_empty()) {
            credentials.certificate = ssh::read_key_file(&host.certificate_path).await?;
        }
        secret = Some(credentials);
    }
    if secret
        .as_ref()
        .is_some_and(|s| s.password.len() > 16384 || s.passphrase.len() > 16384)
    {
        return Err("Credential exceeds size limit".into());
    }
    state
        .store
        .lock()
        .map_err(|_| "Vault lock unavailable")?
        .update(|v| {
            let changed_identity = v
                .hosts
                .iter()
                .find(|h| h.id == host.id)
                .is_some_and(|h| !h.same_login_as(&host));
            if forget_secret || changed_identity {
                v.secrets.remove(&host.id);
            }
            if let Some(s) = secret {
                v.secrets.insert(host.id.clone(), s);
            }
            if let Some(existing) = v.hosts.iter_mut().find(|h| h.id == host.id) {
                *existing = host;
            } else {
                v.hosts.push(host);
            }
            Ok(())
        })
}
#[tauri::command]
fn delete_host(state: State<AppState>, id: String) -> Result<(), String> {
    state
        .store
        .lock()
        .map_err(|_| "Vault lock unavailable")?
        .update(|v| {
            v.hosts.retain(|h| h.id != id);
            v.secrets.remove(&id);
            Ok(())
        })
}
#[tauri::command]
fn save_settings(state: State<AppState>, settings: Settings) -> Result<(), String> {
    settings.validate()?;
    state
        .store
        .lock()
        .map_err(|_| "Vault lock unavailable")?
        .update(|v| {
            v.settings = settings;
            Ok(())
        })
}
#[tauri::command]
fn save_snippet(state: State<AppState>, snippet: Snippet) -> Result<(), String> {
    state
        .store
        .lock()
        .map_err(|_| "Vault lock unavailable")?
        .update(|v| {
            if let Some(s) = v.snippets.iter_mut().find(|s| s.id == snippet.id) {
                *s = snippet;
            } else {
                v.snippets.push(snippet);
            }
            Ok(())
        })
}
#[tauri::command]
fn delete_snippet(state: State<AppState>, id: String) -> Result<(), String> {
    state
        .store
        .lock()
        .map_err(|_| "Vault lock unavailable")?
        .update(|v| {
            v.snippets.retain(|s| s.id != id);
            Ok(())
        })
}
#[tauri::command]
async fn pick_key() -> Result<Option<String>, String> {
    Ok(rfd::AsyncFileDialog::new()
        .set_title("Выберите ключ или сертификат OpenSSH")
        .pick_file()
        .await
        .map(|f| f.path().to_string_lossy().into_owned()))
}
struct PendingGuard(Arc<AtomicUsize>);
impl Drop for PendingGuard {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::SeqCst);
    }
}
#[tauri::command]
async fn connect(
    state: State<'_, AppState>,
    host_id: String,
    id: String,
    secret: Option<Secret>,
    output: Channel<TerminalEvent>,
) -> Result<(), String> {
    if uuid::Uuid::parse_str(&id).is_err() {
        return Err("Invalid session identifier".into());
    }
    if secret
        .as_ref()
        .is_some_and(|s| s.password.len() > 16384 || s.passphrase.len() > 16384)
    {
        return Err("Credential exceeds size limit".into());
    }
    let pending = state.pending.fetch_add(1, Ordering::SeqCst);
    let _guard = PendingGuard(state.pending.clone());
    if pending >= 8 {
        return Err("Too many pending connections".into());
    }
    if state
        .sessions
        .lock()
        .map_err(|_| "Session lock unavailable")?
        .len()
        >= 24
    {
        return Err("Session limit reached".into());
    }
    let (host, secret) = {
        let s = state.store.lock().map_err(|_| "Vault lock unavailable")?;
        let host = s
            .vault
            .hosts
            .iter()
            .find(|h| h.id == host_id)
            .cloned()
            .ok_or("Host not found")?;
        s.require_unlocked()?;
        let mut credentials = s.vault.secrets.get(&host_id).cloned().unwrap_or_default();
        if let Some(input) = secret {
            credentials.password = input.password.clone();
            credentials.passphrase = input.passphrase.clone();
        }
        (host, credentials)
    };
    let (session, channel, input) = ssh::open(host, secret, state.store.clone()).await?;
    {
        let mut sessions = state
            .sessions
            .lock()
            .map_err(|_| "Session lock unavailable")?;
        if sessions.len() >= 24 || sessions.contains_key(&id) {
            return Err("Session limit reached or identifier already in use".into());
        }
        sessions.insert(id.clone(), session.clone());
    }
    let sessions = state.sessions.clone();
    tauri::async_runtime::spawn(async move {
        ssh::pump(id.clone(), session, channel, input, output).await;
        if let Ok(mut s) = sessions.lock() {
            s.remove(&id);
        }
    });
    Ok(())
}
fn session(state: &AppState, id: &str) -> Result<Arc<Session>, String> {
    state
        .sessions
        .lock()
        .map_err(|_| "Session lock unavailable")?
        .get(id)
        .cloned()
        .ok_or("Session is not connected".into())
}
#[tauri::command]
fn terminal_input(state: State<AppState>, id: String, data: Vec<u8>) -> Result<(), String> {
    if data.len() > 65536 {
        return Err("Input exceeds 64 KiB".into());
    }
    session(&state, &id)?
        .input
        .try_send(Input::Data(data))
        .map_err(|_| "Terminal input queue is full".into())
}
#[tauri::command]
fn terminal_resize(state: State<AppState>, id: String, cols: u32, rows: u32) -> Result<(), String> {
    if !(2..=500).contains(&cols) || !(1..=300).contains(&rows) {
        return Err("Invalid terminal size".into());
    }
    session(&state, &id)?
        .input
        .try_send(Input::Resize(cols, rows))
        .map_err(|_| "Terminal input queue is full".into())
}
#[tauri::command]
fn terminal_ack(state: State<AppState>, id: String, bytes: usize) -> Result<(), String> {
    session(&state, &id)?
        .input
        .try_send(Input::Ack(bytes.min(65536)))
        .map_err(|_| "Terminal input queue is full".into())
}
#[tauri::command]
async fn disconnect(state: State<'_, AppState>, id: String) -> Result<(), String> {
    session(&state, &id)?
        .input
        .send(Input::Close)
        .await
        .map_err(|_| "Session already closed".into())
}
#[derive(Serialize)]
struct RemoteEntry {
    name: String,
    size: u64,
    directory: bool,
}
fn remote_path(path: &str) -> Result<(), String> {
    if path.len() > 4096 || path.contains('\0') {
        Err("Invalid remote path".into())
    } else {
        Ok(())
    }
}
#[tauri::command]
async fn sftp_list(
    state: State<'_, AppState>,
    id: String,
    path: String,
) -> Result<Vec<RemoteEntry>, String> {
    remote_path(&path)?;
    let s = session(&state, &id)?;
    let sftp = ssh::sftp(&s).await?;
    let entries = sftp
        .read_dir(path)
        .await
        .map_err(|_| "Cannot read remote directory")?;
    let mut rows: Vec<_> = entries
        .filter(|e| e.file_name() != "." && e.file_name() != "..")
        .take(10000)
        .map(|e| RemoteEntry {
            name: e.file_name(),
            size: e.metadata().size.unwrap_or(0),
            directory: e.metadata().is_dir(),
        })
        .collect();
    rows.sort_by(|a, b| {
        b.directory
            .cmp(&a.directory)
            .then_with(|| a.name.to_lowercase().cmp(&b.name.to_lowercase()))
    });
    Ok(rows)
}
struct TransferGuard<'a>(&'a AtomicUsize);
impl Drop for TransferGuard<'_> {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::SeqCst);
    }
}
#[tauri::command]
async fn sftp_download(
    state: State<'_, AppState>,
    id: String,
    path: String,
) -> Result<bool, String> {
    remote_path(&path)?;
    let s = session(&state, &id)?;
    let name = path
        .rsplit('/')
        .next()
        .filter(|s| !s.is_empty())
        .ok_or("Invalid filename")?;
    if name.contains(['\\', ':']) || name == ".." {
        return Err("Unsafe filename".into());
    }
    let Some(file) = rfd::AsyncFileDialog::new()
        .set_file_name(name)
        .save_file()
        .await
    else {
        return Ok(false);
    };
    if state.transfers.fetch_add(1, Ordering::SeqCst) >= 4 {
        state.transfers.fetch_sub(1, Ordering::SeqCst);
        return Err("Transfer limit reached".into());
    }
    let _guard = TransferGuard(&state.transfers);
    let sftp = ssh::sftp(&s).await?;
    let mut remote = sftp
        .open(path)
        .await
        .map_err(|_| "Cannot open remote file")?;
    let parent = file.path().parent().ok_or("Invalid download folder")?;
    let temp = tempfile::NamedTempFile::new_in(parent).map_err(|_| "Cannot prepare download")?;
    let mut local =
        tokio::fs::File::from_std(temp.reopen().map_err(|_| "Cannot open download file")?);
    tokio::io::copy(&mut remote, &mut local)
        .await
        .map_err(|_| "Download failed; destination not replaced")?;
    local
        .sync_all()
        .await
        .map_err(|_| "Cannot flush downloaded file")?;
    drop(local);
    temp.persist(file.path())
        .map_err(|_| "Cannot finalize download")?;
    Ok(true)
}
#[tauri::command]
async fn sftp_upload(state: State<'_, AppState>, id: String, path: String) -> Result<bool, String> {
    remote_path(&path)?;
    let s = session(&state, &id)?;
    let Some(file) = rfd::AsyncFileDialog::new().pick_file().await else {
        return Ok(false);
    };
    let name = file
        .path()
        .file_name()
        .and_then(|s| s.to_str())
        .ok_or("Invalid filename")?;
    if name.contains(['/', '\\', '\0']) {
        return Err("Invalid filename".into());
    }
    let destination = format!("{}/{}", path.trim_end_matches('/'), name);
    let sftp = ssh::sftp(&s).await?;
    if sftp
        .try_exists(destination.clone())
        .await
        .map_err(|_| "Cannot check destination")?
    {
        return Err(
            "Destination already exists. This development version does not overwrite remote files."
                .into(),
        );
    }
    if state.transfers.fetch_add(1, Ordering::SeqCst) >= 4 {
        state.transfers.fetch_sub(1, Ordering::SeqCst);
        return Err("Transfer limit reached".into());
    }
    let _guard = TransferGuard(&state.transfers);
    // Exclusive creation prevents a check/create race from overwriting existing data.
    use russh_sftp::protocol::OpenFlags;
    let mut remote = sftp
        .open_with_flags(
            destination,
            OpenFlags::WRITE | OpenFlags::CREATE | OpenFlags::EXCLUDE,
        )
        .await
        .map_err(|_| "Cannot exclusively create remote file")?;
    let mut local = tokio::fs::File::open(file.path())
        .await
        .map_err(|_| "Cannot open upload source")?;
    tokio::io::copy(&mut local, &mut remote)
        .await
        .map_err(|_| "Upload failed; partial remote file may remain")?;
    tokio::io::AsyncWriteExt::shutdown(&mut remote)
        .await
        .map_err(|_| "Cannot complete upload")?;
    Ok(true)
}

#[derive(Serialize)]
struct UpdateInfo {
    status: String,
    version: String,
    url: String,
}
#[tauri::command]
async fn check_updates() -> Result<UpdateInfo, String> {
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(12))
        .redirect(reqwest::redirect::Policy::none())
        .build()
        .map_err(|_| "Cannot initialize update check")?;
    let mut response = client
        .get("https://api.github.com/repos/NetGraniz/NordSSH/releases/latest")
        .header("User-Agent", "NordSSH")
        .header("Accept", "application/vnd.github+json")
        .send()
        .await
        .map_err(|_| "GitHub is unavailable; SSH is unaffected")?;
    if response.status() == reqwest::StatusCode::NOT_FOUND {
        return Ok(UpdateInfo {
            status: "no-release".into(),
            version: String::new(),
            url: String::new(),
        });
    }
    if !response.status().is_success() {
        return Err("GitHub rejected the update check; try later".into());
    }
    if response.content_length().is_some_and(|n| n > 1024 * 1024) {
        return Err("Invalid release response".into());
    }
    let mut bytes = Vec::new();
    while let Some(chunk) = response
        .chunk()
        .await
        .map_err(|_| "Cannot read release metadata")?
    {
        if bytes.len() + chunk.len() > 1024 * 1024 {
            return Err("Release metadata exceeds size limit".into());
        }
        bytes.extend_from_slice(&chunk);
    }
    let json: serde_json::Value =
        serde_json::from_slice(&bytes).map_err(|_| "Invalid GitHub response")?;
    let version = json["tag_name"]
        .as_str()
        .ok_or("Missing release version")?
        .trim_start_matches('v');
    let parsed = semver::Version::parse(version).map_err(|_| "Unsupported release version")?;
    let current = semver::Version::parse(env!("CARGO_PKG_VERSION"))
        .map_err(|_| "Invalid application version")?;
    let status = if parsed > current && parsed.pre.is_empty() {
        "available"
    } else {
        "current"
    };
    Ok(UpdateInfo {
        status: status.into(),
        version: version.into(),
        url: "https://github.com/NetGraniz/NordSSH/releases".into(),
    })
}

pub fn run() {
    let _ = rustls::crypto::ring::default_provider().install_default();
    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _, _| {
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.show();
                let _ = window.set_focus();
            }
        }))
        .setup(|app| {
            let dir = app.path().app_data_dir()?;
            let executable = std::env::current_exe()?;
            let appimage = std::env::var_os("APPIMAGE").map(std::path::PathBuf::from);
            let portable = storage::portable_root(
                &executable,
                appimage.as_deref(),
                std::env::args().any(|s| s == "--portable"),
            );
            let root = portable.clone().unwrap_or_else(|| dir.clone());
            let store = storage::Store::open_app(
                root.join("vault.nordvault"),
                dir.join("device-keys"),
                &dir.join("vault.dpapi"),
            )
            .map_err(std::io::Error::other)?;
            app.manage(AppState {
                store: Arc::new(Mutex::new(store)),
                sessions: Arc::new(Mutex::new(HashMap::new())),
                pending: Arc::new(AtomicUsize::new(0)),
                transfers: AtomicUsize::new(0),
                portable: portable.is_some(),
            });
            Ok(())
        })
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                let state = window.state::<AppState>();
                let active = state.pending.load(Ordering::SeqCst) > 0
                    || state.transfers.load(Ordering::SeqCst) > 0
                    || state.sessions.lock().map(|s| !s.is_empty()).unwrap_or(true);
                if active {
                    api.prevent_close();
                    let window = window.clone();
                    tauri::async_runtime::spawn(async move {
                        let answer = rfd::AsyncMessageDialog::new()
                            .set_title("Закрыть NordSSH?")
                            .set_description("SSH-подключения и передачи файлов будут прерваны.")
                            .set_buttons(rfd::MessageButtons::YesNo)
                            .show()
                            .await;
                        if answer == rfd::MessageDialogResult::Yes {
                            let _ = window.destroy();
                        }
                    });
                }
            }
        })
        .invoke_handler(tauri::generate_handler![
            vault_status,
            vault_unlock,
            vault_lock,
            vault_forget_device,
            vault_export,
            vault_import,
            snapshot,
            save_host,
            delete_host,
            save_settings,
            save_snippet,
            delete_snippet,
            pick_key,
            connect,
            terminal_input,
            terminal_resize,
            terminal_ack,
            disconnect,
            sftp_list,
            sftp_download,
            sftp_upload,
            check_updates
        ])
        .run(tauri::generate_context!())
        .expect("NordSSH could not start; existing vault data was not reset");
}
