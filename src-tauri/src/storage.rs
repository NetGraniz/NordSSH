use crate::{
    crypto::{Envelope, MAX_FILE, VaultKey},
    model::Vault,
};
use serde::Serialize;
use std::{
    io::{Read, Write},
    path::{Path, PathBuf},
};

pub struct Store {
    pub path: PathBuf,
    pub vault: Vault,
    envelope: Option<Envelope>,
    key: Option<VaultKey>,
    device_dir: Option<PathBuf>,
    pub migration: bool,
}
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Status {
    pub initialized: bool,
    pub unlocked: bool,
    pub migration: bool,
    pub path: String,
    pub portable: bool,
}

pub fn read_limited(path: &Path, max: u64) -> Result<Vec<u8>, String> {
    let file = std::fs::File::open(path).map_err(|_| "Cannot read selected file")?;
    let mut bytes = Vec::new();
    file.take(max + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| "Cannot read file")?;
    if bytes.len() as u64 > max {
        return Err("File exceeds size limit".into());
    }
    Ok(bytes)
}
pub fn atomic_write(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let parent = path.parent().ok_or("Invalid data path")?;
    std::fs::create_dir_all(parent).map_err(|_| "Cannot create data directory")?;
    let mut file =
        tempfile::NamedTempFile::new_in(parent).map_err(|_| "Cannot create temporary data file")?;
    file.write_all(bytes).map_err(|_| "Cannot write data")?;
    file.as_file().sync_all().map_err(|_| "Cannot flush data")?;
    file.persist(path)
        .map_err(|_| "Cannot replace data atomically")?;
    Ok(())
}
fn encode(e: &Envelope) -> Result<Vec<u8>, String> {
    let bytes = serde_json::to_vec(e).map_err(|_| "Cannot encode encrypted vault")?;
    if bytes.len() as u64 > MAX_FILE {
        return Err("Encrypted file exceeds size limit".into());
    }
    Ok(bytes)
}
pub fn read_envelope(path: &Path) -> Result<Envelope, String> {
    let e: Envelope = serde_json::from_slice(&read_limited(path, MAX_FILE)?)
        .map_err(|_| "Invalid vault file; original data preserved")?;
    e.validate()?;
    Ok(e)
}

