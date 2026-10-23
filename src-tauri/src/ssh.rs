use crate::{
    model::{Host, Secret},
    storage::Store,
};
use russh::{
    ChannelMsg, client,
    keys::{HashAlg, PrivateKeyWithHashAlg, PublicKeyOrCertificate},
};
use russh_sftp::client::SftpSession;
use serde::Serialize;
use std::{
    sync::{Arc, Mutex},
    time::Duration,
};
use tauri::ipc::Channel;
use tokio::sync::{Mutex as AsyncMutex, mpsc};
use zeroize::Zeroizing;

pub type SshHandle = client::Handle<Verifier>;
pub struct Session {
    pub input: mpsc::Sender<Input>,
    pub handle: Arc<AsyncMutex<SshHandle>>,
    pub sftp: AsyncMutex<Option<Arc<SftpSession>>>,
}
pub enum Input {
    Data(Vec<u8>),
    Resize(u32, u32),
    Ack(usize),
    Close,
}
#[derive(Clone, Serialize)]
pub struct TerminalEvent {
    pub id: String,
    pub kind: String,
    pub data: Vec<u8>,
    pub message: String,
}
pub struct Verifier {
    pub host: Host,
    pub store: Arc<Mutex<Store>>,
}

pub fn compare_key(known: Option<&str>, received: &str) -> Result<bool, String> {
    match known {
        Some(key) if key == received => Ok(true),
        Some(_) => {
            Err("SSH HOST KEY CHANGED. Connection refused; verify the server independently.".into())
        }
        None => Ok(false),
    }
}
impl client::Handler for Verifier {
    type Error = russh::Error;
    async fn check_server_key(
        &mut self,
        key: &PublicKeyOrCertificate,
    ) -> Result<bool, Self::Error> {
        // Host certificates require a separate CA trust policy; never silently treat
        // their embedded key as an already trusted plain host key.
        if key.certificate().is_some() {
            return Ok(false);
        }
        let fingerprint = key.public_key().fingerprint(HashAlg::Sha256).to_string();
        let endpoint = self.host.endpoint();
        let known = self
            .store
            .lock()
            .map_err(|_| russh::Error::Disconnect)?
            .vault
            .known_hosts
            .get(&endpoint)
            .cloned();
        match compare_key(known.as_deref(), &fingerprint) {
            Ok(true) => return Ok(true),
            Err(_) => return Ok(false),
            Ok(false) => {}
        }
        let accepted=rfd::AsyncMessageDialog::new().set_title("NordSSH — неизвестный сервер")
            .set_description(format!("{}\n{}\n\n{}\n\nСверьте SHA256-отпечаток с администратором сервера. Доверять этому ключу?",self.host.name,endpoint,fingerprint))
            .set_buttons(rfd::MessageButtons::YesNo).set_level(rfd::MessageLevel::Warning).show().await;
        if accepted != rfd::MessageDialogResult::Yes {
            return Ok(false);
        }
        let mut store = self.store.lock().map_err(|_| russh::Error::Disconnect)?;
        // A concurrent connection may have accepted a different key during the dialog.
        if compare_key(
            store.vault.known_hosts.get(&endpoint).map(String::as_str),
            &fingerprint,
        )
        .is_err()
        {
            return Ok(false);
        }
        Ok(store
            .update(|v| {
                v.known_hosts.insert(endpoint, fingerprint);
                Ok(())
            })
            .is_ok())
    }
}

pub async fn open(
    host: Host,
    secret: Secret,
    store: Arc<Mutex<Store>>,
) -> Result<
    (
        Arc<Session>,
        russh::Channel<client::Msg>,
        mpsc::Receiver<Input>,
    ),
    String,
