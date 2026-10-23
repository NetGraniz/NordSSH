use crate::model::Vault;
use std::{
    io::{Read, Write},
    path::{Path, PathBuf},
};
use zeroize::Zeroizing;

const MAX_VAULT_BYTES: u64 = 8 * 1024 * 1024;
pub struct Store {
    pub path: PathBuf,
    pub vault: Vault,
}

#[cfg(windows)]
fn dpapi(data: &[u8], decrypt: bool) -> Result<Vec<u8>, String> {
    use windows_sys::Win32::{Foundation::LocalFree, Security::Cryptography::*};
    if data.len() > MAX_VAULT_BYTES as usize {
        return Err("Vault is too large".into());
    }
    let input = CRYPT_INTEGER_BLOB {
        cbData: data.len() as u32,
        pbData: data.as_ptr() as *mut u8,
    };
    let mut output = CRYPT_INTEGER_BLOB {
        cbData: 0,
        pbData: std::ptr::null_mut(),
    };
    // DPAPI allocates output with LocalAlloc. Copy and zero it before LocalFree.
    // No LOCAL_MACHINE flag: protection belongs to this Windows user.
    unsafe {
        let ok = if decrypt {
            CryptUnprotectData(
                &input,
                std::ptr::null_mut(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut output,
            )
        } else {
            CryptProtectData(
                &input,
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut output,
            )
        };
        if ok == 0 {
            return Err(
                "Windows could not unlock the vault. Keep the original file and Windows profile."
                    .into(),
            );
        }
        let slice = std::slice::from_raw_parts_mut(output.pbData, output.cbData as usize);
        let result = slice.to_vec();
        zeroize::Zeroize::zeroize(slice);
        LocalFree(output.pbData.cast());
        Ok(result)
    }
}
#[cfg(not(windows))]
fn dpapi(_: &[u8], _: bool) -> Result<Vec<u8>, String> {
    Err("This build requires Windows user-scoped DPAPI".into())
}

pub fn atomic_write(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let parent = path.parent().ok_or("Invalid data path")?;
    std::fs::create_dir_all(parent).map_err(|_| "Cannot create user data directory")?;
    let mut file =
        tempfile::NamedTempFile::new_in(parent).map_err(|_| "Cannot create temporary data file")?;
    file.write_all(bytes).map_err(|_| "Cannot write data")?;
    file.as_file().sync_all().map_err(|_| "Cannot flush data")?;
    file.persist(path)
        .map_err(|_| "Cannot replace data file atomically")?;
    Ok(())
}
impl Store {
    pub fn open(path: PathBuf) -> Result<Self, String> {
        let vault = match std::fs::File::open(&path) {
            Ok(file) => {
                let mut bytes = Vec::new();
                file.take(MAX_VAULT_BYTES + 1)
                    .read_to_end(&mut bytes)
                    .map_err(|_| "Cannot read vault")?;
                if bytes.len() as u64 > MAX_VAULT_BYTES {
                    return Err("Vault exceeds size limit".into());
                }
                let plain = Zeroizing::new(dpapi(&bytes, true)?);
                serde_json::from_slice::<Vault>(&plain)
                    .map_err(|_| "Vault contents are invalid; original file preserved")?
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Vault::default(),
            Err(_) => return Err("Cannot open existing vault; original file preserved".into()),
        };
        vault.validate()?;
        Ok(Self { path, vault })
    }
    pub fn update(
        &mut self,
        edit: impl FnOnce(&mut Vault) -> Result<(), String>,
    ) -> Result<(), String> {
        let mut candidate = self.vault.clone();
        edit(&mut candidate)?;
        candidate.validate()?;
        let plain =
            Zeroizing::new(serde_json::to_vec(&candidate).map_err(|_| "Cannot encode vault")?);
        if plain.len() as u64 > MAX_VAULT_BYTES {
            return Err("Vault exceeds size limit".into());
        }
        let encrypted = dpapi(&plain, false)?;
        atomic_write(&self.path, &encrypted)?;
        self.vault = candidate;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn atomic_replacement_preserves_final_bytes() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("vault");
        atomic_write(&path, b"first").unwrap();
        atomic_write(&path, b"second").unwrap();
        assert_eq!(std::fs::read(path).unwrap(), b"second");
    }
    #[test]
    #[cfg(windows)]
    fn dpapi_roundtrip_and_tamper_detection() {
        let plain = b"synthetic-test-password";
        let mut encrypted = dpapi(plain, false).unwrap();
        assert_ne!(&encrypted[..plain.len()], plain);
        assert_eq!(dpapi(&encrypted, true).unwrap(), plain);
        let last = encrypted.len() - 1;
        encrypted[last] ^= 1;
        assert!(dpapi(&encrypted, true).is_err());
    }
    #[test]
    #[cfg(windows)]
    fn store_reopens_without_a_master_password() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("vault");
        let mut store = Store::open(path.clone()).unwrap();
        store
            .update(|v| {
                v.known_hosts
                    .insert("[localhost]:22".into(), "SHA256:fixture".into());
                Ok(())
            })
            .unwrap();
        assert_eq!(Store::open(path).unwrap().vault.known_hosts.len(), 1);
    }
    #[test]
    fn corrupt_file_is_not_overwritten() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("vault");
        std::fs::write(&path, b"not-a-vault").unwrap();
        assert!(Store::open(path.clone()).is_err());
        assert_eq!(std::fs::read(path).unwrap(), b"not-a-vault");
    }
    #[test]
    #[cfg(windows)]
    fn saved_secrets_are_encrypted_and_failed_edits_do_not_commit() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("vault");
        let mut store = Store::open(path.clone()).unwrap();
        let id = uuid::Uuid::new_v4().to_string();
        store
            .update(|v| {
                v.secrets.insert(
                    id.clone(),
                    crate::model::Secret {
                        password: "synthetic-never-plaintext".into(),
                        passphrase: String::new(),
                    },
                );
                Ok(())
            })
            .unwrap();
        let before = std::fs::read(&path).unwrap();
        assert!(
            !before
                .windows(b"synthetic-never-plaintext".len())
                .any(|w| w == b"synthetic-never-plaintext")
        );
        assert_eq!(
            Store::open(path.clone()).unwrap().vault.secrets[&id].password,
            "synthetic-never-plaintext"
        );
        assert!(
            store
                .update(|v| {
                    v.schema = 999;
                    Ok(())
                })
                .is_err()
        );
        assert_eq!(store.vault.schema, 1);
        assert_eq!(std::fs::read(path).unwrap(), before);
    }
}