impl Store {
    pub fn open(path: PathBuf) -> Result<Self, String> {
        let envelope = match path.try_exists() {
            Ok(true) => Some(read_envelope(&path)?),
            Ok(false) => None,
            Err(_) => return Err("Cannot inspect existing vault".into()),
        };
        Ok(Self {
            path,
            vault: Vault::default(),
            envelope,
            key: None,
            device_dir: None,
            migration: false,
        })
    }
    pub fn open_app(path: PathBuf, device_dir: PathBuf, legacy: &Path) -> Result<Self, String> {
        let mut store = Self::open(path)?;
        store.device_dir = Some(device_dir);
        #[cfg(windows)]
        if store.envelope.is_none()
            && legacy
                .try_exists()
                .map_err(|_| "Cannot inspect legacy vault")?
        {
            let bytes = zeroize::Zeroizing::new(crate::device::dpapi(
                &read_limited(legacy, MAX_FILE)?,
                true,
            )?);
            store.vault = serde_json::from_slice(&bytes)
                .map_err(|_| "Cannot decode legacy vault; original file preserved")?;
            store.vault.validate()?;
            store.migration = true;
        }
        #[cfg(not(windows))]
        let _ = legacy;
        if let (Some(e), Some(dir)) = (&store.envelope, &store.device_dir)
            && let Ok(key) = crate::device::load(dir, &e.id)
            && let Ok(v) = e.read_vault(&key)
        {
            store.vault = v;
            store.key = Some(key);
        }
        Ok(store)
    }
    pub fn unlocked(&self) -> bool {
        self.key.is_some()
    }
    pub fn require_unlocked(&self) -> Result<(), String> {
        if self.unlocked() {
            Ok(())
        } else {
            Err("Unlock the vault first".into())
        }
    }
    pub fn status(&self, portable: bool) -> Status {
        Status {
            initialized: self.envelope.is_some(),
            unlocked: self.unlocked(),
            migration: self.migration,
            path: self.path.to_string_lossy().into_owned(),
            portable,
        }
    }
    pub fn initialize(&mut self, password: &str, remember: bool) -> Result<Option<String>, String> {
        if self.envelope.is_some() || self.path.try_exists().map_err(|_| "Cannot inspect vault")? {
            return Err("Vault already exists; no data changed".into());
        }
        let mut candidate = self.vault.clone();
        if self.migration {
            for host in &candidate.hosts {
                if host.auth != "password" {
                    let secret = candidate.secrets.entry(host.id.clone()).or_default();
                    if secret.private_key.is_empty() {
                        secret.private_key=String::from_utf8(read_limited(Path::new(&host.key_path),1024*1024)?).map_err(|_|"Legacy key must be a text OpenSSH/PEM file; original vault preserved")?;
                    }
                    if host.auth == "certificate" && secret.certificate.is_empty() {
                        secret.certificate = String::from_utf8(read_limited(
                            Path::new(&host.certificate_path),
                            1024 * 1024,
                        )?)
                        .map_err(|_| "Legacy certificate is not UTF-8; original vault preserved")?;
                    }
                }
            }
        }
        let (e, key) = Envelope::create(&candidate, password)?;
        atomic_write(&self.path, &encode(&e)?)?;
        self.envelope = Some(e);
        self.key = Some(key);
        self.vault = candidate;
        self.migration = false;
        Ok(self.remember(remember).err())
    }
    pub fn unlock(&mut self, password: &str, remember: bool) -> Result<Option<String>, String> {
        // Reload the file; an externally modified vault is never overwritten using stale state.
        let e = read_envelope(&self.path)?;
        let (vault, key) = e.unlock(password)?;
        self.vault = vault;
        self.envelope = Some(e);
        self.key = Some(key);
        Ok(self.remember(remember).err())
    }
    fn remember(&self, remember: bool) -> Result<(), String> {
        if let (Some(e), Some(dir), Some(key)) = (&self.envelope, &self.device_dir, &self.key) {
            if remember {
                crate::device::save(dir, &e.id, key)
            } else {
                crate::device::forget(dir, &e.id)
            }
        } else {
            Ok(())
        }
    }
    pub fn forget_device(&self) -> Result<(), String> {
        self.remember(false)
    }
    pub fn lock(&mut self) {
        self.key = None;
        self.vault = Vault::default();
    }
    pub fn update(
        &mut self,
        edit: impl FnOnce(&mut Vault) -> Result<(), String>,
    ) -> Result<(), String> {
        self.require_unlocked()?;
        let mut candidate = self.vault.clone();
        edit(&mut candidate)?;
        candidate.validate()?;
        let mut e = self.envelope.clone().ok_or("Vault is not initialized")?;
        if read_limited(&self.path, MAX_FILE)? != encode(&e)? {
            return Err(
                "Vault changed outside NordSSH. Close and reopen it; no data was overwritten."
                    .into(),
            );
        }
        e.set_vault(&candidate, self.key.as_ref().ok_or("Vault is locked")?)?;
        atomic_write(&self.path, &encode(&e)?)?;
        self.vault = candidate;
        self.envelope = Some(e);
        Ok(())
    }
    pub fn export(&self, path: &Path, password: &str, vault: &Vault) -> Result<(), String> {
        self.require_unlocked()?;
        if path == self.path
            || std::fs::canonicalize(path)
                .ok()
                .is_some_and(|p| std::fs::canonicalize(&self.path).ok() == Some(p))
        {
            return Err("Export must not overwrite the active vault".into());
        }
        let (e, _) = Envelope::create(vault, password)?;
        atomic_write(path, &encode(&e)?)
    }
    pub fn import(
        &mut self,
        path: &Path,
        password: &str,
        remember: bool,
    ) -> Result<Option<String>, String> {
        let incoming = read_envelope(path)?;
        let (vault, key) = incoming.unlock(password)?;
        // Back up exact encrypted bytes before replacing; no plaintext backup is written.
        if self
            .path
            .try_exists()
            .map_err(|_| "Cannot inspect current vault")?
        {
            let current = read_limited(&self.path, MAX_FILE)?;
            let backup = self
                .path
                .with_extension(format!("before-import-{}.bak", uuid::Uuid::new_v4()));
            atomic_write(&backup, &current)?;
        }
        atomic_write(&self.path, &encode(&incoming)?)?;
        self.vault = vault;
        self.key = Some(key);
        self.envelope = Some(incoming);
        self.migration = false;
        Ok(self.remember(remember).err())
    }
}