> {
    host.validate()?;
    let config = client::Config {
        keepalive_interval: Some(Duration::from_secs(30)),
        keepalive_max: 3,
        ..Default::default()
    };
    let verifier = Verifier {
        host: host.clone(),
        store,
    };
    let mut handle=tokio::time::timeout(Duration::from_secs(120),client::connect(Arc::new(config),(host.hostname.as_str(),host.port),verifier)).await
        .map_err(|_| "SSH connection or host-key confirmation timed out")?
        .map_err(|_| "SSH connection failed: check address, port and trusted host key. A changed key is never accepted automatically.")?;
    let authenticated = tokio::time::timeout(Duration::from_secs(30), async {
        if host.auth == "password" {
            handle
                .authenticate_password(&host.username, &secret.password)
                .await
                .map(|r| r.success())
                .map_err(|_| "Password authentication failed".to_string())
        } else {
            let text = Zeroizing::new(read_key_file(&host.key_path).await?);
            let phrase = if secret.passphrase.is_empty() {
                None
            } else {
                Some(secret.passphrase.as_str())
            };
            let key = russh::keys::decode_secret_key(&text, phrase)
                .map_err(|_| "Cannot decode private key; check format and passphrase")?;
            let result = if host.auth == "certificate" {
                let cert = russh::keys::Certificate::from_openssh(
                    read_key_file(&host.certificate_path).await?.trim(),
                )
                .map_err(|_| "Invalid OpenSSH certificate")?;
                handle
                    .authenticate_openssh_cert(&host.username, Arc::new(key), cert)
                    .await
            } else {
                let hash = handle
                    .best_supported_rsa_hash()
                    .await
                    .map_err(|_| "Cannot negotiate RSA signature")?
                    .flatten();
                handle
                    .authenticate_publickey(
                        &host.username,
                        PrivateKeyWithHashAlg::new(Arc::new(key), hash),
                    )
                    .await
            };
            result
                .map(|r| r.success())
                .map_err(|_| "Key authentication failed".to_string())
        }
    })
    .await
    .map_err(|_| "Authentication timed out")??;
    if !authenticated {
        return Err("Authentication rejected. Check username and credentials.".into());
    }
    let channel = tokio::time::timeout(Duration::from_secs(15), async {
        let c = handle.channel_open_session().await?;
        c.request_pty(true, "xterm-256color", 100, 30, 0, 0, &[])
            .await?;
        c.request_shell(true).await?;
        Ok::<_, russh::Error>(c)
    })
    .await
    .map_err(|_| "Opening terminal timed out")?
    .map_err(|_| "Server refused the terminal")?;
    let (tx, rx) = mpsc::channel(128);
    let session = Arc::new(Session {
        input: tx,
        handle: Arc::new(AsyncMutex::new(handle)),
        sftp: AsyncMutex::new(None),
    });
    Ok((session, channel, rx))
}

async fn read_key_file(path: &str) -> Result<String, String> {
    let file = tokio::fs::File::open(path)
        .await
        .map_err(|_| "Cannot open the selected key file")?;
    use tokio::io::AsyncReadExt;
    let mut bytes = Zeroizing::new(Vec::new());
    file.take(1024 * 1024 + 1)
        .read_to_end(&mut bytes)
        .await
        .map_err(|_| "Cannot read key file")?;
    if bytes.len() > 1024 * 1024 {
        return Err("Key file exceeds 1 MiB".into());
    }
    String::from_utf8(bytes.to_vec()).map_err(|_| "Key must be an OpenSSH or PEM text file".into())
}

pub async fn pump(
    id: String,
    session: Arc<Session>,
    mut channel: russh::Channel<client::Msg>,
    mut input: mpsc::Receiver<Input>,
    output: Channel<TerminalEvent>,
) {
    let mut pending = 0usize;
    loop {
        tokio::select! {
            command=input.recv()=>match command {
                Some(Input::Data(data))=>{ if channel.data(data.as_slice()).await.is_err() { break; } },
                Some(Input::Resize(cols,rows))=>{ let _=channel.window_change(cols,rows,0,0).await; },
                Some(Input::Ack(bytes))=>pending=pending.saturating_sub(bytes),
                Some(Input::Close)|None=>break,
            },
            message=channel.wait(), if pending < 256*1024 => match message {
                Some(ChannelMsg::Data { data })|Some(ChannelMsg::ExtendedData { data,.. })=>{
                    pending+=data.len();
                    if output.send(TerminalEvent { id:id.clone(),kind:"data".into(),data:data.to_vec(),message:String::new() }).is_err() { break; }
                },
                Some(ChannelMsg::Eof)|Some(ChannelMsg::Close)|None=>break,
                _=>{}
            }
        }
    }
    let _ = channel.close().await;
    let _ = session
        .handle
        .lock()
        .await
        .disconnect(russh::Disconnect::ByApplication, "Session closed", "en")
        .await;
    let _ = output.send(TerminalEvent {
        id,
        kind: "closed".into(),
        data: vec![],
        message: "Соединение закрыто".into(),
    });
}

