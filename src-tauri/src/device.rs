use crate::crypto::VaultKey;
#[cfg(windows)]
use crate::storage::atomic_write;
use std::path::Path;
use zeroize::Zeroizing;

#[cfg(windows)]
pub fn dpapi(data: &[u8], decrypt: bool) -> Result<Vec<u8>, String> {
    use windows_sys::Win32::{Foundation::LocalFree, Security::Cryptography::*};
    if data.len() > crate::crypto::MAX_FILE as usize {
        return Err("DPAPI data exceeds limit".into());
    }
    let input = CRYPT_INTEGER_BLOB {
        cbData: data.len() as u32,
        pbData: data.as_ptr().cast_mut(),
    };
    let mut output = CRYPT_INTEGER_BLOB {
        cbData: 0,
        pbData: std::ptr::null_mut(),
    };
    // User-scoped only. DPAPI owns the allocated buffer until LocalFree.
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
                "Windows could not unlock the device key. Enter the vault password instead.".into(),
            );
        }
        let slice = std::slice::from_raw_parts_mut(output.pbData, output.cbData as usize);
        let result = slice.to_vec();
        zeroize::Zeroize::zeroize(slice);
        LocalFree(output.pbData.cast());
        Ok(result)
    }
}
pub fn load(dir: &Path, id: &str) -> Result<VaultKey, String> {
    uuid::Uuid::parse_str(id).map_err(|_| "Invalid vault identifier")?;
    #[cfg(windows)]
    let bytes = Zeroizing::new(dpapi(
        &super::storage::read_limited(&dir.join(format!("{id}.dpapi")), 16384)?,
        true,
    )?);
    #[cfg(target_os = "linux")]
    let bytes = {
        let _ = dir;
        Zeroizing::new(
            entry(id)?
                .get_secret()
                .map_err(|_| "No usable device key; enter the vault password")?,
        )
    };
    #[cfg(not(any(windows, target_os = "linux")))]
    let bytes: Zeroizing<Vec<u8>> = {
        let _ = dir;
        return Err("Automatic unlock is unavailable on this OS".into());
    };
    let key: [u8; 32] = bytes
        .as_slice()
        .try_into()
        .map_err(|_| "Invalid device key length")?;
    Ok(Zeroizing::new(key))
}
pub fn save(dir: &Path, id: &str, key: &[u8; 32]) -> Result<(), String> {
    uuid::Uuid::parse_str(id).map_err(|_| "Invalid vault identifier")?;
    #[cfg(windows)]
    {
        atomic_write(&dir.join(format!("{id}.dpapi")), &dpapi(key, false)?)?;
    }
    #[cfg(target_os = "linux")]
    {
        let _ = dir;
        entry(id)?.set_secret(key).map_err(
            |_| "Linux Secret Service is unavailable or locked; use the vault password at startup",
        )?;
    }
    Ok(())
}
pub fn forget(dir: &Path, id: &str) -> Result<(), String> {
    uuid::Uuid::parse_str(id).map_err(|_| "Invalid vault identifier")?;
    #[cfg(windows)]
    {
        let path = dir.join(format!("{id}.dpapi"));
        match std::fs::remove_file(path) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(_) => return Err("Cannot remove remembered device key".into()),
        }
    }
    #[cfg(target_os = "linux")]
    {
        let _ = dir;
        match entry(id)?.delete_credential() {
            Ok(()) | Err(keyring::Error::NoEntry) => {}
            Err(_) => return Err("Cannot remove remembered key from Linux Secret Service".into()),
        }
    }
    Ok(())
}
#[cfg(target_os = "linux")]
fn entry(id: &str) -> Result<keyring::Entry, String> {
    keyring::Entry::new("com.nordssh.desktop", id)
        .map_err(|_| "Cannot access Linux Secret Service".into())
}