pub fn portable_root(executable: &Path, appimage: Option<&Path>, flag: bool) -> Option<PathBuf> {
    let root = appimage.unwrap_or(executable).parent()?;
    if flag || root.join("nordssh.portable").is_file() {
        Some(root.join("NordSSH-data"))
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const PASSWORD: &str = "synthetic-only-vault-password";
    #[test]
    fn interchange_fixture() {
        // All fields are synthetic and this published fixture's password is public.
        let input = std::env::var_os("NORDSSH_FIXTURE_INPUT");
        if let Some(path) = input {
            let e = read_envelope(Path::new(&path)).unwrap();
            let (v, _) = e.unlock(PASSWORD).unwrap();
            assert_eq!(v.settings.font_size, 18);
            assert_eq!(v.secrets["fixture"].private_key, "synthetic-key-material");
            assert_eq!(v.known_hosts["[fixture.invalid]:22"], "SHA256:synthetic");
        }
        if let Some(path) = std::env::var_os("NORDSSH_FIXTURE_OUTPUT") {
            let mut v = Vault::default();
            v.settings.font_size = 18;
            v.secrets.insert(
                "fixture".into(),
                crate::model::Secret {
                    password: "synthetic-password".into(),
                    passphrase: String::new(),
                    private_key: "synthetic-key-material".into(),
                    certificate: String::new(),
                },
            );
            v.known_hosts
                .insert("[fixture.invalid]:22".into(), "SHA256:synthetic".into());
            let (e, _) = Envelope::create(&v, PASSWORD).unwrap();
            atomic_write(Path::new(&path), &encode(&e).unwrap()).unwrap();
        }
    }
    #[test]
    fn portable_transfer_secrets_settings_trust_and_rollback() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("one.nordvault");
        let mut first = Store::open(path.clone()).unwrap();
        assert!(!first.unlocked());
        first.initialize(PASSWORD, false).unwrap();
        first
            .update(|v| {
                v.known_hosts
                    .insert("[localhost]:22".into(), "SHA256:fixture".into());
                v.secrets.insert(
                    "fixture".into(),
                    crate::model::Secret {
                        password: "synthetic-never-plaintext".into(),
                        passphrase: String::new(),
                        private_key: String::new(),
                        certificate: String::new(),
                    },
                );
                v.settings.font_size = 18;
                Ok(())
            })
            .unwrap();
        let bytes = std::fs::read(&path).unwrap();
        assert!(!bytes.windows(25).any(|w| w == b"synthetic-never-plaintext"));
        assert!(
            first
                .update(|v| {
                    v.schema = 999;
                    Ok(())
                })
                .is_err()
        );
        assert_eq!(bytes, std::fs::read(&path).unwrap());
        let export = dir.path().join("export.nordvault");
        first.export(&export, PASSWORD, &first.vault).unwrap();
        let mut second = Store::open(dir.path().join("two.nordvault")).unwrap();
        second.import(&export, PASSWORD, false).unwrap();
        assert_eq!(second.vault.known_hosts.len(), 1);
        assert_eq!(second.vault.settings.font_size, 18);
        assert_eq!(
            second.vault.secrets["fixture"].password,
            "synthetic-never-plaintext"
        );
        second.lock();
        assert!(second.update(|_| Ok(())).is_err());
        assert!(second.unlock("wrong", false).is_err());
        assert!(!second.unlocked());
        second.unlock(PASSWORD, false).unwrap();
        std::fs::write(&path, b"external change").unwrap();
        assert!(first.update(|_| Ok(())).is_err());
        assert_eq!(std::fs::read(&path).unwrap(), b"external change");
    }
    #[test]
    fn corrupt_file_and_bad_import_preserve_destination() {
        let dir = tempfile::tempdir().unwrap();
        let bad = dir.path().join("bad");
        std::fs::write(&bad, b"invalid").unwrap();
        assert!(Store::open(bad.clone()).is_err());
        let path = dir.path().join("good");
        let mut s = Store::open(path.clone()).unwrap();
        s.initialize(PASSWORD, false).unwrap();
        let before = std::fs::read(&path).unwrap();
        assert!(s.import(&bad, PASSWORD, false).is_err());
        assert_eq!(before, std::fs::read(path).unwrap());
    }
    #[test]
    fn portable_selection_does_not_depend_on_working_directory() {
        let dir = tempfile::tempdir().unwrap();
        let exe = dir.path().join("nordssh.exe");
        assert!(portable_root(&exe, None, false).is_none());
        std::fs::write(dir.path().join("nordssh.portable"), b"portable").unwrap();
        assert_eq!(
            portable_root(&exe, None, false).unwrap(),
            dir.path().join("NordSSH-data")
        );
        let image = dir.path().join("image/NordSSH.AppImage");
        assert_eq!(
            portable_root(&exe, Some(&image), true).unwrap(),
            dir.path().join("image/NordSSH-data")
        );
    }
    #[test]
    #[cfg(windows)]
    fn legacy_migration_leaves_original_untouched_and_device_key_is_separate() {
        let dir = tempfile::tempdir().unwrap();
        let old = dir.path().join("vault.dpapi");
        let mut v = Vault::default();
        let key_path = dir.path().join("legacy-fixture-key");
        std::fs::write(&key_path, b"synthetic-legacy-key-not-a-real-identity").unwrap();
        let host_id = uuid::Uuid::new_v4().to_string();
        v.hosts.push(crate::model::Host {
            id: host_id.clone(),
            name: "Legacy fixture".into(),
            hostname: "fixture.invalid".into(),
            port: 22,
            username: "fixture".into(),
            vault: "Test".into(),
            auth: "key".into(),
            key_path: key_path.to_string_lossy().into_owned(),
            certificate_path: String::new(),
            favorite: false,
        });
        v.known_hosts
            .insert("[localhost]:22".into(), "SHA256:fixture".into());
        let ciphertext = crate::device::dpapi(&serde_json::to_vec(&v).unwrap(), false).unwrap();
        std::fs::write(&old, &ciphertext).unwrap();
        let path = dir.path().join("vault.nordvault");
        let device = dir.path().join("device");
        let mut s = Store::open_app(path.clone(), device.clone(), &old).unwrap();
        assert!(s.migration);
        s.initialize(PASSWORD, true).unwrap();
        assert_eq!(std::fs::read(&old).unwrap(), ciphertext);
        assert_eq!(
            s.vault.secrets[&host_id].private_key,
            "synthetic-legacy-key-not-a-real-identity"
        );
        assert!(Store::open_app(path, device, &old).unwrap().unlocked());
        std::fs::remove_file(&key_path).unwrap();
        let incomplete_path = dir.path().join("incomplete.nordvault");
        let mut incomplete = Store::open_app(
            incomplete_path.clone(),
            dir.path().join("another-device"),
            &old,
        )
        .unwrap();
        assert!(incomplete.initialize(PASSWORD, false).is_err());
        assert!(!incomplete_path.exists());
        assert_eq!(std::fs::read(&old).unwrap(), ciphertext);
    }
}