pub async fn sftp(session: &Session) -> Result<Arc<SftpSession>, String> {
    let mut cached = session.sftp.lock().await;
    if let Some(s) = cached.as_ref() {
        return Ok(s.clone());
    }
    let c = session
        .handle
        .lock()
        .await
        .channel_open_session()
        .await
        .map_err(|_| "Cannot open SFTP channel")?;
    c.request_subsystem(true, "sftp")
        .await
        .map_err(|_| "Server does not support SFTP")?;
    let s = Arc::new(
        SftpSession::new(c.into_stream())
            .await
            .map_err(|_| "SFTP initialization failed")?,
    );
    s.set_timeout(30);
    *cached = Some(s.clone());
    Ok(s)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn known_key_matches() {
        assert!(compare_key(Some("SHA256:one"), "SHA256:one").unwrap());
    }
    #[test]
    fn changed_key_is_rejected() {
        assert!(compare_key(Some("SHA256:one"), "SHA256:two").is_err());
    }
    #[test]
    fn unknown_key_needs_confirmation() {
        assert!(!compare_key(None, "SHA256:new").unwrap());
    }

    #[tokio::test]
    #[cfg(windows)]
    async fn isolated_ssh_and_sftp_roundtrip() {
        use std::io::{BufRead, BufReader};
        use tokio::io::{AsyncReadExt, AsyncWriteExt};
        struct Fixture(std::process::Child);
        impl Drop for Fixture {
            fn drop(&mut self) {
                let _ = self.0.kill();
                let _ = self.0.wait();
            }
        }
        let script =
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../test/ssh-fixture.cjs");
        let dir = tempfile::tempdir().unwrap();
        let key_path = dir.path().join("fixture-key");
        let mut child = Fixture(
            std::process::Command::new("node")
                .arg(script)
                .arg(&key_path)
                .stdout(std::process::Stdio::piped())
                .spawn()
                .unwrap(),
        );
        let mut line = String::new();
        BufReader::new(child.0.stdout.take().unwrap())
            .read_line(&mut line)
            .unwrap();
        let fixture: serde_json::Value = serde_json::from_str(&line).unwrap();
        let port = fixture["port"].as_u64().unwrap() as u16;
        let host = Host {
            id: uuid::Uuid::new_v4().to_string(),
            name: "Isolated fixture".into(),
            hostname: "127.0.0.1".into(),
            port,
            username: "fixture".into(),
            vault: "Test".into(),
            auth: "password".into(),
            key_path: String::new(),
            certificate_path: String::new(),
            favorite: false,
        };
        let mut store = Store::open(dir.path().join("vault.dpapi")).unwrap();
        store.vault.known_hosts.insert(
            host.endpoint(),
            fixture["fingerprint"].as_str().unwrap().into(),
        );
        let store = Arc::new(Mutex::new(store));
        let secret = Secret {
            password: "synthetic-only".into(),
            passphrase: String::new(),
        };
        let (session, mut terminal, _rx) = open(host.clone(), secret, store.clone()).await.unwrap();
        async fn receive_until(channel: &mut russh::Channel<client::Msg>, marker: &str) {
            tokio::time::timeout(Duration::from_secs(10), async {
                let mut bytes = Vec::new();
                loop {
                    match channel.wait().await {
                        Some(ChannelMsg::Data { data }) => bytes.extend_from_slice(&data),
                        Some(ChannelMsg::Close) | None => {
                            panic!("Fixture closed before expected output")
                        }
                        _ => {}
                    }
                    assert!(bytes.len() < 65536, "Fixture output exceeded limit");
                    if String::from_utf8_lossy(&bytes).contains(marker) {
                        break;
                    }
                }
            })
            .await
            .unwrap();
        }
        receive_until(&mut terminal, "NORDSSH_FIXTURE_READY").await;
        terminal.data(&b"ping\r"[..]).await.unwrap();
        receive_until(&mut terminal, "pong").await;
        let files = sftp(&session).await.unwrap();
        let entries = files.read_dir("/").await.unwrap();
        assert!(entries.into_iter().any(|e| e.file_name() == "hello.txt"));
        let bytes = files.read("/hello.txt").await.unwrap();
        assert_eq!(bytes, b"NordSSH SFTP fixture\n");
        let mut upload = files
            .open_with_flags(
                "/roundtrip.txt",
                russh_sftp::protocol::OpenFlags::WRITE
                    | russh_sftp::protocol::OpenFlags::CREATE
                    | russh_sftp::protocol::OpenFlags::EXCLUDE,
            )
            .await
            .unwrap();
        upload.write_all(b"roundtrip content").await.unwrap();
        upload.shutdown().await.unwrap();
        let mut download = files.open("/roundtrip.txt").await.unwrap();
        let mut copied = Vec::new();
        download.read_to_end(&mut copied).await.unwrap();
        assert_eq!(copied, b"roundtrip content");
        assert!(
            files
                .open_with_flags(
                    "/roundtrip.txt",
                    russh_sftp::protocol::OpenFlags::WRITE
                        | russh_sftp::protocol::OpenFlags::CREATE
                        | russh_sftp::protocol::OpenFlags::EXCLUDE
                )
                .await
                .is_err()
        );
        let bad = Secret {
            password: "wrong".into(),
            passphrase: String::new(),
        };
        assert!(open(host.clone(), bad, store.clone()).await.is_err());
        let mut key_host = host.clone();
        key_host.auth = "key".into();
        key_host.key_path = key_path.to_string_lossy().into_owned();
        let credentials = Secret {
            password: String::new(),
            passphrase: "fixture-passphrase".into(),
        };
        let (key_session, _, _) = open(key_host.clone(), credentials, store.clone())
            .await
            .unwrap();
        key_session
            .handle
            .lock()
            .await
            .disconnect(
                russh::Disconnect::ByApplication,
                "Key fixture finished",
                "en",
            )
            .await
            .unwrap();
        let wrong_phrase = Secret {
            password: String::new(),
            passphrase: "wrong".into(),
        };
        assert!(open(key_host, wrong_phrase, store.clone()).await.is_err());
        store
            .lock()
            .unwrap()
            .vault
            .known_hosts
            .insert(host.endpoint(), "SHA256:wrong-key".into());
        let valid = Secret {
            password: "synthetic-only".into(),
            passphrase: String::new(),
        };
        assert!(open(host, valid, store).await.is_err());
        session
            .handle
            .lock()
            .await
            .disconnect(russh::Disconnect::ByApplication, "Fixture finished", "en")
            .await
            .unwrap();
    }
}
