use crate::model::Vault;
use aes_gcm::{
    Aes256Gcm, KeyInit, Nonce,
    aead::{Aead, OsRng, Payload, rand_core::RngCore},
};
use argon2::{Algorithm, Argon2, Params, Version};
use base64::{Engine, engine::general_purpose::STANDARD};
use serde::{Deserialize, Serialize};
use zeroize::Zeroizing;

pub const MAX_FILE: u64 = 12 * 1024 * 1024;
pub const MAX_PLAIN: usize = 8 * 1024 * 1024;
pub type VaultKey = Zeroizing<[u8; 32]>;
#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Sealed {
    nonce: [u8; 12],
    ciphertext: String,
}
#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Envelope {
    pub format: u32,
    pub id: String,
    salt: [u8; 16],
    pub wrapped: Sealed,
    pub data: Sealed,
}

fn derive(password: &str, salt: &[u8; 16]) -> Result<VaultKey, String> {
    if password.len() > 4096 {
        return Err("Password exceeds size limit".into());
    }
    let params = Params::new(65536, 3, 1, Some(32)).map_err(|_| "Invalid KDF configuration")?;
    let mut key = Zeroizing::new([0u8; 32]);
    Argon2::new(Algorithm::Argon2id, Version::V0x13, params)
        .hash_password_into(password.as_bytes(), salt, key.as_mut())
        .map_err(|_| "Password derivation failed")?;
    Ok(key)
}
fn seal(key: &[u8; 32], bytes: &[u8], aad: &[u8]) -> Result<Sealed, String> {
    let mut nonce = [0; 12];
    OsRng.fill_bytes(&mut nonce);
    let ciphertext = Aes256Gcm::new_from_slice(key)
        .map_err(|_| "Invalid encryption key")?
        .encrypt(&Nonce::from(nonce), Payload { msg: bytes, aad })
        .map_err(|_| "Encryption failed")?;
    Ok(Sealed {
        nonce,
        ciphertext: STANDARD.encode(ciphertext),
    })
}
fn unseal(key: &[u8; 32], sealed: &Sealed, aad: &[u8]) -> Result<Zeroizing<Vec<u8>>, String> {
    if sealed.ciphertext.len() > MAX_FILE as usize {
        return Err("Encrypted payload exceeds limit".into());
    }
    let bytes = STANDARD
        .decode(&sealed.ciphertext)
        .map_err(|_| "Invalid encrypted encoding")?;
    Aes256Gcm::new_from_slice(key)
        .map_err(|_| "Invalid encryption key")?
        .decrypt(&Nonce::from(sealed.nonce), Payload { msg: &bytes, aad })
        .map(Zeroizing::new)
        .map_err(|_| {
            "Wrong password, wrong device key, or damaged vault. Original data was not changed."
                .into()
        })
}
impl Envelope {
    pub fn validate(&self) -> Result<(), String> {
        if self.format != 1 || uuid::Uuid::parse_str(&self.id).is_err() {
            return Err("Unsupported vault format; original file preserved".into());
        }
        if self.wrapped.ciphertext.len() != 64 || self.data.ciphertext.len() > MAX_FILE as usize {
            return Err("Invalid encrypted vault size".into());
        }
        Ok(())
    }
    fn header(&self) -> Vec<u8> {
        format!("NordSSH/v1/{}/{}", self.id, STANDARD.encode(self.salt)).into_bytes()
    }
    fn data_aad(&self) -> Vec<u8> {
        let mut aad = self.header();
        aad.extend_from_slice(&self.wrapped.nonce);
        aad.extend_from_slice(self.wrapped.ciphertext.as_bytes());
        aad
    }
    pub fn create(vault: &Vault, password: &str) -> Result<(Self, VaultKey), String> {
        if password.chars().count() < 7 || password.len() > 4096 {
            return Err(
                "Use a vault password of at least 7 characters (maximum 4096 bytes).".into(),
            );
        }
        let mut salt = [0; 16];
        OsRng.fill_bytes(&mut salt);
        let mut key = Zeroizing::new([0; 32]);
        OsRng.fill_bytes(key.as_mut());
        let placeholder = Sealed {
            nonce: [0; 12],
            ciphertext: String::new(),
        };
        let mut envelope = Self {
            format: 1,
            id: uuid::Uuid::new_v4().to_string(),
            salt,
            wrapped: placeholder.clone(),
            data: placeholder,
        };
        let wrapping = derive(password, &salt)?;
        envelope.wrapped = seal(&wrapping, key.as_ref(), &envelope.header())?;
        envelope.set_vault(vault, &key)?;
        Ok((envelope, key))
    }
    pub fn unlock(&self, password: &str) -> Result<(Vault, VaultKey), String> {
        self.validate()?;
        let wrapping = derive(password, &self.salt)?;
        let wrapped = unseal(&wrapping, &self.wrapped, &self.header())?;
        let bytes: [u8; 32] = wrapped
            .as_slice()
            .try_into()
            .map_err(|_| "Invalid wrapped vault key")?;
        let key = Zeroizing::new(bytes);
        let vault = self.read_vault(&key)?;
        Ok((vault, key))
    }
    pub fn read_vault(&self, key: &[u8; 32]) -> Result<Vault, String> {
        self.validate()?;
        let bytes = unseal(key, &self.data, &self.data_aad())?;
        if bytes.len() > MAX_PLAIN {
            return Err("Vault exceeds plaintext size limit".into());
        }
        let v: Vault =
            serde_json::from_slice(&bytes).map_err(|_| "Invalid decrypted vault schema")?;
        v.validate()?;
        Ok(v)
    }
    pub fn set_vault(&mut self, vault: &Vault, key: &[u8; 32]) -> Result<(), String> {
        vault.validate()?;
        let plain = Zeroizing::new(serde_json::to_vec(vault).map_err(|_| "Cannot encode vault")?);
        if plain.len() > MAX_PLAIN {
            return Err("Vault exceeds plaintext size limit".into());
        }
        self.data = seal(key, &plain, &self.data_aad())?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    #[cfg(target_os = "linux")]
    fn glib_string_iteration_security_backport() {
        use glib::variant::ToVariant;
        let variant = vec!["one", "two", "three"].to_variant();
        let strings = variant.array_iter_str().unwrap().collect::<Vec<_>>();
        assert_eq!(strings, vec!["one", "two", "three"]);
    }
    #[test]
    fn portable_roundtrip_wrong_password_and_tamper() {
        let (mut e, k) = Envelope::create(&Vault::default(), "fixture-only-password").unwrap();
        assert!(e.unlock("incorrect password").is_err());
        assert!(e.unlock("fixture-only-password").is_ok());
        e.data.nonce[0] ^= 1;
        assert!(e.read_vault(&k).is_err());
    }
    #[test]
    fn header_and_wrapper_are_authenticated_with_device_key() {
        let (mut e, k) = Envelope::create(&Vault::default(), "fixture-only-password").unwrap();
        e.wrapped.nonce[0] ^= 1;
        assert!(e.read_vault(&k).is_err());
        e.wrapped.nonce[0] ^= 1;
        e.salt[0] ^= 1;
        assert!(e.read_vault(&k).is_err());
    }
    #[test]
    fn short_password_and_future_format_rejected() {
        assert!(Envelope::create(&Vault::default(), "short").is_err());
        assert!(Envelope::create(&Vault::default(), "123456").is_err());
        assert!(Envelope::create(&Vault::default(), "пароль").is_err());
        let (seven, _) = Envelope::create(&Vault::default(), "1234567").unwrap();
        assert!(seven.unlock("1234567").is_ok());
        let (mut e, k) = Envelope::create(&Vault::default(), "fixture-only-password").unwrap();
        e.format = 999;
        assert!(e.read_vault(&k).is_err());
    }
}
